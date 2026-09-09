#!/bin/ksh -p
# CDDL HEADER START
#
# The contents of this file are subject to the terms of the
# Common Development and Distribution License (the "License").
# You may not use this file except in compliance with the License.
#
# You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
# or https://opensource.org/licenses/CDDL-1.0.
# See the License for the specific language governing permissions
# and limitations under the License.
#
# When distributing Covered Code, include this CDDL HEADER in each
# file and include the License file at usr/src/OPENSOLARIS.LICENSE.
# If applicable, add the following below this CDDL HEADER, with the
# fields enclosed by brackets "[]" replaced with your own identifying
# information: Portions Copyright [yyyy] [name of copyright owner]
#
# CDDL HEADER END
#

#
# Copyright (c) 2026, Gernot Strasser
#

# DESCRIPTION:
#	Verify that a flat-DDT dedup write which needs more copies than an
#	identical write already in flight for the same entry still receives
#	every copy it asked for.
#
#	When a write is in flight for a DDT entry, later writes of the same
#	block attach to it rather than allocating again.  That is only
#	correct if the in-flight write will produce enough DVAs: a copies=3
#	write arriving behind an in-flight copies=1 write must chain another
#	write for the two missing DVAs, not wait on the first one and finish
#	with a single DVA.
#
#	dedup_gang_copies.ksh writes its copies=1 and copies=3 data in
#	separate txgs, so the first write has always completed by the time
#	the second arrives and this path is never exercised.
#
# STRATEGY:
#	1. Create a fast-dedup pool with a copies=1 and a copies=3 dataset.
#	2. Slow every vdev with zinject so child writes stay in flight.
#	3. Write identical random data to both datasets in the same txg, in
#	   both orders (the dataset dirtied last is synced first), so the
#	   copies=3 writes reach zio_ddt_write() while identical copies=1
#	   writes are still in flight, and the other way round.
#	4. Both files must read back correctly: with the bug, a copies=3
#	   write that joins after the in-flight write's READY is left as a
#	   hole and the file reads back as zeros, with no error reported.
#	5. Every L0 block of every copies=3 file must carry three DVAs.
#	6. Pool accounting must be intact (zdb -bcc).  The bug also leaks
#	   the DDT reference the lost write took.
#

. $STF_SUITE/include/libtest.shlib

verify_runnable "global"

log_assert "Concurrent dedup writes with differing copies all get their copies"

typeset orig_txg_timeout=$(get_tunable zfs_txg_timeout)
typeset tmpfile=$TEST_BASE_DIR/dedup_concurrent_copies.$$

function cleanup
{
	zinject -c all >/dev/null 2>&1
	if poolexists $TESTPOOL ; then
		destroy_pool $TESTPOOL
	fi
	rm -f $tmpfile
	log_must set_tunable32 zfs_txg_timeout $orig_txg_timeout
}

log_onexit cleanup

#
# Keep both copies of the data in one txg: nothing syncs until we ask.
#
log_must set_tunable32 zfs_txg_timeout 600

log_must zpool create -f -o feature@fast_dedup=enabled $TESTPOOL $DISKS
log_must zfs create -o recordsize=128k -o dedup=on -o compression=off \
    -o copies=1 $TESTPOOL/c1
log_must zfs create -o recordsize=128k -o dedup=on -o compression=off \
    -o copies=3 $TESTPOOL/c3

typeset mp1=$(get_prop mountpoint $TESTPOOL/c1)
typeset mp3=$(get_prop mountpoint $TESTPOOL/c3)

#
# Hold every write in flight for a while.  Multiple lanes, so the delay
# widens the window without serialising the whole sync.
#
for d in $DISKS; do
	log_must zinject -d $d -D25:16 $TESTPOOL
done

#
# Print the DVA count of every L0 block of a file, one per line.  At six
# d's zdb prints every DVA of each block pointer as vdev:offset:asize.
#
function l0_dva_counts # dataset file
{
	typeset ds=$1 file=$2
	typeset obj=$(ls -i $file | awk '{print $1}')

	zdb -dddddd $ds $obj | awk '
		$2 == "L0" {
			n = 0
			for (i = 3; i <= NF; i++)
				if ($i ~ /^[0-9]+:[0-9a-f]+:[0-9a-f]+$/)
					n++
			print n
		}'
}

