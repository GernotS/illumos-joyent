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
 * Copyright 2009 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright 2014 Nexenta Systems, Inc.  All rights reserved.
 */
/*
 * Copyright 2014 by Saso Kiselkov. All rights reserved.
 */

#ifndef _KERNEL
#include <stdlib.h>
#include <assert.h>
#include <strings.h>
#endif

#include <sys/strsun.h>
#include <sys/types.h>
#include <modes/modes.h>
#include <sys/crypto/common.h>
#include <sys/crypto/impl.h>

/*
 * Initialize by setting iov_or_mp to point to the current iovec or mp,
 * and by setting current_offset to an offset within the current iovec or mp.
 */
void
crypto_init_ptrs(crypto_data_t *out, void **iov_or_mp, offset_t *current_offset)
{
	offset_t offset;

	switch (out->cd_format) {
	case CRYPTO_DATA_RAW:
		*current_offset = out->cd_offset;
		break;

	case CRYPTO_DATA_UIO: {
		uio_t *uiop = out->cd_uio;
		uintptr_t vec_idx;

		offset = out->cd_offset;
		for (vec_idx = 0; vec_idx < uiop->uio_iovcnt &&
		    offset >= uiop->uio_iov[vec_idx].iov_len;
		    offset -= uiop->uio_iov[vec_idx++].iov_len)
			;

		*current_offset = offset;
		*iov_or_mp = (void *)vec_idx;
		break;
	}

	case CRYPTO_DATA_MBLK: {
		mblk_t *mp;

		offset = out->cd_offset;
		for (mp = out->cd_mp; mp != NULL && offset >= MBLKL(mp);
		    offset -= MBLKL(mp), mp = mp->b_cont)
			;

		*current_offset = offset;
		*iov_or_mp = mp;
		break;

	}
	} /* end switch */
}

/*
 * Get pointers for where in the output to copy a block of encrypted or
 * decrypted data.  The iov_or_mp argument stores a pointer to the current
 * iovec or mp, and offset stores an offset into the current iovec or mp.
 */
void
crypto_get_ptrs(crypto_data_t *out, void **iov_or_mp, offset_t *current_offset,
    uint8_t **out_data_1, size_t *out_data_1_len, uint8_t **out_data_2,
    size_t amt)
{
	offset_t offset;

	switch (out->cd_format) {
	case CRYPTO_DATA_RAW: {
		iovec_t *iov;

		offset = *current_offset;
		iov = &out->cd_raw;
		if ((offset + amt) <= iov->iov_len) {
			/* one block fits */
			*out_data_1 = (uint8_t *)iov->iov_base + offset;
			*out_data_1_len = amt;
			*out_data_2 = NULL;
			*current_offset = offset + amt;
		}
		break;
	}

	case CRYPTO_DATA_UIO: {
		uio_t *uio = out->cd_uio;
		iovec_t *iov;
		offset_t offset;
		uintptr_t vec_idx;
		uint8_t *p;

		offset = *current_offset;
		vec_idx = (uintptr_t)(*iov_or_mp);
		iov = &uio->uio_iov[vec_idx];
		p = (uint8_t *)iov->iov_base + offset;
		*out_data_1 = p;

		if (offset + amt <= iov->iov_len) {
			/* can fit one block into this iov */
			*out_data_1_len = amt;
			*out_data_2 = NULL;
			*current_offset = offset + amt;
		} else {
			/* one block spans two iovecs */
			*out_data_1_len = iov->iov_len - offset;
			if (vec_idx == uio->uio_iovcnt)
				return;
			vec_idx++;
			iov = &uio->uio_iov[vec_idx];
			*out_data_2 = (uint8_t *)iov->iov_base;
			*current_offset = amt - *out_data_1_len;
		}
		*iov_or_mp = (void *)vec_idx;
		break;
	}

	case CRYPTO_DATA_MBLK: {
		mblk_t *mp;
		uint8_t *p;

		offset = *current_offset;
		mp = (mblk_t *)*iov_or_mp;
		p = mp->b_rptr + offset;
		*out_data_1 = p;
		if ((p + amt) <= mp->b_wptr) {
			/* can fit one block into this mblk */
			*out_data_1_len = amt;
			*out_data_2 = NULL;
			*current_offset = offset + amt;
		} else {
			/* one block spans two mblks */
			*out_data_1_len = _PTRDIFF(mp->b_wptr, p);
			if ((mp = mp->b_cont) == NULL)
				return;
			*out_data_2 = mp->b_rptr;
			*current_offset = (amt - *out_data_1_len);
		}
		*iov_or_mp = mp;
		break;
	}
	} /* end switch */
}

