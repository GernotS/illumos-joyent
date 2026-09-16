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
# Copyright (c) 2026, the FDT port author.
#

# DESCRIPTION:
#	Verify that a DDT entry whose in-core class disagrees with the
#	object it is actually stored in does not panic the system, is
#	still removed, and leaves an importable pool.
#
#	Hardening test for openzfs/zfs#17528:
#
#	  panic: VERIFY(ddt_object_remove(ddt, otype, oclass, dde, tx) == 0)
#		ddt_sync() <- dsl_scan_sync() <- spa_sync() <- txg_sync_thread()
#
#	The upstream issue is OPEN with no root cause and no reproducer, so
#	the drift is injected deliberately (ddt_inject_stray_class, patch
#	0075) rather than provoked.  What is under test is the RESPONSE to
#	drift, not the drift itself.
#
#	The panic matters more than it looks: it fires in the sync thread,
#	so it recurs on every subsequent import.  The reporter's machines
#	boot-looped and were recoverable only via "zpool import -o
#	readonly=on".  Hence the export/import step below -- surviving the
#	write is only half the fix.
#
# STRATEGY:
#	1. Create a LEGACY-DDT pool (feature@fast_dedup=disabled).  The
#	   (otype, oclass) signature in the reported VERIFY is the
#	   traditional DDT; the FDT path is covered by the second pass.
#	2. Write unique data -> entries land in the UNIQUE class.
#	3. Enable the injector.
#	4. Duplicate the data -> refcnt 2 -> class changes UNIQUE ->
#	   DUPLICATE, which is exactly what makes ddt_sync_entry() call
#	   ddt_object_remove().  With the injector on, it is handed the
#	   wrong class and must recover.
#	5. Delete the duplicate -> class changes back, second removal.
#	6. Disable the injector, then assert:
#	     - the pool is alive and error-free
#	     - zdb -bcc reports no leaks (the recovery removed the entry
#	       from where it really was; simply swallowing ENOENT would
#	       strand it and show up here)
#	     - the pool exports and re-imports (the boot-loop symptom)
#	7. Repeat the whole thing with fast_dedup enabled to cover
#	   ddt_sync_flush_entry().
#
# NOTES:
#	Without patch 0074 this test panics the test host rather than
#	failing cleanly.  That is inherent -- the bug under test IS a
#	panic -- so do not run it on a machine you are not willing to
#	lose, and expect no test output on failure, only a crash dump.

. $STF_SUITE/include/libtest.shlib

verify_runnable "both"

log_assert "A DDT entry in the wrong class is recovered, not fatal"

typeset -i orig_inject=$(get_tunable ddt_inject_stray_class)

function cleanup
{
	# Clear the injector FIRST: leaving it set would corrupt every
	# later dedup test in the run.
	set_tunable32 ddt_inject_stray_class 0
	if poolexists $TESTPOOL ; then
		destroy_pool $TESTPOOL
	fi
	set_tunable32 ddt_inject_stray_class $orig_inject
}

log_onexit cleanup

#
# One pass.  $1 is the feature@fast_dedup setting, so the same sequence
# covers both ddt_sync_entry() (legacy) and ddt_sync_flush_entry() (FDT).
#
function stray_class_pass
{
	typeset fastdedup="$1"
	typeset mountpoint zdb_out

	log_note "=== pass: feature@fast_dedup=$fastdedup ==="

	log_must zpool create -f -o feature@fast_dedup=$fastdedup \
	    $TESTPOOL $DISKS
	log_must zfs create -o dedup=on $TESTPOOL/$TESTFS
	mountpoint=$(get_prop mountpoint $TESTPOOL/$TESTFS)

	# Unique data first: every entry starts in the UNIQUE class.
	log_must dd if=/dev/urandom of=$mountpoint/f1 bs=1M count=8
	sync_pool $TESTPOOL
	sync_pool $TESTPOOL

	# From here the sync path is deliberately lying about class.
	log_must set_tunable32 ddt_inject_stray_class 1

	# refcnt 1 -> 2 moves entries UNIQUE -> DUPLICATE, and a class
	# change is one of the three conditions that reaches
	# ddt_object_remove().  This is the injected removal.
	log_must cp $mountpoint/f1 $mountpoint/f2
	sync_pool $TESTPOOL
	sync_pool $TESTPOOL

	# ...and back again, for a second removal in the other direction.
	log_must rm $mountpoint/f2
	sync_pool $TESTPOOL
	sync_pool $TESTPOOL

	log_must set_tunable32 ddt_inject_stray_class 0

	# If we are still executing, the VERIFY did not fire.
	log_must check_pool_status $TESTPOOL "errors" "No known data errors"

	# The surviving copy must still be readable and correct.
	log_must dd if=$mountpoint/f1 of=/dev/null bs=1M count=8

	# The recovery must have removed the entry from wherever it really
	# was.  Merely ignoring ENOENT would leave it stranded, which shows
	# up here as leaked space.
	zdb_out=$(zdb -bcc $TESTPOOL 2>&1)
	log_note "$zdb_out"
	if ! echo "$zdb_out" | grep -q "No leaks"; then
		log_fail "stray-class recovery leaked space ($fastdedup)"
	fi

	# The #17528 symptom is an unbootable machine, not a single panic:
	# the damage is only really survived if the pool comes back.
	log_must zpool export $TESTPOOL
	log_must zpool import $TESTPOOL
	log_must check_pool_status $TESTPOOL "errors" "No known data errors"

	log_must destroy_pool $TESTPOOL
}

# Legacy DDT: the path in the reported stack.
stray_class_pass disabled

# FDT: ddt_sync_flush_entry(), where this tree already needed #16277 and
# #15892 for the same ENOENT.
stray_class_pass enabled

log_pass "A DDT entry in the wrong class is recovered, not fatal"
