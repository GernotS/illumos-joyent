/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright (c) 2008, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright 2018, Joyent, Inc.
 * Copyright 2023-2026 RackTop Systems, Inc.
 * Copyright 2026 Gernot Strasser
 */

/*
 * This file implements GCM and GMAC, as decribed in
 *	NIST Special Publication 800-38D
 *	Recommendation for Block Cipher Modes of Operation:
 *	Galois/Counter Mode (GCM) and GMAC
 *
 * Briefly, GMAC uses GCM just for "authentication" (sign/verify),
 * discarding the ouptut data (cipher/clear) that GCM would produce.
 *
 * Some functions below serve both GCM and GMAC, adjusting behavior
 * based on (ctx->gcm_flags & GMAC_MODE) to skip output production
 * or actions needed only when actually doing encrypt or decrypt.
 *
 * Some non-obvious things to note:
 *
 * The struct member gcm_len_a_len_c[] is an array of two uint64_t
 * (AAD length and input data length, in that order, in BITS).
 * The values are needed in that form for a hash computation that
 * happens in the "final" function for GCM or GMAC.  Just before the
 * "final" hash computation, the values are converted to big-endian
 * form as required by the altgorithm specification. Before that
 * point those values are in host order (always BITS).
 *
 * The calling framework (one of uts/common/crypto/io/aes.c
 * or lib/pkcs11/pkcs11_softtoken/common/softAESCrypt.c)
 * uses different "alloc", "init", and "final" functions
 * for GCM vs GMAC.  See calls to:
 *	gcm_alloc_ctx, gmac_alloc_ctx,
 *	gcm_init_ctx,  gmac_init_ctx,
 *	gcm_encrypt_final, gmac_mode_final
 * Operation of the GCM vs GMAC varints of those functions are
 * similar other than encrypt/decrypt in GCM, skipped in GMAC.
 */

#ifndef _KERNEL
#include <strings.h>
#include <limits.h>
#include <security/cryptoki.h>
#endif	/* _KERNEL */

#include <sys/debug.h>
#include <sys/cmn_err.h>
#include <sys/types.h>
#include <sys/kmem.h>
#define	INLINE_CRYPTO_GET_PTRS
#include <modes/modes.h>
#include <sys/crypto/common.h>
#include <sys/crypto/impl.h>
#include <sys/byteorder.h>

#define	COUNTER_MASK	0x00000000ffffffffULL

#ifdef	_KERNEL
#include <sys/sdt.h>
#endif	/* _KERNEL */

#ifdef __amd64

#ifdef _KERNEL
#include <sys/cpuvar.h>		/* cpu_t, CPU */
#include <sys/x86_archext.h>	/* x86_featureset, X86FSET_*, CPUID_* */
#include <sys/disp.h>		/* kpreempt_disable(), kpreempt_enable */
#else
#include <sys/auxv.h>		/* getisax() */
#include <sys/auxv_386.h>	/* AV_386_PCLMULQDQ bit */
#endif	/* _KERNEL */

extern void gcm_mul_pclmulqdq(uint64_t *x_in, uint64_t *y, uint64_t *res);
extern void gcm_accel_save(void *savestate);
extern void gcm_accel_restore(void *savestate);
extern void gcm_init_clmul(const uint64_t hash_init[2], uint8_t Htable[256]);
extern void gcm_ghash_clmul(uint64_t ghash[2], const uint8_t Htable[256],
    const uint8_t *inp, size_t length);
static int intel_pclmulqdq_instruction_present(void);

#ifdef	_KERNEL
/*
 * GCM_ACCEL_ENTER/EXIT guard kpreempt_disable() and the XMM register
 * save/restore required whenever GCM code runs assembly that uses XMM
 * registers (gcm_ghash_clmul, gcm_fastpath128, aes_ctr_mode via
 * encrypt_block).
 *
 * The predicate is intel_pclmulqdq_instruction_present() rather than
 * intel_aes_instructions_present() because GCM requires PCLMULQDQ for
 * its accelerated GHASH (gcm_ghash_clmul); that is the more restrictive
 * capability.  On all shipped x86 hardware, PCLMULQDQ and AES-NI were
 * introduced together (Intel Westmere 2010, AMD Bulldozer 2011), so
 * PCLMUL-present implies AES-NI-present.  The PCLMUL predicate therefore
 * correctly guards all XMM use in GCM paths, including calls to
 * encrypt_block() which dispatch to the AES-NI implementation when it is
 * available.
 *
 * Contrast CCM_ACCEL_ENTER/EXIT, which checks only
 * intel_aes_instructions_present(): CCM uses no CLMUL/GHASH, so AES-NI
 * alone is the right gate.
 */
#define	GCM_ACCEL_SAVESTATE \
	uint8_t savestate[16*16+8] __attribute__((aligned(16)))
#define	GCM_ACCEL_ENTER \
    do { \
	    if (intel_pclmulqdq_instruction_present()) { \
		    kpreempt_disable(); \
		    gcm_accel_save(savestate); \
	    } \
    } while (0)
#define	GCM_ACCEL_EXIT \
    do { \
	    if (intel_pclmulqdq_instruction_present()) { \
		    gcm_accel_restore(savestate); \
		    kpreempt_enable(); \
	    } \
    } while (0)
#else	/* !_KERNEL */
#define	GCM_ACCEL_SAVESTATE
#define	GCM_ACCEL_ENTER
#define	GCM_ACCEL_EXIT
#endif	/* !_KERNEL */
#else	/* !__amd64 */
#define	GCM_ACCEL_SAVESTATE
#define	GCM_ACCEL_ENTER
#define	GCM_ACCEL_EXIT
#endif	/* __amd64 */

struct aes_block {
	uint64_t a;
	uint64_t b;
};


/*
 * gcm_mul()
 * Perform a carry-less multiplication (that is, use XOR instead of the
 * multiply operator) on *x_in and *y and place the result in *res.
 *
 * Byte swap the input (*x_in and *y) and the output (*res).
 *
 * Note: x_in, y, and res all point to 16-byte numbers (an array of two
 * 64-bit integers).
 */
static inline void
gcm_mul(uint64_t *x_in, uint64_t *y, uint64_t *res)
{
#ifdef __amd64
	if (intel_pclmulqdq_instruction_present()) {
		gcm_mul_pclmulqdq(x_in, y, res);
	} else
#endif	/* __amd64 */
	{
		static const uint64_t R = 0xe100000000000000ULL;
		struct aes_block z = {0, 0};
		struct aes_block v;
		uint64_t x;
		int i, j;

		v.a = ntohll(y[0]);
		v.b = ntohll(y[1]);

		for (j = 0; j < 2; j++) {
			x = ntohll(x_in[j]);
			for (i = 0; i < 64; i++, x <<= 1) {
				if (x & 0x8000000000000000ULL) {
					z.a ^= v.a;
					z.b ^= v.b;
				}
				if (v.b & 1ULL) {
					v.b = (v.a << 63)|(v.b >> 1);
					v.a = (v.a >> 1) ^ R;
				} else {
					v.b = (v.a << 63)|(v.b >> 1);
					v.a = v.a >> 1;
				}
			}
		}
		res[0] = htonll(z.a);
		res[1] = htonll(z.b);
	}
}


