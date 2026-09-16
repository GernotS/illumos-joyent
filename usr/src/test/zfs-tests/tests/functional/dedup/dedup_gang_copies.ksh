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
# Copyright (c) 2025 by Klara Inc.
# Copyright (c) 2026, Gernot Strasser
#

# DESCRIPTION:
#	Verify that mixed gang blocks and copies interact correctly with the
#	fast-dedup (FDT) flat-slot extend path.  A dedup write that must grow
#	an existing entry by extra copies must never merge a gang DVA into a
#	non-gang slot (an illegal mixed blkptr); such a write is written
#	plainly instead.  Regression test for the 1KB leak found by ztest and
#	fixed by the ddt_phys_is_gang / zp_no_gang change (upstream #17070 /
#	5f5321e).
#
# STRATEGY:
#	1. Pool with fast_dedup; two datasets, copies=1 and copies=3.
#	2. Store a copies=1 dedup block; then dedup-write it into the copies=3
#	   dataset while forcing ganging, so the extend path meets a gang.
#	3. Repeat with ganging on for the initial store as well.
#	4. After each phase, run `zdb -bcc` and require no leaked/other space
#	   errors, and `zpool status` to require no data errors.
#
# illumos adaptations vs upstream gang_blocks_ddt_copies:
#   - No METASLAB_FORCE_GANGING_PCT tunable here; illumos gangs
#     probabilistically (~1 in 4) once psize >= metaslab_force_ganging, so
#     we loop to make the interaction near-certain.
#   - metaslab_force_ganging is uint64_t -> set_tunable64.
#   - save/restore_tunable -> get_tunable + set_tunable*.
#   - self-contained (no gang_blocks.kshlib, which illumos lacks); the leak
#     check is `zdb -bcc`, exactly what caught the original bug.
#   - block_cloning gate dropped (not in this port).

. $STF_SUITE/include/libtest.shlib

verify_runnable "both"

log_assert "Mixed gang blocks and copies interact correctly in FDT dedup"

typeset orig_txg_max=$(get_tunable zfs_dedup_log_txg_max)
typeset orig_ganging=$(get_tunable metaslab_force_ganging)

function cleanup
{
	if poolexists $TESTPOOL ; then
		destroy_pool $TESTPOOL
	fi
	log_must set_tunable64 metaslab_force_ganging $orig_ganging
	log_must set_tunable32 zfs_dedup_log_txg_max $orig_txg_max
}

log_onexit cleanup

# Flush the dedup log every txg so entries reach the flat slots predictably.
log_must set_tunable32 zfs_dedup_log_txg_max 1

log_must zpool create -f -o ashift=9 -o feature@fast_dedup=enabled $TESTPOOL \
    $DISKS
log_must zfs create -o recordsize=64k -o dedup=on -o compression=off \
    $TESTPOOL/fs1
log_must zfs create -o recordsize=64k -o dedup=on -o compression=off \
    -o copies=3 $TESTPOOL/fs2

typeset mp1=$(get_prop mountpoint $TESTPOOL/fs1)
typeset mp2=$(get_prop mountpoint $TESTPOOL/fs2)

function check_intact
{
	#
	# Match the clean result positively.  zdb -bcc always prints the
	# banner "Traversing all blocks to verify checksums and verify
	# nothing leaked ...", so grepping the output for "leaked" matches
	# every run, clean or not -- and the test then only passes when zdb
	# produces no output at all.  A clean run ends with
	# "No leaks (block sum matches space maps exactly)"; a dirty one
	# prints "block traversal size N != alloc M (leaked X)" instead.
	# Check the exit status too, so a zdb that cannot run is a failure
	# rather than a silent pass.
	#
	typeset out
	typeset -i rc
	out=$(zdb -bcc $TESTPOOL 2>&1)
	rc=$?
	if (( rc != 0 )); then
		log_note "$out"
		log_fail "zdb -bcc failed (exit $rc)"
	fi
	if ! echo "$out" | grep -q "No leaks (block sum matches space maps exactly)"; then
		log_note "$out"
		log_fail "zdb -bcc reported a space accounting error"
	fi
	if ! check_pool_status $TESTPOOL "errors" "No known data errors"; then
		log_note "=== zpool status -v ==="
		log_note "$(zpool status -v $TESTPOOL)"
		log_fail "pool status not clean"
	fi

	#
	# The error log is only one of the two channels zio_done() reports
	# on.  vdev_stat_update() counts the error against the vdev much
	# earlier, so a bug fixed only above the ereport block leaves the
	# log clean while zpool status still shows a non-zero WRITE column.
	# Checking the log alone passes such a kernel.
	#
	# Any non-zero counter is a failure here: the pool is three healthy
	# disks doing a few megabytes of I/O.  Where the count lands says
	# what kind of bug it is -- a count on the pool row with every leaf
	# at zero means no device failed and the error was manufactured in
	# the pipeline, which is the signature this test exists to catch.
	#
	typeset errs
	errs=$(zpool status $TESTPOOL | awk -v pool="$TESTPOOL" '
		$3 ~ /^[0-9]+$/ && $4 ~ /^[0-9]+$/ && $5 ~ /^[0-9]+$/ {
			if ($3 + $4 + $5 > 0)
				printf "%s(r=%s,w=%s,c=%s) ", $1, $3, $4, $5
		}')
	if [[ -n "$errs" ]]; then
		log_note "=== zpool status -v ==="
		log_note "$(zpool status -v $TESTPOOL)"
		log_fail "non-zero vdev error counters: $errs"
	fi
}

#
# Phase 1: unganged copies=1 store, then a ganging copies=3 dedup write
# (the extend path meets a gang -> must fall back to a plain write).
#
log_must dd if=/dev/urandom of=$mp1/f1 bs=64k count=1
log_must sync_pool $TESTPOOL

log_must set_tunable64 metaslab_force_ganging 20000
typeset -i i=0
while (( i < 16 )); do
	log_must cp $mp1/f1 $mp2/f1
	log_must sync_pool $TESTPOOL
	log_must rm $mp2/f1
	log_must sync_pool $TESTPOOL
	(( i += 1 ))
done
check_intact

#
# Phase 2: ganging copies=1 store, then an unganged copies=3 dedup write.
#
log_must rm $mp1/f1
log_must sync_pool $TESTPOOL
log_must dd if=/dev/urandom of=$mp1/f1 bs=64k count=1
log_must sync_pool $TESTPOOL

log_must set_tunable64 metaslab_force_ganging $orig_ganging
i=0
while (( i < 16 )); do
	log_must cp $mp1/f1 $mp2/f1
	log_must sync_pool $TESTPOOL
	log_must rm $mp2/f1
	log_must sync_pool $TESTPOOL
	(( i += 1 ))
done
check_intact

log_pass "Mixed gang blocks and copies interact correctly in FDT dedup"
