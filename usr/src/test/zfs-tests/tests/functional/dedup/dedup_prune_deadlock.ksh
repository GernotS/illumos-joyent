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
# Copyright (c) 2026, (port author).
#

# DESCRIPTION:
#	Regression stress for the ddtprune SCL_ZIO re-entrancy deadlock
#	(upstream cb3c18a, #17793).  ddt_prune_unique_entries() ->
#	prune_candidates_sync() runs as a sync task holding the SCL_ZIO
#	reader lock; the frees it issues re-enter zio_vdev_io_start() and
#	take SCL_ZIO reader again.  If an SCL_ZIO *writer* is waiting
#	(scl_write_wanted), the second reader blocks behind it while that
#	writer blocks behind the first reader -> deadlock.  The fix makes
#	the prune-context reader use spa_config_enter_priority(), cutting in
#	front of pending writers.
#
# STRATEGY:
#	1. dedup pool; flush the dedup log every txg.
#	2. Background loop: add and remove an L2ARC (cache) vdev.  Vdev
#	   config changes take the SCL_ZIO writer lock (spa_vdev_enter),
#	   so this continuously produces the pending-writer condition.
#	3. Foreground loop: write unique data and `ddtprune -p 100`, so a
#	   prune (holding SCL_ZIO reader and issuing frees) is frequently
#	   active while a writer is waiting.
#	4. If the deadlock regresses, the pool wedges and the test hangs
#	   until the ZTS per-test timeout fails it.  On a fixed build the
#	   loops complete; verify the pool scrubs clean.
#
# Best-effort: the race is timing-dependent (upstream shipped the fix
# without a dedicated test).  The loops are sized to hit it with high
# probability without running too long.
#
# illumos adaptations: get/set_tunable; file-based cache vdev; no
# block_cloning gate.

. $STF_SUITE/include/libtest.shlib

verify_runnable "both"

log_assert "ddtprune does not deadlock against SCL_ZIO writers"

typeset orig_txg_max=$(get_tunable zfs_dedup_log_txg_max)
log_must set_tunable32 zfs_dedup_log_txg_max 1

typeset workdir=/var/tmp/dedup_prune_deadlock.$$
typeset cachedev=$workdir/cache
typeset stopfile=$workdir/keep_running

function cleanup
{
	rm -f $stopfile
	wait
	if poolexists $TESTPOOL ; then
		destroy_pool $TESTPOOL
	fi
	rm -rf $workdir
	log_must set_tunable32 zfs_dedup_log_txg_max $orig_txg_max
}

log_onexit cleanup

log_must mkdir -p $workdir
log_must mkfile 128m $cachedev
log_must touch $stopfile

log_must zpool create -f $TESTPOOL $DISKS
log_must zfs create -o recordsize=512 -o dedup=on $TESTPOOL/$TESTFS
typeset mountpoint=$(get_prop mountpoint $TESTPOOL/$TESTFS)

#
# Background SCL_ZIO writer: repeatedly add/remove an L2ARC device.  Each
# spa_vdev_enter() takes the SCL_ZIO writer lock, creating the pending-
# writer condition the prune reader must not deadlock against.
#
while [[ -e $stopfile ]]; do
	zpool add $TESTPOOL cache $cachedev >/dev/null 2>&1
	zpool remove $TESTPOOL $cachedev >/dev/null 2>&1
done &

#
# Foreground: keep a prune active by writing unique data and pruning it.
#
typeset -i round=0
while (( round < 200 )); do
	dd if=/dev/urandom of=$mountpoint/f$round bs=512 count=256 \
	    >/dev/null 2>&1
	sync_pool $TESTPOOL
	zpool ddtprune -p 100 $TESTPOOL >/dev/null 2>&1
	rm -f $mountpoint/f$round
	(( round += 1 ))
done

log_must rm $stopfile
wait

# If we got here, no deadlock.  Confirm the pool is still healthy.
log_must zpool scrub $TESTPOOL
while is_pool_scrubbing $TESTPOOL ; do
	sleep 1
done
log_must check_pool_status $TESTPOOL "errors" "No known data errors"

log_pass "ddtprune does not deadlock against SCL_ZIO writers"
