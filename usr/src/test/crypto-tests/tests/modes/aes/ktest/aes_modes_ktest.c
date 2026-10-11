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
 * aes_modes_ktest -- kernel tests for the KCF modes fastpaths.
 *
 * Suite mac_layout: CMAC and GMAC over the input layouts SMB signing
 *   produces -- a 64-byte header followed by uio segments of arbitrary
 *   length, aligned and unaligned.  Each tag is computed one-shot over a
 *   contiguous buffer, one-shot over a CRYPTO_DATA_UIO input (the
 *   provider is then fed one call per iovec) and multi-part with one
 *   update per segment, each both with the modes fastpaths enabled and
 *   disabled; all must agree.  CMAC is additionally checked against the
 *   RFC 4493 test vectors.
 *
 * Suite output_segments: multi-part ECB, CBC, CTR and GCM encryption into
 *   a CRYPTO_DATA_UIO output of several iovecs, where later updates
 *   start at a cd_offset beyond the first iovec.  The fastpaths resolve
 *   cd_offset to the current iovec, so these updates are expected to
 *   take the fastpath; the result must match a one-shot encryption into
 *   a contiguous buffer.  Also unit-tests crypto_data_contig() directly.
 *
 * Suite ctr_output: CTR on its per-block path, where one call's output
 *   may span several iovecs (including empty ones), and multi-part CTR
 *   whose output starts at a nonzero cd_offset.
 *
 * Test triples:
 *   aes::mac_layout:<test>
 *   aes::output_segments:<test>
 *   aes::ctr_output:<test>
 */

#include <sys/types.h>
#include <sys/kmem.h>
#include <sys/sysmacros.h>
#include <sys/sunddi.h>
#include <sys/uio.h>
#include <sys/stream.h>
#include <sys/crypto/api.h>
#include <sys/crypto/common.h>
#include <sys/ktest.h>

#define	MK_KEY_LEN	16
#define	MK_BLOCK_LEN	16
#define	MK_MAC_LEN	16
#define	MK_GCM_IV_LEN	12
#define	MK_MAX_SEGS	8

/* Fastpath switches and the segment helper, from misc/kcf (modes.c). */
extern boolean_t ecb_fastpath_enabled;
extern boolean_t cbc_fastpath_enabled;
extern boolean_t ctr_fastpath_enabled;
extern boolean_t gcm_fastpath_enabled;
extern uint8_t *crypto_data_contig(crypto_data_t *, size_t *);

extern int aes_modes_ktest_register(ktest_module_hdl_t *);

/* RFC 4493 key and message. */
static const uint8_t mk_key[MK_KEY_LEN] = {
	0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
	0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c
};
static const uint8_t rfc4493_msg[64] = {
	0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96,
	0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a,
	0xae, 0x2d, 0x8a, 0x57, 0x1e, 0x03, 0xac, 0x9c,
	0x9e, 0xb7, 0x6f, 0xac, 0x45, 0xaf, 0x8e, 0x51,
	0x30, 0xc8, 0x1c, 0x46, 0xa3, 0x5c, 0xe4, 0x11,
	0xe5, 0xfb, 0xc1, 0x19, 0x1a, 0x0a, 0x52, 0xef,
	0xf6, 0x9f, 0x24, 0x45, 0xdf, 0x4f, 0x9b, 0x17,
	0xad, 0x2b, 0x41, 0x7b, 0xe6, 0x6c, 0x37, 0x10
};
static const struct {
	size_t		len;
	uint8_t		tag[MK_MAC_LEN];
} rfc4493_vec[] = {
	{ 0, { 0xbb, 0x1d, 0x69, 0x29, 0xe9, 0x59, 0x37, 0x28,
	    0x7f, 0xa3, 0x7d, 0x12, 0x9b, 0x75, 0x67, 0x46 } },
	{ 16, { 0x07, 0x0a, 0x16, 0xb4, 0x6b, 0x4d, 0x41, 0x44,
	    0xf7, 0x9b, 0xdd, 0x9d, 0xd0, 0x4a, 0x28, 0x7c } },
	{ 40, { 0xdf, 0xa6, 0x67, 0x47, 0xde, 0x9a, 0xe6, 0x30,
	    0x30, 0xca, 0x32, 0x61, 0x14, 0x97, 0xc8, 0x27 } },
	{ 64, { 0x51, 0xf0, 0xbe, 0xbf, 0x7e, 0x3b, 0x9d, 0x92,
	    0xfc, 0x49, 0x74, 0x17, 0x79, 0x36, 0x3c, 0xfe } },
};

