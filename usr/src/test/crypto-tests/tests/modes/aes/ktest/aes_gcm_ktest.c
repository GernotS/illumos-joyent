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
 * aes_gcm_ktest -- kernel tests for AES-GCM multi-part decrypt.
 *
 * Suite gcm_streaming: CRYPTO_DATA_UIO scatter-gather decrypt.
 *   GCM decrypt streams plaintext during DecryptUpdate, buffering only the
 *   trailing bytes that could be the authentication tag.  Per-block plaintext
 *   is written via crypto_get_ptrs() which traverses the iovec array of a
 *   uio_t and can span segment boundaries.  These tests verify that scatter
 *   writes land correctly in each iovec segment.
 *
 * Suite gcm_update_bufsize: output-buffer accounting.
 *   Verifies that crypto_decrypt_update decrements cd_length by exactly the
 *   number of plaintext bytes released per call.  GCM holds back the last
 *   GCM_TAG_LEN bytes of accumulated ciphertext input, so the first update
 *   call (feeding <= TAG_LEN bytes) must produce zero output bytes.
 *
 * Tests drive the public kernel crypto API (crypto_decrypt_init /
 * _update / _final) with CRYPTO_DATA_UIO output, the same path used by
 * ZFS.  No private symbols are needed.
 *
 * Test triples:
 *   aes::gcm_streaming:<test>
 *   aes::gcm_update_bufsize:<test>
 */

#include <sys/types.h>
#include <sys/kmem.h>
#include <sys/sunddi.h>
#include <sys/uio.h>
#include <sys/crypto/api.h>
#include <sys/crypto/common.h>
#include <sys/ktest.h>
#include <sys/modctl.h>

#define	ARRAY_SIZE(x)	(sizeof (x) / sizeof (*((x))))

#define	AES_KEY_LEN	16
#define	AES_BLOCK_LEN	16
#define	GCM_IV_LEN	12
#define	GCM_TAG_LEN	16
#define	GCM_TAG_BITS	(GCM_TAG_LEN * 8)

/* Forward declaration for CTR suite registration (aes_ctr_ktest.c). */
extern int aes_ctr_ktest_register(ktest_module_hdl_t *);

static const uint8_t g_key[AES_KEY_LEN] = {
	0xfe, 0xff, 0xe9, 0x92, 0x86, 0x65, 0x73, 0x1c,
	0x6d, 0x6a, 0x8f, 0x94, 0x67, 0x30, 0x83, 0x08
};
static const uint8_t g_iv[GCM_IV_LEN] = {
	0xca, 0xfe, 0xba, 0xbe, 0xfa, 0xce, 0xdb, 0xad,
	0xde, 0xca, 0xf8, 0x88
};

/*
 * Build a crypto_mechanism_t for AES-128-GCM.  gcmp must be caller-allocated
 * and remain live for the duration of the operation.
 */
static int
gcm_mech_init(crypto_mechanism_t *mech, CK_AES_GCM_PARAMS *gcmp)
{
	mech->cm_type = crypto_mech2id("CKM_AES_GCM");
	if (mech->cm_type == CRYPTO_MECH_INVALID)
		return (-1);

	bzero(gcmp, sizeof (*gcmp));
	gcmp->pIv		= (uchar_t *)(uintptr_t)g_iv;
	gcmp->ulIvLen		= GCM_IV_LEN;
	gcmp->ulIvBits		= GCM_IV_LEN * 8;
	gcmp->pAAD		= NULL;
	gcmp->ulAADLen		= 0;
	gcmp->ulTagBits		= GCM_TAG_BITS;

	mech->cm_param		= (caddr_t)gcmp;
	mech->cm_param_len	= sizeof (*gcmp);
	return (0);
}

/*
 * Encrypt pt_len bytes of sequential plaintext (0x00, 0x01, 0x02, ...) with
 * AES-128-GCM.  Returns a kmem_alloc'd buffer of (pt_len + GCM_TAG_LEN)
 * bytes containing ciphertext || tag, or NULL on failure (ctx error set).
 * On success, *ct_lenp is set to pt_len + GCM_TAG_LEN.
 */
