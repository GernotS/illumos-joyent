#!/bin/bash
#
# zstd-fdt-pool-test.sh - exercise zstd compression and fast dedup (FDT) on
# an existing pool, by default the mirror pool "copy".
#
# Usage: zstd-fdt-pool-test.sh [-p pool] [-s] [-k]
#   -p pool   pool to test (default: copy)
#   -s        also scrub the whole pool at the end (can take long)
#   -k        keep the test dataset and source files (default: remove)
#
# Everything is done inside a new dataset <pool>/zstdfdt-test.<pid>, which is
# destroyed at the end; nothing else in the pool is modified. The pool must
# already have feature@zstd_compress and feature@fast_dedup enabled. Note
# that using them is what makes them "active": zstd_compress returns to
# "enabled" once the test dataset is gone (unless other datasets use zstd),
# fast_dedup may stay active.
#
# Needs: a SmartOS/illumos platform with zstd (zdb -Z) and the FDT port.
#

set -u
export LC_ALL=C

POOL=copy
SCRUB=false
KEEP=false
while getopts p:sk opt; do
	case $opt in
	p) POOL=$OPTARG ;;
	s) SCRUB=true ;;
	k) KEEP=true ;;
	*) echo "usage: $0 [-p pool] [-s] [-k]" >&2; exit 2 ;;
	esac
done

BASE=$POOL/zstdfdt-test.$$
SRC=/var/tmp/zstdfdt-src.$$
LEVELS="zstd zstd-1 zstd-9 zstd-19 zstd-fast zstd-fast-10 zstd-fast-1000"

PASS=0
FAIL=0
pass() { echo "  PASS  $*"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL  $*"; FAIL=$((FAIL + 1)); }
info() { echo "        $*"; }
head1() { echo; echo "== $*"; }
die() { echo "ERROR: $*" >&2; exit 2; }

cleanup()
{
	if $KEEP; then
		echo; echo "Kept $BASE and $SRC"
		return
	fi
	# Only ever destroy the dataset this script created.
	case $BASE in
	"$POOL"/zstdfdt-test.[0-9]*)
		zfs list -H -o name "$BASE" >/dev/null 2>&1 &&
		    zfs destroy -r "$BASE"
		;;
	esac
	rm -rf "$SRC"
}

# Expected value of the level field in the zstd block header
# (enum zio_zstd_levels) for a compression property value.
level_enum()
{
	local n
	case $1 in
	zstd)		echo 3 ;;
	zstd-fast)	echo 103 ;;
	zstd-fast-*)
		n=${1#zstd-fast-}
		if [ "$n" -le 10 ]; then echo $((102 + n))
		elif [ "$n" -le 100 ]; then echo $((111 + n / 10))
		elif [ "$n" -eq 500 ]; then echo 122
		else echo 123
		fi ;;
	zstd-*)		echo "${1#zstd-}" ;;
	esac
}

# Print the zstd header level of the first zstd block of a file.
zdb_level()
{
	local ds=$1 file=$2 obj
	obj=$(ls -i "$file" | awk '{print $1}')
	zdb -Zddddddbbbbbb "$ds" "$obj" 2>/dev/null |
	    sed -n 's/.* ZSTD:size=[0-9]*:version=[0-9]*:level=\([0-9]*\):.*/\1/p' |
	    head -1
}

# sha256 of every file under a directory, with relative names.
tree_sums()
{
	(cd "$1" && find . -type f | sort | while read -r f; do
		echo "$(digest -a sha256 "$f")  $f"
	done)
}

pool_alloc() { zpool sync "$POOL"; zpool list -Hp -o alloc "$POOL"; }

zstd_fail_counters()
{
	kstat -p -m zfs -n zstd 2>/dev/null |
	    awk '$1 ~ /fail|invalid/ {s += $2} END {print s + 0}'
}

# ---------------------------------------------------------------------------

head1 "Pool $POOL"
zpool list -H -o name "$POOL" >/dev/null 2>&1 || die "no pool $POOL"
zpool status "$POOL" | grep -q mirror &&
    info "mirror pool" || info "note: $POOL is not a mirror"
for f in zstd_compress fast_dedup; do
	st=$(zpool get -H -o value feature@$f "$POOL" 2>/dev/null)
	info "feature@$f: ${st:-unknown}"
	case $st in
	enabled|active) ;;
	*) die "feature@$f is not enabled on $POOL" ;;
	esac
done
zdb -\? 2>&1 | grep -q -- "-Z show ZSTD" ||
    die "this zdb has no -Z; boot the zstd platform build"

trap cleanup EXIT
FAIL0=$(zstd_fail_counters)

head1 "Preparing test data in $SRC"
mkdir -p "$SRC/text" "$SRC/random" || die "mkdir $SRC"
# Compressible: the man pages, as one file and as a tree.
(cd /usr/share && tar cf "$SRC/text/man.tar" man) 2>/dev/null
cp -r /usr/share/man "$SRC/text/man"
# Incompressible but dedupable: 64 MB of random data.
dd if=/dev/urandom of="$SRC/random/r64" bs=1048576 count=64 2>/dev/null
info "$(du -sh "$SRC" | awk '{print $1}') of test data"
SUMS_TEXT=$(tree_sums "$SRC/text")

zfs create -o compression=off "$BASE" || die "cannot create $BASE"

# ---------------------------------------------------------------------------