static const uint8_t mk_iv[MK_BLOCK_LEN] = {
	0xca, 0xfe, 0xba, 0xbe, 0xfa, 0xce, 0xdb, 0xad,
	0xde, 0xca, 0xf8, 0x88, 0x00, 0x00, 0x00, 0x00
};

/*
 * SMB-signing-like input layouts: a 64-byte SMB2 header, then data
 * segments of assorted lengths.  A zero terminates each layout.
 */
static const size_t mac_layouts[][MK_MAX_SEGS] = {
	{ 64, 0 },
	{ 64, 4096, 0 },
	{ 64, 1000, 0 },
	{ 64, 4096, 4096, 0 },
	{ 64, 7, 4096, 333, 0 },
	{ 64, 1, 15, 17, 8192, 0 },
	{ 64, 4095, 4097, 16, 0 },
	{ 64, 48, 65536, 0 },
	{ 3, 61, 128, 1, 0 },
};

typedef enum { MK_CMAC, MK_GMAC } mk_mac_t;

static void
mk_key_init(crypto_key_t *key)
{
	key->ck_format = CRYPTO_KEY_RAW;
	key->ck_data = (void *)(uintptr_t)mk_key;
	key->ck_length = MK_KEY_LEN * 8;
}

static void
mk_raw(crypto_data_t *cd, void *buf, size_t len)
{
	bzero(cd, sizeof (*cd));
	cd->cd_format = CRYPTO_DATA_RAW;
	cd->cd_length = len;
	cd->cd_raw.iov_base = buf;
	cd->cd_raw.iov_len = len;
}

static int
mk_mac_mech(mk_mac_t which, crypto_mechanism_t *mech,
    CK_AES_GMAC_PARAMS *gp)
{
	if (which == MK_CMAC) {
		mech->cm_type = crypto_mech2id("CKM_AES_CMAC");
		mech->cm_param = NULL;
		mech->cm_param_len = 0;
	} else {
		mech->cm_type = crypto_mech2id("CKM_AES_GMAC");
		bzero(gp, sizeof (*gp));
		gp->pIv = (uchar_t *)(uintptr_t)mk_iv;
		mech->cm_param = (caddr_t)gp;
		mech->cm_param_len = sizeof (*gp);
	}
	return (mech->cm_type == CRYPTO_MECH_INVALID ? -1 : 0);
}

/*
 * Compute a MAC over `msg', laid out as the segments in `segs' (a zero
 * terminated list), in one of three ways:
 *   0: one-shot, contiguous RAW input
 *   1: one-shot, UIO input with one iovec per segment
 *   2: multi-part, one RAW update per segment
 */
static int
mk_mac(mk_mac_t which, int how, const uint8_t *msg, const size_t *segs,
    uint8_t *tag)
{
	crypto_mechanism_t mech;
	CK_AES_GMAC_PARAMS gp;
	crypto_key_t key;
	crypto_data_t in, mac;
	crypto_context_t cctx;
	iovec_t iov[MK_MAX_SEGS];
	uio_t uio;
	size_t total = 0, n = 0;
	int rv;

	while (n < MK_MAX_SEGS && segs[n] != 0)
		total += segs[n++];

	if (mk_mac_mech(which, &mech, &gp) != 0)
		return (CRYPTO_MECHANISM_INVALID);
	mk_key_init(&key);
	mk_raw(&mac, tag, MK_MAC_LEN);

	switch (how) {
	case 0:
		mk_raw(&in, (void *)msg, total);
		return (crypto_mac(&mech, &in, &key, NULL, &mac, NULL));

	case 1:
		for (size_t i = 0, off = 0; i < n; off += segs[i++]) {
			iov[i].iov_base = (caddr_t)msg + off;
			iov[i].iov_len = segs[i];
		}
		bzero(&uio, sizeof (uio));
		uio.uio_iov = iov;
		uio.uio_iovcnt = (int)n;
		uio.uio_segflg = UIO_SYSSPACE;
		uio.uio_resid = (ssize_t)total;
		bzero(&in, sizeof (in));
		in.cd_format = CRYPTO_DATA_UIO;
		in.cd_length = total;
		in.cd_uio = &uio;
		return (crypto_mac(&mech, &in, &key, NULL, &mac, NULL));

	default:
		if ((rv = crypto_mac_init(&mech, &key, NULL, &cctx, NULL)) !=
		    CRYPTO_SUCCESS)
			return (rv);
		for (size_t i = 0, off = 0; i < n; off += segs[i++]) {
			mk_raw(&in, (void *)(msg + off), segs[i]);
			if ((rv = crypto_mac_update(cctx, &in, NULL)) !=
			    CRYPTO_SUCCESS) {
				crypto_cancel_ctx(cctx);
				return (rv);
			}
		}
		return (crypto_mac_final(cctx, &mac, NULL));
	}
}