#define	GHASH(c, d, t) \
	do { \
		xor_block((uint8_t *)(d), (uint8_t *)(c)->gcm_ghash); \
		gcm_mul((uint64_t *)(void *)(c)->gcm_ghash, (c)->gcm_H, \
		    (uint64_t *)(void *)(t)); \
		_NOTE(CONSTCOND) \
	} while (0)

boolean_t gcm_fastpath_enabled = B_TRUE;

/*
 * GCM fastpath for block-aligned input written into a single contiguous
 * output buffer.  On x86-64 with PCLMULQDQ this drives the aggregated
 * GHASH (gcm_ghash_clmul) and the bulk CTR path (cipher_ctr), which is
 * where almost all of the GCM speedup comes from.  Setting the global
 * gcm_fastpath_enabled to B_FALSE forces the per-block slow path.
 */
static void
gcm_fastpath128(gcm_ctx_t *ctx, const uint8_t *data, size_t length,
    uint8_t *out, boolean_t encrypt,
    int (*encrypt_block)(const void *, const uint8_t *, uint8_t *),
    void (*xor_block)(const uint8_t *, uint8_t *),
    int (*cipher_ctr)(const void *, const uint8_t *, uint8_t *,
    uint64_t, uint64_t[2]))
{
	/* When decrypting, `data' holds the ciphertext we need to GHASH. */
	if (!encrypt) {
#ifdef	__amd64
		if (intel_pclmulqdq_instruction_present())
			gcm_ghash_clmul(ctx->gcm_ghash, ctx->gcm_H_table,
			    data, length);
		else
#endif	/* __amd64 */
			for (size_t i = 0; i < length; i += 16)
				GHASH(ctx, &data[i], ctx->gcm_ghash);
	}

	if (cipher_ctr != NULL) {
		/*
		 * GCM is almost but not quite like CTR. GCM increments the
		 * counter value *before* processing the first input block,
		 * whereas CTR does so afterwards. So we need to increment
		 * the counter before calling CTR and decrement it afterwards.
		 */
		uint64_t counter = ntohll(ctx->gcm_cb[1]);

		ctx->gcm_cb[1] = htonll((counter & ~COUNTER_MASK) |
		    ((counter & COUNTER_MASK) + 1));
		cipher_ctr(ctx->gcm_keysched, data, out, length, ctx->gcm_cb);
		counter = ntohll(ctx->gcm_cb[1]);
		ctx->gcm_cb[1] = htonll((counter & ~COUNTER_MASK) |
		    ((counter & COUNTER_MASK) - 1));
	} else {
		uint64_t counter = ntohll(ctx->gcm_cb[1]);

		for (size_t i = 0; i < length; i += 16) {
			/*LINTED(E_BAD_PTR_CAST_ALIGN)*/
			*(uint64_t *)&out[i] = ctx->gcm_cb[0];
			/*LINTED(E_BAD_PTR_CAST_ALIGN)*/
			*(uint64_t *)&out[i + 8] = htonll(counter++);
			encrypt_block(ctx->gcm_keysched, &out[i], &out[i]);
			xor_block(&data[i], &out[i]);
		}

		ctx->gcm_cb[1] = htonll(counter);
	}

	/* When encrypting, `out' holds the ciphertext we need to GHASH. */
	if (encrypt) {
#ifdef	__amd64
		if (intel_pclmulqdq_instruction_present())
			gcm_ghash_clmul(ctx->gcm_ghash, ctx->gcm_H_table,
			    out, length);
		else
#endif	/* __amd64 */
			for (size_t i = 0; i < length; i += 16)
				GHASH(ctx, &out[i], ctx->gcm_ghash);

		/* If no more data comes in, the last block is the auth tag. */
		bcopy(&out[length - 16], ctx->gcm_tmp, 16);
	}

	ctx->gcm_processed_data_len += length;
}

/*
 * helper factored out of gcm_mode_encrypt_contiguous_blocks
 */
static inline void
gcm_encrypt_block(gcm_ctx_t *ctx, uint8_t *datap, crypto_data_t *out,
    size_t block_size, uint8_t *blockp, void *iov_or_mp, offset_t *offset,
    int (*encrypt_block)(const void *, const uint8_t *, uint8_t *),
    void (*copy_block)(const uint8_t *, uint8_t *),
    void (*xor_block)(const uint8_t *, uint8_t *))
{
	uint8_t *out_data_1;
	uint8_t *out_data_2;
	size_t out_data_1_len;
	uint64_t counter;
	uint64_t counter_mask = ntohll(0x00000000ffffffffULL);

	/*
	 * Increment counter. Counter bits are confined
	 * to the bottom 32 bits of the counter block.
	 */
	counter = ntohll(ctx->gcm_cb[1] & counter_mask);
	counter = htonll(counter + 1);
	counter &= counter_mask;
	ctx->gcm_cb[1] = (ctx->gcm_cb[1] & ~counter_mask) | counter;

	encrypt_block(ctx->gcm_keysched, (uint8_t *)ctx->gcm_cb,
	    (uint8_t *)ctx->gcm_tmp);
	xor_block(blockp, (uint8_t *)ctx->gcm_tmp);

	if (out == NULL) {
		if (ctx->gcm_remainder_len > 0) {
			bcopy(blockp, ctx->gcm_copy_to,
			    ctx->gcm_remainder_len);
			bcopy(blockp + ctx->gcm_remainder_len, datap,
			    block_size - ctx->gcm_remainder_len);
		}
	} else {
		uint8_t *tmpp = (uint8_t *)ctx->gcm_tmp;
		crypto_get_ptrs(out, iov_or_mp, offset, &out_data_1,
		    &out_data_1_len, &out_data_2, block_size);

		/* copy block to where it belongs */
		if (out_data_1_len == block_size) {
			copy_block(tmpp, out_data_1);
		} else {
			bcopy(tmpp, out_data_1, out_data_1_len);
			if (out_data_2 != NULL) {
				bcopy(tmpp + out_data_1_len,
				    out_data_2,
				    block_size - out_data_1_len);
			}
		}
		/* update offset */
		out->cd_offset += block_size;
	}
}

/*
 * Common GCM block processing for both encrypt and decrypt.
 * When `encrypt` is B_TRUE, ciphertext blocks are GHASHed and output.
 * When `encrypt` is B_FALSE, ciphertext blocks are GHASHed and decrypted.
 */
