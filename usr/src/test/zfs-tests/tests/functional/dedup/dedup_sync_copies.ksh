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
#	Verify that a synchronous (dmu_sync) write which needs one more copy
#	of an existing dedup entry writes that copy with the block's real,
#	compressed contents.
#
#	A dmu_sync() write reaches the DDT with an override block pointer
#	whose data never went through the write pipeline's compression.  An
#	extra copy built from it used to be the raw, uncompressed data under
#	a compressed block pointer: normal reads still worked, from the
#	first copy, but the second copy was garbage and a scrub found it
#	bad.
#
# STRATEGY:
#	1. Create a fast-dedup pool and an lz4 dedup dataset.
#	2. Write compressible data with copies=1 and sync.
#	3. Set copies=2, sync=always and logbias=throughput, so the second
#	   write of the same data goes through dmu_sync().
#	4. The second file must have deduplicated against the first (so the
#	   path really ran), both DVAs of every block must have the same
#	   allocated size, the data must read back, and a scrub must find
#	   no checksum errors.
#

. $STF_SUITE/include/libtest.shlib

verify_runnable "global"

log_assert "A synchronous dedup write that adds a copy writes real data"

typeset blob=$TEST_BASE_DIR/dedup_sync_copies.data.$$
typeset -i blocks=32

function cleanup
{
	if poolexists $TESTPOOL ; then
		destroy_pool $TESTPOOL
	fi
	rm -f $blob
}

log_onexit cleanup

log_must zpool create -f -o feature@fast_dedup=enabled $TESTPOOL $DISKS
log_must zfs create -o dedup=on -o compression=lz4 -o recordsize=128k \
    $TESTPOOL/fs
typeset mp=$(get_prop mountpoint $TESTPOOL/fs)

# Hex text: compresses to roughly two thirds with lz4, never embeds.
log_must eval "od -An -tx1 /dev/urandom | head -c $((blocks * 131072)) \
    > $blob"

log_must cp $blob $mp/a
log_must sync_pool $TESTPOOL

log_must zfs set copies=2 $TESTPOOL/fs
log_must zfs set sync=always $TESTPOOL/fs
log_must zfs set logbias=throughput $TESTPOOL/fs
log_must dd if=$blob of=$mp/b bs=128k
log_must sync_pool $TESTPOOL

log_must zpool export $TESTPOOL
log_must zpool import $TESTPOOL
typeset mp=$(get_prop mountpoint $TESTPOOL/fs)

log_must cmp $blob $mp/a
log_must cmp $blob $mp/b

typeset -i deduped=$(zdb -DD $TESTPOOL | awk '$1 == "2" { print $2; exit }')
if (( deduped != blocks )); then
	log_note "$(zdb -DD $TESTPOOL)"
	log_fail "expected $blocks blocks deduplicated at refcnt 2, found" \
	    "$deduped; the synchronous write did not reach the DDT"
fi

#
# Count blocks of b whose two DVAs have different allocated sizes: one
# of them then holds something other than the block's compressed data.
#
typeset obj=$(ls -i $mp/b | awk '{print $1}')
typeset -i mixed=$(zdb -dddddd $TESTPOOL/fs $obj | awk '
	$2 == "L0" {
		n = 0
		for (i = 3; i <= NF; i++)
			if ($i ~ /^[0-9]+:[0-9a-f]+:[0-9a-f]+$/) {
				split($i, f, ":"); a[++n] = f[3]
			}
		if (n != 2 || a[1] != a[2])
			m++
	}
	END { print m + 0 }')
if (( mixed != 0 )); then
	log_note "$(zdb -dddddd $TESTPOOL/fs $obj)"
	log_fail "$mixed blocks of the synchronous copy do not carry two" \
	    "equal-sized DVAs"
fi

log_must zpool scrub $TESTPOOL
wait_scrubbed $TESTPOOL
typeset errs=$(zpool status $TESTPOOL | awk '
	$3 ~ /^[0-9]+$/ && $4 ~ /^[0-9]+$/ && $5 ~ /^[0-9]+$/ {
		if ($3 + $4 + $5 > 0)
			printf "%s(r=%s,w=%s,c=%s) ", $1, $3, $4, $5
	}')
if [[ -n "$errs" ]]; then
	log_note "$(zpool status -v $TESTPOOL)"
	log_fail "scrub found errors: $errs"
fi

log_pass "A synchronous dedup write that adds a copy writes real data"
