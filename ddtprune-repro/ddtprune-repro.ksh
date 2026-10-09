#!/bin/ksh
#
# Reproducers for `zpool ddtprune` bugs fixed (or under review) upstream in
# openzfs/zfs, written against the illumos fast-dedup port
# (https://code.illumos.org/c/illumos-gate/+/4810, patchset 38).
#
#   name      upstream PR   port status (by code reading)  expect
#   --------  ------------  -----------------------------  ------
#   pct       #19264        present                        FAIL
#   dayovf    #19263        present (zpool_main.c)         FAIL
#   claim     #19263        present (spa_active_ddt_prune) FAIL  (needs dtrace)
#   stuck     #19263        fixed in port                  PASS
#   leak      #17983        fixed in port (direct free)    PASS
#   logflush  #19223        present                        FAIL
#
# Usage:  ddtprune-repro.ksh [test ...]      (default: all of the above)
# Env:    DISK=<vdev>   use this disk/file instead of a scratch file vdev
#         NODTRACE=1    don't use dtrace (claim is skipped, logflush falls
#                       back to timing only and becomes probabilistic)
#
# Must run as root in the global zone.  Tunables are poked with mdb -kw and
# restored on exit.  Exit status is the number of FAILs.
#

POOL=ddtprune_repro
WORK=${WORK:-/var/tmp/ddtprune-repro.$$}
MNT=/$POOL/fs
typeset -i fail=0
typeset -A saved

log()  { print -- "$*"; }
pass() { log "  PASS: $*"; }
bad()  { log "  FAIL: $*"; fail=fail+1; }
skip() { log "  SKIP: $*"; }

#
# Tunables.  All 32-bit (uint_t / uint32_t / boolean_t).  zfs_ddt_prunes_per_txg
# is static in ddt.c; mdb still resolves it through the module symtab.
#
tget() { print "zfs\`$1/D" | mdb -k 2>/dev/null | nawk 'NF >= 2 {v = $NF} END {print v}'; }
tset() {
	[[ -z ${saved[$1]} ]] && saved[$1]=$(tget $1)
	print "zfs\`$1/W 0t$2" | mdb -kw >/dev/null 2>&1 || {
		log "cannot set tunable $1"; exit 99; }
}
trestore() {
	typeset t
	for t in ${!saved[@]}; do
		print "zfs\`$t/W 0t${saved[$t]}" | mdb -kw >/dev/null 2>&1
	done
}

have_dtrace() { [[ -z $NODTRACE ]] && whence dtrace >/dev/null; }

mkpool() {			# [recordsize]
	zpool destroy -f $POOL 2>/dev/null
	typeset vdev=$DISK
	if [[ -z $vdev ]]; then
		mkdir -p $WORK
		vdev=$WORK/vdev
		rm -f $vdev
		mkfile 512m $vdev || exit 99
	fi
	zpool create -f -o feature@fast_dedup=enabled $POOL $vdev ||
	    { log "cannot create pool"; exit 99; }
	zfs create -o dedup=on -o compression=off \
	    -o recordsize=${1:-128k} $POOL/fs || exit 99
}

ddt_entries() {
	zpool status -D $POOL | nawk '/dedup: DDT entries/ {sub(",", "", $4); n = $4} END {print n + 0}'
}

# Same recipe as the port's dedup_prune*.ksh: swap the log every txg and
# lift the per-txg flush floor so the whole log drains in a couple of syncs.
# Callers that want the log to stay put set the tunables back afterwards.
drain() {
	tset zfs_dedup_log_txg_max 1
	tset zfs_dedup_log_flush_entries_min 1000000
	typeset -i i=0
	while (( i < 6 )); do zpool sync $POOL; i=i+1; done
	assert_fdt
}

# fast_dedup goes enabled -> active only when an FDT (flat + log) DDT is
# created (ddt_create_dir).  If the table came out legacy, ddt_prune_walk()
# skips it and every prune test would pass vacuously.
assert_fdt() {
	(( $(ddt_entries) == 0 )) && return
	[[ $(zpool get -H -o value feature@fast_dedup $POOL) == active ]] || {
		log "DDT is not FDT (feature@fast_dedup not active); aborting"
		exit 99
	}
}

zdb_clean() {
	zdb -bcc $POOL > $WORK/zdb.out 2>&1 || return 1
	grep -q "No leaks (block sum matches space maps exactly)" $WORK/zdb.out
}

dbgmsg() { print ::zfs_dbgmsg | mdb -k 2>/dev/null; }

# ----------------------------------------------------------------------------

