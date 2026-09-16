/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright 2026 RackTop Systems, Inc.  All rights reserved.
 */

/*
 * aes_ctr_ktest -- kernel tests for AES-CTR mode 64-bit counter wrap.
 *
 * Suite ctr_counter_wrap: verifies that incrementing the 64-bit counter
 * field past UINT64_MAX wraps correctly to 0 rather than overflowing into
 * the nonce half of the counter block.
 *
 * Strategy:
 *   1. Encrypt 3 AES blocks (48 bytes) starting at counter = UINT64_MAX - 1.
 *      This exercises counter values UINT64_MAX-1, UINT64_MAX, and 0 (wrap).
 *   2. Decrypt the result with the same parameters and verify round-trip.
 *   3. Cross-check the wrapped block: encrypt a single block at counter = 0
 *      independently and compare with bytes [32..48) of the 3-block output.
 *      A counter that fails to wrap would use counter = UINT64_MAX+1 = 0
 *      with the wrong nonce contribution, producing a different keystream.
 *
 * Test triple: aes::ctr_counter_wrap:<test>
 */

#include <sys/types.h>
#include <sys/kmem.h>
#include <sys/sunddi.h>
#include <sys/crypto/api.h>
#include <sys/crypto/common.h>
#include <sys/ktest.h>
#include <sys/modctl.h>

#define	CTR_KEY_LEN	16
#define	CTR_BLOCK_LEN	16
#define	CTR_NBLOCKS	3

/*
 * AES-128 key used for CTR tests.
 * (NIST AES-128-CTR test key, FIPS 197 Appendix B)
 */
static const uint8_t ctr_key[CTR_KEY_LEN] = {
	0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
	0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c
};

/*
 * Build a CKM_AES_CTR crypto_mechanism_t with a 64-bit counter.
 *
 * Counter block layout (16 bytes, big-endian):
 *   cb[0..7]  = nonce (all zeros)
 *   cb[8..15] = counter_val (big-endian)
 *
 * ulCounterBits = 64 means only cb[8..15] is incremented; cb[0..7] is fixed.
 */
static int
ctr_mech_init(crypto_mechanism_t *mech, CK_AES_CTR_PARAMS *ctrp,
    uint64_t counter_val)
{
	mech->cm_type = crypto_mech2id("CKM_AES_CTR");
	if (mech->cm_type == CRYPTO_MECH_INVALID)
		return (-1);

	bzero(ctrp, sizeof (*ctrp));
	ctrp->ulCounterBits = 64;
	/* nonce: cb[0..7] = 0x00 (already zeroed by bzero) */
	/* counter: cb[8..15] = counter_val big-endian */
	ctrp->cb[8]  = (uint8_t)(counter_val >> 56);
	ctrp->cb[9]  = (uint8_t)(counter_val >> 48);
	ctrp->cb[10] = (uint8_t)(counter_val >> 40);
	ctrp->cb[11] = (uint8_t)(counter_val >> 32);
	ctrp->cb[12] = (uint8_t)(counter_val >> 24);
	ctrp->cb[13] = (uint8_t)(counter_val >> 16);
	ctrp->cb[14] = (uint8_t)(counter_val >>  8);
	ctrp->cb[15] = (uint8_t)(counter_val      );

	mech->cm_param     = (caddr_t)ctrp;
	mech->cm_param_len = sizeof (*ctrp);
	return (0);
}

/*
 * One-shot AES-CTR encrypt or decrypt of len bytes.
 * in and out must not overlap; out must be at least len bytes.
 * Returns 0 on success, -1 on error (ctx result set).
 */
static int
ctr_crypt(uint64_t counter_val, const uint8_t *in, uint8_t *out, size_t len,
    boolean_t encrypt, ktest_ctx_hdl_t *ctx)
{
	crypto_mechanism_t	mech;
	CK_AES_CTR_PARAMS	ctrp;
	crypto_key_t		key;
	crypto_data_t		din, dout;
	int			rv;

	if (ctr_mech_init(&mech, &ctrp, counter_val) != 0) {
		KT_ERROR(ctx, "CKM_AES_CTR not available");
		return (-1);
	}

	key.ck_format = CRYPTO_KEY_RAW;
	key.ck_data   = (void *)(uintptr_t)ctr_key;
	key.ck_length = CTR_KEY_LEN * 8;

	bzero(&din, sizeof (din));
	din.cd_format       = CRYPTO_DATA_RAW;
	din.cd_offset       = 0;
	din.cd_length       = len;
	din.cd_raw.iov_base = (caddr_t)(uintptr_t)in;
	din.cd_raw.iov_len  = len;

	bzero(&dout, sizeof (dout));
	dout.cd_format       = CRYPTO_DATA_RAW;
	dout.cd_offset       = 0;
	dout.cd_length       = len;
	dout.cd_raw.iov_base = (caddr_t)out;
	dout.cd_raw.iov_len  = len;

	if (encrypt)
		rv = crypto_encrypt(&mech, &din, &key, NULL, &dout, NULL);
	else
		rv = crypto_decrypt(&mech, &din, &key, NULL, &dout, NULL);

	if (rv != CRYPTO_SUCCESS) {
		KT_ERROR(ctx, "CTR %s (counter=0x%016llx) failed: %d",
		    encrypt ? "encrypt" : "decrypt",
		    (unsigned long long)counter_val, rv);
		return (-1);
	}
	return (0);
}

