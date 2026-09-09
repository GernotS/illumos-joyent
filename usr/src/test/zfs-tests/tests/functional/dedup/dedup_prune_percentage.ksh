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
#	Verify that `zpool ddtprune -p N` is a no-op when N percent of the
#	unique entries rounds down to zero entries.  Regression test for
#	upstream #19264: ddt_prune_unique_entries() computed
#	target = entries * N / 100; with target == 0 the histogram bin
#	search never ran, the cutoff stayed at "now", and the walk pruned
#	every unique entry older than the current second.
#
# STRATEGY:
#	1. Create a pool with dedup=on and write three unique blocks.
#	2. Drain the dedup log so the entries reach the unique-class ZAP.
#	3. `zpool ddtprune -p 1`: target = 3 * 1 / 100 = 0, so all three
#	   entries must survive.
#	4. `zpool ddtprune -p 100` must then remove all three.
#
# illumos adaptations vs upstream:
#	- get_tunable + set_tunable32 (not save/restore_tunable).
#	- the dedup log is drained explicitly, as in dedup_prune.ksh:
#	  ddt_prune_walk() only sees entries in the unique-class ZAP.

. $STF_SUITE/include/libtest.shlib

verify_runnable "both"

log_assert "ddtprune -p with a zero-entry target prunes nothing"

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
log_must zfs create -o recordsize=128k -o compression=off -o dedup=on \
    $TESTPOOL/$TESTFS
typeset mountpoint=$(get_prop mountpoint $TESTPOOL/$TESTFS)

log_must dd if=/dev/urandom of=$mountpoint/f1 bs=128k count=3
drain_dedup_log

typeset -i entries=$(ddt_entries)
(( entries == 3 )) || log_fail "expected 3 DDT entries, found $entries"

# Entries must be strictly older than the prune cutoff second.
sleep 2

log_must zpool ddtprune -p 1 $TESTPOOL
drain_dedup_log
entries=$(ddt_entries)
(( entries == 3 )) || \
	log_fail "ddtprune -p 1 pruned $(( 3 - entries )) of 3 entries"

log_must zpool ddtprune -p 100 $TESTPOOL
drain_dedup_log
entries=$(ddt_entries)
(( entries == 0 )) || \
	log_fail "ddtprune -p 100 left $entries of 3 entries"

log_pass "ddtprune -p with a zero-entry target prunes nothing"
