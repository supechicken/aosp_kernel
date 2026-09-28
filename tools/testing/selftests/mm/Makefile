# SPDX-License-Identifier: GPL-2.0
# Makefile for mm selftests

LOCAL_HDRS += $(selfdir)/mm/local_config.h $(top_srcdir)/mm/gup_test.h
LOCAL_HDRS += $(selfdir)/mm/mseal_helpers.h

include local_config.mk

ifeq ($(ARCH),)

ifeq ($(CROSS_COMPILE),)
uname_M := $(shell uname -m 2>/dev/null || echo not)
else
uname_M := $(shell echo $(CROSS_COMPILE) | grep -o '^[a-z0-9]\+')
endif
ARCH ?= $(shell echo $(uname_M) | sed -e 's/aarch64.*/arm64/' -e 's/ppc64.*/powerpc/')
endif

# Without this, failed build products remain, with up-to-date timestamps,
# thus tricking Make (and you!) into believing that All Is Well, in subsequent
# make invocations:
.DELETE_ON_ERROR:

# Avoid accidental wrong builds, due to built-in rules working just a little
# bit too well--but not quite as well as required for our situation here.
#
# In other words, "make $SOME_TEST" is supposed to fail to build at all,
# because this Makefile only supports either "make" (all), or "make /full/path".
# However,  the built-in rules, if not suppressed, will pick up CFLAGS and the
# initial LDLIBS (but not the target-specific LDLIBS, because those are only
# set for the full path target!). This causes it to get pretty far into building
# things despite using incorrect values such as an *occasionally* incomplete
# LDLIBS.
MAKEFLAGS += --no-builtin-rules

CFLAGS = -Wall -O2 -I $(top_srcdir) $(EXTRA_CFLAGS) $(KHDR_INCLUDES) $(TOOLS_INCLUDES)
CFLAGS += -Wunreachable-code
LDLIBS = -lrt -lpthread -lm

# Some distributions (such as Ubuntu) configure GCC so that _FORTIFY_SOURCE is
# automatically enabled at -O1 or above. This triggers various unused-result
# warnings where functions such as read() or write() are called and their
# return value is not checked. Disable _FORTIFY_SOURCE to silence those
# warnings.
CFLAGS += -U_FORTIFY_SOURCE

