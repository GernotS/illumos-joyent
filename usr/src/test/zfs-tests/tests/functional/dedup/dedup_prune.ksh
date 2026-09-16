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
# Copyright (c) 2026, Gernot Strasser
#

# DESCRIPTION:
#	Verify that zpool ddtprune successfully reduces the number of entries
#	in the DDT.
#
# STRATEGY:
#	1. Create a pool with dedup=on
#	2. Add duplicate entries to the DDT
#	3. Verify ddtprune doesn't remove duplicate entries
#	4. Convert some entries to non-duplicate
#	5. Verify ddtprune removes non-duplicate entries
#
# illumos adaptations vs upstream PR #17049:
#   - save_tunable / restore_tunable -> get_tunable + set_tunable32 (illumos
#     libtest.shlib does not provide save/restore helpers).
#   - Tunable name DEDUP_LOG_TXG_MAX -> zfs_dedup_log_txg_max (illumos uses
#     the C-symbol form via mdb -kw, not the Linux sysfs short name).
#   - block_cloning feature gate dropped: this port does not include block
#     cloning.

. $STF_SUITE/include/libtest.shlib

verify_runnable "both"

log_assert "Verify DDT pruning correctly removes non-duplicate entries"

# Two tunables are needed to make DDT entry counts predictable.
#
# zfs_dedup_log_txg_max=1 swaps the active log tree to flushing every txg
# instead of batching over several.
#
# zfs_dedup_log_flush_entries_min raises the per-txg flush floor:
# ddt_sync_flush_log() computes flush_min from it and then flush_max as
# flush_min * 5 / 4, so with the default 200 only a couple of hundred
# entries leave the log per txg.  ddt_prune_walk() skips any entry still
# on the log (the stored copy is stale), so an undrained log makes prune
# look like it did nothing.
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

#
# Sync until the dedup log is empty.  One sync is not enough even with
# zfs_dedup_log_txg_max=1: the active tree is swapped to flushing at one
# txg boundary and drained during a later one.
#
function drain_dedup_log
{
	typeset -i i=0
	while (( i < 5 )); do
		sync_pool $TESTPOOL
		(( i += 1 ))
	done
}

function ddt_entries
{
	typeset -i entries=$(zpool status -D $TESTPOOL | \
		grep "dedup: DDT entries" | awk '{print $4}')
	echo ${entries}
}

log_onexit cleanup

log_must zpool create -f $TESTPOOL $DISKS

log_must zfs create -o recordsize=512 -o dedup=on $TESTPOOL/$TESTFS
typeset mountpoint=$(get_prop mountpoint $TESTPOOL/$TESTFS)
log_must dd if=/dev/urandom of=$mountpoint/f1 bs=512k count=1
log_must cp $mountpoint/f1 $mountpoint/f2
drain_dedup_log
entries=$(ddt_entries)
log_note "ddt entries before: $entries"

# ddtprune -p 100 must not remove DUPLICATE entries (refcnt > 1).
log_must zpool ddtprune -p 100 $TESTPOOL
drain_dedup_log
new_entries=$(ddt_entries)
(( entries == new_entries )) || \
	log_fail "DDT entries changed from $entries to $new_entries"

# Make some entries unique by truncating one of the copies; ddtprune should
# then remove them.
log_must truncate -s 128k $mountpoint/f2
drain_dedup_log

#
# ddp_class_start has one-second resolution and ddt_prune_walk() prunes
# strictly older-than the cutoff, so an entry created in the same second
# the prune runs is never a candidate.  Wait out the boundary.
#
sleep 2

log_must zpool ddtprune -p 100 $TESTPOOL
drain_dedup_log

new_entries=$(ddt_entries)
(( entries / 4 == new_entries )) || \
	log_fail "DDT entries did not shrink enough: $entries -> $new_entries"

log_pass "DDT pruning correctly removes non-duplicate entries"