static void
mk_fill(uint8_t *buf, size_t len, uint32_t seed)
{
	for (size_t i = 0; i < len; i++) {
		seed = seed * 1103515245 + 12345;
		buf[i] = (uint8_t)(seed >> 16);
	}
}

static void
mac_layout_common(ktest_ctx_hdl_t *ctx, mk_mac_t which)
{
	boolean_t *fast = (which == MK_CMAC) ? &cbc_fastpath_enabled :
	    &gcm_fastpath_enabled;
	boolean_t saved = *fast;
	const char *name = (which == MK_CMAC) ? "CMAC" : "GMAC";
	uint8_t ref[MK_MAC_LEN], tag[MK_MAC_LEN];
	size_t maxlen = 0;
	uint8_t *msg;

	for (size_t l = 0; l < ARRAY_SIZE(mac_layouts); l++) {
		size_t t = 0;
		for (size_t i = 0; i < MK_MAX_SEGS && mac_layouts[l][i]; i++)
			t += mac_layouts[l][i];
		maxlen = MAX(maxlen, t);
	}
	msg = kmem_alloc(maxlen, KM_SLEEP);
	mk_fill(msg, maxlen, 0x5eed);

	for (size_t l = 0; l < ARRAY_SIZE(mac_layouts); l++) {
		/* Reference: contiguous one-shot, per-block path. */
		*fast = B_FALSE;
		int rv = mk_mac(which, 0, msg, mac_layouts[l], ref);
		*fast = saved;
		if (rv != CRYPTO_SUCCESS) {
			KT_ERROR(ctx, "%s layout %zu: reference failed: 0x%x",
			    name, l, rv);
			goto out;
		}

		for (int f = 0; f < 2; f++) {
			for (int how = 0; how < 3; how++) {
				*fast = (f != 0);
				rv = mk_mac(which, how, msg, mac_layouts[l],
				    tag);
				*fast = saved;
				if (rv != CRYPTO_SUCCESS) {
					KT_ERROR(ctx, "%s layout %zu fast=%d "
					    "how=%d: failed: 0x%x", name, l,
					    f, how, rv);
					goto out;
				}
				if (bcmp(tag, ref, MK_MAC_LEN) != 0) {
					KT_FAIL(ctx, "%s layout %zu fast=%d "
					    "how=%d: tag mismatch", name, l,
					    f, how);
					goto out;
				}
			}
		}
	}
	KT_PASS(ctx);
out:
	*fast = saved;
	kmem_free(msg, maxlen);
}

/*
 * CMAC over SMB-like layouts: fastpath and per-block path, contiguous,
 * uio and multi-part input must all give the same tag.
 */
static void
cmac_layout_test(ktest_ctx_hdl_t *ctx)
{
	mac_layout_common(ctx, MK_CMAC);
}

/* As above, for GMAC (SMB 3.1.1 AES-GMAC signing). */
static void
gmac_layout_test(ktest_ctx_hdl_t *ctx)
{
	mac_layout_common(ctx, MK_GMAC);
}

/* RFC 4493 known answers, every split of the message into two parts. */
static void
cmac_rfc4493_test(ktest_ctx_hdl_t *ctx)
{
	boolean_t saved = cbc_fastpath_enabled;
	uint8_t tag[MK_MAC_LEN];

	for (size_t v = 0; v < ARRAY_SIZE(rfc4493_vec); v++) {
		size_t len = rfc4493_vec[v].len;

		for (size_t cut = 0; cut <= len; cut++) {
			size_t segs[3] = { 0, 0, 0 };
			size_t n = 0;
			int rv;

			if (cut != 0)
				segs[n++] = cut;
			if (len - cut != 0)
				segs[n++] = len - cut;
			for (int f = 0; f < 2; f++) {
				cbc_fastpath_enabled = (f != 0);
				rv = mk_mac(MK_CMAC, n == 0 ? 0 : 2,
				    rfc4493_msg, segs, tag);
				cbc_fastpath_enabled = saved;
				if (rv != CRYPTO_SUCCESS) {
					KT_ERROR(ctx, "len %zu cut %zu: 0x%x",
					    len, cut, rv);
					return;
				}
				if (bcmp(tag, rfc4493_vec[v].tag,
				    MK_MAC_LEN) != 0) {
					KT_FAIL(ctx, "len %zu cut %zu fast=%d: "
					    "tag mismatch", len, cut, f);
					return;
				}
			}
		}
	}
	KT_PASS(ctx);
}