t_pct() {
	log "== pct: -p 1 on a 3-entry DDT prunes every entry (openzfs#19264)"
	log "   target = 3 * 1 / 100 = 0 -> bin search skipped -> cutoff = now"
	mkpool
	dd if=/dev/urandom of=$MNT/f bs=128k count=3 2>/dev/null
	drain
	typeset -i before=$(ddt_entries)
	sleep 2				# class_start is 1s resolution, cutoff is <
	zpool ddtprune -p 1 $POOL
	drain
	typeset -i after=$(ddt_entries)
	log "   entries: $before -> $after"
	(( before == 3 )) || log "   note: expected 3 entries before prune"
	if (( after == before )); then
		pass "-p 1 with a zero-entry target is a no-op"
	else
		bad "-p 1 pruned $((before - after)) of $before entries"
	fi
}

t_dayovf() {
	log "== dayovf: -d N overflows N*86400 and wraps to a small age (openzfs#19263)"
	# 213503982334602 * 86400 mod 2^64 = 61184 s (~17h)
	typeset days=213503982334602
	mkpool
	# give entries fake ages of up to ~24 days so a 17h cutoff bites
	tset ddt_prune_artificial_age 1
	dd if=/dev/urandom of=$MNT/f bs=128k count=1000 2>/dev/null
	tset ddt_prune_artificial_age 0
	drain
	typeset -i before=$(ddt_entries)
	zpool ddtprune -d $days $POOL 2>&1 | sed 's/^/   /'
	drain
	typeset -i after=$(ddt_entries)
	log "   entries: $before -> $after"
	dbgmsg | grep "prune 61184 seconds" | tail -1 | sed 's/^/   dbgmsg: /'
	if (( after != before )); then
		bad "-d $days (older than 5.8e17 years) pruned $((before - after)) entries"
	elif ! zpool ddtprune -d $days $POOL >/dev/null 2>&1; then
		pass "-d $days rejected"
	else
		bad "-d $days accepted (wraps to 61184 s); nothing old enough to show damage"
	fi
}

t_claim() {
	log "== claim: two concurrent prunes both claim spa_active_ddt_prune (openzfs#19263)"
	have_dtrace || { skip "needs dtrace"; return; }
	mkpool 512
	dd if=/dev/urandom of=$MNT/f bs=1024k count=8 2>/dev/null
	drain
	sleep 2
	#
	# Stall the first prune between "if (spa_active_ddt_prune)" and
	# "spa_active_ddt_prune = B_TRUE" (ddt_total_entries() ->
	# ddt_get_dedup_object_stats() sits in that window), and start the
	# second one while it is stalled.  chill() is capped at 500ms/s, so
	# only the first caller is stalled.
	#
	dtrace -q -w -n '
	    fbt::ddt_get_dedup_object_stats:entry
	    /execname == "zpool" && !fired/
	    { fired = 1; chill(400000000); }' 2>/dev/null &
	typeset dt=$!
	sleep 3				# let dtrace enable its probes
	zpool ddtprune -p 100 $POOL > $WORK/a.out 2>&1 &
	typeset a=$!
	sleep 0.1
	zpool ddtprune -p 100 $POOL > $WORK/b.out 2>&1
	typeset -i rb=$?
	wait $a; typeset -i ra=$?
	kill $dt 2>/dev/null; wait $dt 2>/dev/null
	sed 's/^/   A: /' $WORK/a.out; sed 's/^/   B: /' $WORK/b.out
	log "   rc A=$ra B=$rb"
	if (( ra == 0 && rb == 0 )); then
		bad "both prunes ran; the first to finish clears the mark under the other"
	else
		pass "second prune refused while the first held the mark"
	fi
}

t_stuck() {
	log "== stuck: rejected prune must not leave 'already in progress' (openzfs#19263)"
	mkpool
	dd if=/dev/urandom of=$MNT/f bs=128k count=3 2>/dev/null
	drain
	sleep 2
	zpool ddtprune -d 30000 $POOL 2>&1 | sed 's/^/   /'	# before epoch: EINVAL
	if zpool ddtprune -p 100 $POOL 2>&1 | tee $WORK/s.out | grep -qi "in progress"; then
		bad "mark left set: $(cat $WORK/s.out)"
	else
		pass "next prune accepted"
	fi
	drain
	zpool ddtprune -p 100 $POOL >/dev/null 2>&1	# empty table path
	if zpool ddtprune -p 100 $POOL 2>&1 | grep -qi "in progress"; then
		bad "mark left set after empty-table prune"
	else
		pass "empty-table prune leaves no mark"
	fi
}

