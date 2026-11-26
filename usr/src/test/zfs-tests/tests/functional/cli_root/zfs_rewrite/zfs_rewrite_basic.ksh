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
#	zfs rewrite moves file data and applies changed properties without
#	changing the file contents.
#
# STRATEGY:
#	1. Write a compressible file with compression=off.
#	2. Turn compression on and rewrite the file.
#	3. Verify the blocks moved, are now compressed, and the contents are
#	   unchanged.
#	4. Verify -o/-l limit the rewrite to the given range.
#

verify_runnable "both"

function cleanup
{
	rewrite_reset_props
	rm -f $REWRITE_FILE
}
log_onexit cleanup

log_assert "zfs rewrite rewrites file data with the current properties"

log_must zfs set compression=off $TESTPOOL/$TESTFS
rewrite_mkfile 8
before=$(rewrite_first_dva $REWRITE_FILE)
used_before=$(get_prop used $TESTPOOL/$TESTFS)

log_must zfs set compression=on $TESTPOOL/$TESTFS
log_must zfs rewrite -v $REWRITE_FILE
after=$(rewrite_first_dva $REWRITE_FILE)
used_after=$(get_prop used $TESTPOOL/$TESTFS)

[[ $before != $after ]] || log_fail "first block did not move ($before)"
(( used_after < used_before )) || \
    log_fail "space not reclaimed by compression ($used_before -> $used_after)"
rewrite_check_contents

# A range starting past the first block leaves the first block in place.
before=$(rewrite_first_dva $REWRITE_FILE)
log_must zfs rewrite -o 131072 -l 131072 $REWRITE_FILE
after=$(rewrite_first_dva $REWRITE_FILE)
[[ $before == $after ]] || log_fail "block outside the range moved"
rewrite_check_contents

log_pass "zfs rewrite rewrites file data with the current properties"
