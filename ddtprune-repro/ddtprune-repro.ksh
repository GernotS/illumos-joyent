#!/bin/ksh
#
# Reproducers for the `zpool ddtprune` bugs found in openzfs/zfs PRs, to be run
# against an illumos fast-dedup (FDT) port.  CLI only; needs root, a pool-capable
# kernel with feature@fast_dedup, and zdb.  Uses a throw-away file-backed pool.
#
#   1  openzfs/zfs#19264  -p 1 on a tiny DDT prunes EVERY unique entry
#   2  openzfs/zfs#19263  rejected prune leaves "already in progress" stuck
#   3  openzfs/zfs#19263  -d overflow wraps to a small age, prunes recent entries
#   4  openzfs/zfs#17983  pruned entry still in DDT on free => space leak
#   5  openzfs/zfs#19223  prune races log flush => dedup'd block freed while
#                         still referenced (probabilistic stress; see below)
#
# Usage: ddtprune-repro.ksh [test# ...]     (default: all)
# Each test prints PASS / FAIL; exit status is the number of failures.
#

POOL=ddtprune_repro
DIR=${DIR:-/var/tmp/ddtprune-repro.$$}
VDEV=$DIR/vdev
MNT=/$POOL
fail=0

log()  { print -- "$*"; }
pass() { log "PASS: $*"; }
bad()  { log "FAIL: $*"; fail=$((fail + 1)); }


mkpool() {
	zpool destroy -f $POOL 2>/dev/null
	rm -f $VDEV; mkdir -p $DIR
	mkfile 256m $VDEV 2>/dev/null || dd if=/dev/zero of=$VDEV bs=1M count=256 2>/dev/null
	zpool create -f -o feature@fast_dedup=enabled $POOL $VDEV || { log "cannot create pool"; exit 99; }
	zfs set dedup=on $POOL
	zfs set compression=off $POOL
	zfs set recordsize=128k $POOL
}

# write N unique 128k blocks (random, so each is a unique DDT entry)
write_unique() {		# file nblocks
	dd if=/dev/urandom of=$1 bs=128k count=$2 2>/dev/null
}

# count entries in the DDT via zdb -D ("DDT-sha256-zap-unique: N entries" etc)
ddt_entries() {
	zdb -D $POOL 2>/dev/null | nawk '/^DDT-/ { for (i = 2; i <= NF; i++) if ($i == "entries,") n += $(i-1) } END { print n + 0 }'
}

settle() { zpool sync $POOL; sleep 1; zpool sync $POOL; }

# clean = zdb finds no leak, no claim errors
zdb_clean() {
	zdb -bcc $POOL > $DIR/zdb.out 2>&1
	grep -E "leaked|block .* has|bp.*free|error" $DIR/zdb.out >/dev/null && return 1
	return 0
}

t1() {	log "== 1: -p 1 on a tiny DDT must not prune everything (#19264)"
	mkpool
	write_unique $MNT/f 3; settle
	before=$(ddt_entries)
	zpool ddtprune -p 1 $POOL; settle
	after=$(ddt_entries)
	[[ $before -eq 3 ]] || log "  note: expected 3 entries before, saw $before"
	[[ $after -eq $before ]] && pass "-p 1 kept all $before entries" || \
	    bad "-p 1 pruned $before -> $after entries (target rounds to 0 entries; should be a no-op)"
	zpool ddtprune -p 100 $POOL; settle
	[[ $(ddt_entries) -eq 0 ]] && pass "-p 100 pruned all" || bad "-p 100 left entries"
}

t2() {	log "== 2: rejected prune must not leave 'already in progress' (#19263)"
	mkpool
	write_unique $MNT/f 3; settle
	# age before the epoch -> EINVAL in the kernel
	zpool ddtprune -d 30000 $POOL 2>&1 | sed 's/^/  /'
	out=$(zpool ddtprune -p 100 $POOL 2>&1)
	if print -- "$out" | grep -qi "already in progress"; then
		bad "mark stuck after rejected prune: $out"
		log "  (pool I/O also keeps SCL_ZIO priority until export/import)"
	else
		pass "next prune accepted after rejected one"
	fi
	# empty-table path must also release the mark
	zpool ddtprune -p 100 $POOL; zpool ddtprune -p 100 $POOL 2>&1 | grep -qi "already in progress" && \
	    bad "mark held after empty-table prune" || pass "empty-table prune releases mark"
}