head1 "1. zstd levels: data intact, compressed, header level correct"
for lv in $LEVELS; do
	ds=$BASE/$lv
	zfs create -o compression="$lv" -o recordsize=128k "$ds" ||
	    { fail "$lv: zfs create"; continue; }
	cp -r "$SRC/text/." "/$ds/"
	zpool sync "$POOL"

	if [ "$(tree_sums "/$ds")" = "$SUMS_TEXT" ]; then
		pass "$lv: contents identical"
	else
		fail "$lv: contents differ"
	fi

	ratio=$(zfs get -H -o value compressratio "$ds")
	got=$(zdb_level "$ds" "/$ds/man.tar")
	want=$(level_enum "$lv")

	# The very fastest levels may not save the 12.5% ZFS requires before
	# it stores a block compressed; then blocks stay uncompressed, which
	# is correct behaviour, not a failure.
	if [ "$ratio" = 1.00x ] && [ -z "$got" ] && [ "$want" -ge 120 ]; then
		info "$lv: data not compressible enough at this level (1.00x)"
		continue
	fi
	case $ratio in
	1.00x) fail "$lv: compressratio $ratio (nothing compressed)" ;;
	*) pass "$lv: compressratio $ratio" ;;
	esac

	if [ -z "$got" ]; then
		fail "$lv: zdb -Z shows no zstd header"
	elif [ "$got" = "$want" ]; then
		pass "$lv: block header level $got"
	else
		fail "$lv: block header level $got, expected $want"
	fi
done
st=$(zpool get -H -o value feature@zstd_compress "$POOL")
[ "$st" = active ] && pass "feature@zstd_compress is active" ||
    fail "feature@zstd_compress is $st, expected active"

# ---------------------------------------------------------------------------

head1 "2. fast dedup on zstd datasets"
zfs create -o compression=zstd -o dedup=on "$BASE/dd1" &&
    zfs create -o compression=zstd -o dedup=on "$BASE/dd2" ||
    die "cannot create dedup datasets"
SUM_R=$(digest -a sha256 "$SRC/random/r64")

before=$(pool_alloc)
for i in 1 2 3 4; do cp "$SRC/random/r64" "/$BASE/dd1/r64.$i"; done
cp "$SRC/random/r64" "/$BASE/dd2/r64.other"
after=$(pool_alloc)
grew=$(( (after - before) / 1048576 ))
info "5 x 64 MB written (4 in dd1, 1 in dd2), pool allocation grew ${grew} MB"
# Without dedup this would be about 320 MB; allow for metadata and for other
# activity on the pool.
if [ "$grew" -lt 160 ]; then
	pass "duplicates were deduplicated (${grew} MB for 320 MB)"
else
	fail "no dedup: ${grew} MB allocated for 320 MB of identical data"
fi

ok=true
for f in "/$BASE"/dd1/r64.* "/$BASE/dd2/r64.other"; do
	[ "$(digest -a sha256 "$f")" = "$SUM_R" ] || { ok=false; info "bad: $f"; }
done
$ok && pass "all deduplicated copies read back intact" ||
    fail "deduplicated copies differ"

info "dedupratio: $(zpool get -H -o value dedupratio "$POOL")"
st=$(zpool get -H -o value feature@fast_dedup "$POOL")
[ "$st" = active ] && pass "feature@fast_dedup is active" ||
    fail "feature@fast_dedup is $st, expected active"
zpool status -D "$POOL" | sed -n '/dedup:/,/^$/p' | sed 's/^/        /'

# Same text data with dedup: identical files dedup at the same zstd level.
cp -r "$SRC/text/." "/$BASE/dd1/"
cp -r "$SRC/text/." "/$BASE/dd2/"
zpool sync "$POOL"
[ "$(cd "/$BASE/dd2" && tree_sums . | grep -v r64)" = "$SUMS_TEXT" ] &&
    pass "zstd text data intact in dedup dataset" ||
    fail "zstd text data differs in dedup dataset"

# ---------------------------------------------------------------------------

head1 "3. zfs send -c | zfs receive keeps data and zstd level"
zfs snapshot "$BASE/zstd-19@s" || fail "snapshot"
if zfs send -c "$BASE/zstd-19@s" | zfs receive "$BASE/recv"; then
	pass "send -c | receive"
	[ "$(tree_sums "/$BASE/recv")" = "$SUMS_TEXT" ] &&
	    pass "received contents identical" ||
	    fail "received contents differ"
	got=$(zdb_level "$BASE/recv" "/$BASE/recv/man.tar")
	[ "$got" = 19 ] && pass "received blocks still zstd-19" ||
	    fail "received block level ${got:-none}, expected 19"
else
	fail "send -c | receive"
fi

# ---------------------------------------------------------------------------

head1 "4. pool health"
FAILN=$(zstd_fail_counters)
[ "$FAILN" -eq "$FAIL0" ] &&
    pass "zstd kstat failure counters unchanged ($FAILN)" ||
    fail "zstd kstat failure counters went from $FAIL0 to $FAILN"
kstat -p -m zfs -n zstd 2>/dev/null | awk '$2 != 0' | sed 's/^/        /'

if $SCRUB; then
	info "scrubbing $POOL (this can take a while)"
	zpool scrub -w "$POOL"
fi
if zpool status -x "$POOL" | grep -q "is healthy"; then
	pass "zpool status: healthy, no errors"
else
	fail "zpool status reports problems:"
	zpool status -v "$POOL" | sed 's/^/        /'
fi

# ---------------------------------------------------------------------------

head1 "Result: $PASS passed, $FAIL failed"
if ! $KEEP; then
	cleanup
	KEEP=true	# cleanup done; the EXIT trap must not run it again
	trap - EXIT
	info "removed $BASE and $SRC"
	info "feature@zstd_compress now: $(zpool get -H -o value \
	    feature@zstd_compress "$POOL")"
fi
[ "$FAIL" -eq 0 ]
