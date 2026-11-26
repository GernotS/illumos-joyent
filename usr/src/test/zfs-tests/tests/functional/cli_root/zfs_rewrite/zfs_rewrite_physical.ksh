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
#	zfs rewrite -P moves blocks but keeps their logical birth time, so an
#	incremental send does not carry them again, and it activates the
#	physical_rewrite feature.  A plain rewrite is sent again.
#

verify_runnable "both"

SNAP1=$TESTPOOL/$TESTFS@phys1
SNAP2=$TESTPOOL/$TESTFS@phys2
SNAP3=$TESTPOOL/$TESTFS@phys3

function cleanup
{
	for s in $SNAP3 $SNAP2 $SNAP1; do
		datasetexists $s && log_must zfs destroy $s
	done
	rewrite_reset_props
	rm -f $REWRITE_FILE
}
log_onexit cleanup

# Estimated size in bytes of an incremental stream.
function incr_size # from to
{
	zfs send -nvP -i $1 $2 | awk '$1 == "size" { print $2 }'
}

log_assert "zfs rewrite -P keeps the logical birth time of blocks"

[[ $(get_pool_prop feature@physical_rewrite $TESTPOOL) != "disabled" ]] || \
    log_unsupported "physical_rewrite feature is disabled"

log_must zfs set compression=off $TESTPOOL/$TESTFS
rewrite_mkfile 16
log_must zfs snapshot $SNAP1

log_must zfs set compression=on $TESTPOOL/$TESTFS
before=$(rewrite_first_dva $REWRITE_FILE)
log_must zfs rewrite -P $REWRITE_FILE
after=$(rewrite_first_dva $REWRITE_FILE)
[[ $before != $after ]] || log_fail "-P did not move the block ($before)"
rewrite_check_contents
log_must zfs snapshot $SNAP2

[[ $(get_pool_prop feature@physical_rewrite $TESTPOOL) == "active" ]] || \
    log_fail "physical_rewrite feature not active after zfs rewrite -P"

size=$(incr_size $SNAP1 $SNAP2)
(( size < 1048576 )) || log_fail "incremental after -P carries data ($size)"

# A plain rewrite counts as a modification and is sent again.
log_must zfs set checksum=sha256 $TESTPOOL/$TESTFS
log_must zfs rewrite $REWRITE_FILE
log_must zfs snapshot $SNAP3
size=$(incr_size $SNAP2 $SNAP3)
(( size >= 1048576 )) || log_fail "incremental after rewrite too small ($size)"
rewrite_check_contents

# The snapshots still read back the same data.
log_must eval "[[ \$(digest -a sha256 \
    $TESTDIR/.zfs/snapshot/phys1/$(basename $REWRITE_FILE)) == $REWRITE_SUM ]]"

log_pass "zfs rewrite -P keeps the logical birth time of blocks"