# P3S (per-process page size) tests.  Those needing a kernel-side fixture get
# it from the modules in p3s_modules/, loaded by ../p3s/p3s_run_module.sh.
P3S_TESTS := \
	zero_sibling_mlock_p3s partial_anon_reclaim_p3s \
	devzero_slice_p3s \
	access_phys_p3s \
	aio_ring_p3s alsa_pcm_mmap_p3s anon_pageout_p3s anon_vma_name_p3s ashmem_p3s \
	anon_rmap_split_window_p3s \
	bpf_map_mmap_p3s bpf_stack_build_id_p3s \
	btf_vmlinux_mmap_p3s cow_notifier_p3s \
	dma_heap_p3s dma_mmap_p3s devzero_mthp_p3s dontneed_refault_p3s elf_core_p3s exec_env_p3s \
	dma_mmap_full_p3s dma_mmap_bounds_p3s \
	exec_user_args_p3s fadvise_dontneed_p3s fault_in_p3s \
	file_cow_p3s file_cow_refs_p3s filemap_eof_p3s folio_within_range_p3s \
	fork_dontfork_slice_p3s fork_filemap_p3s fork_page_size_p3s \
	futex_shared_p3s \
	gup_retry_p3s hole_anon_p3s \
	io_uring_mmap_p3s iommu_dma_mmap_p3s iov_iter_p3s kcov_p3s \
	madv_cold_large_file_p3s madvise_free_p3s madvise_free_holes_p3s \
	madvise_guard_p3s \
	madvise_remove_p3s madvise_willneed_p3s mbind_p3s memfd_eof_p3s \
	memory_failure_anon_p3s mglru_folded_p3s mglru_p3s mincore_p3s mixed_context_switch_p3s \
	mixed_mm_access_p3s mlock_large_file_p3s mlock_file_slices_p3s mlock_onfault_p3s \
	mlock_partial_file_p3s mlock_reclaim_p3s \
	mmap_action_cleanup_p3s mmap_hint_p3s mmap_page_zero_p3s \
	mmu_notifier_p3s mremap_commit_p3s mremap_file_slice_p3s mremap_hint_p3s mremap_split_p3s \
	mremap_pgoff_p3s mremap_rlimit_p3s msync_offset_p3s munmap_p3s \
	mthp_native_anon_p3s \
	oom_anon_score_p3s oom_score_p3s oom_trace_p3s packed_anon_p3s \
	packed_swap_p3s page_range_p3s  pagemap_ioctl_p3s pagemap_scan_p3s pagewalk_p3s pci_mmap_p3s \
	perf_mmap_offset_p3s  perf_phys_addr_p3s \
	print_vma_addr_p3s proc_pagewalk_p3s process_madvise_p3s \
	process_mrelease_p3s process_vm_access_p3s remap_file_pages_p3s \
	procfs_native_rmap_p3s \
	remap_pfn_cow_p3s remap_pfn_range_p3s rmap_file_referenced_p3s rss_stat_trace_p3s \
	remote_write_dontneed_p3s \
	rusage_p3s selinux_status_mmap_p3s \
	shmem_mmap_offset_p3s \
	shmem_swap_usage_p3s smaps_native_fallback_p3s smaps_page_size_p3s \
	 \
	stack_guard_gap_p3s swap_header_p3s swap_pss_p3s swap_vma_readahead_p3s swapoff_p3s \
	swapoff_shared_slice_p3s \
	statm_cross_geometry_p3s \
	swap_header_badpages_p3s swap_badpages_loop_p3s \
	sysv_shm_lock_p3s sysv_shm_p3s taskstats_p3s \
	tcp_zerocopy_align_p3s tcp_zerocopy_reject_p3s thp_mmap_offset_p3s \
	trace_ring_buffer_mmap_p3s truncate_p3s truncate_remote_p3s truncate_tail_p3s \
	userfaultfd_mixed_p3s userfaultfd_poison_remote_p3s uffd_move_swap_window_p3s \
	uffd_move_swapcache_lock_p3s uffd_move_policy_p3s \
	uffd_move_swapped_slice_p3s \
	userfaultfd_remap_p3s userfaultfd_remote_p3s \
	userfaultfd_shmem_p3s \
	userfaultfd_unregister_remote_p3s userfaultfd_wp_remote_p3s \
	uffd_sigbus_anon_p3s \
	v9fs_mmap_close_p3s vb2_mmap_p3s vb2_userptr_reject_p3s \
	vhost_dirty_log_p3s vm_iomap_memory_p3s \
	vma_range_slice_p3s \
	vm_map_pages_p3s vmalloc_align_p3s xdp_umem_p3s \
	vmsplice_slice_p3s \
	vma_merge_file_slice_p3s \
	vma_merge_next_pgoff_p3s vma_merge_devzero_p3s

P3S_TESTS += uffd_copy_tuple_p3s mremap_slice_cow_p3s swap_slice_mremap_p3s

P3S_TESTS_arm64 := \
	brk_aslr_p3s brk_aslr_p3s_sample exec_aslr_p3s \
	elf_4k_align_p3s elf_4k_align_probe \
	exec_aslr_p3s_helper exec_aslr_p3s_interp exec_stack_p3s \
	exec_stack_random_p3s mmap_aslr_p3s mte_anon_p3s \
	shmem_thp_mmap_p3s stack_aslr_p3s