static uint8_t *
gcm_encrypt_raw(size_t pt_len, size_t *ct_lenp, ktest_ctx_hdl_t *ctx)
{
	uint8_t		*pt = NULL;
	uint8_t		*ct = NULL;
	size_t		ct_len = pt_len + GCM_TAG_LEN;
	crypto_mechanism_t mech;
	CK_AES_GCM_PARAMS gcmp;
	crypto_key_t	key;
	crypto_data_t	in, out;
	int		rv;

	pt = kmem_alloc(pt_len, KM_SLEEP);
	ct = kmem_alloc(ct_len, KM_SLEEP);

	for (size_t i = 0; i < pt_len; i++)
		pt[i] = (uint8_t)i;

	if (gcm_mech_init(&mech, &gcmp) != 0) {
		KT_ERROR(ctx, "CKM_AES_GCM not available");
		goto fail;
	}

	key.ck_format	= CRYPTO_KEY_RAW;
	key.ck_data	= (void *)(uintptr_t)g_key;
	key.ck_length	= AES_KEY_LEN * 8;

	bzero(&in, sizeof (in));
	in.cd_format		= CRYPTO_DATA_RAW;
	in.cd_offset		= 0;
	in.cd_length		= pt_len;
	in.cd_raw.iov_base	= (caddr_t)pt;
	in.cd_raw.iov_len	= pt_len;

	bzero(&out, sizeof (out));
	out.cd_format		= CRYPTO_DATA_RAW;
	out.cd_offset		= 0;
	out.cd_length		= ct_len;
	out.cd_raw.iov_base	= (caddr_t)ct;
	out.cd_raw.iov_len	= ct_len;

	rv = crypto_encrypt(&mech, &in, &key, NULL, &out, NULL);
	if (rv != CRYPTO_SUCCESS) {
		KT_ERROR(ctx, "crypto_encrypt failed: %d", rv);
		goto fail;
	}

	kmem_free(pt, pt_len);
	*ct_lenp = ct_len;
	return (ct);

fail:
	kmem_free(pt, pt_len);
	kmem_free(ct, ct_len);
	return (NULL);
}

/*
 * Core scatter-gather decrypt helper.
 *
 * Decrypts (ct, ct_len) via multi-part AES-GCM using a CRYPTO_DATA_UIO
 * output buffer split across nseg equally-sized iovec segments.  Ciphertext
 * is fed in nchunks chunks of sizes chunks[0..nchunks-1].  Verifies that
 * the assembled plaintext equals (0x00, 0x01, 0x02, ..., pt_len-1).
 *
 * Caller should prepend loop context via ktest_msg_prepend() before calling
 * if this helper is invoked from a loop.  ctx result is set on failure.
 * Returns B_TRUE on pass, B_FALSE if ctx result has already been set.
 */
