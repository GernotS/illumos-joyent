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
#	Verify that two concurrent prunes of one pool cannot both claim the
#	active-prune mark, and that the refused one reports an error rather
#	than crashing zpool.  Covers the claim part of upstream #19263:
#	ddt_prune_unique_entries() tested spa_active_ddt_prune on entry but
#	set it only after reading the DDT statistics, so two prunes could
#	both pass the test.  The first to finish then cleared the mark while
#	the second was still pruning, dropping its SCL_ZIO priority against
#	a waiting config writer.  The refused prune gets EALREADY, which
#	zpool_standard_error() did not map, so zpool printed "internal
#	error" and aborted.
#
# STRATEGY:
#	1. Create a pool with dedup=on, write unique blocks and drain the
#	   dedup log.
#	2. With DTrace, stall the first prune inside the claim window
#	   (ddt_get_dedup_object_stats() called from
#	   ddt_prune_unique_entries()), then start a second prune.
#	3. The first prune must succeed.  The second must fail with
#	   "already in progress", without dying on a signal.
#
# Not upstream.  DTrace stands in for the in-process debugger hold used
# to check this upstream.  ddt_get_dedup_object_stats() is also called
# from spa_config_generate() when zpool opens the pool, so the stall is
# scoped to ddt_prune_unique_entries().

. $STF_SUITE/include/libtest.shlib

verify_runnable "global"

log_assert "Concurrent ddtprunes cannot both claim the prune mark"

typeset orig_txg_max=$(get_tunable zfs_dedup_log_txg_max)
typeset orig_flush_min=$(get_tunable zfs_dedup_log_flush_entries_min)
log_must set_tunable32 zfs_dedup_log_txg_max 1
log_must set_tunable32 zfs_dedup_log_flush_entries_min 1000000

typeset workdir=/var/tmp/dedup_prune_concurrent.$$
typeset dtrace_pid=""

function cleanup
{
	[[ -n $dtrace_pid ]] && kill $dtrace_pid 2>/dev/null
	wait
	if poolexists $TESTPOOL ; then
		destroy_pool $TESTPOOL
	fi
	rm -rf $workdir
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

log_onexit cleanup

log_must mkdir -p $workdir

log_must zpool create -f $TESTPOOL $DISKS
log_must zfs create -o recordsize=512 -o dedup=on $TESTPOOL/$TESTFS
typeset mountpoint=$(get_prop mountpoint $TESTPOOL/$TESTFS)

log_must dd if=/dev/urandom of=$mountpoint/f1 bs=1024k count=8
drain_dedup_log
sleep 2

#
# chill() is capped at 500ms per second, so stall only the first prune.
#
dtrace -qw -o $workdir/dtrace.out -n '
    BEGIN { printf("ready\n"); }
    fbt::ddt_prune_unique_entries:entry { self->in = 1; }
    fbt::ddt_prune_unique_entries:return { self->in = 0; }
    fbt::ddt_get_dedup_object_stats:entry
    /self->in && !fired/
    { fired = 1; printf("stalled\n"); chill(400000000); }' \
    2>$workdir/dtrace.err &
dtrace_pid=$!

typeset -i i=0
while ! grep -q ready $workdir/dtrace.out 2>/dev/null; do
	(( i += 1 ))
	(( i > 30 )) && log_unsupported "DTrace probes could not be enabled"
	sleep 1
done

zpool ddtprune -p 100 $TESTPOOL > $workdir/first.out 2>&1 &
typeset first=$!
sleep 0.1
zpool ddtprune -p 100 $TESTPOOL > $workdir/second.out 2>&1
typeset -i second_rc=$?
wait $first
typeset -i first_rc=$?

kill $dtrace_pid 2>/dev/null
wait $dtrace_pid 2>/dev/null
dtrace_pid=""

log_note "first prune (rc $first_rc): $(cat $workdir/first.out)"
log_note "second prune (rc $second_rc): $(cat $workdir/second.out)"

grep -q stalled $workdir/dtrace.out || \
	log_unsupported "DTrace stall never fired; race not exercised"

(( first_rc == 0 )) || log_fail "first prune failed"

# ksh93 reports death by signal as 256 + signo.
(( second_rc > 256 )) && \
	log_fail "second prune died on signal $(( second_rc - 256 ))"
grep -q "internal error" $workdir/second.out && \
	log_fail "second prune hit an unmapped error"
(( second_rc != 0 )) || \
	log_fail "both prunes claimed the prune mark"
grep -q "already in progress" $workdir/second.out || \
	log_fail "second prune failed for the wrong reason"

log_pass "Concurrent ddtprunes cannot both claim the prune mark"