TEST_GEN_FILES = cow
TEST_GEN_FILES += compaction_test
TEST_GEN_FILES += gup_longterm
TEST_GEN_FILES += gup_test
TEST_GEN_FILES += hmm-tests
TEST_GEN_FILES += hugetlb-madvise
TEST_GEN_FILES += hugetlb-read-hwpoison
TEST_GEN_FILES += hugetlb-soft-offline
TEST_GEN_FILES += hugepage-mmap
TEST_GEN_FILES += hugepage-mremap
TEST_GEN_FILES += hugepage-shm
TEST_GEN_FILES += hugepage-vmemmap
TEST_GEN_FILES += khugepaged
TEST_GEN_FILES += madv_populate
TEST_GEN_FILES += map_fixed_noreplace
TEST_GEN_FILES += map_hugetlb
TEST_GEN_FILES += map_populate
ifneq (,$(filter $(ARCH),arm64 riscv riscv64 x86 x86_64 loongarch32 loongarch64))
TEST_GEN_FILES += memfd_secret
endif
TEST_GEN_FILES += migration
TEST_GEN_FILES += mkdirty
TEST_GEN_FILES += mlock-random-test
TEST_GEN_FILES += mlock2-tests
TEST_GEN_FILES += mrelease_test
TEST_GEN_FILES += mremap_dontunmap
TEST_GEN_FILES += mremap_test
TEST_GEN_FILES += mseal_test
TEST_GEN_FILES += on-fault-limit
TEST_GEN_FILES += pagemap_ioctl
TEST_GEN_FILES += pfnmap
TEST_GEN_FILES += process_madv
TEST_GEN_FILES += prctl_thp_disable
TEST_GEN_FILES += thuge-gen
TEST_GEN_FILES += transhuge-stress
TEST_GEN_FILES += uffd-stress
TEST_GEN_FILES += uffd-unit-tests
TEST_GEN_FILES += uffd-wp-mremap
TEST_GEN_FILES += split_huge_page_test
TEST_GEN_FILES += ksm_tests
TEST_GEN_FILES += ksm_functional_tests
TEST_GEN_FILES += mdwe_test
TEST_GEN_FILES += hugetlb_fault_after_madv
TEST_GEN_FILES += hugetlb_madv_vs_map
TEST_GEN_FILES += hugetlb_dio
TEST_GEN_FILES += droppable
TEST_GEN_FILES += guard-regions
TEST_GEN_FILES += merge
TEST_GEN_FILES += rmap
TEST_GEN_FILES += $(P3S_TESTS)
ifeq ($(ARCH),arm64)
TEST_GEN_FILES += $(P3S_TESTS_arm64)
endif

ifneq ($(ARCH),arm64)
TEST_GEN_FILES += soft-dirty
endif

ifeq ($(ARCH),x86_64)
CAN_BUILD_I386 := $(shell ./../x86/check_cc.sh "$(CC)" ../x86/trivial_32bit_program.c -m32)
CAN_BUILD_X86_64 := $(shell ./../x86/check_cc.sh "$(CC)" ../x86/trivial_64bit_program.c)
CAN_BUILD_WITH_NOPIE := $(shell ./../x86/check_cc.sh "$(CC)" ../x86/trivial_program.c -no-pie)

VMTARGETS := protection_keys
VMTARGETS += pkey_sighandler_tests
BINARIES_32 := $(VMTARGETS:%=%_32)
BINARIES_64 := $(VMTARGETS:%=%_64)

ifeq ($(CAN_BUILD_WITH_NOPIE),1)
CFLAGS += -no-pie
endif

ifeq ($(CAN_BUILD_I386),1)
TEST_GEN_FILES += $(BINARIES_32)
endif

ifeq ($(CAN_BUILD_X86_64),1)
TEST_GEN_FILES += $(BINARIES_64)
endif

else ifeq ($(ARCH),arm64)
TEST_GEN_FILES += protection_keys
TEST_GEN_FILES += pkey_sighandler_tests
else ifeq ($(ARCH),powerpc)
TEST_GEN_FILES += protection_keys
endif

ifneq (,$(filter $(ARCH),arm64 mips64 parisc64 powerpc riscv64 s390x sparc64 x86_64 s390))
TEST_GEN_FILES += va_high_addr_switch
ifneq ($(ARCH),riscv64)
TEST_GEN_FILES += virtual_address_range
endif
TEST_GEN_FILES += write_to_hugetlbfs
endif

