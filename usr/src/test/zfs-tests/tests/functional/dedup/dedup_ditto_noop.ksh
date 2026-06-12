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
#	Verify that the dedupditto pool property is accepted but no longer
#	creates ditto copies.
#
#	Dedup ditto copies were removed upstream (OpenZFS PR 8310) because
#	scrub and resilver never maintained the redundancy they claimed.
#	The property is kept, hidden, so existing scripts that set it keep
#	working, but setting it must have no effect.
#
# STRATEGY:
#	1. Create a pool with a legacy DDT (fast_dedup disabled); ditto
#	   copies only ever existed for legacy tables.
#	2. Set dedupditto=100.  This must succeed.
#	3. Reference one block 110 times and sync.
#	4. A legacy DDT must exist (so dedup really happened), but no ditto
#	   table may appear.
#

. $STF_SUITE/include/libtest.shlib

verify_runnable "global"

log_assert "dedupditto is accepted but writes no ditto copies"

typeset blob=$TEST_BASE_DIR/dedup_ditto_noop.data.$$
typeset -i refs=110

function cleanup
{
	if poolexists $TESTPOOL ; then
		destroy_pool $TESTPOOL
	fi
	rm -f $blob
}

log_onexit cleanup

log_must zpool create -f -o feature@fast_dedup=disabled $TESTPOOL $DISKS
log_must zpool set dedupditto=100 $TESTPOOL
log_must zfs create -o dedup=on -o compression=off $TESTPOOL/fs
typeset mp=$(get_prop mountpoint $TESTPOOL/fs)

log_must dd if=/dev/urandom of=$blob bs=128k count=1
typeset -i i=0
while (( i < refs )); do
	log_must cp $blob $mp/f$i
	(( i += 1 ))
done
log_must sync_pool $TESTPOOL

typeset ddt=$(zdb -DD $TESTPOOL)
if ! echo "$ddt" | grep -q 'DDT-sha256-zap-'; then
	log_note "$ddt"
	log_fail "no legacy DDT found; the block was not deduplicated"
fi
if echo "$ddt" | grep -q 'zap-ditto'; then
	log_note "$ddt"
	log_fail "a ditto copy was written although dedupditto is a no-op"
fi

log_pass "dedupditto is accepted but writes no ditto copies"