static int
gcm_process_contiguous_blocks(gcm_ctx_t *ctx, char *data, size_t length,
    crypto_data_t *out, size_t block_size, boolean_t encrypt,
    int (*encrypt_block)(const void *, const uint8_t *, uint8_t *),
    void (*copy_block)(const uint8_t *, uint8_t *),
    void (*xor_block)(const uint8_t *, uint8_t *),
    int (*cipher_ctr)(const void *, const uint8_t *, uint8_t *,
    uint64_t, uint64_t[2]))
{
	size_t remainder = length;
	size_t need;
	uint8_t *datap = (uint8_t *)data;
	uint8_t *blockp;
	void *iov_or_mp;
	offset_t offset;
	int rv = CRYPTO_SUCCESS;

	if (length == 0)
		return (CRYPTO_SUCCESS);

	/*
	 * GCM fastpath: when the input is block-aligned, nothing is
	 * buffered from a previous call, the 32-bit counter won't
	 * overflow and the output is a single contiguous buffer with
	 * room for the entire contiguous write (CRYPTO_DATA_HAS_ROOM;
	 * otherwise fall back to the generic path and its per-segment
	 * handling), gcm_fastpath128() handles the whole request at once.
	 */
	if (gcm_fastpath_enabled && block_size == 16 &&
	    ctx->gcm_remainder_len == 0 && (length & (block_size - 1)) == 0 &&
	    (ntohll(ctx->gcm_cb[1]) & COUNTER_MASK) <=
	    COUNTER_MASK - length / block_size &&
	    CRYPTO_DATA_IS_CONTIGUOUS(out, length) &&
	    CRYPTO_DATA_HAS_ROOM(out, length)) {
		gcm_fastpath128(ctx, (uint8_t *)data, length,
		    CRYPTO_DATA_FIRST_BLOCK(out), encrypt, encrypt_block,
		    xor_block, cipher_ctr);
		out->cd_offset += length;
		goto out;
	}

	if (out != NULL)
		crypto_init_ptrs(out, &iov_or_mp, &offset);

	do {
		/* Unprocessed data from last call. */
		if (ctx->gcm_remainder_len > 0) {
			need = block_size - ctx->gcm_remainder_len;

			if (need > remainder) {
				rv = CRYPTO_DATA_LEN_RANGE;
				goto out;
			}

			bcopy(datap,
			    (uint8_t *)ctx->gcm_remainder + ctx->gcm_remainder_len,
			    need);
			blockp = (uint8_t *)ctx->gcm_remainder;
		} else {
			blockp = datap;
		}

		if (!encrypt)
			GHASH(ctx, blockp, ctx->gcm_ghash);

		gcm_encrypt_block(ctx, datap, out, block_size, blockp,
		    &iov_or_mp, &offset, encrypt_block, copy_block, xor_block);

		if (encrypt)
			GHASH(ctx, ctx->gcm_tmp, ctx->gcm_ghash);

		/* Update pointer to next block of data to be processed. */
		if (ctx->gcm_remainder_len != 0) {
			datap += need;
			ctx->gcm_remainder_len = 0;
		} else {
			datap += block_size;
		}

		remainder = (size_t)&data[length] - (size_t)datap;

		/* Incomplete last block. */
		if (remainder > 0 && remainder < block_size) {
			bcopy(datap, ctx->gcm_remainder, remainder);
			ctx->gcm_remainder_len = remainder;
			ctx->gcm_processed_data_len += (length - remainder);
			goto out;
		}
	} while (remainder > 0);

	ctx->gcm_processed_data_len += length;
out:
	return (rv);
}

/*
 * Encrypt multiple blocks of data in GCM mode.  Decrypt for GCM mode
 * is done in another function: gcm_mode_decrypt_contiguous_blocks().
 *
 * When doing GCM, gcm_processed_data_len is advanced (which is the
 * encrypted/decrypted data bytes, excluding AAD).  When this is doing
 * GMAC (serving C_Sign) it advances the "input" pointers instead:
 * gcm_len_a_len_c[0] is the ADD input length, and
 * gcm_len_a_len_c[1] is the data input length.
 * (Details at the top of this file).
 */
