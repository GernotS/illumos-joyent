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
#	zfs rewrite rejects bad arguments and handles directories.
#

verify_runnable "both"

DIR=$TESTDIR/rewrite.dir

function cleanup
{
	rm -rf $DIR
}
log_onexit cleanup

log_assert "zfs rewrite argument handling"

log_must mkdir -p $DIR/sub
log_must eval "yes a | head -c 262144 > $DIR/f1"
log_must eval "yes b | head -c 262144 > $DIR/sub/f2"

log_mustnot zfs rewrite
log_mustnot zfs rewrite -Q $DIR/f1
log_mustnot zfs rewrite -o bogus $DIR/f1
log_mustnot zfs rewrite $DIR/nonexistent
log_mustnot zfs rewrite $DIR
log_mustnot zfs rewrite /dev/null

out=$(zfs rewrite -rv $DIR) || log_fail "zfs rewrite -r failed"
echo "$out" | grep -q "f1$" || log_fail "f1 not rewritten: $out"
echo "$out" | grep -q "sub/f2$" || log_fail "sub/f2 not rewritten: $out"

# Offsets past the end of the file are a no-op.
log_must zfs rewrite -o 1g $DIR/f1

log_pass "zfs rewrite argument handling"
