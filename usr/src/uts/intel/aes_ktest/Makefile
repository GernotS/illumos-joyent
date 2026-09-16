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
# Copyright 2026 RackTop Systems, Inc.  All rights reserved.
#

#
# uts/intel/aes_ktest/Makefile
#

UTSBASE	= ../..

MODULE		= aes_ktest
OBJECTS		= $(AES_KTEST_OBJS:%=$(OBJS_DIR)/%)
ROOTMODULE	= $(USR_KTEST_DIR)/$(MODULE)

include $(UTSBASE)/intel/Makefile.intel

ALL_TARGET	= $(BINARY)
INSTALL_TARGET	= $(BINARY) $(ROOTMODULE)

#
# drv/ktest: provides ktest_create_module, ktest_add_suite, ktest_add_test, etc.
# misc/kcf:  provides crypto_mech2id, crypto_encrypt, crypto_decrypt_*.
#
LDFLAGS		+= -N drv/ktest -N misc/kcf

#
# The source lives in the crypto-tests ktest directory rather than in
# uts/common.  Provide an explicit rule since Makefile.rules has no
# pattern for usr/src/test/.
#
AES_GCM_KTEST_SRC = $(SRC)/test/crypto-tests/tests/modes/aes/ktest

.KEEP_STATE:

def:		$(DEF_DEPS)

all:		$(ALL_DEPS)

clean:		$(CLEAN_DEPS)

clobber:	$(CLOBBER_DEPS)

install:	$(INSTALL_DEPS)

$(OBJS_DIR)/aes_gcm_ktest.o: $(AES_GCM_KTEST_SRC)/aes_gcm_ktest.c
	$(COMPILE.c) -o $@ $(AES_GCM_KTEST_SRC)/aes_gcm_ktest.c
	$(CTFCONVERT_O)

$(OBJS_DIR)/aes_ctr_ktest.o: $(AES_GCM_KTEST_SRC)/aes_ctr_ktest.c
	$(COMPILE.c) -o $@ $(AES_GCM_KTEST_SRC)/aes_ctr_ktest.c
	$(CTFCONVERT_O)

include $(UTSBASE)/intel/Makefile.targ