int
gcm_mode_encrypt_contiguous_blocks(gcm_ctx_t *ctx, char *data, size_t length,
    crypto_data_t *out, size_t block_size,
    int (*encrypt_block)(const void *, const uint8_t *, uint8_t *),
    void (*copy_block)(const uint8_t *, uint8_t *),
    void (*xor_block)(const uint8_t *, uint8_t *),
    int (*cipher_ctr)(const void *, const uint8_t *, uint8_t *,
    uint64_t, uint64_t[2]))
{
	int rv = CRYPTO_SUCCESS;
	size_t remainder = length;
	size_t need;
	uint8_t *datap = (uint8_t *)data;
	uint8_t *blockp;
	void *iov_or_mp;
	offset_t offset;
	GCM_ACCEL_SAVESTATE;

	if (length + ctx->gcm_remainder_len < block_size) {
		/* accumulate bytes here and return */
		bcopy(datap,
		    (uint8_t *)ctx->gcm_remainder + ctx->gcm_remainder_len,
		    length);
		ctx->gcm_remainder_len += length;
		ctx->gcm_copy_to = datap;
		return (CRYPTO_SUCCESS);
	}

	/*
	 * Re-enter the fastpath for streams that are not block-aligned:
	 * complete a buffered partial block with just enough input on the
	 * per-block path, run the block-aligned middle through the
	 * fastpath and buffer the sub-block tail.  This is done before
	 * GCM_ACCEL_ENTER so that the FPU state is never saved twice.
	 * GMAC never takes the fastpath.
	 */
	if (gcm_fastpath_enabled && block_size == 16 && out != NULL &&
	    (ctx->gcm_flags & GMAC_MODE) == 0 && length > block_size) {
		size_t head = (ctx->gcm_remainder_len == 0) ? 0 :
		    block_size - ctx->gcm_remainder_len;
		size_t body = (length - head) & ~(block_size - 1);
		size_t tail = length - head - body;

		if (body != 0 && (head != 0 || tail != 0)) {
			if (head != 0 && (rv =
			    gcm_mode_encrypt_contiguous_blocks(ctx, data, head,
			    out, block_size, encrypt_block, copy_block,
			    xor_block, cipher_ctr)) != CRYPTO_SUCCESS)
				return (rv);
			ASSERT0(ctx->gcm_remainder_len);
			if ((rv = gcm_mode_encrypt_contiguous_blocks(ctx,
			    data + head, body, out, block_size, encrypt_block,
			    copy_block, xor_block, cipher_ctr)) !=
			    CRYPTO_SUCCESS || tail == 0)
				return (rv);
			return (gcm_mode_encrypt_contiguous_blocks(ctx,
			    data + head + body, tail, out, block_size,
			    encrypt_block, copy_block, xor_block, cipher_ctr));
		}
	}

	GCM_ACCEL_ENTER;

	/*
	 * GCM fastpath -- see gcm_process_contiguous_blocks().  This is
	 * the encrypt entry point, so `encrypt' is always B_TRUE here.
	 */
	/*
	 * GMAC must NOT take this fastpath: it would call gcm_fastpath128()
	 * with encrypt=B_TRUE and write `length' bytes of ciphertext into
	 * `out', which for GMAC is the 16-byte signature buffer in the
	 * caller's frame (e.g. smb2_sign_reply's reply_sig).  The slow path
	 * below correctly checks GMAC_MODE and GHASHes the input without
	 * producing any output.
	 */
	if (gcm_fastpath_enabled && block_size == 16 &&
	    (ctx->gcm_flags & GMAC_MODE) == 0 &&
	    ctx->gcm_remainder_len == 0 && (length & (block_size - 1)) == 0 &&
	    (ntohll(ctx->gcm_cb[1]) & COUNTER_MASK) <=
	    COUNTER_MASK - length / block_size &&
	    CRYPTO_DATA_IS_CONTIGUOUS(out, length) &&
	    CRYPTO_DATA_HAS_ROOM(out, length)) {
		gcm_fastpath128(ctx, (uint8_t *)data, length,
		    CRYPTO_DATA_FIRST_BLOCK(out), B_TRUE, encrypt_block,
		    xor_block, cipher_ctr);
		out->cd_offset += length;
		goto out;
	}

	/*
	 * GMAC fastpath: bulk PCLMULQDQ GHASH of AAD, no ciphertext produced.
	 * Gets SMB signing GCM-tier throughput for block-aligned input.
	 */
	if (gcm_fastpath_enabled && block_size == 16 &&
	    (ctx->gcm_flags & GMAC_MODE) != 0 &&
	    ctx->gcm_remainder_len == 0 &&
	    (length & (block_size - 1)) == 0) {
#ifdef	__amd64
		if (intel_pclmulqdq_instruction_present()) {
			gcm_ghash_clmul(ctx->gcm_ghash, ctx->gcm_H_table,
			    (const uint8_t *)data, length);
		} else
#endif	/* __amd64 */
		{
			for (size_t i = 0; i < length; i += block_size)
				GHASH(ctx, &((const uint8_t *)data)[i],
				    ctx->gcm_ghash);
		}
		ctx->gcm_len_a_len_c[0] += CRYPTO_BYTES2BITS(length);
		goto out;
	}

	if (out != NULL)
		crypto_init_ptrs(out, &iov_or_mp, &offset);

	do {
		/* Unprocessed data from last call. */
		if (ctx->gcm_remainder_len > 0) {
			need = block_size - ctx->gcm_remainder_len;

			if (need > remainder)
				return (CRYPTO_DATA_LEN_RANGE);

			bcopy(datap, &((uint8_t *)ctx->gcm_remainder)
			    [ctx->gcm_remainder_len], need);

			blockp = (uint8_t *)ctx->gcm_remainder;
		} else {
			blockp = datap;
		}

		if ((ctx->gcm_flags & GMAC_MODE) != 0) {
			/* add AAD to the hash */
			ctx->gcm_len_a_len_c[0] +=
			    CRYPTO_BYTES2BITS(block_size);
			GHASH(ctx, blockp, ctx->gcm_ghash);
		} else {
			gcm_encrypt_block(ctx, datap, out, block_size, blockp,
			    &iov_or_mp, &offset, encrypt_block, copy_block,
			    xor_block);
			/* add ciphertext to the hash */
			ctx->gcm_processed_data_len += block_size;
			GHASH(ctx, ctx->gcm_tmp, ctx->gcm_ghash);
		}

		/* Update pointer to next block of data to be processed. */
		if (ctx->gcm_remainder_len != 0) {
			datap += need;
			ctx->gcm_remainder_len = 0;
		} else {
			datap += block_size;
		}

		remainder = (size_t)&data[length] - (size_t)datap;

		/* Incomplete last block. */
		if (remainder > 0 && remainder < block_size) {
			bcopy(datap, ctx->gcm_remainder, remainder);
			ctx->gcm_remainder_len = remainder;
			ctx->gcm_copy_to = datap;
			goto out;
		}
		ctx->gcm_copy_to = NULL;

	} while (remainder > 0);

out:
	GCM_ACCEL_EXIT;
	return (rv);
}

/* ARGSUSED */
int
gcm_encrypt_final(gcm_ctx_t *ctx, crypto_data_t *out, size_t block_size,
    int (*encrypt_block)(const void *, const uint8_t *, uint8_t *),
    void (*copy_block)(const uint8_t *, uint8_t *),
    void (*xor_block)(const uint8_t *, uint8_t *))
{
	uint64_t counter_mask = ntohll(0x00000000ffffffffULL);
	uint8_t *ghash, *macp;
	int i, rv = CRYPTO_SUCCESS;
	GCM_ACCEL_SAVESTATE;

	if (out->cd_length <
	    (ctx->gcm_remainder_len + ctx->gcm_tag_len)) {
		return (CRYPTO_DATA_LEN_RANGE);
	}

	GCM_ACCEL_ENTER;

	ghash = (uint8_t *)ctx->gcm_ghash;

	if (ctx->gcm_remainder_len > 0) {
		uint64_t counter;
		uint8_t *tmpp = (uint8_t *)ctx->gcm_tmp;

		/*
		 * Here is where we deal with data that is not a
		 * multiple of the block size.
		 */

		/*
		 * Increment counter.
		 */
		counter = ntohll(ctx->gcm_cb[1] & counter_mask);
		counter = htonll(counter + 1);
		counter &= counter_mask;
		ctx->gcm_cb[1] = (ctx->gcm_cb[1] & ~counter_mask) | counter;

		encrypt_block(ctx->gcm_keysched, (uint8_t *)ctx->gcm_cb,
		    (uint8_t *)ctx->gcm_tmp);

		macp = (uint8_t *)ctx->gcm_remainder;
		bzero(macp + ctx->gcm_remainder_len,
		    block_size - ctx->gcm_remainder_len);

		/* XOR with counter block */
		for (i = 0; i < ctx->gcm_remainder_len; i++) {
			macp[i] ^= tmpp[i];
		}

		/* add ciphertext to the hash */
		GHASH(ctx, macp, ghash);

		ctx->gcm_processed_data_len += ctx->gcm_remainder_len;
	}

	/*
	 * The gcm_len_a_len_c values are in host order until final,
	 * where we convert them to network order before GHASH
	 */
	ctx->gcm_len_a_len_c[0] = htonll(ctx->gcm_len_a_len_c[0]);
	ctx->gcm_len_a_len_c[1] =
	    htonll(CRYPTO_BYTES2BITS(ctx->gcm_processed_data_len));
	GHASH(ctx, ctx->gcm_len_a_len_c, ghash);
	encrypt_block(ctx->gcm_keysched, (uint8_t *)ctx->gcm_J0,
	    (uint8_t *)ctx->gcm_J0);
	xor_block((uint8_t *)ctx->gcm_J0, ghash);

	if (ctx->gcm_remainder_len > 0) {
		void *iov_or_mp;
		offset_t cur_offset;
		uint8_t *out_data_1, *out_data_2;
		size_t out_data_1_len;
		size_t len = ctx->gcm_remainder_len;

		crypto_init_ptrs(out, &iov_or_mp, &cur_offset);
		crypto_get_ptrs(out, &iov_or_mp, &cur_offset,
		    &out_data_1, &out_data_1_len, &out_data_2, len);
		if (out_data_1_len == len) {
			bcopy(macp, out_data_1, len);
		} else {
			bcopy(macp, out_data_1, out_data_1_len);
			if (out_data_2 != NULL)
				bcopy(macp + out_data_1_len, out_data_2,
				    len - out_data_1_len);
		}
	}
	out->cd_offset += ctx->gcm_remainder_len;
	ctx->gcm_remainder_len = 0;
	{
		void *iov_or_mp;
		offset_t cur_offset;
		uint8_t *out_data_1, *out_data_2;
		size_t out_data_1_len;

		crypto_init_ptrs(out, &iov_or_mp, &cur_offset);
		crypto_get_ptrs(out, &iov_or_mp, &cur_offset,
		    &out_data_1, &out_data_1_len, &out_data_2,
		    ctx->gcm_tag_len);
		if (out_data_1_len == ctx->gcm_tag_len) {
			bcopy(ghash, out_data_1, ctx->gcm_tag_len);
		} else {
			bcopy(ghash, out_data_1, out_data_1_len);
			if (out_data_2 != NULL)
				bcopy(ghash + out_data_1_len, out_data_2,
				    ctx->gcm_tag_len - out_data_1_len);
		}
	}
	out->cd_offset += ctx->gcm_tag_len;

	GCM_ACCEL_EXIT;
	return (rv);
}