t_leak() {
	log "== leak: free after prune must not leak (openzfs#17983)"
	mkpool
	dd if=/dev/urandom of=$MNT/f bs=1024k count=16 2>/dev/null
	drain
	sleep 2
	zpool ddtprune -p 100 $POOL
	rm $MNT/f
	drain
	if zdb_clean; then
		pass "zdb -bcc: no leaks"
	else
		bad "zdb -bcc after prune + free:"; tail -8 $WORK/zdb.out | sed 's/^/   /'
	fi
}

#
# openzfs#19223.  ddt_prune_walk() reads candidates from the on-disk UNIQUE
# ZAP, which lags the dedup log.  If an entry's second reference is still in
# the log when the walk reads it, and the log flushes (moving it to the
# DUPLICATE ZAP) before prune_candidates_sync() runs, the recheck there
# misses it: it is not on ddt_tree, ddt_lookup_unique() falls through to the
# DUPLICATE class and loads it with dde_logged == B_FALSE, and the DVAs
# still match.  The refcnt-2 entry is cleared.  Freeing either reference
# then goes through zio_ddt_free()'s direct-free fallback and frees a block
# the other reference still points to.
#
# Setup: every UNIQUE-ZAP entry gets a second reference that is pinned in
# the active log (txg_max huge => no swap).  The prune walks in small batches;
# once the walk is under way the log is released (txg_max=1, flush floor
# huge) and a sync loop drains it in one txg.  dtrace stalls each batch
# just before its sync task so that flush lands between walk and recheck.
# Correct behaviour: nothing is pruned (every entry has refcnt 2).
#
t_logflush() {
	log "== logflush: prune clears an entry that gained a ref via log flush (openzfs#19223)"
	mkpool 512
	dd if=/dev/urandom of=$MNT/f1 bs=1024k count=4 2>/dev/null	# 8192 entries
	drain
	typeset -i n=$(ddt_entries)
	sleep 2

	# Pin the log: no swap, so nothing flushes.
	tset zfs_dedup_log_txg_max 1000000
	cp $MNT/f1 $MNT/f2		# every entry -> refcnt 2, in the log only
	zpool sync $POOL; zpool sync $POOL
	tset zfs_ddt_prunes_per_txg $((n / 32))

	typeset dt=""
	if have_dtrace; then
		dtrace -q -w -n '
		    fbt::ddt_prune_walk:entry /arg1 != 0/ { self->p = 1; }
		    fbt::ddt_prune_walk:return /self->p/ { self->p = 0; }
		    fbt::dsl_sync_task:entry /self->p/ { chill(300000000); }' \
		    2>/dev/null &
		dt=$!
		sleep 3
	else
		log "   (no dtrace: timing-only, may need several runs)"
	fi

	print > $WORK/syncing
	( while [[ -f $WORK/syncing ]]; do zpool sync $POOL; done ) &
	typeset sl=$!

	zpool ddtprune -p 100 $POOL &
	typeset pr=$!
	sleep 0.5			# walk is running; release the log
	tset zfs_dedup_log_txg_max 1
	wait $pr
	rm -f $WORK/syncing; wait $sl
	[[ -n $dt ]] && { kill $dt 2>/dev/null; wait $dt 2>/dev/null; }

	tset zfs_ddt_prunes_per_txg ${saved[zfs_ddt_prunes_per_txg]}
	drain
	typeset -i after=$(ddt_entries)
	dbgmsg | grep "pruned .* entries" | tail -1 | sed 's/^/   dbgmsg: /'
	log "   entries (all refcnt 2): $n -> $after"
	if (( after == n )); then
		pass "no duplicate entry pruned"
		return
	fi
	bad "$((n - after)) refcnt-2 entries pruned"

	# Show the consequence: free one copy, the shared blocks go with it.
	rm $MNT/f2
	drain
	if zdb_clean; then
		log "   zdb -bcc clean after freeing f2 (unexpected)"
	else
		log "   zdb -bcc after freeing f2 (f1 now points at free space):"
		grep -v "^Traversing\|^loading\|^$" $WORK/zdb.out | tail -8 | sed 's/^/     /'
	fi
}

# ----------------------------------------------------------------------------

cleanup() {
	rm -f $WORK/syncing
	zpool destroy -f $POOL 2>/dev/null
	trestore
	rm -rf $WORK
}
trap cleanup EXIT
trap 'exit 98' INT TERM

[[ $(zonename 2>/dev/null) == global ]] || { log "run in the global zone"; exit 99; }
mkdir -p $WORK

tests=${*:-pct dayovf claim stuck leak logflush}
for t in $tests; do
	t_$t
done
log
log "$fail failure(s)"
exit $fail
