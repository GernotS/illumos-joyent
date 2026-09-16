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
#	Prune an all-unique DDT to empty and require that no space is
#	leaked.  Exercises ddt_prune_walk() -> prune_candidates_sync() ->
#	ddt_phys_free() for the whole table (the prune free path), and, run
#	under forced ganging, exercises pruning of *ganged* unique entries --
#	the ddt_bp_fill / zio_free round-trip that frees the entire gang tree
#	rather than just the gang header.
#
#	Complements the existing dedup prune tests:
#	  - dedup_prune.ksh       prune keeps duplicates, removes uniques
#	  - dedup_prune_race.ksh  prune-to-empty races open-context readers
#	Neither asserts the space accounting after a full prune; a lost DVA
#	(e.g. freeing only a gang header) is invisible to `zpool scrub` /
#	`zpool status` but shows up in `zdb -bcc` as leaked space.  That
#	assertion is the point of this test.
#
# STRATEGY:
#	1. fast_dedup pool, small recordsize, dedup=on.
#	2. Write only UNIQUE blocks, so every DDT entry is a refcnt==1
#	   unique-class entry and therefore prune-eligible.  Do it twice:
#	   once unganged, once with metaslab_force_ganging low enough that
#	   the dedup blocks gang.
#	3. sleep 1 so entries are strictly older than the prune cutoff
#	   second, then `zpool ddtprune -p 100` and drain the log until the
#	   DDT is empty (re-prune as log entries flush into the ZAP).
#	4. Require the DDT is empty, `zdb -bcc` reports no space error, and
#	   `zpool status` reports no data errors.
#
# illumos adaptations vs upstream:
#	- get_tunable + set_tunable* (not save/restore_tunable).
#	- metaslab_force_ganging is uint64_t -> set_tunable64; illumos gangs
#	  probabilistically once psize >= metaslab_force_ganging, so the
#	  ganged phase writes several blocks to make ganging near-certain.
#	- self-contained; the leak check is `zdb -bcc`, matching
#	  dedup_gang_copies.ksh.
#	- block_cloning gate dropped (not in this port).

. $STF_SUITE/include/libtest.shlib

verify_runnable "both"

log_assert "Pruning an all-unique DDT to empty leaks no space"

typeset orig_txg_max=$(get_tunable zfs_dedup_log_txg_max)
typeset orig_ganging=$(get_tunable metaslab_force_ganging)

function cleanup
{
	if poolexists $TESTPOOL ; then
		destroy_pool $TESTPOOL
	fi
	log_must set_tunable64 metaslab_force_ganging $orig_ganging
	log_must set_tunable32 zfs_dedup_log_txg_max $orig_txg_max
}

log_onexit cleanup

function ddt_entries
{
	typeset -i entries=$(zpool status -D $TESTPOOL | \
	    grep "dedup: DDT entries" | awk '{print $4}')
	echo ${entries}
}

# Prune the whole table and drain the dedup log until the DDT is empty.
# Fresh entries can still be log-resident (not yet in the unique-class ZAP)
# when the first prune walks it, so re-issue the prune as the log drains.
function prune_all
{
	sleep 1
	log_must zpool ddtprune -p 100 $TESTPOOL
	typeset -i i=0
	while (( $(ddt_entries) > 0 && i < 20 )); do
		sync_pool $TESTPOOL
		sleep 1
		zpool ddtprune -p 100 $TESTPOOL
		sync_pool $TESTPOOL
		((i = i + 1))
	done
	(( $(ddt_entries) == 0 )) || \
	    log_fail "DDT not empty after prune -p 100"
}

function check_intact
{
	#
	# Match the clean result positively.  zdb -bcc always prints the
	# banner "Traversing all blocks to verify checksums and verify
	# nothing leaked ...", so grepping the output for "leaked" matches
	# every run, clean or not -- and the test then only passes when zdb
	# produces no output at all.  A clean run ends with
	# "No leaks (block sum matches space maps exactly)"; a dirty one
	# prints "block traversal size N != alloc M (leaked X)" instead.
	# Check the exit status too, so a zdb that cannot run is a failure
	# rather than a silent pass.
	#
	typeset out
	typeset -i rc
	out=$(zdb -bcc $TESTPOOL 2>&1)
	rc=$?
	if (( rc != 0 )); then
		log_note "$out"
		log_fail "zdb -bcc failed (exit $rc)"
	fi
	if ! echo "$out" | grep -q "No leaks (block sum matches space maps exactly)"; then
		log_note "$out"
		log_fail "zdb -bcc reported a space accounting error"
	fi
	log_must check_pool_status $TESTPOOL "errors" "No known data errors"
}

# Flush the dedup log every txg so prune -> empty ZAP happens promptly.
log_must set_tunable32 zfs_dedup_log_txg_max 1

log_must zpool create -f -o ashift=9 -o feature@fast_dedup=enabled $TESTPOOL \
    $DISKS
log_must zfs create -o recordsize=512 -o dedup=on -o compression=off \
    $TESTPOOL/$TESTFS
typeset mountpoint=$(get_prop mountpoint $TESTPOOL/$TESTFS)

#
# Phase 1: unganged.  Write unique blocks, prune the lot, require no leak.
#
log_must set_tunable64 metaslab_force_ganging $orig_ganging
log_must dd if=/dev/urandom of=$mountpoint/u1 bs=512 count=2048
sync_pool $TESTPOOL
(( $(ddt_entries) > 0 )) || log_fail "no DDT entries after unganged write"

prune_all
check_intact

log_must rm $mountpoint/u1
sync_pool $TESTPOOL

#
# Phase 2: ganged.  Force ganging so the unique dedup blocks are gang
# blocks, then prune them all -- exercises freeing ganged DDT entries.
#
log_must set_tunable64 metaslab_force_ganging 20000
log_must dd if=/dev/urandom of=$mountpoint/g1 bs=128k count=64
sync_pool $TESTPOOL
(( $(ddt_entries) > 0 )) || log_fail "no DDT entries after ganged write"

prune_all
check_intact

log_pass "Pruning an all-unique DDT to empty leaks no space"