/*
 * This is used in the AES encrypt operations when we're using them
 * for MAC computations. In these cases encrypted data is discarded
 * and we keep only the final data block (used as the MAC).
 */
int
gmac_mode_final(gcm_ctx_t *ctx, crypto_data_t *out, size_t block_size,
    int (*encrypt_block)(const void *, const uint8_t *, uint8_t *),
    void (*xor_block)(const uint8_t *, uint8_t *))
{
	uint8_t *ghash;
	int rv = CRYPTO_SUCCESS;
	GCM_ACCEL_SAVESTATE;

	/* Unlike encrypt_final, this has no output but the tag. */
	if (out->cd_length < ctx->gcm_tag_len)
		return (CRYPTO_DATA_LEN_RANGE);

	GCM_ACCEL_ENTER;

	ghash = (uint8_t *)ctx->gcm_ghash;

	if (ctx->gcm_remainder_len > 0) {
		uint8_t *macp;

		/*
		 * Here is where we deal with data that is not a
		 * multiple of the block size.
		 *
		 * Not encrypting, so no counter, gcm_cb[].
		 */

		macp = (uint8_t *)ctx->gcm_remainder;
		bzero(macp + ctx->gcm_remainder_len,
		    block_size - ctx->gcm_remainder_len);

		ctx->gcm_len_a_len_c[0] +=
		    CRYPTO_BYTES2BITS(ctx->gcm_remainder_len);
		ctx->gcm_remainder_len = 0;
		/* add AAD to the hash */
		GHASH(ctx, macp, ghash);
	}

	/*
	 * We've stored the total auth data in bits here, but before we
	 * add it to the hash, we need to convert to network order.
	 * GMAC keeps gcm_len_a_len_c[1] = 0.
	 */
	ctx->gcm_len_a_len_c[0] = htonll(ctx->gcm_len_a_len_c[0]);
	GHASH(ctx, ctx->gcm_len_a_len_c, ghash);
	encrypt_block(ctx->gcm_keysched, (uint8_t *)ctx->gcm_J0,
	    (uint8_t *)ctx->gcm_J0);
	xor_block((uint8_t *)ctx->gcm_J0, ghash);

	{
		void *iov_or_mp;
		offset_t cur_offset;
		uint8_t *out_data_1, *out_data_2;
		size_t out_data_1_len;

		crypto_init_ptrs(out, &iov_or_mp, &cur_offset);
		crypto_get_ptrs(out, &iov_or_mp, &cur_offset,
		    &out_data_1, &out_data_1_len, &out_data_2,
		    ctx->gcm_tag_len);
		if (out_data_1_len == ctx->gcm_tag_len) {
			bcopy(ghash, out_data_1, ctx->gcm_tag_len);
		} else {
			bcopy(ghash, out_data_1, out_data_1_len);
			if (out_data_2 != NULL)
				bcopy(ghash + out_data_1_len, out_data_2,
				    ctx->gcm_tag_len - out_data_1_len);
		}
	}
	out->cd_offset += ctx->gcm_tag_len;

	GCM_ACCEL_EXIT;
	return (rv);
}

/*
 * This will only deal with decrypting the last block of the input that
 * might not be a multiple of block length.
 */
/*ARGSUSED*/
static void
gcm_decrypt_incomplete_block(gcm_ctx_t *ctx, uint8_t *data, size_t length,
    size_t block_size, crypto_data_t *out,
    int (*encrypt_block)(const void *, const uint8_t *, uint8_t *),
    void (*xor_block)(const uint8_t *, uint8_t *))
{
	uint64_t counter;
	uint64_t counter_mask = ntohll(0x00000000ffffffffULL);

	/* pad last block and add to GHASH */
	bcopy(data, ctx->gcm_tmp, length);
	bzero(((uint8_t *)ctx->gcm_tmp) + length,
	    sizeof (ctx->gcm_tmp) - length);
	GHASH(ctx, ctx->gcm_tmp, ctx->gcm_ghash);

	/*
	 * Increment counter.
	 * Counter bits are confined to the bottom 32 bits.
	 */
	counter = ntohll(ctx->gcm_cb[1] & counter_mask);
	counter = htonll(counter + 1);
	counter &= counter_mask;
	ctx->gcm_cb[1] = (ctx->gcm_cb[1] & ~counter_mask) | counter;

	encrypt_block(ctx->gcm_keysched, (uint8_t *)ctx->gcm_cb,
	    (uint8_t *)ctx->gcm_tmp);

	/* XOR with counter block */
	for (size_t i = 0; i < length; i++)
		((uint8_t *)ctx->gcm_tmp)[i] ^= data[i];

	if (out != NULL) {
		/*
		 * Use crypto_init_ptrs / crypto_get_ptrs rather than
		 * crypto_put_output_data so that UIO (scatter-gather) output
		 * buffers are handled correctly.  crypto_put_output_data does
		 * not traverse the iovec array for UIO output and silently
		 * drops the write; the get_ptrs pair derives the right iovec
		 * and intra-iovec offset from cd_offset and can split the
		 * copy across two iovec segments when necessary.
		 */
		void *iov_or_mp;
		offset_t cur_offset;
		uint8_t *out_data_1;
		uint8_t *out_data_2;
		size_t out_data_1_len;

		crypto_init_ptrs(out, &iov_or_mp, &cur_offset);
		crypto_get_ptrs(out, &iov_or_mp, &cur_offset,
		    &out_data_1, &out_data_1_len, &out_data_2, length);
		if (out_data_1_len == length) {
			bcopy(ctx->gcm_tmp, out_data_1, length);
		} else {
			bcopy(ctx->gcm_tmp, out_data_1, out_data_1_len);
			if (out_data_2 != NULL)
				bcopy((uint8_t *)ctx->gcm_tmp + out_data_1_len,
				    out_data_2, length - out_data_1_len);
		}
		out->cd_offset += length;
	} else {
		bcopy(ctx->gcm_tmp, data, length);
	}
}