static boolean_t
run_scatter(ktest_ctx_hdl_t *ctx,
    const uint8_t *ct, size_t ct_len, size_t pt_len,
    size_t nseg,
    const size_t *chunks, size_t nchunks)
{
	crypto_mechanism_t mech;
	CK_AES_GCM_PARAMS gcmp;
	crypto_key_t	key;
	crypto_context_t cctx = NULL;
	iovec_t		*iovs = NULL;
	uint8_t		**segs = NULL;
	uio_t		uio;
	crypto_data_t	out;
	uint8_t		*assembled = NULL;
	boolean_t	ok = B_FALSE;
	int		rv;
	size_t		seg_len = pt_len / nseg;
	size_t		last_seg_len = pt_len - seg_len * (nseg - 1);

	iovs = kmem_alloc(nseg * sizeof (iovec_t), KM_SLEEP);
	segs = kmem_zalloc(nseg * sizeof (uint8_t *), KM_SLEEP);
	assembled = kmem_alloc(pt_len, KM_SLEEP);

	for (size_t i = 0; i < nseg; i++) {
		size_t slen = (i == nseg - 1) ? last_seg_len : seg_len;
		segs[i] = kmem_zalloc(slen, KM_SLEEP);
		iovs[i].iov_base = (caddr_t)segs[i];
		iovs[i].iov_len  = slen;
	}

	bzero(&uio, sizeof (uio));
	uio.uio_iov    = iovs;
	uio.uio_iovcnt = (int)nseg;
	uio.uio_segflg = UIO_SYSSPACE;
	uio.uio_resid  = (ssize_t)pt_len;

	bzero(&out, sizeof (out));
	out.cd_format  = CRYPTO_DATA_UIO;
	out.cd_offset  = 0;
	out.cd_length  = pt_len;
	out.cd_uio     = &uio;

	if (gcm_mech_init(&mech, &gcmp) != 0) {
		KT_ERROR(ctx, "CKM_AES_GCM not available");
		goto cleanup;
	}

	key.ck_format  = CRYPTO_KEY_RAW;
	key.ck_data    = (void *)(uintptr_t)g_key;
	key.ck_length  = AES_KEY_LEN * 8;

	rv = crypto_decrypt_init(&mech, &key, NULL, &cctx, NULL);
	if (rv != CRYPTO_SUCCESS) {
		KT_ERROR(ctx, "crypto_decrypt_init failed: %d", rv);
		goto cleanup;
	}

	size_t off = 0;
	size_t pt_written = 0;
	for (size_t i = 0; i < nchunks; i++) {
		crypto_data_t in;

		bzero(&in, sizeof (in));
		in.cd_format		= CRYPTO_DATA_RAW;
		in.cd_offset		= 0;
		in.cd_length		= chunks[i];
		in.cd_raw.iov_base	= (caddr_t)(ct + off);
		in.cd_raw.iov_len	= chunks[i];

		/*
		 * crypto_decrypt_update() writes starting at out.cd_offset and,
		 * on return, sets out.cd_length to the number of plaintext bytes
		 * produced by this call while restoring out.cd_offset to its
		 * pre-call value.  The caller must advance the output position
		 * itself, so reset offset/length to the remaining region before
		 * every call.
		 */
		out.cd_offset = pt_written;
		out.cd_length = pt_len - pt_written;

		rv = crypto_decrypt_update(cctx, &in, &out, NULL);
		if (rv != CRYPTO_SUCCESS) {
			KT_ERROR(ctx, "chunk %zu: crypto_decrypt_update "
			    "failed: %d", i, rv);
			(void) crypto_cancel_ctx(cctx);
			cctx = NULL;
			goto cleanup;
		}
		pt_written += out.cd_length;
		off += chunks[i];
	}

	/*
	 * crypto_decrypt_final verifies the GCM tag and writes any remaining
	 * buffered plaintext.  It always consumes the context (success or
	 * failure), so no crypto_cancel_ctx() is needed afterward.
	 *
	 * final() also writes at out.cd_offset, so position it past the
	 * plaintext already emitted by the update calls.
	 */
	out.cd_offset = pt_written;
	out.cd_length = pt_len - pt_written;
	rv = crypto_decrypt_final(cctx, &out, NULL);
	cctx = NULL;
	if (rv != CRYPTO_SUCCESS) {
		KT_ERROR(ctx, "crypto_decrypt_final failed: %d "
		    "(tag mismatch?)", rv);
		goto cleanup;
	}

	/* Reassemble iovec segments and verify byte by byte. */
	size_t pos = 0;
	for (size_t i = 0; i < nseg; i++) {
		size_t slen = iovs[i].iov_len;
		bcopy(segs[i], assembled + pos, slen);
		pos += slen;
	}

	for (size_t i = 0; i < pt_len; i++) {
		if (assembled[i] != (uint8_t)i) {
			KT_FAIL(ctx,
			    "plaintext mismatch at byte %zu: "
			    "got 0x%02x, expected 0x%02x",
			    i, assembled[i], (uint8_t)i);
			goto cleanup;
		}
	}
	ok = B_TRUE;

cleanup:
	if (cctx != NULL)
		(void) crypto_cancel_ctx(cctx);
	for (size_t i = 0; i < nseg; i++) {
		size_t slen = (i == nseg - 1) ? last_seg_len : seg_len;
		if (segs[i] != NULL)
			kmem_free(segs[i], slen);
	}
	kmem_free(iovs, nseg * sizeof (iovec_t));
	kmem_free(segs, nseg * sizeof (uint8_t *));
	kmem_free(assembled, pt_len);
	return (ok);
}

/*
 * scatter_2seg_1chunk_test
 *
 * 2 iovec segments; all ciphertext delivered in a single DecryptUpdate call.
 * Exercises the common case where segment boundaries don't align with chunk
 * boundaries.  Two plaintext sizes are checked to cover both block-aligned
 * and non-block-aligned lengths.
 */
