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
#	zfs rewrite -W skips blocks that a rewrite would not change, and
#	rewrites them once a relevant property changes.
#

verify_runnable "both"

function cleanup
{
	rewrite_reset_props
	rm -f $REWRITE_FILE
}
log_onexit cleanup

log_assert "zfs rewrite -W skips blocks that are already current"

log_must zfs set compression=on $TESTPOOL/$TESTFS
rewrite_mkfile 4

before=$(rewrite_first_dva $REWRITE_FILE)
log_must zfs rewrite -W $REWRITE_FILE
after=$(rewrite_first_dva $REWRITE_FILE)
[[ $before == $after ]] || log_fail "-W rewrote a current block"

log_must zfs set checksum=sha256 $TESTPOOL/$TESTFS
log_must zfs rewrite -W $REWRITE_FILE
after=$(rewrite_first_dva $REWRITE_FILE)
[[ $before != $after ]] || log_fail "-W skipped a block with a stale checksum"

before=$after
log_must zfs set copies=2 $TESTPOOL/$TESTFS
log_must zfs rewrite -W $REWRITE_FILE
after=$(rewrite_first_dva $REWRITE_FILE)
[[ $before != $after ]] || log_fail "-W skipped a block with too few copies"
rewrite_check_contents

log_pass "zfs rewrite -W skips blocks that are already current"
