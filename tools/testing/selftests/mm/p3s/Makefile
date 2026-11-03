# SPDX-License-Identifier: GPL-2.0
# P3S (per-process page size) selftests for native 16K arm64 kernels.

ifeq ($(ARCH),)
ifeq ($(CROSS_COMPILE),)
uname_M := $(shell uname -m 2>/dev/null || echo not)
else
uname_M := $(shell echo $(CROSS_COMPILE) | grep -o '^[a-z0-9]\+')
endif
ARCH ?= $(shell echo $(uname_M) | sed -e 's/aarch64.*/arm64/' -e 's/ppc64.*/powerpc/')
endif

.DELETE_ON_ERROR:
MAKEFLAGS += --no-builtin-rules
CFLAGS = -Wall -O2 -I$(top_srcdir) -I.. -I../.. $(EXTRA_CFLAGS) $(KHDR_INCLUDES) $(TOOLS_INCLUDES)
CFLAGS += -Wunreachable-code -U_FORTIFY_SOURCE
LDLIBS = -lrt -lpthread -lm
# The launcher starts as a native 16K process.  Only elf_4k_align_probe is
# intentionally linked with 4K PT_LOAD alignment.
LDFLAGS += -Wl,-z,max-page-size=16384

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
	packed_swap_p3s page_range_p3s pagemap_ioctl_p3s pagemap_scan_p3s pagewalk_p3s pci_mmap_p3s \
	perf_mmap_offset_p3s perf_phys_addr_p3s \
	print_vma_addr_p3s proc_pagewalk_p3s process_madvise_p3s \
	process_mrelease_p3s process_vm_access_p3s remap_file_pages_p3s \
	procfs_native_rmap_p3s \
	remap_pfn_cow_p3s remap_pfn_range_p3s pfn_insert_p3s rmap_file_referenced_p3s rss_stat_trace_p3s \
	remote_write_dontneed_p3s \
	rusage_p3s selinux_status_mmap_p3s \
	shmem_mmap_offset_p3s \
	shmem_swap_usage_p3s smaps_native_fallback_p3s smaps_page_size_p3s \
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
TEST_GEN_FILES := $(P3S_TESTS) binder_alloc_p3s pagemap_ioctl \
	bpf_remote_access_p3s drm_gem_slice_p3s futex_uffd_p3s \
	iov_range_retry_p3s pagemap_exclusive_p3s perf_page_size_p3s \
	smaps_pss_p3s uprobe_p3s
ifeq ($(ARCH),arm64)
TEST_GEN_FILES += $(P3S_TESTS_arm64)
endif
TEST_PROGS := run_p3stests.sh
TEST_FILES := README.p3s p3s_run_module.sh \
	cow_notifier_p3s.sh iommu_dma_mmap_p3s.sh mmu_notifier_p3s.sh \
	selinux_status_mmap_p3s.sh v9fs_mmap_close_p3s.sh vmalloc_align_p3s.sh
KDIR ?= $(if $(O),$(O),/lib/modules/$(shell uname -r)/build)
LOCAL_HDRS += kselftest_p3s.h

include ../../lib.mk

P3S_MM_TARGETS := $(addprefix $(OUTPUT)/,$(P3S_TESTS) $(P3S_TESTS_arm64) \
	bpf_remote_access_p3s drm_gem_slice_p3s futex_uffd_p3s \
	iov_range_retry_p3s pagemap_exclusive_p3s perf_page_size_p3s \
	smaps_pss_p3s uprobe_p3s)
$(filter-out $(OUTPUT)/taskstats_p3s,$(P3S_MM_TARGETS)): ../vm_util.c ../thp_settings.c
$(OUTPUT)/pagemap_ioctl: ../pagemap_ioctl.c ../vm_util.c ../thp_settings.c
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@
$(OUTPUT)/anon_vma_name_p3s $(OUTPUT)/iov_iter_p3s \
$(OUTPUT)/mremap_pgoff_p3s: LDFLAGS += -pie
$(OUTPUT)/binder_alloc_p3s: CFLAGS += -pthread
$(OUTPUT)/taskstats_p3s: CFLAGS := -Wall -O2 $(EXTRA_CFLAGS)
$(OUTPUT)/taskstats_p3s: CFLAGS += -Wunreachable-code -U_FORTIFY_SOURCE
$(OUTPUT)/taskstats_p3s: CFLAGS += -I$(selfdir) -I.. -I../..

