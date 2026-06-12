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
#	Verify that growing an encrypted, deduplicated block's copies keeps
#	the salt/IV stored in its flat DDT entry.
#
#	On an encrypted block pointer DVA[2] holds the salt and the first
#	half of the IV, and the DDT entry stores it so that later dedup
#	hits can be decrypted.  A write that only adds copies to an existing
#	entry used to zero it: the entry kept salt=0, and every later copy
#	of the same data, filled from that entry, could not be decrypted.
#
# STRATEGY:
#	1. Create a fast-dedup pool and an encrypted dedup dataset.
#	2. Write a file with copies=1 and sync.
#	3. Set copies=2 and write the same data again: this extends the
#	   existing entries by one DVA.
#	4. Export/import so the DDT is reloaded from disk, then write the
#	   same data a third time; its block pointers come from the entries.
#	5. Export/import again.  All three files must read back, every
#	   entry must be referenced three times (so the path really ran),
#	   and no entry may show salt=0.
#

. $STF_SUITE/include/libtest.shlib

verify_runnable "global"

log_assert "Extending an encrypted dedup entry keeps its salt/IV"

typeset keyfile=$TEST_BASE_DIR/dedup_encrypted_copies.key.$$
typeset blob=$TEST_BASE_DIR/dedup_encrypted_copies.data.$$
typeset -i blocks=8

function cleanup
{
	if poolexists $TESTPOOL ; then
		destroy_pool $TESTPOOL
	fi
	rm -f $keyfile $blob
}

log_onexit cleanup

function reload
{
	log_must zpool export $TESTPOOL
	log_must zpool import $TESTPOOL
	log_must zfs load-key $TESTPOOL/enc
	log_must zfs mount $TESTPOOL/enc
}

log_must eval "echo 'dedup-encrypted-copies' > $keyfile"

log_must zpool create -f -o feature@fast_dedup=enabled $TESTPOOL $DISKS
log_must zfs create -o encryption=on -o keyformat=passphrase \
    -o keylocation=file://$keyfile -o dedup=on -o compression=off \
    -o recordsize=128k $TESTPOOL/enc

typeset mp=$(get_prop mountpoint $TESTPOOL/enc)

log_must dd if=/dev/urandom of=$blob bs=128k count=$blocks
log_must cp $blob $mp/a
log_must sync_pool $TESTPOOL

log_must zfs set copies=2 $TESTPOOL/enc
log_must cp $blob $mp/b
log_must sync_pool $TESTPOOL

reload
typeset mp=$(get_prop mountpoint $TESTPOOL/enc)
log_must cp $blob $mp/c
log_must sync_pool $TESTPOOL

reload
typeset mp=$(get_prop mountpoint $TESTPOOL/enc)
for f in a b c; do
	if ! cmp -s $blob $mp/$f; then
		log_note "$(zpool status -v $TESTPOOL)"
		log_fail "encrypted file $f does not read back correctly"
	fi
done

typeset entries=$(zdb -DDDDD $TESTPOOL | grep '^index')
typeset -i triple=$(echo "$entries" | grep -c 'refcnt 3 ')
typeset -i zero=$(echo "$entries" | grep -c 'salt=0 ')
if (( triple != blocks )); then
	log_note "$entries"
	log_fail "expected $blocks DDT entries referenced three times," \
	    "found $triple; the extend path was not exercised"
fi
if (( zero != 0 )); then
	log_note "$entries"
	log_fail "$zero DDT entries lost their salt/IV (salt=0)"
fi

log_pass "Extending an encrypted dedup entry keeps its salt/IV"
