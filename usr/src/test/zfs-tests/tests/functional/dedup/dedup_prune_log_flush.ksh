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
#	Verify that ddtprune does not remove an entry that gained a second
#	reference while the prune was running.  Regression test for
#	upstream #19223: ddt_prune_walk() reads candidates from the on-disk
#	unique-class ZAP, which lags the dedup log.  If an entry's second
#	reference is still on the log when the walk reads it, and the log
#	flushes (moving the entry to the duplicate class) before
#	prune_candidates_sync() runs, the recheck there found it neither
#	live nor logged and cleared it.  Freeing either reference then
#	freed a block the other still points to.
#
# STRATEGY:
#	1. Create a pool with dedup=on, write unique data and drain the
#	   dedup log, so every entry is in the unique-class ZAP.
#	2. Hold the log (zfs_dedup_log_txg_max very large, so the active
#	   log is never swapped) and copy the file.  Every entry now has
#	   refcnt 2 on the log but is still unique in the ZAP.
#	3. Prune in small batches (zfs_ddt_prunes_per_txg).  DTrace stalls
#	   each batch just before its sync task.  Once the walk is under
#	   way, release the log and keep syncing, so the log flushes
#	   between the walk reading a candidate and the recheck.
#	4. No entry may be removed.  Free one copy and verify with
#	   `zdb -bcc` that nothing the other copy references was freed.
#
# illumos adaptations vs upstream:
#	- upstream builds the interleaving in a libzpool test program; here
#	  DTrace chill() and the log tunables do it against the kernel.
#	- get_tunable + set_tunable32 (not save/restore_tunable).

. $STF_SUITE/include/libtest.shlib

verify_runnable "global"

log_assert "ddtprune keeps entries that gain a reference via log flush"

typeset orig_txg_max=$(get_tunable zfs_dedup_log_txg_max)
typeset orig_flush_min=$(get_tunable zfs_dedup_log_flush_entries_min)
typeset orig_per_txg=$(get_tunable zfs_ddt_prunes_per_txg)
log_must set_tunable32 zfs_dedup_log_txg_max 1
log_must set_tunable32 zfs_dedup_log_flush_entries_min 1000000

typeset workdir=/var/tmp/dedup_prune_log_flush.$$
typeset stopfile=$workdir/keep_syncing
typeset dtrace_pid=""

function cleanup
{
	rm -f $stopfile
	[[ -n $dtrace_pid ]] && kill $dtrace_pid 2>/dev/null
	wait
	if poolexists $TESTPOOL ; then
		destroy_pool $TESTPOOL
	fi
	rm -rf $workdir
	log_must set_tunable32 zfs_dedup_log_txg_max $orig_txg_max
	log_must set_tunable32 zfs_dedup_log_flush_entries_min $orig_flush_min
	log_must set_tunable32 zfs_ddt_prunes_per_txg $orig_per_txg
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

log_must mkdir -p $workdir

log_must zpool create -f $TESTPOOL $DISKS
log_must zfs create -o recordsize=512 -o dedup=on $TESTPOOL/$TESTFS
typeset mountpoint=$(get_prop mountpoint $TESTPOOL/$TESTFS)

# 8192 unique entries, all in the unique-class ZAP.
log_must dd if=/dev/urandom of=$mountpoint/f1 bs=1024k count=4
drain_dedup_log
typeset -i entries=$(ddt_entries)
(( entries > 0 )) || log_fail "no DDT entries after write"
sleep 2

# Hold the log, then give every entry a second reference on it.
log_must set_tunable32 zfs_dedup_log_txg_max 1000000
log_must cp $mountpoint/f1 $mountpoint/f2
sync_pool $TESTPOOL
sync_pool $TESTPOOL

log_must set_tunable32 zfs_ddt_prunes_per_txg $(( entries / 32 ))

#
# Stall each prune batch just before its sync task, so the log flush
# lands between the walk and prune_candidates_sync().  The stall is
# scoped to the pruning walk (cutoff != 0), not the histogram walk.
#
dtrace -qw -o $workdir/dtrace.out -n '
    BEGIN { printf("ready\n"); }
    fbt::ddt_prune_walk:entry /arg1 != 0/ { self->p = 1; }
    fbt::ddt_prune_walk:return /self->p/ { self->p = 0; }
    fbt::dsl_sync_task:entry /self->p/ { chill(300000000); }' \
    2>$workdir/dtrace.err &
dtrace_pid=$!

typeset -i i=0
while ! grep -q ready $workdir/dtrace.out 2>/dev/null; do
	(( i += 1 ))
	(( i > 30 )) && log_unsupported "DTrace probes could not be enabled"
	sleep 1
done

log_must touch $stopfile
while [[ -e $stopfile ]]; do
	zpool sync $TESTPOOL >/dev/null 2>&1
done &

zpool ddtprune -p 100 $TESTPOOL &
typeset prune_pid=$!
# Release the log once the walk is under way.
sleep 0.5
log_must set_tunable32 zfs_dedup_log_txg_max 1
wait $prune_pid
typeset -i prune_rc=$?

log_must rm $stopfile
kill $dtrace_pid 2>/dev/null
wait
dtrace_pid=""

(( prune_rc == 0 )) || log_fail "zpool ddtprune failed"

log_must set_tunable32 zfs_ddt_prunes_per_txg $orig_per_txg
drain_dedup_log

typeset -i after=$(ddt_entries)
(( after == entries )) || \
	log_fail "ddtprune removed $(( entries - after )) refcnt-2 entries"

# Free one copy; the other must still own every block.
log_must rm $mountpoint/f2
drain_dedup_log

typeset zdb_out
typeset -i zdb_rc
zdb_out=$(zdb -bcc $TESTPOOL 2>&1)
zdb_rc=$?
log_note "$zdb_out"
if (( zdb_rc != 0 )); then
	log_fail "zdb -bcc failed (exit $zdb_rc)"
fi
if ! echo "$zdb_out" | \
    grep -q "No leaks (block sum matches space maps exactly)"; then
	log_fail "space accounting is wrong after freeing one copy"
fi
log_must check_pool_status $TESTPOOL "errors" "No known data errors"

log_pass "ddtprune keeps entries that gain a reference via log flush"