/*
 * Output-segment layouts.  The input is split the same way, so each
 * provider call writes exactly one output segment and every call after
 * the first starts at a cd_offset at the start of a later iovec.  GCM
 * writes its tag into one more 16-byte iovec, like the ZFS cipher uio.
 */
static const size_t seg_layouts[][MK_MAX_SEGS] = {
	{ 4096, 4096, 0 },
	{ 1024, 3072, 4096, 0 },
	{ 16, 4080, 4096, 0 },
	{ 16, 16, 16, 8144, 0 },
};

typedef enum { MK_ECB, MK_CBC, MK_CTR, MK_GCM, MK_NCIPHERS } mk_cipher_t;
static const char *mk_cipher_name[] = { "ECB", "CBC", "CTR", "GCM" };

static int
mk_cipher_mech(mk_cipher_t c, crypto_mechanism_t *mech,
    CK_AES_CTR_PARAMS *ctrp, CK_AES_GCM_PARAMS *gcmp)
{
	mech->cm_param = NULL;
	mech->cm_param_len = 0;
	switch (c) {
	case MK_ECB:
		mech->cm_type = crypto_mech2id("CKM_AES_ECB");
		break;
	case MK_CBC:
		mech->cm_type = crypto_mech2id("CKM_AES_CBC");
		mech->cm_param = (caddr_t)mk_iv;
		mech->cm_param_len = MK_BLOCK_LEN;
		break;
	case MK_CTR:
		mech->cm_type = crypto_mech2id("CKM_AES_CTR");
		bzero(ctrp, sizeof (*ctrp));
		ctrp->ulCounterBits = 64;
		bcopy(mk_iv, ctrp->cb, MK_BLOCK_LEN);
		mech->cm_param = (caddr_t)ctrp;
		mech->cm_param_len = sizeof (*ctrp);
		break;
	default:
		mech->cm_type = crypto_mech2id("CKM_AES_GCM");
		bzero(gcmp, sizeof (*gcmp));
		gcmp->pIv = (uchar_t *)(uintptr_t)mk_iv;
		gcmp->ulIvLen = MK_GCM_IV_LEN;
		gcmp->ulIvBits = MK_GCM_IV_LEN * 8;
		gcmp->ulTagBits = MK_MAC_LEN * 8;
		mech->cm_param = (caddr_t)gcmp;
		mech->cm_param_len = sizeof (*gcmp);
		break;
	}
	return (mech->cm_type == CRYPTO_MECH_INVALID ? -1 : 0);
}

/*
 * Encrypt `len' bytes of `pt' into a UIO laid out as `segs' (plus a
 * 16-byte tag iovec for GCM) and compare with the one-shot contiguous
 * encryption in `ref'.  With `atomic', this is a single crypto_encrypt()
 * whose input is a UIO segmented like the output, which the provider
 * processes one input iovec at a time with cd_offset accumulating across
 * the calls (the ZFS and SMB3 shape).  Otherwise it is multi-part, one
 * crypto_encrypt_update() per segment.
 */