TEST_PROGS := run_vmtests.sh vmalloc_align_p3s.sh

TEST_FILES := test_vmalloc.sh README.p3s
TEST_FILES += test_hmm.sh
TEST_FILES += va_high_addr_switch.sh
TEST_FILES += charge_reserved_hugetlb.sh
TEST_FILES += hugetlb_reparenting_test.sh
TEST_FILES += cow_notifier_p3s.sh iommu_dma_mmap_p3s.sh mmu_notifier_p3s.sh \
	      selinux_status_mmap_p3s.sh v9fs_mmap_close_p3s.sh \
	      ../p3s/p3s_run_module.sh

# required by charge_reserved_hugetlb.sh
TEST_FILES += write_hugetlb_memory.sh

KDIR ?= /lib/modules/$(shell uname -r)/build
TEST_FILES += test_page_frag.sh
ifneq (,$(wildcard $(KDIR)/Module.symvers))
ifneq (,$(wildcard $(KDIR)/include/linux/page_frag_cache.h))
TEST_GEN_MODS_DIR := page_frag
else
PAGE_FRAG_WARNING = "missing page_frag_cache.h, please use a newer kernel"
endif
else
PAGE_FRAG_WARNING = "missing Module.symvers, please have the kernel built first"
endif

TEST_GEN_FILES += drm_gem_slice_p3s

TEST_GEN_FILES += bpf_remote_access_p3s futex_uffd_p3s iov_range_retry_p3s

TEST_GEN_FILES += uprobe_p3s

TEST_GEN_FILES += perf_page_size_p3s

TEST_GEN_FILES += pagemap_exclusive_p3s

TEST_GEN_FILES += smaps_pss_p3s

include ../lib.mk

# Keep shared launcher includes and dependencies when the bulk list replaces
# the individual early registrations.
P3S_MM_TARGETS := $(addprefix $(OUTPUT)/,$(P3S_TESTS) $(P3S_TESTS_arm64))
$(P3S_MM_TARGETS): CFLAGS += -I..
$(P3S_MM_TARGETS): LOCAL_HDRS += ../kselftest_p3s.h
$(P3S_MM_TARGETS): ../kselftest_p3s.h
$(OUTPUT)/elf_core_p3s: LOCAL_HDRS += iov_iter_p3s.h
$(OUTPUT)/elf_core_p3s: iov_iter_p3s.h
P3S_16K_LINK_TARGETS := \
	bpf_stack_build_id_p3s elf_core_p3s vb2_mmap_p3s \
	vb2_userptr_reject_p3s anon_vma_name_p3s iov_iter_p3s \
	mremap_pgoff_p3s pagemap_ioctl pagemap_ioctl_p3s pagemap_scan_p3s \
	process_madvise_p3s exec_aslr_p3s fork_filemap_p3s io_uring_mmap_p3s \
	munmap_p3s ashmem_p3s drm_gem_slice_p3s truncate_tail_p3s mlock_file_slices_p3s
$(addprefix $(OUTPUT)/,$(P3S_16K_LINK_TARGETS)): LDFLAGS += -Wl,-z,max-page-size=16384

$(OUTPUT)/brk_aslr_p3s_sample: brk_aslr_p3s_sample.S
	$(CC) -nostdlib -static -no-pie $< -o $@

$(OUTPUT)/exec_aslr_p3s_helper: exec_aslr_p3s_helper.S
	$(CC) -nostdlib -pie -Wl,-e,_start \
		-Wl,--dynamic-linker=/proc/self/fd/3 -Wl,--build-id=none \
		-Wl,-z,max-page-size=16384 $< -o $@

$(OUTPUT)/exec_aslr_p3s_interp: exec_aslr_p3s_interp.S
	$(CC) -nostdlib -static-pie -Wl,-e,_start -Wl,--build-id=none \
		-Wl,-z,max-page-size=16384 $< -o $@

$(TEST_GEN_PROGS): vm_util.c thp_settings.c
$(TEST_GEN_FILES): vm_util.c thp_settings.c