/*
 * Resolve the current output position of a crypto_data_t object to the
 * segment it falls in.  cd_offset is relative to the start of the whole
 * object; for a uio it is resolved to an iovec and for an mblk chain to
 * a b_cont block, exactly as crypto_init_ptrs() does.  Returns a pointer
 * to that position and stores in *lenp the number of bytes that can be
 * written there contiguously, i.e. up to the end of that iovec or mblk.
 * Returns NULL, with *lenp set to 0, when `cd' is NULL or cd_offset
 * lies at or beyond the end of the object.
 *
 * The fastpaths write their whole output with a single contiguous store,
 * so they are taken only when this length covers the entire request;
 * anything else goes to the per-block path, which can split blocks
 * across segment boundaries.  Because the position is resolved from
 * cd_offset rather than taken from the first segment, a stream whose
 * earlier updates have already filled one or more leading iovecs or
 * mblks still reaches the fastpath for the segment it is now writing.
 * Shapes this covers include the ZFS cipher uio (the ciphertext in
 * iov[0] and the 16-byte MAC in iov[1], see zio_crypt_init_uios()),
 * multi-iovec SMB3 encryption uios, and IPsec ESP b_cont chains.  Note
 * that the segments of one message are chained with b_cont; b_next
 * links whole messages on a queue and is not followed.
 */
uint8_t *
crypto_data_contig(crypto_data_t *cd, size_t *lenp)
{
	offset_t offset;

	*lenp = 0;
	if (cd == NULL || (offset = cd->cd_offset) < 0)
		return (NULL);

	switch (cd->cd_format) {
	case CRYPTO_DATA_RAW:
		if ((size_t)offset >= cd->cd_raw.iov_len)
			return (NULL);
		*lenp = cd->cd_raw.iov_len - (size_t)offset;
		return ((uint8_t *)cd->cd_raw.iov_base + offset);

	case CRYPTO_DATA_UIO: {
		uio_t *uiop = cd->cd_uio;
		int vec_idx;

		for (vec_idx = 0; vec_idx < uiop->uio_iovcnt &&
		    (size_t)offset >= uiop->uio_iov[vec_idx].iov_len;
		    offset -= uiop->uio_iov[vec_idx++].iov_len)
			;
		if (vec_idx == uiop->uio_iovcnt)
			return (NULL);
		*lenp = uiop->uio_iov[vec_idx].iov_len - (size_t)offset;
		return ((uint8_t *)uiop->uio_iov[vec_idx].iov_base + offset);
	}

	case CRYPTO_DATA_MBLK: {
		mblk_t *mp;

		for (mp = cd->cd_mp; mp != NULL &&
		    (size_t)offset >= MBLKL(mp);
		    offset -= MBLKL(mp), mp = mp->b_cont)
			;
		if (mp == NULL)
			return (NULL);
		*lenp = MBLKL(mp) - (size_t)offset;
		return (mp->b_rptr + offset);
	}
	} /* end switch */

	return (NULL);
}

