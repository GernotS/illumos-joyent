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
# Verify that a healthy special allocation-class vdev carrying an embedded-log
# metaslab (vdev_log_mg in spa_special_embedded_log_class) can be removed with
# no space leak and no data corruption.  This exercises evacuation of the
# special-vdev embedded-log carve-out added by the "ZIL on special vdevs"
# patch stack -- the path that motivated dropping ASSERT0(vs_alloc) from
# vdev_removal.c.
#
# STRATEGY:
# 1. Force > zfs_embedded_slog_min_ms metaslabs so the special vdev gets an
#    embedded-log carve-out, and clear zfs_removal_suspend_progress (a leftover
#    debug pause from another removal test otherwise parks the removal thread
#    in spa_vdev_remove_thread's delay() loop, so nothing makes progress).
# 2. Create a pool with a normal + special vdev and NO dedicated slog.
# 3. Place real small-block data ON the special vdev (so removal has work), and
#    drive sync=always writes whose blocks are LARGER than special_small_blocks
#    so their ZIL routes through the special embedded-log class rather than an
#    indirect write.
# 4. Confirm the carve-out is active (zdb 'Special embedded log' class line).
# 5. Remove the special vdev and wait for evacuation to complete.
# 6. Verify no leak (zdb -bcc), consistent accounting (zdb -c), a clean scrub,
#    and no data errors.
#

verify_runnable "global"

MIN_MS_COUNT=160
VDEV_DIR=$TEST_BASE_DIR/special_emb_log.$$
NORMAL_VDEV=$VDEV_DIR/normal
SPECIAL_VDEV=$VDEV_DIR/special

typeset orig_min_ms
typeset orig_suspend

#
# illumos does not ship a portable set_tunable helper, so read/write the
# kernel variables directly with mdb.  get returns the current value; both
# variables are 'int', so a 32-bit /W write is correct.
#
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
	[[ -n $orig_min_ms ]] && \
	    set_tunable_mdb zfs_vdev_min_ms_count $orig_min_ms
	[[ -n $orig_suspend ]] && \
	    set_tunable_mdb zfs_removal_suspend_progress $orig_suspend
	rm -rf $VDEV_DIR
}

log_onexit cleanup

log_assert "A healthy special vdev with an embedded-log carve-out removes cleanly"

orig_min_ms=$(get_tunable_mdb zfs_vdev_min_ms_count)
orig_suspend=$(get_tunable_mdb zfs_removal_suspend_progress)

# Enough metaslabs for the carve-out to engage; must be set before create.
log_must set_tunable_mdb zfs_vdev_min_ms_count $MIN_MS_COUNT
# Don't let a leftover debug pause from another removal test park us at 0 B/s.
log_must set_tunable_mdb zfs_removal_suspend_progress 0

log_must mkdir -p $VDEV_DIR
log_must mkfile -n 8g $NORMAL_VDEV
log_must mkfile -n 2g $SPECIAL_VDEV

log_must zpool create -f $TESTPOOL $NORMAL_VDEV special $SPECIAL_VDEV
# The ZIL-on-special path only applies without a dedicated slog.
log_mustnot eval "zpool status $TESTPOOL | grep -q logs"

#
# Dataset whose sync writes route the ZIL through the special embedded log:
# blocksize (64k) > special_small_blocks (0), so the data itself does NOT land
# on special (per zil_special_is_slog's on_special check), forcing the ZIL to
# the special embedded-log class instead of an indirect write.
#
log_must zfs create -o sync=always -o special_small_blocks=0 \
    -o recordsize=64k $TESTPOOL/$TESTFS

#
# Dataset whose small blocks DO land on special, giving the removal real data
# to evacuate.  An almost-empty special vdev evacuates trivially and does not
# meaningfully test the copy path.
#
log_must zfs create -o sync=disabled -o recordsize=16k \
    -o special_small_blocks=16k $TESTPOOL/bulk

log_must dd if=/dev/urandom of=/$TESTPOOL/bulk/data bs=16k count=8192
log_must dd if=/dev/urandom of=/$TESTPOOL/$TESTFS/f bs=64k count=1024
log_must sync_pool $TESTPOOL

#
# Confirm the special embedded-log carve-out actually engaged.  If the special
# vdev did not reach > zfs_embedded_slog_min_ms metaslabs (e.g. too small),
# the feature can't be exercised -- report unsupported rather than fail.
#
if ! zdb -bcc $TESTPOOL 2>&1 | grep -q "Special embedded log"; then
	log_unsupported "special vdev did not reach > zfs_embedded_slog_min_ms metaslabs; carve-out inactive"
fi

# Remove the special vdev and wait for evacuation to complete.
log_must zpool remove $TESTPOOL $SPECIAL_VDEV
log_must wait_for_removal $TESTPOOL
log_mustnot eval "zpool status -v $TESTPOOL | grep -qF $SPECIAL_VDEV"
log_must sync_pool $TESTPOOL

# No leak, consistent space accounting, clean data.
log_must zdb -bcc $TESTPOOL
log_must zdb -c $TESTPOOL
log_must zpool scrub $TESTPOOL
while ! is_pool_scrubbed $TESTPOOL; do
	sleep 1
done
log_must check_pool_status $TESTPOOL "errors" "No known data errors"

log_pass "Special vdev with embedded-log carve-out removed cleanly"