$(OUTPUT)/uffd-stress: uffd-common.c
$(OUTPUT)/uffd-unit-tests: uffd-common.c
$(OUTPUT)/uffd-wp-mremap: uffd-common.c
$(OUTPUT)/protection_keys: pkey_util.c
$(OUTPUT)/pkey_sighandler_tests: pkey_util.c

# Headers a P3S test shares with its fixture module or with the other tuple
# tests.  Declaring them per target (instead of in LOCAL_HDRS) keeps a header
# edit from rebuilding the whole directory; the target-specific LOCAL_HDRS
# keeps lib.mk's link rule from passing the header to the compiler.
define p3s_test_hdrs
$(OUTPUT)/$(1): LOCAL_HDRS += $(2)
$(OUTPUT)/$(1): $(2)
endef
$(eval $(call p3s_test_hdrs,fault_in_p3s,fault_in_p3s.h))
$(eval $(call p3s_test_hdrs,folio_within_range_p3s,folio_within_range_p3s.h))
$(eval $(call p3s_test_hdrs,gup_retry_p3s,gup_retry_p3s.h))
$(eval $(call p3s_test_hdrs,iov_iter_p3s,iov_iter_p3s.h))
$(eval $(call p3s_test_hdrs,iov_range_retry_p3s,iov_range_retry_p3s.h))
$(eval $(call p3s_test_hdrs,page_range_p3s,iov_iter_p3s.h page_range_p3s.h))
$(eval $(call p3s_test_hdrs,file_cow_refs_p3s,iov_iter_p3s.h page_range_p3s.h))
$(eval $(call p3s_test_hdrs,mmu_notifier_p3s,mmu_notifier_p3s.h))
$(eval $(call p3s_test_hdrs,cow_notifier_p3s,mmu_notifier_p3s.h))
$(eval $(call p3s_test_hdrs,pagewalk_p3s,pagewalk_p3s.h))
$(eval $(call p3s_test_hdrs,vm_map_pages_p3s,vm_map_pages_p3s.h))
$(foreach t,mincore_p3s mlock_onfault_p3s, \
	$(eval $(call p3s_test_hdrs,$(t),../kselftest_p3s.h)))
$(foreach t,dontneed_refault_p3s file_cow_p3s hole_anon_p3s mte_anon_p3s packed_anon_p3s \
	    packed_swap_p3s uffd_copy_tuple_p3s uffd_move_swap_window_p3s \
	    remote_write_dontneed_p3s anon_rmap_split_window_p3s \
	    uffd_move_swapped_slice_p3s uffd_move_swapcache_lock_p3s uffd_move_policy_p3s \
	    swapoff_shared_slice_p3s vmsplice_slice_p3s, \
	$(eval $(call p3s_test_hdrs,$(t),p3s_tuple_test.h)))

ifeq ($(ARCH),x86_64)
BINARIES_32 := $(patsubst %,$(OUTPUT)/%,$(BINARIES_32))
BINARIES_64 := $(patsubst %,$(OUTPUT)/%,$(BINARIES_64))

$(BINARIES_32) $(BINARIES_64): pkey_util.c

define gen-target-rule-32
$(1) $(1)_32: $(OUTPUT)/$(1)_32
.PHONY: $(1) $(1)_32
endef

define gen-target-rule-64
$(1) $(1)_64: $(OUTPUT)/$(1)_64
.PHONY: $(1) $(1)_64
endef

ifeq ($(CAN_BUILD_I386),1)
$(BINARIES_32): CFLAGS += -m32 -mxsave
$(BINARIES_32): LDLIBS += -lrt -ldl -lm
$(BINARIES_32): $(OUTPUT)/%_32: %.c
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) $(notdir $^) $(LDLIBS) -o $@
$(foreach t,$(VMTARGETS),$(eval $(call gen-target-rule-32,$(t))))
endif

ifeq ($(CAN_BUILD_X86_64),1)
$(BINARIES_64): CFLAGS += -m64 -mxsave
$(BINARIES_64): LDLIBS += -lrt -ldl
$(BINARIES_64): $(OUTPUT)/%_64: %.c
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) $(notdir $^) $(LDLIBS) -o $@
$(foreach t,$(VMTARGETS),$(eval $(call gen-target-rule-64,$(t))))
endif