void
scatter_2seg_1chunk_test(ktest_ctx_hdl_t *ctx)
{
	const struct {
		size_t	pt_len;
	} cases[] = {
		{ 64  },	/* 4 full AES blocks, block-aligned */
		{ 237 },	/* non-block-aligned */
	};

	for (size_t ci = 0; ci < ARRAY_SIZE(cases); ci++) {
		size_t pt_len = cases[ci].pt_len;
		size_t ct_len;

		ktest_msg_prepend(ctx, "pt_len=%zu: ", pt_len);

		uint8_t *ct = gcm_encrypt_raw(pt_len, &ct_len, ctx);
		if (ct == NULL) {
			ktest_msg_clear(ctx);
			return;
		}

		size_t chunks[] = { ct_len };
		boolean_t ok = run_scatter(ctx, ct, ct_len, pt_len,
		    2, chunks, 1);
		kmem_free(ct, ct_len);
		ktest_msg_clear(ctx);

		if (!ok)
			return;
	}

	KT_PASS(ctx);
}

/*
 * scatter_4seg_4chunks_test
 *
 * 4 iovec segments; ciphertext in 4 equal-sized chunks.  Segment and chunk
 * boundaries are intentionally misaligned (chunk size = ct_len/4, segment
 * size = pt_len/4) to exercise cross-boundary scatter writes in every block.
 */
void
scatter_4seg_4chunks_test(ktest_ctx_hdl_t *ctx)
{
	const size_t pt_len = 128;	/* 8 full AES blocks */
	size_t ct_len;

	uint8_t *ct = gcm_encrypt_raw(pt_len, &ct_len, ctx);
	KT_EASSERT3P(ct, !=, NULL, ctx);

	/* ct_len = 128 + 16 = 144, divisible by 4 */
	if (ct_len % 4 != 0) {
		KT_ERROR(ctx, "ct_len %zu not divisible by 4", ct_len);
		kmem_free(ct, ct_len);
		return;
	}

	size_t qlen = ct_len / 4;
	size_t chunks[] = { qlen, qlen, qlen, qlen };
	boolean_t ok = run_scatter(ctx, ct, ct_len, pt_len,
	    4, chunks, 4);
	kmem_free(ct, ct_len);

	if (!ok)
		return;
	KT_PASS(ctx);
}

/*
 * scatter_3seg_byte_by_byte_test
 *
 * 3 iovec segments; ciphertext fed one byte per DecryptUpdate call.
 * Exercises the gcm_last_input ring buffer at every conceivable fill level
 * and triggers per-block UIO scatter writes at arbitrary offsets within
 * each iovec segment.
 */
void
scatter_3seg_byte_by_byte_test(ktest_ctx_hdl_t *ctx)
{
	const size_t pt_len = 64;	/* 4 AES blocks, manageable chunk count */
	size_t ct_len;

	uint8_t *ct = gcm_encrypt_raw(pt_len, &ct_len, ctx);
	KT_EASSERT3P(ct, !=, NULL, ctx);

	size_t *chunks = kmem_alloc(ct_len * sizeof (size_t), KM_SLEEP);
	for (size_t i = 0; i < ct_len; i++)
		chunks[i] = 1;

	boolean_t ok = run_scatter(ctx, ct, ct_len, pt_len,
	    3, chunks, ct_len);
	kmem_free(chunks, ct_len * sizeof (size_t));
	kmem_free(ct, ct_len);

	if (!ok)
		return;
	KT_PASS(ctx);
}

/*
 * scatter_nonaligned_3seg_test
 *
 * Non-block-aligned plaintext (237 bytes) across 3 iovec segments; three
 * unequal ciphertext chunks.  The partial final block exercises the path
 * in gcm_decrypt_final that writes a sub-block-sized output into a UIO
 * segment that may cross a segment boundary.
 */
