# SPDX-License-Identifier: GPL-2.0

CFLAGS += $(KHDR_INCLUDES) -pthread
TEST_GEN_PROGS := binderfs_test binder_alloc_p3s

include ../../lib.mk

$(OUTPUT)/binder_alloc_p3s: LOCAL_HDRS += ../../kselftest_p3s.h
$(OUTPUT)/binder_alloc_p3s: ../../kselftest_p3s.h