# x86_64 users should be encouraged to install 32-bit libraries
ifeq ($(CAN_BUILD_I386)$(CAN_BUILD_X86_64),01)
all: warn_32bit_failure

warn_32bit_failure:
	@echo "Warning: you seem to have a broken 32-bit build" 2>&1;		\
	echo  "environment. This will reduce test coverage of 64-bit" 2>&1;	\
	echo  "kernels. If you are using a Debian-like distribution," 2>&1;	\
	echo  "try:"; 2>&1;							\
	echo  "";								\
	echo  "  apt-get install gcc-multilib libc6-i386 libc6-dev-i386";	\
	echo  "";								\
	echo  "If you are using a Fedora-like distribution, try:";		\
	echo  "";								\
	echo  "  yum install glibc-devel.*i686";				\
	exit 0;
endif
endif

# IOURING_EXTRA_LIBS may get set in local_config.mk, or it may be left empty.
$(OUTPUT)/cow: LDLIBS += $(IOURING_EXTRA_LIBS)

# Use one coherent set of libc UAPI headers for the taskstats netlink ABI.
$(OUTPUT)/taskstats_p3s: CFLAGS := -Wall -O2 $(EXTRA_CFLAGS)
$(OUTPUT)/taskstats_p3s: CFLAGS += -Wunreachable-code -U_FORTIFY_SOURCE
$(OUTPUT)/taskstats_p3s: CFLAGS += -I$(selfdir)

$(OUTPUT)/anon_vma_name_p3s $(OUTPUT)/iov_iter_p3s \
$(OUTPUT)/mremap_pgoff_p3s: LDFLAGS += -pie

$(OUTPUT)/gup_longterm: LDLIBS += $(IOURING_EXTRA_LIBS)

$(OUTPUT)/mlock-random-test $(OUTPUT)/memfd_secret: LDLIBS += -lcap

$(OUTPUT)/ksm_tests: LDLIBS += -lnuma

$(OUTPUT)/migration: LDLIBS += -lnuma

$(OUTPUT)/rmap: LDLIBS += -lnuma
$(OUTPUT)/elf_4k_align_probe: elf_4k_align_probe.S
	$(CC) -nostdlib -static -Wl,-no-pie -Wl,--build-id=none \
		-Wl,-z,max-page-size=4096 $< -o $@

local_config.mk local_config.h: check_config.sh
	/bin/sh ./check_config.sh $(CC)

EXTRA_CLEAN += local_config.mk local_config.h

ifeq ($(IOURING_EXTRA_LIBS),)
all: warn_missing_liburing

warn_missing_liburing:
	@echo ; \
	echo "Warning: missing liburing support. Some tests will be skipped." ; \
	echo
endif

ifneq ($(PAGE_FRAG_WARNING),)
all: warn_missing_page_frag

warn_missing_page_frag:
	@echo ; \
	echo "Warning: $(PAGE_FRAG_WARNING). page_frag test will be skipped." ; \
	echo
endif

$(eval $(call p3s_test_hdrs,drm_gem_slice_p3s,../kselftest_p3s.h))

# Do not make one fixture directory replace the upstream page_frag directory.
all: p3s_modules
.PHONY: p3s_modules clean_p3s_modules install_p3s_modules
p3s_modules:
	$(MAKE) -C p3s_modules KDIR=$(KDIR)

clean: clean_p3s_modules
clean_p3s_modules:
	$(MAKE) -C p3s_modules KDIR=$(KDIR) clean

install: install_p3s_modules
install_p3s_modules:
	@if [ -n "$(INSTALL_PATH)" ]; then \
		mkdir -p "$(INSTALL_PATH)/p3s_modules"; \
		for f in p3s_modules/*.ko; do \
			[ ! -f "$$f" ] || cp "$$f" "$(INSTALL_PATH)/p3s_modules/"; \
		done; \
	fi
