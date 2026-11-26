#!/bin/ksh -p
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

#
# Copyright 2026 Gernot Strasser
#

. $STF_SUITE/tests/functional/cli_root/zfs_rewrite/zfs_rewrite.kshlib

#
# DESCRIPTION:
#	zfs rewrite -S leaves blocks that the latest snapshot still
#	references; without -S they are rewritten.
#

verify_runnable "both"

SNAP=$TESTPOOL/$TESTFS@rewrite_skip

function cleanup
{
	datasetexists $SNAP && log_must zfs destroy $SNAP
	rm -f $REWRITE_FILE
}
log_onexit cleanup

log_assert "zfs rewrite -S skips snapshot-shared blocks"

rewrite_mkfile 4
log_must zfs snapshot $SNAP

before=$(rewrite_first_dva $REWRITE_FILE)
log_must zfs rewrite -S $REWRITE_FILE
after=$(rewrite_first_dva $REWRITE_FILE)
[[ $before == $after ]] || log_fail "-S rewrote a snapshot-shared block"

log_must zfs rewrite $REWRITE_FILE
after=$(rewrite_first_dva $REWRITE_FILE)
[[ $before != $after ]] || log_fail "block was not rewritten without -S"
rewrite_check_contents

# Now the block is newer than the snapshot, so -S rewrites it.
before=$after
log_must zfs rewrite -S $REWRITE_FILE
after=$(rewrite_first_dva $REWRITE_FILE)
[[ $before != $after ]] || log_fail "-S skipped a block born after the snapshot"

log_pass "zfs rewrite -S skips snapshot-shared blocks"
