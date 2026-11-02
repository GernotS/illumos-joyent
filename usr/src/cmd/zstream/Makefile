#
# CDDL HEADER START
#
# The contents of this file are subject to the terms of the
# Common Development and Distribution License (the "License").
# You may not use this file except in compliance with the License.
#
# You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
# or http://www.opensolaris.org/os/licensing.
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
# Copyright 2009 Sun Microsystems, Inc.  All rights reserved.
# Use is subject to license terms.
#
# Copyright (c) 2018, Joyent, Inc.
# Copyright 2024 Oxide Computer Company
# Copyright 2026 <YOUR NAME/ORG HERE>

PROG=		zstream
OBJS=		zstream.o \
		zstream_byteswap.o \
		zstream_chain.o \
		zstream_compress.o \
		zstream_decompress.o \
		zstream_drop_record.o \
		zstream_dump.o \
		zstream_fletcher4.o \
		zstream_io.o \
		zstream_queue.o \
		zstream_redup.o \
		zstream_token.o \
		zstream_validate.o \
		zstream_shared.o \
		zstream_blake3.o \
		zstream_dedup.o \
		allocator.o \
		linear_hash.o \
		linear_hash_debug.o
POFILE=		zstream.po

#
# zstream drives the ZFS userland subsystems (abd, zio compression, fletcher4,
# refcount) which live in libzpool and are brought up via kernel_init() from
# libfakekernel - exactly like zdb and ztest. That machinery is 64-bit only,
# so we build 64-bit (Makefile.cmd.64), pull in the private libzpool/libzutil
# headers, define _FAKE_KERNEL, and link -lzpool/-lfakekernel/-lumem/-lavl.
# This Makefile deliberately mirrors cmd/zdb/Makefile; keep them in sync.
#
include ../Makefile.cmd
include ../Makefile.cmd.64
include ../Makefile.ctf

INCS += -I../../lib/libzpool/common
INCS +=	-I../../uts/common/fs/zfs
INCS +=	-I../../common/zfs
INCS += -I../../lib/libzutil/common

# -lmd provides SHA2Init/SHA2Update/SHA2Final, used by the dedup hash stage
# (zstream_blake3.c), which on illumos computes SHA-256 in place of BLAKE3.
LDLIBS += -lzpool -lzfs -lumem -lnvpair -lzutil -lfakekernel -lpthread -lmd

CSTD=	$(CSTD_GNU99)

CPPFLAGS.first = -I$(SRC)/lib/libfakekernel/common -D_FAKE_KERNEL
CPPFLAGS += -D_LARGEFILE64_SOURCE=1 -D_REENTRANT $(INCS)
$(NOT_RELEASE_BUILD)CPPFLAGS += -DDEBUG

CERRWARN += -_gcc=-Wno-switch

# not linted
SMATCH=off

.KEEP_STATE:

.PARALLEL:

all: $(PROG)

$(PROG): $(OBJS)
	$(LINK.c) -o $@ $(OBJS) $(LDLIBS)
	$(POST_PROCESS)

install: all $(ROOTUSRSBINPROG)

clean:
	$(RM) $(OBJS)

FRC:

include ../Makefile.targ