t3() {	log "== 3: -d overflow must be rejected, not wrapped (#19263)"
	mkpool
	write_unique $MNT/f 3; settle
	before=$(ddt_entries)
	# days*86400 overflows 64-bit time; wraps to a small age
	zpool ddtprune -d 213503982334602 $POOL 2>&1 | sed 's/^/  /'; rc=$?
	settle
	after=$(ddt_entries)
	[[ $after -eq $before ]] && pass "entries untouched by overflowing -d" || \
	    bad "overflowing -d pruned recent entries ($before -> $after)"
}

t4() {	log "== 4: prune must not leak space (#17983)"
	mkpool
	zpool list -Hpo free $POOL > $DIR/free0
	write_unique $MNT/f 200; settle
	zpool ddtprune -p 100 $POOL; settle      # entries gone, blocks still live
	rm -f $MNT/f; settle; settle
	zdb_clean && pass "zdb -bcc clean after prune + rm" || { bad "leak/claim errors"; sed 's/^/  /' $DIR/zdb.out | tail -15; }
	f0=$(cat $DIR/free0); f1=$(zpool list -Hpo free $POOL)
	[[ $f1 -ge $((f0 - 1048576)) ]] && pass "free space returned ($f0 -> $f1)" || bad "space leaked: free $f0 -> $f1"
}

# 5: probabilistic.  A duplicate whose 2nd ref is still in the DDT log is
# sitting in the *unique* ZAP class on disk; a log flush between the prune walk
# and the prune sync task moves it to the duplicate class.  If the port's
# recheck accepts it, the entry is removed with refcount 2.
# We maximise the window: constant dup writes + frequent txg sync + prune loop.
t5() {	log "== 5: prune vs log flush race (#19223) - stress, ${T5_SECS:-120}s"
	mkpool
	zfs create $POOL/a; zfs create $POOL/b
	# tiny txgs so the log flushes often (illumos: set in /etc/system or live)
	#   echo zfs_txg_timeout/W0t1 | mdb -kw
	[[ -n $T5_ISOLATED_TXG ]] && echo "zfs_txg_timeout/W0t1" | mdb -kw >/dev/null 2>&1
	end=$((SECONDS + ${T5_SECS:-120}))
	( i=0; while ((SECONDS < end)); do
		dd if=/dev/urandom of=/$POOL/a/blk.$i bs=128k count=1 2>/dev/null
		i=$((i+1)); done ) &
	w=$!
	( while ((SECONDS < end)); do
		for f in /$POOL/a/blk.*; do cp $f /$POOL/b/${f##*/} 2>/dev/null; done
		zpool ddtprune -p 100 $POOL >/dev/null 2>&1; done ) &
	p=$!
	( while ((SECONDS < end)); do zpool sync $POOL; done ) &
	s=$!
	wait $w $p $s
	settle
	zfs snapshot -r $POOL@s
	# freeing one reference must not free the block under the other
	rm -rf /$POOL/a/*; settle; settle
	zdb_clean && pass "no claim errors / frees-while-referenced" || \
	    { bad "dedup'd block freed while still referenced"; sed 's/^/  /' $DIR/zdb.out | tail -20; }
	# data still readable & identical
	scrub_out=$(zpool scrub $POOL; sleep 5; zpool status $POOL | grep -i "errors:")
	print -- "  $scrub_out"
}

cleanup() { zpool destroy -f $POOL 2>/dev/null; rm -rf $DIR; }
trap cleanup EXIT

tests=${*:-1 2 3 4 5}
for n in $tests; do t$n; done
log; log "$fail failure(s)"
exit $fail
