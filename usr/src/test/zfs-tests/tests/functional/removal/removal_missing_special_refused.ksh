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
# Copyright 2026 Gernot Strasser
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/removal/removal.kshlib

#
# DESCRIPTION:
# A special allocation-class vdev is required data, unlike a dedicated slog.
# Importing a pool with the special vdev missing ('zpool import -m') must be
# REFUSED.  This is a safety-regression guard for the "ZIL on special vdevs"
# stack: adding an embedded-log carve-out to a special vdev must not make the
# special vdev disposable the way a missing slog is.
#
# STRATEGY:
# 1. Create a pool with a normal + special vdev and a sync=always dataset.
# 2. Write sync data (populating the special embedded-log ZIL) and export.
# 3. Move the special vdev file OUT of the import search directory so it is
#    genuinely missing (renaming in place is not enough; import rescans the
#    directory by GUID and rediscovers a renamed file).
# 4. 'zpool import -m' MUST fail (missing required data vdev).
#

verify_runnable "global"

VDEV_DIR=$TEST_BASE_DIR/missing_special.$$
NORMAL_VDEV=$VDEV_DIR/normal
SPECIAL_VDEV=$VDEV_DIR/special
HIDDEN_VDEV=$TEST_BASE_DIR/missing_special_hidden.$$

typeset orig_suspend

function get_tunable_mdb # name
{
	echo "${1}/D" | mdb -k 2>/dev/null | awk 'NF { v = $NF } END { print v }'
}

function set_tunable_mdb # name value
{
	echo "${1}/W 0t${2}" | mdb -kw > /dev/null 2>&1
}

function cleanup
{
	poolexists $TESTPOOL && destroy_pool $TESTPOOL
	[[ -n $orig_suspend ]] && \
	    set_tunable_mdb zfs_removal_suspend_progress $orig_suspend
	[[ -f $HIDDEN_VDEV ]] && mv $HIDDEN_VDEV $SPECIAL_VDEV 2>/dev/null
	rm -rf $VDEV_DIR
	rm -f $HIDDEN_VDEV
}

log_onexit cleanup

log_assert "Importing with a missing special (required data) vdev is refused"

# A leftover debug pause doesn't affect import, but keep the box sane for
# whatever runs next.
orig_suspend=$(get_tunable_mdb zfs_removal_suspend_progress)
log_must set_tunable_mdb zfs_removal_suspend_progress 0

log_must mkdir -p $VDEV_DIR
log_must mkfile -n 4g $NORMAL_VDEV
log_must mkfile -n 1g $SPECIAL_VDEV

log_must zpool create -f $TESTPOOL $NORMAL_VDEV special $SPECIAL_VDEV
log_must zfs create -o sync=always $TESTPOOL/$TESTFS
log_must dd if=/dev/urandom of=/$TESTPOOL/$TESTFS/f bs=64k count=512
log_must sync_pool $TESTPOOL

log_must zpool export $TESTPOOL

# Make the special device genuinely missing for the import scan.
log_must mv $SPECIAL_VDEV $HIDDEN_VDEV

# A missing REQUIRED special vdev must be refused even with -m.
log_mustnot zpool import -d $VDEV_DIR -m -f $TESTPOOL

# Restore the device so the pool can be cleaned up.
log_must mv $HIDDEN_VDEV $SPECIAL_VDEV
log_must zpool import -d $VDEV_DIR -f $TESTPOOL

log_pass "Import with a missing special vdev was correctly refused"