/*
 * See notes above gcm_mode_encrypt_contiguous_blocks for GMAC
 * cases (serving C_Verify here) -- same applies here.
 */
int
gcm_mode_decrypt_contiguous_blocks(gcm_ctx_t *ctx, char *data, size_t length,
    crypto_data_t *out, size_t block_size,
    int (*encrypt_block)(const void *, const uint8_t *, uint8_t *),
    void (*copy_block)(const uint8_t *, uint8_t *),
    void (*xor_block)(const uint8_t *, uint8_t *),
    int (*cipher_ctr)(const void *, const uint8_t *, uint8_t *,
    uint64_t, uint64_t[2]))
{
	int rv = CRYPTO_SUCCESS;
	GCM_ACCEL_SAVESTATE;

	GCM_ACCEL_ENTER;

	/*
	 * Previous calls accumulate data in the input buffer to make sure
	 * we have the auth tag (the last part of the ciphertext) when we
	 * receive a final() call.
	 */
	if (ctx->gcm_last_input_fill > 0) {
		/* Try to complete the input buffer */
		size_t to_copy = MIN(length,
		    sizeof (ctx->gcm_last_input) - ctx->gcm_last_input_fill);

		bcopy(data, ctx->gcm_last_input + ctx->gcm_last_input_fill,
		    to_copy);
		data += to_copy;
		ctx->gcm_last_input_fill += to_copy;
		length -= to_copy;

		if (ctx->gcm_last_input_fill < sizeof (ctx->gcm_last_input))
			/* Not enough input data to continue */
			goto out;

		if (length < ctx->gcm_tag_len) {
			/*
			 * There isn't enough data ahead to constitute a full
			 * auth tag, so only crunch one input block and copy
			 * the remainder of the input into our buffer.
			 */
			rv = gcm_process_contiguous_blocks(ctx,
			    (char *)ctx->gcm_last_input, block_size, out,
			    block_size, B_FALSE, encrypt_block, copy_block,
			    xor_block, cipher_ctr);
			if (rv != CRYPTO_SUCCESS)
				goto out;
			ctx->gcm_last_input_fill -= block_size;
			bcopy(ctx->gcm_last_input + block_size,
			    ctx->gcm_last_input, ctx->gcm_last_input_fill);
			bcopy(data, ctx->gcm_last_input +
			    ctx->gcm_last_input_fill, length);
			ctx->gcm_last_input_fill += length;
			/* No more input left */
			goto out;
		}
		/*
		 * There is enough data ahead for the auth tag, so crunch
		 * everything in our buffer now and empty it.
		 */
		rv = gcm_process_contiguous_blocks(ctx,
		    (char *)ctx->gcm_last_input, ctx->gcm_last_input_fill,
		    out, block_size, B_FALSE, encrypt_block, copy_block,
		    xor_block, cipher_ctr);
		if (rv != CRYPTO_SUCCESS)
			goto out;
		ctx->gcm_last_input_fill = 0;
	}
	/*
	 * Last input buffer is empty, so what's left ahead is block-aligned.
	 * Crunch all the blocks up until the near end, which might be our
	 * auth tag and we must NOT decrypt.
	 */
	ASSERT(ctx->gcm_last_input_fill == 0);
	if (length >= block_size + ctx->gcm_tag_len) {
		size_t to_decrypt = (length - ctx->gcm_tag_len) &
		    ~(block_size - 1);

		rv = gcm_process_contiguous_blocks(ctx, data, to_decrypt, out,
		    block_size, B_FALSE, encrypt_block, copy_block, xor_block,
		    cipher_ctr);
		if (rv != CRYPTO_SUCCESS)
			goto out;
		data += to_decrypt;
		length -= to_decrypt;
	}
	/*
	 * Copy the remainder into our input buffer, it's potentially
	 * the auth tag and a last partial block.
	 */
	ASSERT(length < sizeof (ctx->gcm_last_input));
	bcopy(data, ctx->gcm_last_input, length);
	ctx->gcm_last_input_fill += length;
out:
	GCM_ACCEL_EXIT;

	return (rv);
}

/*
 * Compute the number of plaintext bytes that a subsequent call to
 * gcm_mode_decrypt_contiguous_blocks() with `length' bytes of input
 * ciphertext will emit into `out'.  Callers (the KCF aes provider and
 * pkcs11_softtoken) use this to validate output buffer sizes before
 * the decryption is performed, so this function must mirror the
 * buffering logic of gcm_mode_decrypt_contiguous_blocks() exactly.
 */
size_t
gcm_decrypt_update_out_len(gcm_ctx_t *ctx, size_t length, size_t block_size)
{
	size_t fill = ctx->gcm_last_input_fill;
	size_t out_len = 0;

	if (fill > 0) {
		size_t to_copy = MIN(length,
		    sizeof (ctx->gcm_last_input) - fill);

		fill += to_copy;
		length -= to_copy;

		if (fill < sizeof (ctx->gcm_last_input))
			return (0);

		if (length < ctx->gcm_tag_len)
			return (block_size);

		out_len = fill;
	}
	if (length >= block_size + ctx->gcm_tag_len)
		out_len += (length - ctx->gcm_tag_len) & ~(block_size - 1);

	return (out_len);
}