void
scatter_nonaligned_3seg_test(ktest_ctx_hdl_t *ctx)
{
	const size_t pt_len = 237;
	size_t ct_len;

	uint8_t *ct = gcm_encrypt_raw(pt_len, &ct_len, ctx);
	KT_EASSERT3P(ct, !=, NULL, ctx);

	size_t c0 = ct_len / 3;
	size_t c1 = ct_len / 3;
	size_t c2 = ct_len - c0 - c1;
	size_t chunks[] = { c0, c1, c2 };

	boolean_t ok = run_scatter(ctx, ct, ct_len, pt_len,
	    3, chunks, 3);
	kmem_free(ct, ct_len);

	if (!ok)
		return;
	KT_PASS(ctx);
}

/*
 * ── Suite: gcm_update_uio ──────────────────────────────────────────────────
 *
 * Basic functional tests for CRYPTO_DATA_UIO output from multi-part GCM
 * decrypt.  These exercise the single-segment UIO path (one iovec) before
 * the scatter-gather complexity of gcm_streaming.  A failure here points to
 * a fundamental UIO output regression; a failure only in gcm_streaming
 * points to a segment-boundary bug.
 */

/*
 * uio_single_seg_one_chunk_test
 *
 * Decrypt a single ciphertext in one crypto_decrypt_update call into a
 * single-segment UIO.  The simplest possible UIO-output multi-part decrypt.
 */
void
uio_single_seg_one_chunk_test(ktest_ctx_hdl_t *ctx)
{
	const size_t	pt_len = 64;
	size_t		ct_len;
	uint8_t		*ct;

	ct = gcm_encrypt_raw(pt_len, &ct_len, ctx);
	KT_EASSERT3P(ct, !=, NULL, ctx);

	size_t chunks[] = { ct_len };
	boolean_t ok = run_scatter(ctx, ct, ct_len, pt_len, 1, chunks, 1);
	kmem_free(ct, ct_len);

	if (!ok)
		return;
	KT_PASS(ctx);
}

/*
 * uio_single_seg_multi_chunk_test
 *
 * Decrypt into a single-segment UIO, feeding ciphertext in four equal
 * chunks.  Verifies that repeated crypto_decrypt_update calls correctly
 * advance the UIO write position within a single iovec.
 */
void
uio_single_seg_multi_chunk_test(ktest_ctx_hdl_t *ctx)
{
	const struct {
		size_t pt_len;
		size_t nchunks;
	} cases[] = {
		{ 64,  4 },	/* 4 blocks, 4 equal chunks */
		{ 237, 4 },	/* non-block-aligned, 4 unequal chunks */
	};

	for (size_t ci = 0; ci < ARRAY_SIZE(cases); ci++) {
		size_t pt_len  = cases[ci].pt_len;
		size_t nchunks = cases[ci].nchunks;
		size_t ct_len;

		ktest_msg_prepend(ctx, "pt_len=%zu: ", pt_len);

		uint8_t *ct = gcm_encrypt_raw(pt_len, &ct_len, ctx);
		if (ct == NULL) {
			ktest_msg_clear(ctx);
			return;
		}

		/* Build chunk sizes: equal splits, last absorbs remainder. */
		size_t *chunks = kmem_alloc(nchunks * sizeof (size_t), KM_SLEEP);
		size_t base = ct_len / nchunks;
		for (size_t i = 0; i < nchunks - 1; i++)
			chunks[i] = base;
		chunks[nchunks - 1] = ct_len - base * (nchunks - 1);

		boolean_t ok = run_scatter(ctx, ct, ct_len, pt_len,
		    1, chunks, nchunks);
		kmem_free(chunks, nchunks * sizeof (size_t));
		kmem_free(ct, ct_len);
		ktest_msg_clear(ctx);

		if (!ok)
			return;
	}

	KT_PASS(ctx);
}

/*
 * ── Suite: gcm_update_bufsize ─────────────────────────────────────────────
 *
 * gcm_update_bufsize_test
 *
 * Verifies that crypto_decrypt_update accounts cd_length correctly when
 * using CRYPTO_DATA_UIO output.  GCM holds back the last GCM_TAG_LEN bytes
 * of accumulated ciphertext so that the final GCM_TAG_LEN bytes fed via
 * update (the actual authentication tag) never appear as plaintext output.
 *
 * Feed schedule: 3 AES blocks of plaintext → 64-byte ciphertext (48 + tag),
 * delivered in 4 chunks of 16 bytes each.
 *
 *   chunk 0 (ct[0:16]):   buffer fills,  0 bytes of pt out, cd_length stays 48
 *   chunk 1 (ct[16:32]):  16 bytes out,  cd_length = 32
 *   chunk 2 (ct[32:48]):  16 bytes out,  cd_length = 16
 *   chunk 3 (ct[48:64]):  16 bytes out,  cd_length = 0   (tag is now buffered)
 *   final:                0 bytes out,   tag verified
 */
