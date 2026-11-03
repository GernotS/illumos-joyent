#
# This file and its contents are supplied under the terms of the
# Common Development and Distribution License ("CDDL"), version 1.0.
# You may only use this file in accordance with the terms of version
# 1.0 of the CDDL.
#
# A full copy of the text of the CDDL should have accompanied this
# source.  A copy of the CDDL is also available via the Internet at
# http://www.illumos.org/license/CDDL.
#

#
# Copyright 2021 Toomas Soome <tsoome@me.com>
#

SRCS +=	$(ZFSSRC)/zfs.c
SRCS +=	$(ZFSSRC)/zstd_boot.c
SRCS +=	$(ZFSSRC)/gzip.c
SRCS +=	$(SRC)/common/list/list.c
OBJECTS +=	zfs.o
OBJECTS +=	zstd_boot.o
OBJECTS +=	gzip.o
OBJECTS +=	nvlist.o
OBJECTS +=	list.o

objs/zfs.o pics/zfs.o := CPPFLAGS +=	-I$(BOOTSRC)/common
objs/zfs.o pics/zfs.o := CPPFLAGS +=	-I$(BOOTSRC)/sys/cddl/boot/zfs -I$(LZ4)
objs/zfs.o pics/zfs.o := CPPFLAGS +=	-I$(SRC)/uts/common/fs/zfs
objs/zfs.o pics/zfs.o := CPPFLAGS +=	-I$(CRYPTOSRC)
objs/nvlist.o pics/nvlist.o := CPPFLAGS +=	-I$(BOOTSRC)/common

# zstd_boot.c compiles the unmodified upstream zstd decompressor sources;
# do not hold them to illumos warning or smatch policy.
objs/zstd_boot.o pics/zstd_boot.o := CPPFLAGS +=	-I$(SRC)/common/zstd/lib
objs/zstd_boot.o pics/zstd_boot.o := CFLAGS +=	-_gcc=-w
objs/zstd_boot.o pics/zstd_boot.o := SMATCH =	off
objs/nvlist.o pics/nvlist.o := CPPFLAGS +=	-I$(BOOTSRC)/sys/cddl/boot/zfs

pics/%.o objs/%.o:	$(ZFSSRC)/%.c
	$(COMPILE.c) -o $@ $<

pics/%.o objs/%.o:	$(SRC)/common/list/%.c
	$(COMPILE.c) -DNDEBUG -o $@ $<

zfs.o: $(ZFSSRC)/zfsimpl.c