int
gcm_decrypt_final(gcm_ctx_t *ctx, crypto_data_t *out, size_t block_size,
    int (*encrypt_block)(const void *, const uint8_t *, uint8_t *),
    void (*copy_block)(const uint8_t *, uint8_t *),
    void (*xor_block)(const uint8_t *, uint8_t *),
    int (*cipher_ctr)(const void *, const uint8_t *, uint8_t *,
    uint64_t, uint64_t[2]))
{
	int rv = CRYPTO_SUCCESS;

	/* Check there's enough data to at least compute a tag */
	if (ctx->gcm_last_input_fill < ctx->gcm_tag_len)
		return (CRYPTO_DATA_LEN_RANGE);

	GCM_ACCEL_SAVESTATE;
	GCM_ACCEL_ENTER;

	/* Finish any unprocessed input */
	if (ctx->gcm_last_input_fill > ctx->gcm_tag_len) {
		/*
		 * gcm_last_input can hold up to two blocks, so with a
		 * tag shorter than a block there may be a full block
		 * *and* a partial block in front of the tag.  Do not
		 * clamp to block_size here, or the partial block is
		 * dropped after the full one has been processed.
		 */
		size_t last_blk_len =
		    ctx->gcm_last_input_fill - ctx->gcm_tag_len;

		/* Finish last full block */
		if (last_blk_len >= block_size) {
			rv = gcm_process_contiguous_blocks(ctx,
			    (char *)ctx->gcm_last_input, block_size, out,
			    block_size, B_FALSE, encrypt_block, copy_block,
			    xor_block, cipher_ctr);
			if (rv != CRYPTO_SUCCESS)
				goto errout;

			last_blk_len -= block_size;
			/* gcm_process_contiguous_blocks already updated gcm_processed_data_len */
			ctx->gcm_last_input_fill -= block_size;
			bcopy(ctx->gcm_last_input + block_size,
			    ctx->gcm_last_input, ctx->gcm_last_input_fill);
		}
		ASSERT3U(last_blk_len, <, block_size);
		/* Finish last incomplete block before auth tag */
		if (last_blk_len > 0) {
			gcm_decrypt_incomplete_block(ctx, ctx->gcm_last_input,
			    last_blk_len, block_size, out, encrypt_block,
			    xor_block);

			ctx->gcm_processed_data_len += last_blk_len;
			ctx->gcm_last_input_fill -= last_blk_len;
			bcopy(ctx->gcm_last_input + last_blk_len,
			    ctx->gcm_last_input, ctx->gcm_last_input_fill);
		}
		/* Now the last_input buffer holds just the auth tag */
	}

	ASSERT(ctx->gcm_last_input_fill == ctx->gcm_tag_len);

	/*
	 * For GMAC, a trailing chunk of AAD that was not a full block is
	 * stashed in gcm_remainder by gcm_init() and is not folded into the
	 * hash (nor counted in gcm_len_a_len_c[0]) until the operation is
	 * finalized.  The MAC/encrypt entry points do this in
	 * gmac_mode_final(); the decrypt path must do it here too, otherwise
	 * the computed tag is missing the last partial AAD block and every
	 * GMAC verification over non-block-aligned AAD fails with
	 * CRYPTO_INVALID_MAC.
	 */
	if ((ctx->gcm_flags & GMAC_MODE) != 0 && ctx->gcm_remainder_len > 0) {
		uint8_t *macp = (uint8_t *)ctx->gcm_remainder;

		bzero(macp + ctx->gcm_remainder_len,
		    block_size - ctx->gcm_remainder_len);
		ctx->gcm_len_a_len_c[0] +=
		    CRYPTO_BYTES2BITS(ctx->gcm_remainder_len);
		GHASH(ctx, macp, ctx->gcm_ghash);
		ctx->gcm_remainder_len = 0;
	}

	/*
	 * The gcm_len_a_len_c values are in host order until final,
	 * where we convert them to network order before GHASH.
	 */
	ctx->gcm_len_a_len_c[0] = htonll(ctx->gcm_len_a_len_c[0]);
	ctx->gcm_len_a_len_c[1] =
	    htonll(CRYPTO_BYTES2BITS(ctx->gcm_processed_data_len));
	GHASH(ctx, ctx->gcm_len_a_len_c, ctx->gcm_ghash);
	encrypt_block(ctx->gcm_keysched, (uint8_t *)ctx->gcm_J0,
	    (uint8_t *)ctx->gcm_J0);
	xor_block((uint8_t *)ctx->gcm_J0, (uint8_t *)ctx->gcm_ghash);

	GCM_ACCEL_EXIT;

	/* compare the input authentication tag with what we calculated */
	if (bcmp(&ctx->gcm_last_input, ctx->gcm_ghash,
	    ctx->gcm_tag_len) != 0)
		return (CRYPTO_INVALID_MAC);

	return (CRYPTO_SUCCESS);

errout:
	GCM_ACCEL_EXIT;
	return (rv);
}
static int
gcm_validate_args(CK_AES_GCM_PARAMS *gcm_param)
{
	size_t tag_len;

	/*
	 * Check the length of the authentication tag (in bits).
	 */
	tag_len = gcm_param->ulTagBits;
	switch (tag_len) {
	case 32:
	case 64:
	case 96:
	case 104:
	case 112:
	case 120:
	case 128:
		break;
	default:
		return (CRYPTO_MECHANISM_PARAM_INVALID);
	}

	if (gcm_param->ulIvLen == 0)
		return (CRYPTO_MECHANISM_PARAM_INVALID);

	return (CRYPTO_SUCCESS);
}

static void
gcm_format_initial_blocks(uchar_t *iv, ulong_t iv_len,
    gcm_ctx_t *ctx, size_t block_size,
    void (*copy_block)(const uint8_t *, uint8_t *),
    void (*xor_block)(const uint8_t *, uint8_t *))
{
	uint8_t *cb;
	ulong_t remainder = iv_len;
	ulong_t processed = 0;
	uint8_t *datap, *ghash;
	uint64_t len_a_len_c[2];

	ghash = (uint8_t *)ctx->gcm_ghash;
	cb = (uint8_t *)ctx->gcm_cb;
	if (iv_len == 12) {
		bcopy(iv, cb, 12);
		cb[12] = 0;
		cb[13] = 0;
		cb[14] = 0;
		cb[15] = 1;
		/* J0 will be used again in the final */
		copy_block(cb, (uint8_t *)ctx->gcm_J0);
	} else {
		/* GHASH the IV */
		do {
			if (remainder < block_size) {
				bzero(cb, block_size);
				bcopy(&(iv[processed]), cb, remainder);
				datap = (uint8_t *)cb;
				remainder = 0;
			} else {
				datap = (uint8_t *)(&(iv[processed]));
				processed += block_size;
				remainder -= block_size;
			}
			GHASH(ctx, datap, ghash);
		} while (remainder > 0);

		len_a_len_c[0] = 0;
		len_a_len_c[1] = htonll(CRYPTO_BYTES2BITS(iv_len));
		GHASH(ctx, len_a_len_c, ctx->gcm_J0);

		/* J0 will be used again in the final */
		copy_block((uint8_t *)ctx->gcm_J0, (uint8_t *)cb);
	}
}

/*
 * The following function is called at encrypt or decrypt init time
 * for AES GCM mode.
 */