void
crypto_free_mode_ctx(void *ctx)
{
	common_ctx_t *common_ctx = (common_ctx_t *)ctx;

	switch (common_ctx->cc_flags & (ECB_MODE|CBC_MODE|CMAC_MODE|CTR_MODE|
	    CCM_MODE|GCM_MODE|GMAC_MODE)) {
	case ECB_MODE:
#ifdef _KERNEL
		kmem_free(common_ctx, sizeof (ecb_ctx_t));
#else
		free(common_ctx);
#endif
		break;

	case CBC_MODE:
	case CMAC_MODE:
#ifdef _KERNEL
		kmem_free(common_ctx, sizeof (cbc_ctx_t));
#else
		free(common_ctx);
#endif
		break;

	case CTR_MODE:
#ifdef _KERNEL
		kmem_free(common_ctx, sizeof (ctr_ctx_t));
#else
		free(common_ctx);
#endif
		break;

	case CCM_MODE:
#ifdef _KERNEL
		if (((ccm_ctx_t *)ctx)->ccm_pt_buf != NULL)
			kmem_free(((ccm_ctx_t *)ctx)->ccm_pt_buf,
			    ((ccm_ctx_t *)ctx)->ccm_data_len);

		kmem_free(ctx, sizeof (ccm_ctx_t));
#else
		if (((ccm_ctx_t *)ctx)->ccm_pt_buf != NULL)
			free(((ccm_ctx_t *)ctx)->ccm_pt_buf);
		free(ctx);
#endif
		break;

	case GCM_MODE:
	case GMAC_MODE:
#ifdef _KERNEL
		kmem_free(ctx, sizeof (gcm_ctx_t));
#else
		free(ctx);
#endif
	}
}

/*
 * Utility routine to apply the command, 'cmd', to the
 * data in the uio structure.
 */
int
crypto_uio_data(crypto_data_t *data, uchar_t *buf, int len, cmd_type_t cmd,
    void *digest_ctx, void (*update)())
{
	uio_t *uiop = data->cd_uio;
	off_t offset = data->cd_offset;
	size_t length = len;
	uint_t vec_idx;
	size_t cur_len;
	uchar_t *datap;

#ifdef _KERNEL
	ASSERT3U(data->cd_format, ==, CRYPTO_DATA_UIO);
#else
	assert(data->cd_format == CRYPTO_DATA_UIO);
#endif
	if (uiop->uio_segflg != UIO_SYSSPACE) {
		return (CRYPTO_ARGUMENTS_BAD);
	}

	/*
	 * Jump to the first iovec containing data to be
	 * processed.
	 */
	for (vec_idx = 0; vec_idx < uiop->uio_iovcnt &&
	    offset >= uiop->uio_iov[vec_idx].iov_len;
	    offset -= uiop->uio_iov[vec_idx++].iov_len)
		;

	if (vec_idx == uiop->uio_iovcnt && length > 0) {
		/*
		 * The caller specified an offset that is larger than
		 * the total size of the buffers it provided.
		 */
		return (CRYPTO_DATA_LEN_RANGE);
	}

	while (vec_idx < uiop->uio_iovcnt && length > 0) {
		cur_len = MIN(uiop->uio_iov[vec_idx].iov_len -
		    offset, length);

		datap = (uchar_t *)(uiop->uio_iov[vec_idx].iov_base +
		    offset);
		switch (cmd) {
		case COPY_FROM_DATA:
			bcopy(datap, buf, cur_len);
			buf += cur_len;
			break;
		case COPY_TO_DATA:
			bcopy(buf, datap, cur_len);
			buf += cur_len;
			break;
		case COMPARE_TO_DATA:
			if (bcmp(datap, buf, cur_len))
				return (CRYPTO_SIGNATURE_INVALID);
			buf += cur_len;
			break;
		case MD5_DIGEST_DATA:
		case SHA1_DIGEST_DATA:
		case SHA2_DIGEST_DATA:
		case GHASH_DATA:
			update(digest_ctx, datap, cur_len);
			break;
		}

		length -= cur_len;
		vec_idx++;
		offset = 0;
	}

	if (vec_idx == uiop->uio_iovcnt && length > 0) {
		/*
		 * The end of the specified iovec's was reached but
		 * the length requested could not be processed.
		 */
		switch (cmd) {
		case COPY_TO_DATA:
			data->cd_length = len;
			return (CRYPTO_BUFFER_TOO_SMALL);
		default:
			return (CRYPTO_DATA_LEN_RANGE);
		}
	}

	return (CRYPTO_SUCCESS);
}

/*
 * Utility routine to apply the command, 'cmd', to the
 * data in the mblk structure.
 */
