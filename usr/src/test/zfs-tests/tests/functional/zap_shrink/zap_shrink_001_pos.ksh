#!/bin/ksh -p
#
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
# Copyright 2024, Klara Inc.
# Copyright (c) 2026, Gernot Strasser
#

. $STF_SUITE/include/libtest.shlib

#
# DESCRIPTION:
# Create a large number of files in a directory. Then remove all files and
# check that the directory zap was shrunk. Use zdb to check that the zap object
# contains only one leaf block using zdb.
#
# illumos shrinks only dedup table ZAPs by default, so this test turns on
# zap_shrink_all, which makes every fat ZAP shrink as on OpenZFS.  It then
# turns the tunable off again and checks that the shrunk directory still
# works: this stands in for a pool whose ZAPs were shrunk by OpenZFS.
#

verify_runnable "global"

DIR=largedir

NR_FILES=100000
BATCH=1000
CWD=$PWD

typeset orig_shrink_all=$(get_tunable zap_shrink_all)

function cleanup
{
	cd $CWD
	log_must set_tunable32 zap_shrink_all $orig_shrink_all
}

function create_files
{
	typeset -i i

	cd $TESTDIR/$DIR
	# In order to prevent arguments overflowing, create NR_FILES in
	# BATCH at once.
	for i in $(seq $(($NR_FILES/$BATCH))); do
		touch $(seq $((($i-1)*$BATCH+1)) $(($i*$BATCH))) || \
		    log_fail "touch failed in batch $i"
	done
	cd $CWD
}

function remove_files
{
	typeset -i i

	cd $TESTDIR/$DIR
	for i in $(seq $(($NR_FILES/$BATCH))); do
		rm $(seq $((($i-1)*$BATCH+1)) $(($i*$BATCH))) || \
		    log_fail "rm failed in batch $i"
	done
	cd $CWD
}

function nfiles
{
	ls $TESTDIR/$DIR | wc -l
}

function nleafs
{
	zdb -v -O $TESTPOOL/$TESTFS $DIR | grep "Leaf blocks:" | \
	    awk -F\: '{print($2);}'
}

log_assert "Create a large number of files ($NR_FILES) in a directory. " \
	"Make sure that the directory ZAP object was shrunk."

log_onexit cleanup

log_must set_tunable32 zap_shrink_all 1

log_must mkdir $TESTDIR/$DIR

create_files
log_must test $NR_FILES -eq $(nfiles)
sync_pool $TESTPOOL
log_must test 1 -lt $(nleafs)

# remove all files in $DIR directory
remove_files
sync_pool $TESTPOOL

log_must test 0 -eq $(nfiles)

# check whether zap_shrink works
log_must test 1 -eq $(nleafs)

log_must zpool export $TESTPOOL
log_must zpool import $TESTPOOL

# check whether zap_shrink works
log_must test 1 -eq $(nleafs)

#
# Back to the default: the shrunk directory must keep working, and must
# not shrink again.
#
log_must set_tunable32 zap_shrink_all 0

create_files
log_must test $NR_FILES -eq $(nfiles)
sync_pool $TESTPOOL
typeset grown=$(nleafs)
log_must test 1 -lt $grown

remove_files
sync_pool $TESTPOOL
log_must test 0 -eq $(nfiles)
log_must test $grown -eq $(nleafs)

log_must zpool export $TESTPOOL
log_must zpool import $TESTPOOL
log_must test 0 -eq $(nfiles)
log_must test $grown -eq $(nleafs)

log_pass "Directory ZAP object shrank with zap_shrink_all."