static boolean_t
mk_seg_one(ktest_ctx_hdl_t *ctx, mk_cipher_t c, const size_t *segs,
    const uint8_t *pt, size_t len, const uint8_t *ref, size_t ref_len,
    boolean_t atomic)
{
	crypto_mechanism_t mech;
	CK_AES_CTR_PARAMS ctrp;
	CK_AES_GCM_PARAMS gcmp;
	crypto_key_t key;
	crypto_context_t cctx;
	crypto_data_t in, out;
	iovec_t iov[MK_MAX_SEGS + 1], iniov[MK_MAX_SEGS];
	uint8_t *seg[MK_MAX_SEGS + 1];
	uio_t uio, inuio;
	size_t n = 0, written = 0, off = 0;
	boolean_t ok = B_FALSE;
	int rv;

	while (n < MK_MAX_SEGS && segs[n] != 0) {
		seg[n] = kmem_zalloc(segs[n], KM_SLEEP);
		iov[n].iov_base = (caddr_t)seg[n];
		iov[n].iov_len = segs[n];
		n++;
	}
	if (c == MK_GCM) {
		seg[n] = kmem_zalloc(MK_MAC_LEN, KM_SLEEP);
		iov[n].iov_base = (caddr_t)seg[n];
		iov[n].iov_len = MK_MAC_LEN;
		n++;
	}
	bzero(&uio, sizeof (uio));
	uio.uio_iov = iov;
	uio.uio_iovcnt = (int)n;
	uio.uio_segflg = UIO_SYSSPACE;
	uio.uio_resid = (ssize_t)ref_len;
	bzero(&out, sizeof (out));
	out.cd_format = CRYPTO_DATA_UIO;
	out.cd_uio = &uio;

	(void) mk_cipher_mech(c, &mech, &ctrp, &gcmp);
	mk_key_init(&key);

	if (atomic) {
		size_t nin = 0;

		for (; nin < MK_MAX_SEGS && segs[nin] != 0; nin++) {
			iniov[nin].iov_base = (caddr_t)pt + off;
			iniov[nin].iov_len = segs[nin];
			off += segs[nin];
		}
		bzero(&inuio, sizeof (inuio));
		inuio.uio_iov = iniov;
		inuio.uio_iovcnt = (int)nin;
		inuio.uio_segflg = UIO_SYSSPACE;
		inuio.uio_resid = (ssize_t)len;
		bzero(&in, sizeof (in));
		in.cd_format = CRYPTO_DATA_UIO;
		in.cd_length = len;
		in.cd_uio = &inuio;
		out.cd_offset = 0;
		out.cd_length = ref_len;
		if ((rv = crypto_encrypt(&mech, &in, &key, NULL, &out,
		    NULL)) != CRYPTO_SUCCESS) {
			KT_ERROR(ctx, "%s: crypto_encrypt: 0x%x",
			    mk_cipher_name[c], rv);
			goto out;
		}
		written = out.cd_length;
		goto check;
	}

	if ((rv = crypto_encrypt_init(&mech, &key, NULL, &cctx, NULL)) !=
	    CRYPTO_SUCCESS) {
		KT_ERROR(ctx, "%s: crypto_encrypt_init: 0x%x",
		    mk_cipher_name[c], rv);
		goto out;
	}
	for (size_t i = 0; i < MK_MAX_SEGS && segs[i] != 0; i++) {
		mk_raw(&in, (void *)(pt + off), segs[i]);
		out.cd_offset = written;
		out.cd_length = ref_len - written;
		if ((rv = crypto_encrypt_update(cctx, &in, &out, NULL)) !=
		    CRYPTO_SUCCESS) {
			KT_ERROR(ctx, "%s: update %zu: 0x%x",
			    mk_cipher_name[c], i, rv);
			crypto_cancel_ctx(cctx);
			goto out;
		}
		written += out.cd_length;
		off += segs[i];
	}
	out.cd_offset = written;
	out.cd_length = ref_len - written;
	if ((rv = crypto_encrypt_final(cctx, &out, NULL)) != CRYPTO_SUCCESS) {
		KT_ERROR(ctx, "%s: final: 0x%x", mk_cipher_name[c], rv);
		goto out;
	}
	written += out.cd_length;
check:
	if (written != ref_len || off != len) {
		KT_FAIL(ctx, "%s: wrote %zu bytes, expected %zu",
		    mk_cipher_name[c], written, ref_len);
		goto out;
	}
	off = 0;
	for (size_t i = 0; i < n; off += iov[i++].iov_len) {
		if (bcmp(seg[i], ref + off, iov[i].iov_len) != 0) {
			KT_FAIL(ctx, "%s: mismatch in iovec %zu",
			    mk_cipher_name[c], i);
			goto out;
		}
	}
	ok = B_TRUE;
out:
	for (size_t i = 0; i < n; i++)
		kmem_free(seg[i], iov[i].iov_len);
	return (ok);
}

static void
uio_continuation_test(ktest_ctx_hdl_t *ctx)
{
	const size_t len = 8192;
	uint8_t *pt = kmem_alloc(len, KM_SLEEP);
	uint8_t *ref = kmem_alloc(len + MK_MAC_LEN, KM_SLEEP);
	boolean_t sv[4] = { ecb_fastpath_enabled, cbc_fastpath_enabled,
	    ctr_fastpath_enabled, gcm_fastpath_enabled };

	mk_fill(pt, len, 0xc0ffee);

	for (mk_cipher_t c = MK_ECB; c < MK_NCIPHERS; c++) {
		crypto_mechanism_t mech;
		CK_AES_CTR_PARAMS ctrp;
		CK_AES_GCM_PARAMS gcmp;
		crypto_key_t key;
		crypto_data_t in, out;
		size_t ref_len = len + (c == MK_GCM ? MK_MAC_LEN : 0);
		int rv;

		if (mk_cipher_mech(c, &mech, &ctrp, &gcmp) != 0) {
			KT_ERROR(ctx, "%s not available", mk_cipher_name[c]);
			goto out;
		}
		mk_key_init(&key);
		mk_raw(&in, pt, len);
		mk_raw(&out, ref, ref_len);
		if ((rv = crypto_encrypt(&mech, &in, &key, NULL, &out,
		    NULL)) != CRYPTO_SUCCESS) {
			KT_ERROR(ctx, "%s: crypto_encrypt: 0x%x",
			    mk_cipher_name[c], rv);
			goto out;
		}

		for (size_t l = 0; l < ARRAY_SIZE(seg_layouts); l++) {
			for (int f = 0; f < 4; f++) {
				boolean_t atomic = (f & 2) != 0;
				boolean_t ok;

				ecb_fastpath_enabled = cbc_fastpath_enabled =
				    ctr_fastpath_enabled =
				    gcm_fastpath_enabled = (f & 1) != 0;
				ok = mk_seg_one(ctx, c, seg_layouts[l], pt, len,
				    ref, ref_len, atomic);
				ecb_fastpath_enabled = sv[0];
				cbc_fastpath_enabled = sv[1];
				ctr_fastpath_enabled = sv[2];
				gcm_fastpath_enabled = sv[3];
				if (!ok) {
					ktest_msg_prepend(ctx,
					    "layout %zu fast=%d atomic=%d: ", l,
					    f & 1, atomic);
					goto out;
				}
			}
		}
	}
	KT_PASS(ctx);
out:
	kmem_free(pt, len);
	kmem_free(ref, len + MK_MAC_LEN);
}