int
crypto_mblk_data(crypto_data_t *data, uchar_t *buf, int len, cmd_type_t cmd,
    void *digest_ctx, void (*update)())
{
	off_t offset = data->cd_offset;
	size_t length = len;
	mblk_t *mp;
	size_t cur_len;
	uchar_t *datap;

#ifdef _KERNEL
	ASSERT3U(data->cd_format, ==, CRYPTO_DATA_MBLK);
#else
	assert(data->cd_format == CRYPTO_DATA_MBLK);
#endif
	/*
	 * Jump to the first mblk_t containing data to be processed.
	 */
	for (mp = data->cd_mp; mp != NULL && offset >= MBLKL(mp);
	    offset -= MBLKL(mp), mp = mp->b_cont)
		;
	if (mp == NULL) {
		/*
		 * The caller specified an offset that is larger
		 * than the total size of the buffers it provided.
		 */
		return (CRYPTO_DATA_LEN_RANGE);
	}

	/*
	 * Now do the processing on the mblk chain.
	 */
	while (mp != NULL && length > 0) {
		cur_len = MIN(MBLKL(mp) - offset, length);

		datap = (uchar_t *)(mp->b_rptr + offset);
		switch (cmd) {
		case COPY_FROM_DATA:
			bcopy(datap, buf, cur_len);
			buf += cur_len;
			break;
		case COPY_TO_DATA:
			bcopy(buf, datap, cur_len);
			buf += cur_len;
			break;
		case COMPARE_TO_DATA:
			if (bcmp(datap, buf, cur_len))
				return (CRYPTO_SIGNATURE_INVALID);
			buf += cur_len;
			break;
		case MD5_DIGEST_DATA:
		case SHA1_DIGEST_DATA:
		case SHA2_DIGEST_DATA:
		case GHASH_DATA:
			update(digest_ctx, datap, cur_len);
			break;
		}

		length -= cur_len;
		offset = 0;
		mp = mp->b_cont;
	}

	if (mp == NULL && length > 0) {
		/*
		 * The end of the mblk was reached but the length
		 * requested could not be processed.
		 */
		switch (cmd) {
		case COPY_TO_DATA:
			data->cd_length = len;
			return (CRYPTO_BUFFER_TOO_SMALL);
		default:
			return (CRYPTO_DATA_LEN_RANGE);
		}
	}

	return (CRYPTO_SUCCESS);
}

/*
 * Utility routine to copy a buffer to a crypto_data structure.
 */
int
crypto_put_output_data(uchar_t *buf, crypto_data_t *output, int len)
{
	switch (output->cd_format) {
	case CRYPTO_DATA_RAW:
		if (MAXOFF_T - output->cd_offset < (off_t)len) {
			return (CRYPTO_ARGUMENTS_BAD);
		}
		if (output->cd_raw.iov_len < len + output->cd_offset) {
			output->cd_length = len;
			return (CRYPTO_BUFFER_TOO_SMALL);
		}
		bcopy(buf, (uchar_t *)(output->cd_raw.iov_base +
		    output->cd_offset), len);
		break;

	case CRYPTO_DATA_UIO:
		return (crypto_uio_data(output, buf, len,
		    COPY_TO_DATA, NULL, NULL));

	case CRYPTO_DATA_MBLK:
		return (crypto_mblk_data(output, buf, len,
		    COPY_TO_DATA, NULL, NULL));

	default:
		return (CRYPTO_ARGUMENTS_BAD);
	}

	return (CRYPTO_SUCCESS);
}

/*
 * Constant-time buffer comparison.
 *
 * Returns 0 if the two buffers are identical and non-zero otherwise,
 * like bcmp(), but the execution time depends only on `len' and not on
 * the buffer contents or on the position of the first differing byte.
 *
 * This must be used in place of bcmp() whenever a computed
 * authentication tag or MAC is compared against a caller-supplied
 * value: bcmp() is free to return as soon as it finds a difference, so
 * its timing reveals how many leading bytes of a forged tag were
 * guessed correctly, which reduces forgery to a per-byte search.
 *
 * The accumulator is volatile so that the compiler may not rewrite the
 * loop into an early-exit comparison.
 */
int
crypto_memcmp(const void *s1, const void *s2, size_t len)
{
	const uint8_t *p1 = s1;
	const uint8_t *p2 = s2;
	volatile uint8_t diff = 0;
	size_t i;

	for (i = 0; i < len; i++) {
		diff |= p1[i] ^ p2[i];
	}

	return (diff != 0);
}