int
gcm_init(gcm_ctx_t *ctx, unsigned char *iv, size_t iv_len,
    unsigned char *auth_data, size_t auth_data_len, size_t block_size,
    int (*encrypt_block)(const void *, const uint8_t *, uint8_t *),
    void (*copy_block)(const uint8_t *, uint8_t *),
    void (*xor_block)(const uint8_t *, uint8_t *))
{
	uint8_t *ghash, *datap, *authp;
	size_t remainder, processed;
	GCM_ACCEL_SAVESTATE;

	GCM_ACCEL_ENTER;

	/*
	 * Encrypt a zero block to derive the GHASH subkey H.  This runs
	 * within GCM_ACCEL_ENTER, so when AES-NI is active kpreempt is
	 * already disabled and XMM state saved (PCLMUL-present implies
	 * AES-NI-present on all shipped hardware).  H must be computed
	 * unconditionally: both the CLMUL-accelerated GHASH path and the
	 * software fallback need it.
	 */
	bzero(ctx->gcm_H, sizeof (ctx->gcm_H));
	encrypt_block(ctx->gcm_keysched, (uint8_t *)ctx->gcm_H,
	    (uint8_t *)ctx->gcm_H);

#ifdef	__amd64
	if (intel_pclmulqdq_instruction_present()) {
		/*
		 * gcm_ghash_clmul()'s key table must be built from the
		 * hash subkey H in host (little-endian) byte order.
		 */
		uint64_t H_bswap64[2] = {
		    ntohll(ctx->gcm_H[0]), ntohll(ctx->gcm_H[1])
		};

		gcm_init_clmul(H_bswap64, ctx->gcm_H_table);
	}
#endif	/* __amd64 */

	gcm_format_initial_blocks(iv, iv_len, ctx, block_size,
	    copy_block, xor_block);

	authp = (uint8_t *)ctx->gcm_tmp;
	ghash = (uint8_t *)ctx->gcm_ghash;
	bzero(authp, block_size);
	bzero(ghash, block_size);

	processed = 0;
	remainder = auth_data_len;
	do {
		if (remainder < block_size) {
			if ((ctx->gcm_flags & GMAC_MODE) != 0) {
				/*
				 * GMAC does not encrypt or decrypt, and
				 * therefore doesn't keep any out buffer,
				 * so gcm_remainder holds any remainder
				 * that GMAC needs to handle.
				 */
				bcopy(&(auth_data[processed]),
				    ctx->gcm_remainder, remainder);
				ctx->gcm_remainder_len = remainder;
				break;
			}
			/*
			 * There's not a block full of data, pad rest of
			 * buffer with zero
			 */
			bzero(authp, block_size);
			bcopy(&(auth_data[processed]), authp, remainder);
			datap = (uint8_t *)authp;
			remainder = 0;
		} else {
			datap = (uint8_t *)(&(auth_data[processed]));
			processed += block_size;
			remainder -= block_size;
		}

		/* add auth data to the hash */
		GHASH(ctx, datap, ghash);

	} while (remainder > 0);

	if ((ctx->gcm_flags & GMAC_MODE) != 0) {
		ctx->gcm_len_a_len_c[0] =
		    CRYPTO_BYTES2BITS(auth_data_len - remainder);
	}

	GCM_ACCEL_EXIT;
	return (CRYPTO_SUCCESS);
}

int
gcm_init_ctx(gcm_ctx_t *gcm_ctx, char *param, size_t block_size,
    int (*encrypt_block)(const void *, const uint8_t *, uint8_t *),
    void (*copy_block)(const uint8_t *, uint8_t *),
    void (*xor_block)(const uint8_t *, uint8_t *))
{
	int rv;
	CK_AES_GCM_PARAMS *gcm_param;

	if (param != NULL) {
		gcm_param = (CK_AES_GCM_PARAMS *)(void *)param;

		if ((rv = gcm_validate_args(gcm_param)) != 0) {
			return (rv);
		}

		gcm_ctx->gcm_tag_len = gcm_param->ulTagBits;
		gcm_ctx->gcm_tag_len >>= 3;
		gcm_ctx->gcm_processed_data_len = 0;

		/* these values are in bits */
		gcm_ctx->gcm_len_a_len_c[0] =
		    CRYPTO_BYTES2BITS(gcm_param->ulAADLen);

		rv = CRYPTO_SUCCESS;
		gcm_ctx->gcm_flags |= GCM_MODE;
	} else {
		rv = CRYPTO_MECHANISM_PARAM_INVALID;
		goto out;
	}

	if (gcm_init(gcm_ctx, gcm_param->pIv, gcm_param->ulIvLen,
	    gcm_param->pAAD, gcm_param->ulAADLen, block_size,
	    encrypt_block, copy_block, xor_block) != 0) {
		rv = CRYPTO_MECHANISM_PARAM_INVALID;
	}
out:
	return (rv);
}

int
gmac_init_ctx(gcm_ctx_t *gcm_ctx, char *param, size_t block_size,
    int (*encrypt_block)(const void *, const uint8_t *, uint8_t *),
    void (*copy_block)(const uint8_t *, uint8_t *),
    void (*xor_block)(const uint8_t *, uint8_t *))
{
	int rv;
	CK_AES_GMAC_PARAMS *gmac_param;

	if (param == NULL)
		return (CRYPTO_MECHANISM_PARAM_INVALID);

	gmac_param = (CK_AES_GMAC_PARAMS *)(void *)param;

	gcm_ctx->gcm_tag_len = CRYPTO_BITS2BYTES(AES_GMAC_TAG_BITS);
	gcm_ctx->gcm_processed_data_len = 0;

	/* these values are in bits */
	gcm_ctx->gcm_len_a_len_c[0] = 0;
	gcm_ctx->gcm_len_a_len_c[1] = 0;

	rv = CRYPTO_SUCCESS;
	gcm_ctx->gcm_flags |= GMAC_MODE;

	if (gcm_init(gcm_ctx, gmac_param->pIv, AES_GMAC_IV_LEN,
	    gmac_param->pAAD, gmac_param->ulAADLen, block_size,
	    encrypt_block, copy_block, xor_block) != 0) {
		rv = CRYPTO_MECHANISM_PARAM_INVALID;
	}

	return (rv);
}

void *
gcm_alloc_ctx(int kmflag)
{
	gcm_ctx_t *gcm_ctx;

	/* Free in crypto_free_mode_ctx() */
#ifdef _KERNEL
	if ((gcm_ctx = kmem_zalloc(sizeof (gcm_ctx_t), kmflag)) == NULL)
#else
	if ((gcm_ctx = calloc(1, sizeof (gcm_ctx_t))) == NULL)
#endif
		return (NULL);

	gcm_ctx->gcm_flags = GCM_MODE;
	return (gcm_ctx);
}

void *
gmac_alloc_ctx(int kmflag)
{
	gcm_ctx_t *gcm_ctx;

	/* Free in crypto_free_mode_ctx() */
#ifdef _KERNEL
	if ((gcm_ctx = kmem_zalloc(sizeof (gcm_ctx_t), kmflag)) == NULL)
#else
	if ((gcm_ctx = calloc(1, sizeof (gcm_ctx_t))) == NULL)
#endif
		return (NULL);

	gcm_ctx->gcm_flags = GMAC_MODE;
	return (gcm_ctx);
}

void
gcm_set_kmflag(gcm_ctx_t *ctx, int kmflag)
{
	ctx->gcm_kmflag = kmflag;
}


#ifdef __amd64
/*
 * Return 1 if executing on Intel with PCLMULQDQ instructions,
 * otherwise 0 (i.e., Intel without PCLMULQDQ or AMD64).
 * Cache the result, as the CPU can't change.
 *
 * Note: the userland version uses getisax().  The kernel version uses
 * is_x86_featureset().
 */
static int
intel_pclmulqdq_instruction_present(void)
{
	static int	cached_result = -1;

	if (cached_result == -1) { /* first time */
#ifdef _KERNEL
		cached_result =
		    is_x86_feature(x86_featureset, X86FSET_PCLMULQDQ);
#else
		uint_t		ui = 0;

		(void) getisax(&ui, 1);
		cached_result = (ui & AV_386_PCLMULQDQ) != 0;
#endif	/* _KERNEL */
	}

	return (cached_result);
}
#endif	/* __amd64 */
