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
#	This makefile drives the production of the zfs file system
#	kernel module.
#
# Copyright 2013 Saso Kiselkov. All rights reserved.
#
# Copyright (c) 2016 by Delphix. All rights reserved.
#
# Copyright 2019 Joyent, Inc.

#
#	Path to the base of the uts directory tree (usually /usr/src/uts).
#
UTSBASE	= ../..

ARCHDIR:sh = cd ..; basename `pwd`

#
#	Define the module and object file sets.
#
MODULE		= zfs
ZSTD_OBJECTS	= $(ZSTD_OBJS:%=$(OBJS_DIR)/%)
OBJECTS		= $(ZFS_OBJS:%=$(OBJS_DIR)/%) $(LUA_OBJS:%=$(OBJS_DIR)/%) \
		  $(ZSTD_OBJECTS)
ROOTMODULE	= $(ROOT_DRV_DIR)/$(MODULE)
ROOTLINK	= $(ROOT_FS_DIR)/$(MODULE)
CONF_SRCDIR	= $(UTSBASE)/common/fs/zfs

#
#	Include common rules.
#
include ../Makefile.$(ARCHDIR)

#
#	Define targets
#
ALL_TARGET	= $(BINARY) $(SRC_CONFILE)
INSTALL_TARGET	= $(BINARY) $(ROOTMODULE) $(ROOTLINK) $(ROOT_CONFFILE)

#
#	Overrides and depends_on
#
LDFLAGS		+= -Nfs/specfs -Ncrypto/swrand -Nmisc/idmap -Nmisc/sha2 \
	-Nmisc/skein -Nmisc/edonr

INC_PATH	+= -I$(UTSBASE)/common/fs/zfs
INC_PATH	+= -I$(UTSBASE)/common/fs/zfs/lua
INC_PATH	+= -I$(SRC)/common
INC_PATH	+= -I$(COMMONBASE)/zfs
INC_PATH	+= -I$(UTSBASE)/i86pc

#
# For now, disable these warnings; maintainers should endeavor
# to investigate and remove these for maximum coverage.
# Please do not carry these forward to new Makefiles.
#

CERRWARN	+= -_gcc=-Wno-type-limits
CERRWARN	+= -_gcc=-Wno-switch
CERRWARN	+= -_gcc=-Wno-parentheses
CERRWARN	+= -_gcc=-Wno-unused-variable
CERRWARN	+= -_gcc=-Wno-unused-function

# needs work
SMOFF += all_func_returns,indenting
$(OBJS_DIR)/llex.o := SMOFF += index_overflow
$(OBJS_DIR)/metaslab.o := SMOFF += no_if_block
$(OBJS_DIR)/zfs_vnops.o := SMOFF += signed
# needs work
$(OBJS_DIR)/zvol.o := SMOFF += deref_check,signed

# false positive
$(OBJS_DIR)/zfs_ctldir.o := SMOFF += strcpy_overflow

#
# The zstd library is unmodified upstream code (see usr/src/common/zstd):
# build it the way OpenZFS does, and do not hold it to illumos warning and
# smatch policy. Assembly and SIMD intrinsics are disabled, as the kernel
# may not use the FPU.
#
$(ZSTD_OBJECTS) := CPPFLAGS += -I$(COMMONBASE)/zstd/include \
	-DZSTD_DISABLE_ASM -DZSTD_NO_INTRINSICS -DZSTD_TRACE=0 \
	-DMEM_MODULE -DXXH_NAMESPACE=ZSTD_ -DXXH_PRIVATE_API -DXXH_INLINE_ALL \
	-DZSTD_LEGACY_SUPPORT=0 -DZSTD_LIB_DICTBUILDER=0 \
	-DZSTD_LIB_DEPRECATED=0 -DZSTD_NOBENCH -DDEBUGLEVEL=0 \
	-DZSTD_DEPS_ASSERT
$(ZSTD_OBJECTS) := CERRWARN += -_gcc=-w
$(ZSTD_OBJECTS) := SMATCH = off

#
#	Default build targets.
#
.KEEP_STATE:

def:		$(DEF_DEPS)

all:		$(ALL_DEPS)

clean:		$(CLEAN_DEPS)

clobber:	$(CLOBBER_DEPS)

install:	$(INSTALL_DEPS)

$(ROOTLINK):	$(ROOT_FS_DIR) $(ROOTMODULE)
	-$(RM) $@; ln $(ROOTMODULE) $@

#
#	Include common targets.
#
include ../Makefile.targ