/* crypto_data_contig(): resolve cd_offset to the current segment. */
static void
contig_helper_test(ktest_ctx_hdl_t *ctx)
{
	static const size_t lens[] = { 32, 0, 48, 16 };
	static const struct {
		off_t	off;
		int	seg;		/* -1: none */
		size_t	segoff;
		size_t	avail;
	} cases[] = {
		{ 0, 0, 0, 32 }, { 31, 0, 31, 1 }, { 32, 2, 0, 48 },
		{ 79, 2, 47, 1 }, { 80, 3, 0, 16 }, { 95, 3, 15, 1 },
		{ 96, -1, 0, 0 }, { 4096, -1, 0, 0 }, { -1, -1, 0, 0 },
	};
	uint8_t buf[96 + 1];
	uint8_t *base[ARRAY_SIZE(lens)];
	iovec_t iov[ARRAY_SIZE(lens)];
	mblk_t mb[ARRAY_SIZE(lens)];
	uio_t uio;
	crypto_data_t cd;
	size_t pos = 0, avail;
	uint8_t *p;

	bzero(mb, sizeof (mb));
	for (size_t i = 0; i < ARRAY_SIZE(lens); i++) {
		base[i] = &buf[pos];
		iov[i].iov_base = (caddr_t)base[i];
		iov[i].iov_len = lens[i];
		mb[i].b_rptr = base[i];
		mb[i].b_wptr = base[i] + lens[i];
		mb[i].b_cont = (i + 1 < ARRAY_SIZE(lens)) ? &mb[i + 1] : NULL;
		pos += lens[i];
	}
	bzero(&uio, sizeof (uio));
	uio.uio_iov = iov;
	uio.uio_iovcnt = ARRAY_SIZE(lens);

	for (int fmt = CRYPTO_DATA_UIO; fmt <= CRYPTO_DATA_MBLK; fmt++) {
		bzero(&cd, sizeof (cd));
		cd.cd_format = fmt;
		if (fmt == CRYPTO_DATA_UIO)
			cd.cd_uio = &uio;
		else
			cd.cd_mp = &mb[0];
		for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
			uint8_t *want = (cases[i].seg < 0) ? NULL :
			    base[cases[i].seg] + cases[i].segoff;

			cd.cd_offset = cases[i].off;
			p = crypto_data_contig(&cd, &avail);
			if (p != want || avail != cases[i].avail) {
				KT_FAIL(ctx, "fmt %d offset %lld: got %p/%zu, "
				    "expected %p/%zu", fmt,
				    (longlong_t)cases[i].off, p, avail, want,
				    cases[i].avail);
				return;
			}
		}
	}

	mk_raw(&cd, buf, 40);
	cd.cd_offset = 8;
	p = crypto_data_contig(&cd, &avail);
	if (p != &buf[8] || avail != 32) {
		KT_FAIL(ctx, "RAW offset 8: got %p/%zu", p, avail);
		return;
	}
	cd.cd_offset = 40;
	p = crypto_data_contig(&cd, &avail);
	if (p != NULL || avail != 0) {
		KT_FAIL(ctx, "RAW offset 40: got %p/%zu", p, avail);
		return;
	}
	if (crypto_data_contig(NULL, &avail) != NULL) {
		KT_FAIL(ctx, "NULL crypto_data_t: got non-NULL");
		return;
	}
	KT_PASS(ctx);
}

