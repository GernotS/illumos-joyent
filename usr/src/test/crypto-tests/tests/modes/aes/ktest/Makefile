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
# Does NOT include Makefile.subdirs or Makefile.crypto -- both define
# install: with recipes that would conflict.  TESTROOT is set directly.
#
SCRIPTS		= aes_ktest
TESTROOT	= $(ROOT)/opt/crypto-tests/tests/aes
TESTDIR		= $(TESTROOT)/ktest
PROGS		= $(SCRIPTS:%=$(TESTDIR)/%)
$(PROGS)	:= FILEMODE = 0555

include $(SRC)/cmd/Makefile.cmd

.KEEP_STATE:

all:

install: $(PROGS)

clean:

clobber:

$(PROGS): $(TESTDIR)

$(TESTROOT):
	$(INS.dir)

$(TESTDIR): $(TESTROOT)
	$(INS.dir)

$(TESTDIR)/%: %.ksh
	$(INS.rename)
