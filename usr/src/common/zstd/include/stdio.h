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
 * <stdio.h> shim for the vendored zstd library (usr/src/common/zstd/lib).
 * zstd includes standard C headers that do not exist in the kernel; in
 * the kernel they map onto illumos <sys/...> headers, and elsewhere
 * (libzpool, userland) they defer to the real header.
 */

#ifndef	_ZSTD_STDIO_H
#define	_ZSTD_STDIO_H

#ifdef	_KERNEL
/* zstd never prints in the kernel (DEBUGLEVEL is 0) */
#else
#include_next <stdio.h>
#endif

#endif	/* _ZSTD_STDIO_H */
