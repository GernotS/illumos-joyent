#!/bin/ksh -p
#
# CDDL HEADER START
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
# CDDL HEADER END
#

#
# Copyright (c) 2026, TrueNAS.
# Copyright 2026 Gernot Strasser
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/removal/removal.kshlib

#
# DESCRIPTION:
# Verify that a missing SLOG device can be removed even when
# ZIL blocks exist on it.
#
# STRATEGY:
# 1. Create a pool with a SLOG device and a sync=always dataset
# 2. Freeze the pool and write data, populating the ZIL
# 3. Export the pool (ZIL blocks remain uncommitted)
# 4. Import with -N to claim logs without replay
# 5. Export and clear SLOG device labels to simulate failure
# 6. Import with -m (missing devices allowed)
# 7. Remove the missing SLOG vdev
# 8. Verify pool is healthy and space accounting is correct
#

verify_runnable "global"

log_assert "Removal of missing SLOG with ZIL blocks succeeds"

function cleanup
{
	poolexists $TESTPOOL && destroy_pool $TESTPOOL
}

log_onexit cleanup

VDEV1="$(echo $DISKS | cut -d' ' -f1)"
VDEV2="$(echo $DISKS | cut -d' ' -f2)"

# Create pool with SLOG and a dataset whose writes all go through the ZIL.
log_must zpool create $TESTPOOL $VDEV1 log $VDEV2
log_must zfs create -o sync=always $TESTPOOL/$TESTFS

# Create initial ZIL header (required before freezing).
log_must dd if=/dev/zero of=/$TESTPOOL/$TESTFS/init bs=1 count=1
log_must sync

# Freeze pool and write data into the ZIL.
log_must zpool freeze $TESTPOOL
log_must dd if=/dev/urandom of=/$TESTPOOL/$TESTFS/file1 bs=128k count=128

# Export with uncommitted ZIL transactions.
log_must zpool export $TESTPOOL

# Import with -N to claim logs without mounting/replaying.
log_must zpool import -N $TESTPOOL
log_must zpool export $TESTPOOL

# Clear SLOG labels to simulate device failure.
log_must zpool labelclear -f $VDEV2

# Import with missing SLOG allowed.
log_must zpool import -m $TESTPOOL
log_must eval "zpool status $TESTPOOL | grep UNAVAIL"

# Remove the missing SLOG - should succeed.
log_must zpool remove $TESTPOOL $VDEV2
log_must wait_for_removal $TESTPOOL
sync_pool $TESTPOOL
log_mustnot eval "zpool status -v $TESTPOOL | grep $VDEV2"

# Verify pool health.
log_must zpool scrub $TESTPOOL
while ! is_pool_scrubbed $TESTPOOL; do
	sleep 1
done
log_must check_pool_status $TESTPOOL "errors" "No known data errors"

# Verify space accounting is correct.
log_must zdb -c $TESTPOOL

log_pass "Removal of missing SLOG with ZIL blocks succeeded"
