#!/bin/ksh -p
# SPDX-License-Identifier: CDDL-1.0
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
# Copyright (c) 2024 by Klara Inc. (ported to illumos)
#

# Simple test of dedup table operations (legacy)
#
# Covers the pure-legacy path: with feature@fast_dedup disabled, DDT
# objects must be created as children of the MOS root (named
# "DDT-sha256-zap-<class>"), NOT inside an FDT directory object, and the
# feature must never activate.  The port's ddt_configure() chooses
# legacy vs FDT, and ddt_object_create()/_destroy()/_load() select the
# parent object with
#
#	(ddt_version == DDT_VERSION_FDT) ? ddt_dir_object
#					 : DMU_POOL_DIRECTORY_OBJECT
#
# so a regression here is invisible to the FDT tests and would only
# surface on pools that never enabled the feature.
#
# illumos adaptations vs upstream:
#   - dropped "-O xattr=sa": illumos xattr is on|off only (SA-based
#     xattrs are a Linux/OpenZFS feature), and the upstream rationale
#     for it -- keeping SELinux xattrs out of the accounting -- does not
#     apply here.

. $STF_SUITE/include/libtest.shlib

verify_runnable "global"

log_assert "basic dedup (legacy) operations work"

function cleanup
{
	destroy_pool $TESTPOOL
}

log_onexit cleanup

# Create a pool with legacy dedup enabled.  Compression is disabled so
# our writes produce predictable results on disk.
log_must zpool create -f \
    -o feature@fast_dedup=disabled \
    -O dedup=on \
    -O compression=off \
    $TESTPOOL $DISKS

# confirm the feature is disabled
log_must test $(get_pool_prop feature@fast_dedup $TESTPOOL) = "disabled"

# confirm there are no DDT keys in the MOS root
#
# A pipeline's exit status is the last command's, so
# "log_mustnot eval \"zdb ... | grep -q X\"" also passes when zdb itself
# fails and prints nothing -- the assertion silently stops testing
# anything.  Capture the output, fail if zdb did not run, then check.
#
typeset zdb_out
zdb_out=$(zdb -dddd $TESTPOOL 1 2>&1) || log_fail "zdb -dddd failed"
if echo "$zdb_out" | grep -q DDT-sha256; then
	log_fail "a DDT already exists before any dedup write"
fi

# Create a file.  This is four full blocks, so it produces four entries
# in the dedup table.
log_must dd if=/dev/urandom of=/$TESTPOOL/file1 bs=128k count=4
log_must zpool sync

# feature should still be disabled
log_must test $(get_pool_prop feature@fast_dedup $TESTPOOL) = "disabled"

# should be four entries in the unique table
log_must eval "zdb -D $TESTPOOL | grep -q 'DDT-sha256-zap-unique: 4 entries'"

# should be just one DDT ZAP in the MOS
log_must test $(zdb -dddd $TESTPOOL 1 | grep DDT-sha256-zap- | wc -l) -eq 1

# copy the file
log_must dd if=/$TESTPOOL/file1 of=/$TESTPOOL/file2 bs=128k
log_must zpool sync

# now four entries in the duplicate table
log_must eval "zdb -D $TESTPOOL | grep -q 'DDT-sha256-zap-duplicate: 4 entries'"

# Now two DDT ZAPs in the MOS; DDT ZAPs are not cleaned up until the
# entire logical table is destroyed.
log_must test $(zdb -dddd $TESTPOOL 1 | grep DDT-sha256-zap- | wc -l) -eq 2

# remove the files
log_must rm -f /$TESTPOOL/file*
log_must zpool sync

# feature should still be disabled
log_must test $(get_pool_prop feature@fast_dedup $TESTPOOL) = "disabled"

# all DDTs empty
log_must eval "zdb -D $TESTPOOL | grep -q 'All DDTs are empty'"

# logical table now destroyed; all DDT ZAPs removed
log_must test $(zdb -dddd $TESTPOOL 1 | grep DDT-sha256-zap- | wc -l) -eq 0

log_must zdb -b $TESTPOOL

log_pass "basic dedup (legacy) operations work"
