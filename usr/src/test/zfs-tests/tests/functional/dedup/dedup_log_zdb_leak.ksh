#!/bin/ksh -p
#
# This file and its contents are supplied under the terms of the
# Common Development and Distribution License ("CDDL"), version 1.0.
# You may only use this file in accordance with the terms of version
# 1.0 of the CDDL.
#
# A full copy of the text of the CDDL should have accompanied this
# source.  A copy of the CDDL is also available via the Internet at
# http://www.illumos.org/license/CDDL.
#

#
# Copyright (c) 2026, Gernot Strasser
#

# DESCRIPTION:
#	With fast dedup, the last decref of a block lands in the DDT log
#	and the physical free happens only when the log flushes.  Blocks
#	inside that window are referenced by no block pointer but still
#	allocated, so zdb must account them as pending frees rather than
#	report them as leaked -- and must not claim anything else from the
#	log: live entries and pruned-entry tombstones.  Behind a removed
#	vdev's indirect mapping the same window used to fail zdb with an
#	obsolete indirect mapping count mismatch.
#
# STRATEGY:
#	1. Write dedup'd blocks (one set shared twice, one unique) with the
#	   DDT log flushing every txg, so the entries land in the ZAP.
#	2. Pin the log, ddtprune the unique entries (leaving zero-phys
#	   tombstones), and drop one reference to the shared blocks
#	   (leaving a positive-refcount log entry).  Neither is a pending
#	   free.
#	3. zdb: the "DDT pending free" bucket must be empty and nothing may
#	   be double-claimed.
#	4. Drop the last reference to the shared blocks; their
#	   decref-to-zero entries stay pending in the pinned log.
#	5. zdb: exactly those blocks are in "DDT pending free", no leaks.
#	6. Write the same content again before the log flushes: the pending
#	   entries are resurrected, so nothing may remain pending.
#	7. Repeat the window on a pool whose data was migrated by device
#	   removal: zdb must exit 0 with no obsolete mapping count mismatch.
#
# Ported from OpenZFS db78d8b07 (dedup_log_zdb_leak.ksh, PR #18820).
# illumos adaptations:
#   - save_tunable / restore_tunable -> get_tunable + set_tunable32, with
#     the C-symbol tunable names.
#   - zdb -D prints "DDT-log-<cksum>-<n>: N log entries" here (OpenZFS
#     0ffa6f346's "entries=N" attribute format is not ported).
#   - cmp_xxh128 -> cmp; xattr=sa dropped (not supported on illumos).
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/removal/removal.kshlib

verify_runnable "global"
verify_disk_count "$DISKS" 2

log_assert "zdb accounts pending DDT-log frees instead of reporting leaks"

typeset orig_txg_max=$(get_tunable zfs_dedup_log_txg_max)
typeset orig_flush_min=$(get_tunable zfs_dedup_log_flush_entries_min)

# Content is written from a seed outside the pool so it can be written
# again later, to resurrect an entry that is pending free.
typeset seed=$TEST_BASE_DIR/dedup_log_zdb_leak.seed

function cleanup
{
	if poolexists $TESTPOOL ; then
		destroy_pool $TESTPOOL
	fi
	rm -f $seed
	log_must set_tunable32 zfs_dedup_log_txg_max $orig_txg_max
	log_must set_tunable32 zfs_dedup_log_flush_entries_min $orig_flush_min
}

log_onexit cleanup

set -A disks $DISKS

# Block count of the "DDT pending free" bucket; "-" when it is empty.
function pending_count
{
	echo "$1" | awk '/DDT pending free/ {print $1; exit}'
}

# True when the DDT log holds at least one entry.  zdb's status is
# checked separately, so a zdb failure can't be hidden by grep finding
# nothing.
function ddt_log_has_entries
{
	typeset out
	out=$(zdb -D $TESTPOOL 2>&1) || log_fail "zdb -D failed: $out"
	echo "$out" | grep -q 'DDT-log-.*: [1-9][0-9]* log entries'
}

# Run zdb -bbcc and fail on a nonzero exit or any leaked space.
function zdb_check # message
{
	zdb_out=$(zdb -bbcc $TESTPOOL 2>&1)
	typeset rc=$?
	echo "$zdb_out"
	if (( rc != 0 )) || echo "$zdb_out" | grep -q "leaked space"; then
		log_fail "$1"
	fi
}

typeset zdb_out

# Phase 1: pending frees on a concrete vdev.
log_must zpool create -f -o feature@fast_dedup=enabled \
    -O dedup=on -O compression=off $TESTPOOL ${disks[0]}
