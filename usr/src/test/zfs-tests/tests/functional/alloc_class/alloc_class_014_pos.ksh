#!/bin/ksh -p
# SPDX-License-Identifier: CDDL-1.0

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

. $STF_SUITE/tests/functional/alloc_class/alloc_class.kshlib

#
# DESCRIPTION:
#	File blocks and zvol blocks, where special_small_blocks is active,
#	are expected to end up in the special class.  For a zvol the
#	property is only active when set on the zvol itself; a value
#	inherited from a parent file system is ignored.
#
# STRATEGY:
#	1. Write 10M to a file system with special_small_blocks=32K: the
#	   special vdev grows by at least 10M.
#	2. Write 10M to a zvol below that file system, which inherits
#	   special_small_blocks: the special vdev grows by less than 1M.
#	3. Set special_small_blocks=32K on the zvol and write 10M to it: the
#	   special vdev grows by at least 10M.
#

verify_runnable "global"

claim="File and zvol blocks using special_small_blocks end up in special class"

#
# There are no vdev properties on illumos; read the space allocated on the
# special vdev from zpool list -v.
#
function special_alloc
{
	sync_pool $TESTPOOL
	zpool list -Hpv $TESTPOOL | \
	    awk -v d="$CLASS_DISK0" '$1 == d { print $3 }'
}

function check_growth
{
	typeset before=$1
	typeset op=$2
	typeset limit=$3
	typeset msg=$4
	typeset after=$(special_alloc)
	typeset -i growth

	[[ -n "$after" ]] || log_fail "cannot read allocation of $CLASS_DISK0"
	(( growth = after - before ))
	log_note "$msg: special vdev grew by $growth bytes"
	if [[ $op == "ge" ]]; then
		(( growth >= limit )) || log_fail "$msg: expected >= $limit"
	else
		(( growth < limit )) || log_fail "$msg: expected < $limit"
	fi
}

log_assert $claim
log_onexit cleanup
log_must disk_setup

log_must zpool create $TESTPOOL $ZPOOL_DISKS special $CLASS_DISK0

typeset -i tenmeg=10485760
typeset -i onemeg=1048576
typeset vol=$TESTPOOL/$TESTFS/$TESTVOL

# 1. A file system with special_small_blocks
log_must zfs create -o compression=off -o special_small_blocks=32K \
	-o recordsize=32K $TESTPOOL/$TESTFS
before=$(special_alloc)
log_must dd if=/dev/urandom of=/$TESTPOOL/$TESTFS/testfile bs=1M count=10
check_growth $before ge $tenmeg "file blocks"

# 2. A zvol inheriting special_small_blocks from its parent
log_must zfs create -V 100M -b 32K -o compression=off $vol
block_device_wait "$ZVOL_DEVDIR/$vol"
log_must eval "zfs get -H -o source special_small_blocks $vol | \
    grep -q inherited"
before=$(special_alloc)
log_must dd if=/dev/urandom of=$ZVOL_RDEVDIR/$vol bs=1M count=10
check_growth $before lt $onemeg "zvol, inherited special_small_blocks"

# 3. The same zvol with special_small_blocks set locally
log_must zfs set special_small_blocks=32K $vol
before=$(special_alloc)
log_must dd if=/dev/urandom of=$ZVOL_RDEVDIR/$vol bs=1M count=10 \
	oseek=20
check_growth $before ge $tenmeg "zvol, local special_small_blocks"

zpool list -v $TESTPOOL

log_must zpool destroy -f "$TESTPOOL"
log_pass $claim
