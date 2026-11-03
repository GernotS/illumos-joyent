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
 * Copyright 2026 Gernot Strasser
 */

/*
 * <stdlib.h> shim for the vendored zstd library (usr/src/common/zstd/lib).
 * zstd includes standard C headers that do not exist in the kernel; in
 * the kernel they map onto illumos <sys/...> headers, and elsewhere
 * (libzpool, userland) they defer to the real header.
 */

#ifndef	_ZSTD_STDLIB_H
#define	_ZSTD_STDLIB_H

#ifdef	_KERNEL
/*
 * zstd allocates only through the ZSTD_customMem allocators supplied by
 * zfs_zstd.c; make any stray default allocation fail.
 */
#define	calloc(n, sz)	NULL
#define	malloc(sz)	NULL
#define	free(ptr)
#else
#include_next <stdlib.h>
#endif

#endif	/* _ZSTD_STDLIB_H */
