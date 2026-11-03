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
# Copyright (c) 2007, Sun Microsystems Inc. All rights reserved.
# Copyright (c) 2021, Rich Ercolani.
# Copyright 2026 Gernot Strasser
# Use is subject to license terms.
#

. $STF_SUITE/include/properties.shlib
. $STF_SUITE/include/libtest.shlib

#
# DESCRIPTION:
# Import a pool containing variously-permuted zstd-compressed files,
# written by OpenZFS on x86_64 and on big-endian ppc64 FreeBSD, then try
# to copy them out. This checks that illumos reads zstd blocks written by
# OpenZFS, including their byte-swapped headers.
#
# The pool image is 1 GiB but sparse. OpenZFS ships it as a GNU sparse
# tarball; here it is stored as its non-empty 64 KiB chunks
# (testpool_zstd.bz2) and their offsets (testpool_zstd_map.txt), from which
# the image is rebuilt byte-for-byte with truncate and dd.
#

typeset DIR=$STF_SUITE/tests/functional/compression
typeset IMG=$TEST_BASE_DIR/testpool_zstd
typeset CHUNKS=$TEST_BASE_DIR/testpool_zstd.chunks

verify_runnable "both"

function cleanup
{
	poolexists testpool_zstd && destroy_pool testpool_zstd
	rm -f $IMG $CHUNKS
}

function rebuild_image
{
	typeset -i i=0
	typeset off

	log_must truncate -s 1073741824 $IMG
	log_must eval "bzcat $DIR/testpool_zstd.bz2 > $CHUNKS"
	while read off; do
		dd if=$CHUNKS of=$IMG bs=65536 iseek=$i oseek=$off count=1 \
		    conv=notrunc 2>/dev/null || log_fail "dd of chunk $i failed"
		(( i += 1 ))
	done < $DIR/testpool_zstd_map.txt
	rm -f $CHUNKS
}

log_assert "Trying to read data from variously mangled zstd datasets"
log_onexit cleanup

rebuild_image
log_must zpool import -d $TEST_BASE_DIR testpool_zstd
log_must dd if=/testpool_zstd/x86_64/zstd of=/dev/null
log_must dd if=/testpool_zstd/ppc64_fbsd/zstd of=/dev/null

log_pass "Reading from mangled zstd datasets works as expected."