/*
 * Encrypt `len' bytes of `pt' with AES-CTR into a UIO of `nseg'
 * segments of the given lengths, starting at output offset `start'.
 * The input is fed in pieces of the lengths in `chunks' (zero
 * terminated): multi-part, one update per piece, or with `atomic' one
 * crypto_encrypt() over a UIO input split the same way.  The output
 * must equal `ref' and the bytes before `start' and after the output
 * must be untouched.
 */
static boolean_t
ctr_out_one(ktest_ctx_hdl_t *ctx, const size_t *seglens, size_t nseg,
    size_t start, const size_t *chunks, boolean_t atomic,
    const uint8_t *pt, const uint8_t *ref, size_t len)
{
	crypto_mechanism_t mech;
	CK_AES_CTR_PARAMS ctrp;
	CK_AES_GCM_PARAMS gcmp;
	crypto_key_t key;
	crypto_context_t cctx;
	crypto_data_t in, out;
	iovec_t iov[MK_MAX_SEGS], iniov[MK_MAX_SEGS];
	uio_t uio, inuio;
	uint8_t *buf;
	size_t total = 0, written = 0, off = 0, n = 0;
	boolean_t ok = B_FALSE;
	int rv;

	for (size_t i = 0; i < nseg; i++)
		total += seglens[i];
	buf = kmem_alloc(total, KM_SLEEP);
	for (size_t i = 0; i < total; i++)
		buf[i] = 0xa5;
	for (size_t i = 0, p = 0; i < nseg; p += seglens[i++]) {
		iov[i].iov_base = (caddr_t)buf + p;
		iov[i].iov_len = seglens[i];
	}
	bzero(&uio, sizeof (uio));
	uio.uio_iov = iov;
	uio.uio_iovcnt = (int)nseg;
	uio.uio_segflg = UIO_SYSSPACE;
	uio.uio_resid = (ssize_t)total;
	bzero(&out, sizeof (out));
	out.cd_format = CRYPTO_DATA_UIO;
	out.cd_uio = &uio;

	(void) mk_cipher_mech(MK_CTR, &mech, &ctrp, &gcmp);
	mk_key_init(&key);

	if (atomic) {
		for (; n < MK_MAX_SEGS && chunks[n] != 0; n++) {
			iniov[n].iov_base = (caddr_t)pt + off;
			iniov[n].iov_len = chunks[n];
			off += chunks[n];
		}
		bzero(&inuio, sizeof (inuio));
		inuio.uio_iov = iniov;
		inuio.uio_iovcnt = (int)n;
		inuio.uio_segflg = UIO_SYSSPACE;
		inuio.uio_resid = (ssize_t)len;
		bzero(&in, sizeof (in));
		in.cd_format = CRYPTO_DATA_UIO;
		in.cd_length = len;
		in.cd_uio = &inuio;
		out.cd_offset = start;
		out.cd_length = len;
		if ((rv = crypto_encrypt(&mech, &in, &key, NULL, &out,
		    NULL)) != CRYPTO_SUCCESS) {
			KT_FAIL(ctx, "crypto_encrypt: 0x%x", rv);
			goto out;
		}
		written = out.cd_length;
	} else {
		if ((rv = crypto_encrypt_init(&mech, &key, NULL, &cctx,
		    NULL)) != CRYPTO_SUCCESS) {
			KT_ERROR(ctx, "crypto_encrypt_init: 0x%x", rv);
			goto out;
		}
		for (; n < MK_MAX_SEGS && chunks[n] != 0; n++) {
			mk_raw(&in, (void *)(pt + off), chunks[n]);
			out.cd_offset = start + written;
			out.cd_length = len - written;
			if ((rv = crypto_encrypt_update(cctx, &in, &out,
			    NULL)) != CRYPTO_SUCCESS) {
				KT_FAIL(ctx, "update %zu at offset %zu: 0x%x",
				    n, start + written, rv);
				crypto_cancel_ctx(cctx);
				goto out;
			}
			written += out.cd_length;
			off += chunks[n];
		}
		out.cd_offset = start + written;
		out.cd_length = len - written;
		if ((rv = crypto_encrypt_final(cctx, &out, NULL)) !=
		    CRYPTO_SUCCESS) {
			KT_FAIL(ctx, "final: 0x%x", rv);
			goto out;
		}
		written += out.cd_length;
	}

	if (written != len || off != len) {
		KT_FAIL(ctx, "wrote %zu bytes, expected %zu", written, len);
		goto out;
	}
	for (size_t i = 0; i < total; i++) {
		uint8_t want = (i >= start && i < start + len) ?
		    ref[i - start] : 0xa5;

		if (buf[i] != want) {
			KT_FAIL(ctx, "byte %zu: got 0x%02x, expected 0x%02x%s",
			    i, buf[i], want, (i >= start && i < start + len) ?
			    "" : " (outside the output)");
			goto out;
		}
	}
	ok = B_TRUE;
out:
	kmem_free(buf, total);
	return (ok);
}