log_must zfs create -o recordsize=128k $TESTPOOL/$TESTFS
typeset mountpoint=$(get_prop mountpoint $TESTPOOL/$TESTFS)

# Flush the log every txg while writing, so the entries land in the DDT
# ZAP where ddtprune can see them.
log_must set_tunable32 zfs_dedup_log_txg_max 1
log_must set_tunable32 zfs_dedup_log_flush_entries_min 100000
log_must dd if=/dev/urandom of=$seed bs=128k count=4
log_must cp $seed $mountpoint/file1
log_must cp $seed $mountpoint/file2
log_must dd if=/dev/urandom of=$mountpoint/file3 bs=128k count=4
# The active tree is swapped to flushing at one txg boundary and drained
# in a later one, so sync until the log is empty (as dedup_prune does).
typeset -i i=0
while ddt_log_has_entries && (( i < 10 )); do
	sync_pool $TESTPOOL
	(( i += 1 ))
done
log_mustnot ddt_log_has_entries

# Age the entries past ddtprune's cutoff, then pin the log so everything
# logged from here on stays pending.
sleep 1
log_must set_tunable32 zfs_dedup_log_txg_max 1000000

# Prune file3's unique entries (zero-phys tombstones for blocks file3
# still references), assert them before anything is dereferenced, then
# drop one of the two references to the shared blocks (a 2->1 entry for
# blocks file2 still references).
log_must zpool ddtprune -p 100 $TESTPOOL
sync_pool $TESTPOOL
log_must ddt_log_has_entries

log_must rm $mountpoint/file1
sync_pool $TESTPOOL
log_must ddt_log_has_entries

# No block is pending free yet.  A bogus claim of a tombstone or live
# entry would collide with the traversal's claim and fail zdb, and would
# show up in the "DDT pending free" bucket.
zdb_check "zdb claimed a tombstone or a live log entry as pending"
typeset pending=$(pending_count "$zdb_out")
[[ "$pending" == "-" ]] || \
    log_fail "blocks claimed as pending while none are: '$pending'"

# Drop the last reference: four decref-to-zero entries stay pending.
log_must rm $mountpoint/file2
sync_pool $TESTPOOL
log_must ddt_log_has_entries

zdb_check "zdb misreported pending DDT-log frees as leaks"
log_must eval "echo \"\$zdb_out\" | grep -q 'No leaks'"
pending=$(pending_count "$zdb_out")
[[ "$pending" == "4" ]] || \
    log_fail "expected 4 blocks in the DDT pending free bucket, got '$pending'"

# Writing the same content while the entries are pending resurrects
# them: nothing is pending and nothing has been freed.
log_must cp $seed $mountpoint/file4
sync_pool $TESTPOOL

zdb_check "zdb misreported a resurrected DDT-log entry"
pending=$(pending_count "$zdb_out")
[[ "$pending" == "-" ]] || \
    log_fail "blocks still pending after resurrection: '$pending'"
log_must cmp $mountpoint/file4 $seed

# Phase 2: the same window behind a removed vdev's indirect mapping.
destroy_pool $TESTPOOL
log_must zpool create -f -o feature@fast_dedup=enabled \
    -O dedup=on -O compression=off $TESTPOOL ${disks[0]}
log_must zfs create -o recordsize=128k $TESTPOOL/$TESTFS
mountpoint=$(get_prop mountpoint $TESTPOOL/$TESTFS)
log_must dd if=/dev/urandom of=$mountpoint/file1 bs=128k count=4
log_must dd if=$mountpoint/file1 of=$mountpoint/file2 bs=128k
sync_pool $TESTPOOL

# Migrate everything to the second disk; the dedup'd blocks now live
# behind the removed vdev's indirect mapping.
log_must zpool add $TESTPOOL ${disks[1]}
log_must zpool remove $TESTPOOL ${disks[0]}
log_must wait_for_removal $TESTPOOL

log_must rm $mountpoint/file1 $mountpoint/file2
sync_pool $TESTPOOL
log_must ddt_log_has_entries

zdb_out=$(zdb -bcc $TESTPOOL 2>&1)
typeset rc=$?
echo "$zdb_out"
if (( rc != 0 )) || \
    echo "$zdb_out" | grep -q "obsolete indirect mapping count mismatch"; then
	log_fail "pending DDT-log frees tripped the obsolete mapping check"
fi

log_pass "zdb accounts pending DDT-log frees instead of reporting leaks"