$(OUTPUT)/brk_aslr_p3s_sample: brk_aslr_p3s_sample.S
	$(CC) -nostdlib -static -no-pie -Wl,-z,max-page-size=16384 $< -o $@
$(OUTPUT)/exec_aslr_p3s_helper: exec_aslr_p3s_helper.S
	$(CC) -nostdlib -pie -Wl,-e,_start \
		-Wl,--dynamic-linker=/proc/self/fd/3 -Wl,--build-id=none \
		-Wl,-z,max-page-size=16384 $< -o $@
$(OUTPUT)/exec_aslr_p3s_interp: exec_aslr_p3s_interp.S
	$(CC) -nostdlib -static-pie -Wl,-e,_start -Wl,--build-id=none \
		-Wl,-z,max-page-size=16384 $< -o $@
$(OUTPUT)/elf_4k_align_probe: elf_4k_align_probe.S
	$(CC) -nostdlib -static -Wl,-no-pie -Wl,--build-id=none \
		-Wl,-z,max-page-size=4096 $< -o $@

# Shared headers used by userspace tests and their fixture modules.
define p3s_test_hdrs
$(OUTPUT)/$(1): LOCAL_HDRS += $(2)
$(OUTPUT)/$(1): $(2)
endef
$(eval $(call p3s_test_hdrs,fault_in_p3s,fault_in_p3s.h))
$(eval $(call p3s_test_hdrs,folio_within_range_p3s,folio_within_range_p3s.h))
$(eval $(call p3s_test_hdrs,pfn_insert_p3s,pfn_insert_p3s.h))
$(eval $(call p3s_test_hdrs,gup_retry_p3s,gup_retry_p3s.h))
$(eval $(call p3s_test_hdrs,iov_iter_p3s,iov_iter_p3s.h))
$(eval $(call p3s_test_hdrs,iov_range_retry_p3s,iov_range_retry_p3s.h))
$(eval $(call p3s_test_hdrs,page_range_p3s,iov_iter_p3s.h page_range_p3s.h))
$(eval $(call p3s_test_hdrs,file_cow_refs_p3s,iov_iter_p3s.h page_range_p3s.h))
$(eval $(call p3s_test_hdrs,mmu_notifier_p3s,mmu_notifier_p3s.h))
$(eval $(call p3s_test_hdrs,cow_notifier_p3s,mmu_notifier_p3s.h))
$(eval $(call p3s_test_hdrs,pagewalk_p3s,pagewalk_p3s.h))
$(eval $(call p3s_test_hdrs,vm_map_pages_p3s,vm_map_pages_p3s.h))
$(eval $(call p3s_test_hdrs,elf_core_p3s,iov_iter_p3s.h))
$(foreach t,dontneed_refault_p3s file_cow_p3s hole_anon_p3s mte_anon_p3s packed_anon_p3s \
	packed_swap_p3s uffd_copy_tuple_p3s uffd_move_swap_window_p3s \
	remote_write_dontneed_p3s anon_rmap_split_window_p3s \
	uffd_move_swapped_slice_p3s uffd_move_swapcache_lock_p3s uffd_move_policy_p3s \
	swapoff_shared_slice_p3s vmsplice_slice_p3s, \
	$(eval $(call p3s_test_hdrs,$(t),p3s_tuple_test.h)))

all: p3s_modules
.PHONY: p3s_modules clean_p3s_modules install_p3s_modules
# A userspace CC (for example the Android NDK wrapper) must not leak into
# Kbuild, which needs the compiler that produced the tested kernel.
p3s_module_cc = $(if $(LLVM),$(if $(filter 1,$(LLVM)),clang,$(if $(filter -%,$(LLVM)),clang$(LLVM),$(LLVM)clang)),$(CROSS_COMPILE)gcc)
p3s_modules:
	$(MAKE) -C p3s_modules KDIR=$(KDIR) O=$(KDIR) CC=$(p3s_module_cc)
clean: clean_p3s_modules
clean_p3s_modules:
	$(MAKE) -C p3s_modules KDIR=$(KDIR) O=$(KDIR) clean
install: install_p3s_modules
install_p3s_modules: p3s_modules
	@if [ -n "$(INSTALL_PATH)" ]; then \
		mkdir -p "$(INSTALL_PATH)/p3s_modules"; \
		for f in p3s_modules/*.ko; do \
			[ ! -f "$$f" ] || cp "$$f" "$(INSTALL_PATH)/p3s_modules/"; \
		done; \
	fi