/*
 * CTR output that spans segments, written by the per-block path (and,
 * with the fastpath on, by the fastpath where a whole aligned piece
 * fits one segment), multi-part and atomic, starting at offset 0 and
 * at a nonzero offset.
 */
static void
ctr_output_test(ktest_ctx_hdl_t *ctx)
{
	static const size_t seglens[] = { 40, 0, 23, 160, 1, 300, 0, 100 };
	static const size_t chunk_sets[][MK_MAX_SEGS] = {
		{ 600, 0 },
		{ 33, 200, 7, 160, 200, 0 },
		{ 16, 16, 64, 1, 503, 0 },
		{ 1, 1, 1, 1, 596, 0 },
	};
	static const size_t starts[] = { 0, 3, 16, 24 };
	const size_t len = 600;
	boolean_t saved = ctr_fastpath_enabled;
	crypto_mechanism_t mech;
	CK_AES_CTR_PARAMS ctrp;
	CK_AES_GCM_PARAMS gcmp;
	crypto_key_t key;
	crypto_data_t in, out;
	uint8_t *pt = kmem_alloc(len, KM_SLEEP);
	uint8_t *ref = kmem_alloc(len, KM_SLEEP);
	int rv;

	mk_fill(pt, len, 0xc7c7);
	if (mk_cipher_mech(MK_CTR, &mech, &ctrp, &gcmp) != 0) {
		KT_ERROR(ctx, "CKM_AES_CTR not available");
		goto out;
	}
	mk_key_init(&key);
	mk_raw(&in, pt, len);
	mk_raw(&out, ref, len);
	if ((rv = crypto_encrypt(&mech, &in, &key, NULL, &out, NULL)) !=
	    CRYPTO_SUCCESS) {
		KT_ERROR(ctx, "crypto_encrypt: 0x%x", rv);
		goto out;
	}

	for (size_t s = 0; s < ARRAY_SIZE(starts); s++) {
		for (size_t c = 0; c < ARRAY_SIZE(chunk_sets); c++) {
			for (int f = 0; f < 4; f++) {
				boolean_t atomic = (f & 2) != 0;
				boolean_t ok;

				ctr_fastpath_enabled = (f & 1) != 0;
				ok = ctr_out_one(ctx, seglens,
				    ARRAY_SIZE(seglens), starts[s],
				    chunk_sets[c], atomic, pt, ref, len);
				ctr_fastpath_enabled = saved;
				if (!ok) {
					ktest_msg_prepend(ctx, "start %zu "
					    "chunks %zu fast=%d atomic=%d: ",
					    starts[s], c, f & 1, atomic);
					goto out;
				}
			}
		}
	}
	KT_PASS(ctx);
out:
	ctr_fastpath_enabled = saved;
	kmem_free(pt, len);
	kmem_free(ref, len);
}

int
aes_modes_ktest_register(ktest_module_hdl_t *km)
{
	ktest_suite_hdl_t *ks;
	int ret;

	if ((ret = ktest_add_suite(km, "mac_layout", &ks)) != 0 ||
	    (ret = ktest_add_test(ks, "cmac_rfc4493_test", cmac_rfc4493_test,
	    KTEST_FLAG_NONE)) != 0 ||
	    (ret = ktest_add_test(ks, "cmac_layout_test", cmac_layout_test,
	    KTEST_FLAG_NONE)) != 0 ||
	    (ret = ktest_add_test(ks, "gmac_layout_test", gmac_layout_test,
	    KTEST_FLAG_NONE)) != 0)
		return (ret);

	if ((ret = ktest_add_suite(km, "output_segments", &ks)) != 0 ||
	    (ret = ktest_add_test(ks, "uio_continuation_test",
	    uio_continuation_test, KTEST_FLAG_NONE)) != 0 ||
	    (ret = ktest_add_test(ks, "contig_helper_test",
	    contig_helper_test, KTEST_FLAG_NONE)) != 0)
		return (ret);

	if ((ret = ktest_add_suite(km, "ctr_output", &ks)) != 0 ||
	    (ret = ktest_add_test(ks, "ctr_output_test", ctr_output_test,
	    KTEST_FLAG_NONE)) != 0)
		return (ret);

	return (0);
}
