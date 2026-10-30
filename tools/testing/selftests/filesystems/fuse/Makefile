# SPDX-License-Identifier: GPL-2.0-or-later

CFLAGS += -Wall -O2 -g $(KHDR_INCLUDES)

TEST_GEN_PROGS := fusectl_test
TEST_GEN_PROGS += write_extend_eof_test
TEST_GEN_PROGS += fuse_test fuse_daemon
TEST_GEN_PROGS += fuse_bpf_android_test
TEST_GEN_FILES := fuse_mnt
TEST_GEN_FILES += test_bpf.bpf fd_bpf.bpf fd.sh

# fuse_acl_cache_test requires libfuse3; add it only when the library is present.
ACL_CFLAGS := $(shell pkg-config fuse3 --cflags 2>/dev/null)
ACL_LDLIBS := $(shell pkg-config fuse3 --libs 2>/dev/null)
ifneq ($(ACL_CFLAGS),)
TEST_GEN_PROGS += fuse_acl_cache_test
endif

EXTRA_CLEAN := *.bpf *.ir

include ../../lib.mk

BPF_ARCH := $(if $(ARCH),$(ARCH),$(shell uname -m))
SRCARCH := $(shell echo $(BPF_ARCH) | sed -e s/i.86/x86/ -e s/x86_64/x86/ \
					  -e s/aarch64.*/arm64/ -e s/arm.*/arm/)
MULTIARCH := $(shell $(CC) -print-multiarch 2>/dev/null)
BPF_FLAGS = -Wall -Werror -O2 -g -emit-llvm \
	    -I $(top_srcdir)/include \
	    -idirafter /usr/local/include \
	    $(if $(MULTIARCH),-idirafter /usr/include/$(MULTIARCH)) \
	    -idirafter /usr/include \
	    -idirafter $(top_srcdir)/arch/$(SRCARCH)/include

$(OUTPUT)/write_extend_eof_test: LDLIBS += -lpthread

VAR_CFLAGS := $(shell pkg-config fuse --cflags 2>/dev/null)
ifeq ($(VAR_CFLAGS),)
VAR_CFLAGS := -D_FILE_OFFSET_BITS=64 -I/usr/include/fuse
endif

VAR_LDLIBS := $(shell pkg-config fuse --libs 2>/dev/null)
ifeq ($(VAR_LDLIBS),)
VAR_LDLIBS := -lfuse -pthread
endif

$(OUTPUT)/fuse_mnt: CFLAGS += $(VAR_CFLAGS)
$(OUTPUT)/fuse_mnt: LDLIBS += $(VAR_LDLIBS)

$(OUTPUT)/fuse_acl_cache_test: CFLAGS += $(ACL_CFLAGS)
$(OUTPUT)/fuse_acl_cache_test: LDLIBS += $(ACL_LDLIBS)

FUSE_BPF_PROGS := $(OUTPUT)/fuse_test $(OUTPUT)/fuse_daemon
$(FUSE_BPF_PROGS): bpf_loader.c
$(FUSE_BPF_PROGS): CFLAGS += -D_FILE_OFFSET_BITS=64 -I$(selfdir) -I$(top_srcdir) \
			     -I$(top_srcdir)/tools/include
$(FUSE_BPF_PROGS): LDLIBS += -lpthread -lelf

$(OUTPUT)/fuse_bpf_android_test: CFLAGS += -D_FILE_OFFSET_BITS=64 \
					   -I$(top_srcdir)/include/uapi
$(OUTPUT)/fuse_bpf_android_test: LDLIBS += -lpthread

$(OUTPUT)/%.ir: %.c
	clang $(BPF_FLAGS) -c $< -o $@

$(OUTPUT)/%.bpf: $(OUTPUT)/%.ir
	llc -march=bpf -filetype=obj -o $@ $<

$(OUTPUT)/fd.sh: fd.txt
	cp $< $@
	chmod 755 $@
