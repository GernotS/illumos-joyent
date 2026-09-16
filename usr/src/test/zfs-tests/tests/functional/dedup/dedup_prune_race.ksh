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
#	Stress the race between sync-context DDT table ZAP destruction and
#	open-context readers of the table objects (upstream 765929cb4,
#	ported as the ddt_objects_lock patch).
#
# STRATEGY:
#	1. Start background loops of open-context DDT readers:
#	   `zpool status -D` (ddt_object_count) and
#	   `zpool prefetch -t ddt` (ddt_object_prefetch_all).
#	2. Repeatedly: write unique dedup data (creates the unique-class
#	   store object), then `zpool ddtprune -p 100` and sync until the
#	   DDT is empty.  Emptying it makes sync context destroy the store
#	   object (and the FDT dir) while the readers hammer it.
#	3. Verify the pool survives intact (scrub, no errors).
#
#	Without ddt_objects_lock this races open-context ZAP ops against
#	object destruction; a lost race is a VERIFY panic in zap_lockdir.

. $STF_SUITE/include/libtest.shlib

verify_runnable "both"

log_assert "DDT table destruction does not race open-context DDT readers"

# Flush the dedup log every txg so prune -> empty ZAP -> object destroy
# happens promptly and repeatedly within the test's time budget.
typeset orig_txg_max=$(get_tunable zfs_dedup_log_txg_max)
log_must set_tunable32 zfs_dedup_log_txg_max 1

typeset racedir=/tmp/dedup_prune_race.$$
typeset stopfile=$racedir/keep_running

function cleanup
{
	rm -rf $racedir
	wait
	if poolexists $TESTPOOL ; then
		destroy_pool $TESTPOOL
	fi
	log_must set_tunable32 zfs_dedup_log_txg_max $orig_txg_max
}

function ddt_entries
{
	typeset -i entries=$(zpool status -D $TESTPOOL | \
		grep "dedup: DDT entries" | awk '{print $4}')
	echo ${entries}
}

log_onexit cleanup

log_must mkdir -p $racedir
log_must touch $stopfile

log_must zpool create -f $TESTPOOL $DISKS
log_must zfs create -o recordsize=512 -o dedup=on $TESTPOOL/$TESTFS
typeset mountpoint=$(get_prop mountpoint $TESTPOOL/$TESTFS)

#
# Open-context DDT readers, hammering while the store objects come and go.
#
while [[ -e $stopfile ]]; do
	zpool status -D $TESTPOOL >/dev/null 2>&1
done &
while [[ -e $stopfile ]]; do
	zpool prefetch -t ddt $TESTPOOL >/dev/null 2>&1
done &

#
# Each round populates the unique class, then prunes it empty, which
# destroys the emptied store object (and FDT dir) in sync context.
#
typeset -i round
for round in 1 2 3 4 5; do
	log_must dd if=/dev/urandom of=$mountpoint/f$round bs=512k count=2
	sync_pool $TESTPOOL
	(( $(ddt_entries) > 0 )) || \
		log_fail "no DDT entries after write (round $round)"

	# Entries must be strictly older than the prune cutoff second.
	sleep 1
	log_must zpool ddtprune -p 100 $TESTPOOL

	# Let the log flush drain so the emptied store object is destroyed.
	# Fresh entries may still be in the dedup log (not yet in the
	# unique-class ZAP) when the first prune walks it, so re-issue the
	# prune as the log drains.
	typeset -i i=0
	while (( $(ddt_entries) > 0 && i < 20 )); do
		sync_pool $TESTPOOL
		sleep 1
		zpool ddtprune -p 100 $TESTPOOL
		sync_pool $TESTPOOL
		((i = i + 1))
	done
	(( $(ddt_entries) == 0 )) || \
		log_fail "DDT not empty after prune (round $round)"

	log_must rm $mountpoint/f$round
	sync_pool $TESTPOOL
done

log_must rm $stopfile
wait

log_must zpool scrub $TESTPOOL
while is_pool_scrubbing $TESTPOOL ; do
	sleep 1
done
log_must check_pool_status $TESTPOOL "errors" "No known data errors"

log_pass "DDT table destruction survives concurrent open-context readers"