void
gcm_update_bufsize_test(ktest_ctx_hdl_t *ctx)
{
	const size_t	pt_len  = 3 * AES_BLOCK_LEN;		/* 48 bytes */
	const size_t	ct_len  = pt_len + GCM_TAG_LEN;	/* 64 bytes */
	const size_t	chunk   = GCM_TAG_LEN;			/* 16 bytes / chunk */
	const size_t	nchunks = ct_len / chunk;		/* 4 */
	const size_t	expected_out[] = { 0, AES_BLOCK_LEN, AES_BLOCK_LEN,
	    AES_BLOCK_LEN };

	crypto_mechanism_t	mech;
	CK_AES_GCM_PARAMS	gcmp;
	crypto_key_t		key;
	crypto_context_t	cctx = NULL;
	uint8_t			*ct = NULL;
	uint8_t			*buf = NULL;
	iovec_t			iov;
	uio_t			uio;
	crypto_data_t		out;
	size_t			ct_actual;
	boolean_t		ok = B_FALSE;
	int			rv;

	ASSERT3U(ct_len % chunk, ==, 0);
	ASSERT3U(ARRAY_SIZE(expected_out), ==, nchunks);

	ct = gcm_encrypt_raw(pt_len, &ct_actual, ctx);
	if (ct == NULL)
		return;
	if (ct_actual != ct_len) {
		KT_ERROR(ctx, "unexpected ct_len %zu (expected %zu)",
		    ct_actual, ct_len);
		goto cleanup;
	}

	buf = kmem_zalloc(pt_len, KM_SLEEP);

	iov.iov_base = (caddr_t)buf;
	iov.iov_len  = pt_len;

	bzero(&uio, sizeof (uio));
	uio.uio_iov    = &iov;
	uio.uio_iovcnt = 1;
	uio.uio_segflg = UIO_SYSSPACE;
	uio.uio_resid  = (ssize_t)pt_len;

	bzero(&out, sizeof (out));
	out.cd_format = CRYPTO_DATA_UIO;
	out.cd_offset = 0;
	out.cd_length = pt_len;
	out.cd_uio    = &uio;

	if (gcm_mech_init(&mech, &gcmp) != 0) {
		KT_ERROR(ctx, "CKM_AES_GCM not available");
		goto cleanup;
	}

	key.ck_format = CRYPTO_KEY_RAW;
	key.ck_data   = (void *)(uintptr_t)g_key;
	key.ck_length = AES_KEY_LEN * 8;

	rv = crypto_decrypt_init(&mech, &key, NULL, &cctx, NULL);
	if (rv != CRYPTO_SUCCESS) {
		KT_ERROR(ctx, "crypto_decrypt_init failed: %d", rv);
		goto cleanup;
	}

	size_t pt_written = 0;
	for (size_t i = 0; i < nchunks; i++) {
		crypto_data_t	in;
		size_t		written;

		bzero(&in, sizeof (in));
		in.cd_format       = CRYPTO_DATA_RAW;
		in.cd_offset       = 0;
		in.cd_length       = chunk;
		in.cd_raw.iov_base = (caddr_t)(ct + i * chunk);
		in.cd_raw.iov_len  = chunk;

		ktest_msg_prepend(ctx, "chunk %zu: ", i);

		/*
		 * update() writes at out.cd_offset and returns the bytes it
		 * produced in out.cd_length (restoring out.cd_offset), so the
		 * caller advances the running output offset between calls.
		 */
		out.cd_offset = pt_written;
		out.cd_length = pt_len - pt_written;

		rv = crypto_decrypt_update(cctx, &in, &out, NULL);
		if (rv != CRYPTO_SUCCESS) {
			KT_ERROR(ctx, "crypto_decrypt_update failed: %d", rv);
			(void) crypto_cancel_ctx(cctx);
			cctx = NULL;
			ktest_msg_clear(ctx);
			goto cleanup;
		}

		written = out.cd_length;
		if (written != expected_out[i]) {
			KT_FAIL(ctx,
			    "update wrote %zu bytes, expected %zu",
			    written, expected_out[i]);
			(void) crypto_cancel_ctx(cctx);
			cctx = NULL;
			ktest_msg_clear(ctx);
			goto cleanup;
		}
		pt_written += written;
		ktest_msg_clear(ctx);
	}

	out.cd_offset = pt_written;
	out.cd_length = pt_len - pt_written;
	rv = crypto_decrypt_final(cctx, &out, NULL);
	cctx = NULL;
	if (rv != CRYPTO_SUCCESS) {
		KT_ERROR(ctx, "crypto_decrypt_final failed: %d", rv);
		goto cleanup;
	}

	if (out.cd_length != 0) {
		KT_FAIL(ctx, "unexpected remaining cd_length: %zu",
		    out.cd_length);
		goto cleanup;
	}

	for (size_t i = 0; i < pt_len; i++) {
		if (buf[i] != (uint8_t)i) {
			KT_FAIL(ctx,
			    "plaintext mismatch at byte %zu: "
			    "got 0x%02x, expected 0x%02x",
			    i, buf[i], (uint8_t)i);
			goto cleanup;
		}
	}

	ok = B_TRUE;

cleanup:
	if (cctx != NULL)
		(void) crypto_cancel_ctx(cctx);
	if (buf != NULL)
		kmem_free(buf, pt_len);
	if (ct != NULL)
		kmem_free(ct, ct_len);
	if (ok)
		KT_PASS(ctx);
}

