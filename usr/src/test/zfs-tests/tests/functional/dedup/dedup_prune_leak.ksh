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
# Copyright (c) 2025, Klara Inc.
# Copyright (c) 2025, Nutanix Inc.
# Ported to illumos by the FDT port author.
#

# DESCRIPTION:
#	Verify that pruning the DDT and then freeing the underlying blocks
#	does not leak space.  Regression test for upstream 0c19435
#	(#17982 / #17983): zio_ddt_free() must fall back to a normal free
#	when the matched DDT entry no longer has a phys for this BP (the
#	entry was pruned but a BP still references the block).  Without the
#	fallback, "ddtprune -p 100; rm" leaks every pruned block, visible as
#	"leaked space" under `zdb -bcc`.
#
# STRATEGY:
#	1. Create a pool with dedup=on.
#	2. Write only UNIQUE data (all refcnt==1 unique-class entries).
#	3. ddtprune -p 100 to empty the DDT.
#	4. Remove the file (frees blocks whose DDT entry was pruned).
#	5. `zdb -bcc` must report no leaked space.
#
# illumos adaptations vs upstream:
#	- get/set_tunable (not save/restore_tunable).
#	- zfs_dedup_log_txg_max and zfs_dedup_log_flush_entries_min are both
#	  set, as in dedup_prune.ksh: the log must be fully drained before
#	  pruning, because a prune never removes entries still on it.
#	- no events_common.kshlib include (unused upstream).

. $STF_SUITE/include/libtest.shlib

verify_runnable "both"

log_assert "DDT pruning followed by free does not leak space"

# Flush the dedup log every txg, and lift the per-txg flush floor so a
# flush pass drains the whole tree rather than the default ~200 entries.
# A prune never removes an entry still on the log, so an undrained log
# silently makes the prune a no-op.
typeset orig_txg_max=$(get_tunable zfs_dedup_log_txg_max)
typeset orig_flush_min=$(get_tunable zfs_dedup_log_flush_entries_min)
log_must set_tunable32 zfs_dedup_log_txg_max 1
log_must set_tunable32 zfs_dedup_log_flush_entries_min 1000000

function cleanup
{
	if poolexists $TESTPOOL ; then
		destroy_pool $TESTPOOL
	fi
	log_must set_tunable32 zfs_dedup_log_txg_max $orig_txg_max
	log_must set_tunable32 zfs_dedup_log_flush_entries_min $orig_flush_min
}

log_onexit cleanup

log_must zpool create -f $TESTPOOL $DISKS
log_must zfs create -o dedup=on $TESTPOOL/$TESTFS
typeset mountpoint=$(get_prop mountpoint $TESTPOOL/$TESTFS)

# Unique (non-dedup-able) data, so every DDT entry is a prunable unique entry.
log_must dd if=/dev/urandom of=$mountpoint/f1 bs=1M count=16

# A few txg syncs make the entries reach the unique-class ZAP consistently.
typeset -i i=0
while (( i < 20 )); do
	sync_pool $TESTPOOL
	(( i += 1 ))
done

#
# ddp_class_start has one-second resolution and ddt_prune_walk() prunes
# strictly older-than the cutoff, so an entry created in the same second
# the prune runs is never a candidate.  Wait out the boundary.
#
sleep 2

# Prune everything, then free the blocks by removing the file.
log_must zpool ddtprune -p 100 $TESTPOOL
log_must rm $mountpoint/f1
sync_pool $TESTPOOL

#
# Match the clean result positively.  zdb -bcc always prints the banner
# "Traversing all blocks to verify checksums and verify nothing leaked
# ...", so grepping the output for "leaked" matches every run, clean or
# not -- and the test then only passes when zdb produces no output at
# all.  A clean run ends with "No leaks (block sum matches space maps
# exactly)"; a dirty one prints "block traversal size N != alloc M
# (leaked X)" instead.  Check the exit status too, so a zdb that cannot
# run is a failure rather than a silent pass.
#
typeset zdb_out
typeset -i zdb_rc
zdb_out=$(zdb -bcc $TESTPOOL 2>&1)
zdb_rc=$?
log_note "$zdb_out"
if (( zdb_rc != 0 )); then
	log_fail "zdb -bcc failed (exit $zdb_rc)"
fi
if ! echo "$zdb_out" | grep -q "No leaks (block sum matches space maps exactly)"; then
	log_fail "DDT pruning followed by free leaked space"
fi
log_must check_pool_status $TESTPOOL "errors" "No known data errors"

log_pass "DDT pruning followed by free does not leak space"
