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
#	Verify ddtprune argument validation and that a rejected or empty
#	prune does not leave the pool's active-prune mark set.  Covers the
#	argument and mark-release parts of upstream #19263:
#	  - an age reaching before the epoch is rejected by the kernel
#	    (EINVAL) and the next prune is still accepted;
#	  - a day count whose conversion to seconds overflows is rejected
#	    by zpool rather than wrapped to a small age, which would prune
#	    recent entries;
#	  - pruning an empty table leaves no mark behind.
#
# STRATEGY:
#	1. Create a pool with dedup=on.  Write unique blocks with
#	   ddt_prune_artificial_age set, so their class_start is spread over
#	   the past ~24 days, and drain the dedup log.
#	2. `ddtprune -d 30000` (before the epoch) must fail; the next
#	   prune must not report "already in progress".
#	3. `ddtprune -d 213503982334602` must fail: 213503982334602 * 86400
#	   wraps to 61184 seconds, which would prune every entry older
#	   than ~17 hours.  No entry may be removed.
#	4. Prune everything, then prune the empty table twice; neither
#	   may report "already in progress".
#
# illumos adaptations vs upstream:
#	- get_tunable + set_tunable32 (not save/restore_tunable).
#	- ddt_prune_artificial_age stands in for waiting out real ages.

. $STF_SUITE/include/libtest.shlib

verify_runnable "both"

log_assert "ddtprune rejects bad ages and never leaves the prune mark set"

typeset orig_txg_max=$(get_tunable zfs_dedup_log_txg_max)
typeset orig_flush_min=$(get_tunable zfs_dedup_log_flush_entries_min)
typeset orig_art_age=$(get_tunable ddt_prune_artificial_age)
log_must set_tunable32 zfs_dedup_log_txg_max 1
log_must set_tunable32 zfs_dedup_log_flush_entries_min 1000000

function cleanup
{
	if poolexists $TESTPOOL ; then
		destroy_pool $TESTPOOL
	fi
	log_must set_tunable32 zfs_dedup_log_txg_max $orig_txg_max
	log_must set_tunable32 zfs_dedup_log_flush_entries_min $orig_flush_min
	log_must set_tunable32 ddt_prune_artificial_age $orig_art_age
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

# Run a prune that must be accepted; fail if the mark was left set.
function prune_not_stuck
{
	typeset out
	out=$(zpool ddtprune "$@" $TESTPOOL 2>&1)
	if (( $? != 0 )); then
		log_note "$out"
		echo "$out" | grep -q "already in progress" && \
			log_fail "prune mark left set by an earlier prune"
		log_fail "zpool ddtprune $* failed"
	fi
}

log_onexit cleanup

log_must zpool create -f $TESTPOOL $DISKS
log_must zfs create -o recordsize=128k -o compression=off -o dedup=on \
    $TESTPOOL/$TESTFS
typeset mountpoint=$(get_prop mountpoint $TESTPOOL/$TESTFS)

log_must set_tunable32 ddt_prune_artificial_age 1
log_must dd if=/dev/urandom of=$mountpoint/f1 bs=128k count=1000
log_must set_tunable32 ddt_prune_artificial_age 0
drain_dedup_log

typeset -i entries=$(ddt_entries)
(( entries > 0 )) || log_fail "no DDT entries after write"

# An age before the epoch is rejected in the kernel, mark released.
log_mustnot zpool ddtprune -d 30000 $TESTPOOL
(( $(ddt_entries) == entries )) || log_fail "rejected prune removed entries"

# Overflowing day count must be rejected, not wrapped.
log_mustnot zpool ddtprune -d 213503982334602 $TESTPOOL
drain_dedup_log
typeset -i after=$(ddt_entries)
(( after == entries )) || \
	log_fail "overflowing -d pruned $(( entries - after )) entries"

# The rejected prunes must not have left the mark set.
sleep 2
prune_not_stuck -p 100
drain_dedup_log
(( $(ddt_entries) == 0 )) || log_fail "ddtprune -p 100 left entries"

# Empty-table path, twice.
prune_not_stuck -p 100
prune_not_stuck -p 100

log_pass "ddtprune rejects bad ages and never leaves the prune mark set"
