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
#	Verify that a flat-DDT dedup write never shares a gang block that
#	has fewer copies of the data than the write asked for.
#
#	A gang block's DVAs are copies of its gang header, and the header
#	gets one copy more than the data: a copies=1 gang block has two
#	DVAs but one copy of the data, a copies=2 gang block three DVAs but
#	two copies.  Counting DVAs, a copies=2 write took the first as good
#	enough and a copies=3 write the second, and both silently ended up
#	with less redundancy than requested (OpenZFS issue 17070, cases 2
#	and 4).  Such writes must not dedup against the gang block.
#
#	dedup_gang_copies.ksh only covers copies=1 followed by copies=3,
#	where the DVA count is already too low.
#
# STRATEGY:
#	1. Fast-dedup pool with copies=1, copies=2 and copies=3 datasets.
#	2. Force ganging and write random data with copies=1 and with
#	   copies=2 until the DDT holds gang entries with two and with three
#	   DVAs.  Stop forcing ganging.
#	3. Copy the copies=1 files to the copies=2 dataset and the copies=2
#	   files to the copies=3 dataset.  No gang entry may gain a
#	   reference: those writes must not dedup against them.
#	4. Control: copy the files again within their own datasets.  Gang
#	   entries of both kinds must now be shared (refcnt 2), so dedup
#	   against a gang block that has enough copies still works.
#	5. All files must read back, and zdb -bcc and zpool status must be
#	   clean.
#

. $STF_SUITE/include/libtest.shlib

verify_runnable "global"

log_assert "Dedup never shares a gang block with fewer copies than requested"

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

# Flush the dedup log every txg so entries reach the flat slots predictably.
log_must set_tunable32 zfs_dedup_log_txg_max 1

log_must zpool create -f -o ashift=9 -o feature@fast_dedup=enabled $TESTPOOL \
    $DISKS
for c in 1 2 3; do
	log_must zfs create -o recordsize=64k -o dedup=on -o compression=off \
	    -o copies=$c $TESTPOOL/c$c
done
typeset mp1=$(get_prop mountpoint $TESTPOOL/c1)
typeset mp2=$(get_prop mountpoint $TESTPOOL/c2)
typeset mp3=$(get_prop mountpoint $TESTPOOL/c3)

function sync_ddt
{
	typeset -i n=0
	while (( n < 4 )); do
		log_must sync_pool $TESTPOOL
		(( n += 1 ))
	done
}

#
# Print "refcnt ndvas" for every gang DDT entry.  zdb -DDDDD prints one
# "index <i> refcnt <n> flat <blkptr>" line per entry, from the ZAP and
# from the dedup log; a log line may be an older copy of the same entry,
# with a lower refcnt, so callers only ever look for refcnts that are too
# high or for the presence of an expected one.
#
function gang_entries
{
	zdb -DDDDD $TESTPOOL | awk '
		$1 == "index" && / gang / {
			n = gsub(/DVA\[/, "&")
			print $4, n
		}'
}

#
# Ganging is probabilistic on illumos: once metaslab_force_ganging is set,
# an allocation of at least that size gangs when (lbolt & 3) == 0.  Keep
# writing new files until the DDT holds a gang entry with the wanted
# number of DVAs.
#
function make_gang # mountpoint ndvas
{
	typeset mp=$1 ndvas=$2
	typeset -i n=0
	while (( n < 20 )); do
		log_must dd if=/dev/urandom of=$mp/f$n bs=64k count=32
		sync_ddt
		if gang_entries | awk -v d=$ndvas '$2 == d' | grep -q .; then
			return
		fi
		(( n += 1 ))
	done
	log_fail "no gang DDT entry with $ndvas DVAs after $n files"
}

log_must set_tunable64 metaslab_force_ganging 20000
make_gang $mp1 2
make_gang $mp2 3
log_must set_tunable64 metaslab_force_ganging $orig_ganging

log_note "gang entries before copying: $(gang_entries | wc -l)"

#
# copies=1 -> copies=2 and copies=2 -> copies=3.
#
for f in $mp1/f*; do
	log_must cp $f $mp2/up.${f##*/}
done
for f in $mp2/f*; do
	log_must cp $f $mp3/up.${f##*/}
done
sync_ddt

typeset shared=$(gang_entries | awk '$1 != 1')
if [[ -n "$shared" ]]; then
	log_note "$(zdb -DDDDD $TESTPOOL | grep ' gang ')"
	log_fail "a write that needed more copies deduplicated against a" \
	    "gang block (refcnt ndvas): $shared"
fi

#
# Control: within the same dataset the gang block has enough copies, so
# these writes must dedup against it.
#
for f in $mp1/f*; do
	log_must cp $f $mp1/same.${f##*/}
done
for f in $mp2/f*; do
	log_must cp $f $mp2/same.${f##*/}
done
sync_ddt

typeset entries=$(gang_entries)
for d in 2 3; do
	if ! echo "$entries" | awk -v d=$d '$1 == 2 && $2 == d' | grep -q .
	then
		log_note "$(zdb -DDDDD $TESTPOOL | grep ' gang ')"
		log_fail "no gang entry with $d DVAs was shared; the test" \
		    "did not exercise dedup against gang blocks"
	fi
done

#
# Everything reads back after a reload.
#
log_must zpool export $TESTPOOL
log_must zpool import $TESTPOOL
for f in $mp1/f*; do
	log_must cmp $f $mp2/up.${f##*/}
	log_must cmp $f $mp1/same.${f##*/}
done
for f in $mp2/f*; do
	log_must cmp $f $mp3/up.${f##*/}
	log_must cmp $f $mp2/same.${f##*/}
done

typeset out
typeset -i rc
out=$(zdb -bcc $TESTPOOL 2>&1)
rc=$?
if (( rc != 0 )); then
	log_note "$out"
	log_fail "zdb -bcc failed (exit $rc)"
fi
if ! echo "$out" | grep -q "No leaks (block sum matches space maps exactly)"
then
	log_note "$out"
	log_fail "zdb -bcc reported a space accounting error"
fi
if ! check_pool_status $TESTPOOL "errors" "No known data errors"; then
	log_note "$(zpool status -v $TESTPOOL)"
	log_fail "pool status not clean"
fi

log_pass "Dedup never shares a gang block with fewer copies than requested"