static struct modlmisc aes_ktest_modlmisc = {
	.misc_modops   = &mod_miscops,
	.misc_linkinfo = "aes ktest module"
};

static struct modlinkage aes_ktest_modlinkage = {
	.ml_rev     = MODREV_1,
	.ml_linkage = { &aes_ktest_modlmisc, NULL }
};

int
_init(void)
{
	int ret;
	ktest_module_hdl_t *km = NULL;
	ktest_suite_hdl_t  *ks = NULL;

	VERIFY0(ktest_create_module("aes", &km));

	VERIFY0(ktest_add_suite(km, "gcm_streaming", &ks));
	VERIFY0(ktest_add_test(ks, "scatter_2seg_1chunk_test",
	    scatter_2seg_1chunk_test, KTEST_FLAG_NONE));
	VERIFY0(ktest_add_test(ks, "scatter_4seg_4chunks_test",
	    scatter_4seg_4chunks_test, KTEST_FLAG_NONE));
	VERIFY0(ktest_add_test(ks, "scatter_3seg_byte_by_byte_test",
	    scatter_3seg_byte_by_byte_test, KTEST_FLAG_NONE));
	VERIFY0(ktest_add_test(ks, "scatter_nonaligned_3seg_test",
	    scatter_nonaligned_3seg_test, KTEST_FLAG_NONE));

	VERIFY0(ktest_add_suite(km, "gcm_update_uio", &ks));
	VERIFY0(ktest_add_test(ks, "uio_single_seg_one_chunk_test",
	    uio_single_seg_one_chunk_test, KTEST_FLAG_NONE));
	VERIFY0(ktest_add_test(ks, "uio_single_seg_multi_chunk_test",
	    uio_single_seg_multi_chunk_test, KTEST_FLAG_NONE));

	VERIFY0(ktest_add_suite(km, "gcm_update_bufsize", &ks));
	VERIFY0(ktest_add_test(ks, "gcm_update_bufsize_test",
	    gcm_update_bufsize_test, KTEST_FLAG_NONE));

	if ((ret = aes_ctr_ktest_register(km)) != 0) {
		ktest_free_module(km);
		return (ret);
	}

	if ((ret = ktest_register_module(km)) != 0) {
		ktest_free_module(km);
		return (ret);
	}

	if ((ret = mod_install(&aes_ktest_modlinkage)) != 0) {
		ktest_unregister_module("aes");
		return (ret);
	}

	return (0);
}

int
_fini(void)
{
	ktest_unregister_module("aes");
	return (mod_remove(&aes_ktest_modlinkage));
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&aes_ktest_modlinkage, modinfop));
}
