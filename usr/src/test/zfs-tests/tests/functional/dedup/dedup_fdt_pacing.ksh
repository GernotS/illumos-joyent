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
# Copyright (c) 2025 Klara, Inc.
# Ported to illumos by the FDT port author.
#

# DESCRIPTION:
#	Ensure the FDT dedup log flushes are paced (upstream 1b495ee /
#	#17038).  With the flush rate pinned to one entry per txg, the log
#	must drain slowly (a handful of entries over several txgs), not all
#	at once; then, with the rate raised, it must drain fully.
#
# illumos adaptations vs upstream:
#	- get/set_tunable (not save/restore_tunable).
#	- block_cloning gate dropped (not in this port).

. $STF_SUITE/include/libtest.shlib

verify_runnable "both"

log_assert "dedup (FDT) paces out log entries appropriately"

typeset orig_max=$(get_tunable zfs_dedup_log_flush_entries_max)

function get_ddt_log_entries
{
	# zdb prints log lines as "DDT-log-sha256-N: <count> log entries"
	# (zdb.c: DMU_POOL_DDT_LOG ": %llu log entries\n") -- there is no
	# "entries=" token, so take field 2.  Stripping on "entries=" left
	# the line untouched and summed $1, which is the object name, so
	# this always evaluated to 0.
	zdb -D $TESTPOOL | grep -- "-log-sha256-" | \
	    awk '{sum += $2} END {print sum+0}'
}

function cleanup
{
	if poolexists $TESTPOOL; then
		destroy_pool $TESTPOOL
	fi
	log_must set_tunable32 zfs_dedup_log_flush_entries_max $orig_max
}

log_onexit cleanup

log_must zpool create -f -o feature@fast_dedup=enabled $TESTPOOL $DISKS

# Small recordsize -> more DDT entries; no compression -> predictable
# on-disk results; sha256 checksum for a stable dedup table name.
log_must zfs create \
    -o dedup=on \
    -o compression=off \
    -o checksum=sha256 \
    -o recordsize=4k $TESTPOOL/fs

# Pin the flush rate to a single entry per txg.
log_must set_tunable32 zfs_dedup_log_flush_entries_max 1

# 256 full blocks -> 256 entries in the dedup log.
log_must dd if=/dev/urandom of=/$TESTPOOL/fs/file1 bs=128k count=8
sync_pool

typeset -i log_entries=$(get_ddt_log_entries)
[[ "$log_entries" -gt 240 ]] || \
    log_fail "Fewer than 240 entries in dedup log: $log_entries"

# Let 5 txgs sync; with max=1, only a few entries should drain.
typeset -i i=0
while (( i < 5 )); do
	sync_pool
	(( i += 1 ))
done

typeset -i log_entries2=$(get_ddt_log_entries)
[[ $((log_entries - log_entries2)) -lt 20 ]] || \
    log_fail "Too many entries pruned from dedup log: " \
    "from $log_entries to $log_entries2"
[[ $((log_entries - log_entries2)) -gt 5 ]] || \
    log_fail "Too few entries pruned from dedup log: " \
    "from $log_entries to $log_entries2"

# Raise the flush rate high enough to drain the whole log.
log_must set_tunable32 zfs_dedup_log_flush_entries_max 1024
sync_pool

typeset -i log_entries3=$(get_ddt_log_entries)
[[ "$log_entries3" -eq 0 ]] || \
    log_fail "Entries still present in dedup log: $log_entries3"

# All 256 entries should now be in the unique store ZAP.
log_must eval "zdb -D $TESTPOOL | \
    grep -q 'DDT-sha256-zap-unique: *256 entries,'"

log_pass "dedup (FDT) paces out log entries appropriately"