#
# Birth txg of the first L0 block of a file.
#
function l0_birth # dataset file
{
	typeset ds=$1 file=$2
	typeset obj=$(ls -i $file | awk '{print $1}')

	zdb -dddddd $ds $obj | awk '
		$2 == "L0" {
			for (i = 3; i <= NF; i++)
				if ($i ~ /^B=/) {
					sub(/^B=/, "", $i)
					sub(/\/.*/, "", $i)
					print $i
					exit
				}
		}'
}

typeset -i rounds=8
typeset -i blocks=32
typeset -i r=0
while (( r < rounds )); do
	log_must dd if=/dev/urandom of=$tmpfile bs=128k count=$blocks

	#
	# Alternate the order: the dataset dirtied last is synced first, so
	# half the rounds put the copies=1 writes in flight first.
	#
	if (( r % 2 == 0 )); then
		log_must cp $tmpfile $mp3/f$r
		log_must cp $tmpfile $mp1/f$r
	else
		log_must cp $tmpfile $mp1/f$r
		log_must cp $tmpfile $mp3/f$r
	fi
	log_must sync_pool $TESTPOOL

	#
	# Data first.  On a kernel with the bug, a copies=3 write that joins
	# an in-flight copies=1 write after its READY never gets its block
	# pointer filled: the file is left as holes and reads back as zeros.
	#
	log_must cmp $tmpfile $mp1/f$r
	if ! cmp -s $tmpfile $mp3/f$r; then
		log_fail "round $r: copies=3 file reads back wrong;" \
		    "$(l0_dva_counts $TESTPOOL/c3 $mp3/f$r | grep -c .) of" \
		    "$blocks L0 blocks are allocated"
	fi

	typeset counts=$(l0_dva_counts $TESTPOOL/c3 $mp3/f$r)
	typeset -i nblk=$(echo "$counts" | grep -c .)
	typeset -i short=$(echo "$counts" | awk '$1 != 3' | grep -c .)
	if (( nblk != blocks )); then
		log_fail "round $r: expected $blocks L0 blocks, zdb shows $nblk"
	fi
	if (( short != 0 )); then
		log_note "$(zdb -dddddd $TESTPOOL/c3 \
		    $(ls -i $mp3/f$r | awk '{print $1}'))"
		log_fail "round $r: $short of $nblk copies=3 blocks have" \
		    "fewer than 3 DVAs"
	fi

	#
	# Everything checked out, so make sure that is not because the two
	# writes simply happened in different txgs and never overlapped.
	#
	typeset b1=$(l0_birth $TESTPOOL/c1 $mp1/f$r)
	typeset b3=$(l0_birth $TESTPOOL/c3 $mp3/f$r)
	if [[ -z "$b1" || "$b1" != "$b3" ]]; then
		log_fail "round $r: writes not in one txg (c1 B=$b1," \
		    "c3 B=$b3); the test did not exercise concurrent writes"
	fi
	(( r += 1 ))
done

log_must zinject -c all

#
# Same accounting checks as dedup_gang_copies.ksh: a chained write that
# failed to merge, or merged twice, shows up as a leak or a double free.
#
typeset out
typeset -i rc
out=$(zdb -bcc $TESTPOOL 2>&1)
rc=$?
if (( rc != 0 )); then
	log_note "$out"
	log_fail "zdb -bcc failed (exit $rc)"
fi
if ! echo "$out" | grep -q "No leaks (block sum matches space maps exactly)"
then
	log_note "$out"
	log_fail "zdb -bcc reported a space accounting error"
fi
if ! check_pool_status $TESTPOOL "errors" "No known data errors"; then
	log_note "$(zpool status -v $TESTPOOL)"
	log_fail "pool status not clean"
fi

log_pass "Concurrent dedup writes with differing copies all get their copies"