/*
 * ctr_counter_wrap_test
 *
 * Encrypt 3 AES blocks starting at counter = UINT64_MAX - 1 and verify:
 *
 *   (a) Round-trip: decrypt with the same starting counter reproduces the
 *       original plaintext.  This would fail if any counter value was wrong.
 *
 *   (b) Wrap: independently encrypt one block starting at counter = 0 and
 *       confirm it matches the 3rd block of the 3-block output.  If the
 *       counter failed to wrap (e.g. saturated at UINT64_MAX or corrupted
 *       the nonce half), the keystream for block 3 would differ.
 */
void
ctr_counter_wrap_test(ktest_ctx_hdl_t *ctx)
{
	const size_t	pt_len = CTR_NBLOCKS * CTR_BLOCK_LEN;	/* 48 bytes */
	/*
	 * Start one before UINT64_MAX so the three counter values exercised
	 * are: UINT64_MAX-1, UINT64_MAX, 0 (wrap).
	 */
	const uint64_t	start_ctr = (uint64_t)0xFFFFFFFFFFFFFFFEULL;

	uint8_t		pt[CTR_NBLOCKS * CTR_BLOCK_LEN];
	uint8_t		ct[CTR_NBLOCKS * CTR_BLOCK_LEN];
	uint8_t		pt_dec[CTR_NBLOCKS * CTR_BLOCK_LEN];
	uint8_t		ct_wrap[CTR_BLOCK_LEN];	/* single block at counter=0 */

	/* Plaintext: sequential bytes 0x00, 0x01, ... */
	for (size_t i = 0; i < pt_len; i++)
		pt[i] = (uint8_t)i;

	/* Step 1: 3-block CTR encrypt starting at UINT64_MAX - 1. */
	if (ctr_crypt(start_ctr, pt, ct, pt_len, B_TRUE, ctx) != 0)
		return;

	/* Step 2: Round-trip -- decrypt and compare. */
	if (ctr_crypt(start_ctr, ct, pt_dec, pt_len, B_FALSE, ctx) != 0)
		return;
	for (size_t i = 0; i < pt_len; i++) {
		if (pt_dec[i] != pt[i]) {
			KT_FAIL(ctx,
			    "round-trip mismatch at byte %zu: "
			    "got 0x%02x, expected 0x%02x",
			    i, pt_dec[i], pt[i]);
			return;
		}
	}

	/*
	 * Step 3: Cross-check the wrapped block.
	 * Encrypt pt[32..48) starting at counter = 0 in isolation.
	 * The result must equal ct[32..48).
	 */
	if (ctr_crypt(0, pt + 2 * CTR_BLOCK_LEN, ct_wrap,
	    CTR_BLOCK_LEN, B_TRUE, ctx) != 0)
		return;
	for (size_t i = 0; i < CTR_BLOCK_LEN; i++) {
		if (ct_wrap[i] != ct[2 * CTR_BLOCK_LEN + i]) {
			KT_FAIL(ctx,
			    "wrap-block mismatch at byte %zu: "
			    "got 0x%02x from single-block, "
			    "0x%02x from 3-block run",
			    i, ct_wrap[i],
			    ct[2 * CTR_BLOCK_LEN + i]);
			return;
		}
	}

	KT_PASS(ctx);
}

/*
 * Register the ctr_counter_wrap suite into an already-created ktest module.
 * Called from aes_gcm_ktest.c's _init().
 */
int
aes_ctr_ktest_register(ktest_module_hdl_t *km)
{
	ktest_suite_hdl_t *ks;
	int ret;

	if ((ret = ktest_add_suite(km, "ctr_counter_wrap", &ks)) != 0)
		return (ret);

	return (ktest_add_test(ks, "ctr_counter_wrap_test",
	    ctr_counter_wrap_test, KTEST_FLAG_NONE));
}
