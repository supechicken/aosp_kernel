#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Please run as root

# Kselftest framework requirement - SKIP code is 4.
ksft_skip=4

count_total=0
count_pass=0
count_fail=0
count_skip=0
exitcode=0

usage() {
	cat <<EOF
usage: ${BASH_SOURCE[0]:-$0} [ options ]

  -a: run all tests, including extra ones (other than destructive ones)
  -t: specify specific categories to tests to run
  -h: display this message
  -n: disable TAP output
  -d: run destructive tests

The default behavior is to run required tests only.  If -a is specified,
will run all tests.

Alternatively, specific groups tests can be run by passing a string
to the -t argument containing one or more of the following categories
separated by spaces:
- mmap
	tests for mmap(2)
- gup_test
	tests for gup
- userfaultfd
	tests for  userfaultfd(2)
- compaction
	a test for the patch "Allow compaction of unevictable pages"
- mlock
	tests for mlock(2)
- mremap
	tests for mremap(2)
- hugevm
	tests for very large virtual address space
- vmalloc
	vmalloc smoke tests
- hmm
	hmm smoke tests
- madv_guard
	test madvise(2) MADV_GUARD_INSTALL and MADV_GUARD_REMOVE options
- madv_populate
	test memadvise(2) MADV_POPULATE_{READ,WRITE} options
- memfd_secret
	test memfd_secret(2)
- process_mrelease
	test process_mrelease(2)
- ksm
	ksm tests that do not require >=2 NUMA nodes
- ksm_numa
	ksm tests that require >=2 NUMA nodes
- pkey
	memory protection key tests
- soft_dirty
	test soft dirty page bit semantics
- pagemap
	test pagemap_scan IOCTL
- pfnmap
	tests for VM_PFNMAP handling
- process_madv
	test for process_madv
- cow
	test copy-on-write semantics
- thp
	test transparent huge pages
- hugetlb
	test hugetlbfs huge pages
- migration
	invoke move_pages(2) to exercise the migration entry code
	paths in the kernel
- mkdirty
	test handling of code that might set PTE/PMD dirty in
	read-only VMAs
- mdwe
	test prctl(PR_SET_MDWE, ...)
- page_frag
	test handling of page fragment allocation and freeing
- vma_merge
	test VMA merge cases behave as expected
- rmap
	test rmap behaves as expected

example: ./run_vmtests.sh -t "hmm mmap ksm"
EOF
	exit 0
}

RUN_ALL=false
RUN_DESTRUCTIVE=false
TAP_PREFIX="# "

while getopts "aht:n" OPT; do
	case ${OPT} in
		"a") RUN_ALL=true ;;
		"h") usage ;;
		"t") VM_SELFTEST_ITEMS=${OPTARG} ;;
		"n") TAP_PREFIX= ;;
		"d") RUN_DESTRUCTIVE=true ;;
	esac
done
shift $((OPTIND -1))

# default behavior: run all tests
VM_SELFTEST_ITEMS=${VM_SELFTEST_ITEMS:-default}

test_selected() {
	if [ "$VM_SELFTEST_ITEMS" == "default" ]; then
		# If no VM_SELFTEST_ITEMS are specified, run all tests
		return 0
	fi
	# If test selected argument is one of the test items
	if [[ " ${VM_SELFTEST_ITEMS[*]} " =~ " ${1} " ]]; then
	        return 0
	else
	        return 1
	fi
}

run_gup_matrix() {
    # -t: thp=on, -T: thp=off, -H: hugetlb=on
    local hugetlb_mb=$(( needmem_KB / 1024 ))

    for huge in -t -T "-H -m $hugetlb_mb"; do
        # -u: gup-fast, -U: gup-basic, -a: pin-fast, -b: pin-basic, -L: pin-longterm
        for test_cmd in -u -U -a -b -L; do
            # -w: write=1, -W: write=0
            for write in -w -W; do
                # -S: shared
                for share in -S " "; do
                    # -n: How many pages to fetch together?  512 is special
                    # because it's default thp size (or 2M on x86), 123 to
                    # just test partial gup when hit a huge in whatever form
                    for num in "-n 1" "-n 512" "-n 123" "-n -1"; do
                        CATEGORY="gup_test" run_test ./gup_test \
                                $huge $test_cmd $write $share $num
                    done
                done
            done
        done
    done
}

# get huge pagesize and freepages from /proc/meminfo
while read -r name size unit; do
	if [ "$name" = "HugePages_Free:" ]; then
		freepgs="$size"
	fi
	if [ "$name" = "Hugepagesize:" ]; then
		hpgsize_KB="$size"
	fi
done < /proc/meminfo

# Simple hugetlbfs tests have a hardcoded minimum requirement of
# huge pages totaling 256MB (262144KB) in size.  The userfaultfd
# hugetlb test requires a minimum of 2 * nr_cpus huge pages.  Take
# both of these requirements into account and attempt to increase
# number of huge pages available.
nr_cpus=$(nproc)
uffd_min_KB=$((hpgsize_KB * nr_cpus * 2))
hugetlb_min_KB=$((256 * 1024))
if [[ $uffd_min_KB -gt $hugetlb_min_KB ]]; then
	needmem_KB=$uffd_min_KB
else
	needmem_KB=$hugetlb_min_KB
fi

# set proper nr_hugepages
if [ -n "$freepgs" ] && [ -n "$hpgsize_KB" ]; then
	orig_nr_hugepgs=$(cat /proc/sys/vm/nr_hugepages)
	needpgs=$((needmem_KB / hpgsize_KB))
	tries=2
	while [ "$tries" -gt 0 ] && [ "$freepgs" -lt "$needpgs" ]; do
		lackpgs=$((needpgs - freepgs))
		echo 3 > /proc/sys/vm/drop_caches
		if ! echo $((lackpgs + orig_nr_hugepgs)) > /proc/sys/vm/nr_hugepages; then
			echo "Please run this test as root"
			exit $ksft_skip
		fi
		while read -r name size unit; do
			if [ "$name" = "HugePages_Free:" ]; then
				freepgs=$size
			fi
		done < /proc/meminfo
		tries=$((tries - 1))
	done
	nr_hugepgs=$(cat /proc/sys/vm/nr_hugepages)
	if [ "$freepgs" -lt "$needpgs" ]; then
		printf "Not enough huge pages available (%d < %d)\n" \
		       "$freepgs" "$needpgs"
	fi
	HAVE_HUGEPAGES=1
else
	echo "no hugetlbfs support in kernel?"
	HAVE_HUGEPAGES=0
fi

# filter 64bit architectures
ARCH64STR="arm64 mips64 parisc64 ppc64 ppc64le riscv64 s390x sparc64 x86_64"
if [ -z "$ARCH" ]; then
	ARCH=$(uname -m 2>/dev/null | sed -e 's/aarch64.*/arm64/')
fi
VADDR64=0
echo "$ARCH64STR" | grep "$ARCH" &>/dev/null && VADDR64=1

tap_prefix() {
	sed -e "s/^/${TAP_PREFIX}/"
}

tap_output() {
	if [[ ! -z "$TAP_PREFIX" ]]; then
		read str
		echo $str
	fi
}

pretty_name() {
	echo "$*" | sed -e 's/^\(bash \)\?\.\///'
}

# Usage: run_test [test binary] [arbitrary test arguments...]
run_test() {
	if test_selected ${CATEGORY}; then
		local skip=0

		# On memory constrainted systems some tests can fail to allocate hugepages.
		# perform some cleanup before the test for a higher success rate.
		if [ ${CATEGORY} == "thp" -o ${CATEGORY} == "hugetlb" ]; then
			if [ "${HAVE_HUGEPAGES}" = "1" ]; then
				echo 3 > /proc/sys/vm/drop_caches
				sleep 2
				echo 1 > /proc/sys/vm/compact_memory
				sleep 2
			else
				echo "hugepages not supported" | tap_prefix
				skip=1
			fi
		fi

		local test=$(pretty_name "$*")
		local title="running $*"
		local sep=$(echo -n "$title" | tr "[:graph:][:space:]" -)
		printf "%s\n%s\n%s\n" "$sep" "$title" "$sep" | tap_prefix

		if [ "${skip}" != "1" ]; then
			("$@" 2>&1) | tap_prefix
			local ret=${PIPESTATUS[0]}
		else
			local ret=$ksft_skip
		fi
		count_total=$(( count_total + 1 ))
		if [ $ret -eq 0 ]; then
			count_pass=$(( count_pass + 1 ))
			echo "[PASS]" | tap_prefix
			echo "ok ${count_total} ${test}" | tap_output
		elif [ $ret -eq $ksft_skip ]; then
			count_skip=$(( count_skip + 1 ))
			echo "[SKIP]" | tap_prefix
			echo "ok ${count_total} ${test} # SKIP" | tap_output
			exitcode=$ksft_skip
		else
			count_fail=$(( count_fail + 1 ))
			echo "[FAIL]" | tap_prefix
			echo "not ok ${count_total} ${test} # exit=$ret" | tap_output
			exitcode=1
		fi
	fi # test_selected
}

# Run a P3S test that needs one of the fixture modules in p3s_modules/:
# load <module>_p3s_module.ko, make sure /dev/<module>_p3s exists, run the
# command, unload the module again.
p3s_runner=./p3s_run_module.sh
[ -x "$p3s_runner" ] || p3s_runner=../p3s/p3s_run_module.sh
run_p3s_module_test() {
	local module=$1
	shift
	run_test "$p3s_runner" "p3s_modules/${module}_p3s_module.ko" \
		"/dev/${module}_p3s" -- "$@"
}

# Like run_p3s_module_test, for a test that needs two fixture modules.
run_p3s_module2_test() {
	local module=$1 module2=$2
	shift 2
	run_p3s_module_test "$module" "$p3s_runner" \
		"p3s_modules/${module2}_p3s_module.ko" "/dev/${module2}_p3s" \
		-- "$@"
}

echo "TAP version 13" | tap_output

CATEGORY="hugetlb" run_test ./hugepage-mmap

shmmax=$(cat /proc/sys/kernel/shmmax)
shmall=$(cat /proc/sys/kernel/shmall)
echo 268435456 > /proc/sys/kernel/shmmax
echo 4194304 > /proc/sys/kernel/shmall
CATEGORY="hugetlb" run_test ./hugepage-shm
echo "$shmmax" > /proc/sys/kernel/shmmax
echo "$shmall" > /proc/sys/kernel/shmall

CATEGORY="hugetlb" run_test ./map_hugetlb
CATEGORY="hugetlb" run_test ./hugepage-mremap
CATEGORY="hugetlb" run_test ./hugepage-vmemmap
CATEGORY="hugetlb" run_test ./hugetlb-madvise
CATEGORY="hugetlb" run_test ./hugetlb_dio

if [ "${HAVE_HUGEPAGES}" = "1" ]; then
	nr_hugepages_tmp=$(cat /proc/sys/vm/nr_hugepages)
	# For this test, we need one and just one huge page
	echo 1 > /proc/sys/vm/nr_hugepages
	CATEGORY="hugetlb" run_test ./hugetlb_fault_after_madv
	CATEGORY="hugetlb" run_test ./hugetlb_madv_vs_map
	# Restore the previous number of huge pages, since further tests rely on it
	echo "$nr_hugepages_tmp" > /proc/sys/vm/nr_hugepages
fi

if test_selected "hugetlb"; then
	echo "NOTE: These hugetlb tests provide minimal coverage.  Use"	  | tap_prefix
	echo "      https://github.com/libhugetlbfs/libhugetlbfs.git for" | tap_prefix
	echo "      hugetlb regression testing."			  | tap_prefix
fi

CATEGORY="mmap" run_test ./map_fixed_noreplace
CATEGORY="mmap" run_test ./mglru_folded_p3s
if [ -x ./elf_4k_align_p3s ]; then
	CATEGORY="mmap" run_test ./elf_4k_align_p3s
fi
CATEGORY="mmap" run_test ./elf_core_p3s
CATEGORY="mmap" run_test ./taskstats_p3s
CATEGORY="mmap" run_test ./rusage_p3s
CATEGORY="mmap" run_test ./oom_anon_score_p3s
CATEGORY="mmap" run_test ./sysv_shm_p3s
CATEGORY="mlock" run_test ./sysv_shm_lock_p3s
CATEGORY="mmap" run_test ./io_uring_mmap_p3s
CATEGORY="mmap" run_test ./aio_ring_p3s --native
CATEGORY="mmap" run_test ./aio_ring_p3s
CATEGORY="mmap" run_test ./stack_guard_gap_p3s
CATEGORY="mmap" run_test ./mglru_p3s
CATEGORY="mmap" run_test ./mmu_notifier_p3s.sh
CATEGORY="mmap" run_test ./cow_notifier_p3s.sh
CATEGORY="mmap" run_test ./anon_pageout_p3s
CATEGORY="mmap" run_test ./madvise_free_p3s
CATEGORY="mmap" run_test ./madvise_free_holes_p3s
CATEGORY="mmap" run_test ./madvise_free_holes_p3s --native
CATEGORY="mmap" run_test ./madvise_guard_p3s
CATEGORY="mmap" run_test ./thp_mmap_offset_p3s
CATEGORY="mmap" run_test ./mthp_native_anon_p3s
CATEGORY="mmap" run_test ./devzero_mthp_p3s
CATEGORY="mmap" run_test ./shmem_mmap_offset_p3s
CATEGORY="mmap" run_test ./memfd_eof_p3s secretmem
CATEGORY="mmap" run_test ./memfd_eof_p3s shmem
CATEGORY="mmap" run_test ./mincore_p3s
CATEGORY="mmap" run_test ./mincore_p3s --native
CATEGORY="mmap" run_p3s_module_test gup_retry ./mmap_action_cleanup_p3s
CATEGORY="mmap" run_test ./selinux_status_mmap_p3s.sh
CATEGORY="mmap" run_p3s_module2_test vm_iomap_memory vm_iomap_unaligned ./vm_iomap_memory_p3s
CATEGORY="mmap" run_p3s_module2_test vm_iomap_memory vm_iomap_unaligned ./vm_iomap_memory_p3s --native
CATEGORY="mmap" run_test ./dma_heap_p3s --native
CATEGORY="mmap" run_test ./dma_heap_p3s
CATEGORY="mmap" run_p3s_module_test remap_pfn_cow ./remap_pfn_cow_p3s --native
CATEGORY="mmap" run_p3s_module_test remap_pfn_cow ./remap_pfn_cow_p3s
CATEGORY="mmap" run_p3s_module_test dma_mmap_attrs ./dma_mmap_p3s attrs
CATEGORY="mmap" run_p3s_module_test dma_mmap_pages ./dma_mmap_p3s pages
CATEGORY="mmap" run_p3s_module_test dma_mmap_attrs ./dma_mmap_full_p3s attrs
CATEGORY="mmap" run_p3s_module_test dma_mmap_pages ./dma_mmap_full_p3s pages
CATEGORY="mmap" run_p3s_module_test dma_mmap_bounds ./dma_mmap_bounds_p3s
CATEGORY="mmap" run_p3s_module_test dma_mmap_bounds ./dma_mmap_bounds_p3s --native
CATEGORY="mmap" run_test ./iommu_dma_mmap_p3s.sh
CATEGORY="mmap" run_test ./v9fs_mmap_close_p3s.sh
CATEGORY="mmap" run_p3s_module_test vm_map_pages ./vm_map_pages_p3s
CATEGORY="mmap" run_p3s_module_test vb2_mmap sh -c './vb2_mmap_p3s --native && exec ./vb2_mmap_p3s'
CATEGORY="mmap" run_test ./drm_gem_slice_p3s --native
CATEGORY="mmap" run_test ./drm_gem_slice_p3s

CATEGORY="mmap" run_test ./mmap_hint_p3s
CATEGORY="mmap" run_test ./brk_aslr_p3s
CATEGORY="mmap" run_test ./mmap_aslr_p3s
CATEGORY="mmap" run_test ./exec_aslr_p3s
CATEGORY="mmap" run_test ./shmem_thp_mmap_p3s
CATEGORY="mmap" run_test ./smaps_page_size_p3s
CATEGORY="mmap" run_test ./fork_page_size_p3s
CATEGORY="mmap" run_test ./mmap_page_zero_p3s
CATEGORY="mmap" run_test ./exec_stack_random_p3s
CATEGORY="mmap" run_test ./vma_range_slice_p3s
CATEGORY="mmap" run_test ./vma_merge_next_pgoff_p3s
CATEGORY="mmap" run_test ./vma_merge_file_slice_p3s
CATEGORY="mmap" run_test ./vma_merge_devzero_p3s --native
CATEGORY="mmap" run_test ./vma_merge_devzero_p3s
CATEGORY="mmap" run_test ./stack_aslr_p3s
CATEGORY="mmap" run_test ./perf_mmap_offset_p3s
CATEGORY="mmap" run_test ./perf_phys_addr_p3s
CATEGORY="mmap" run_test ./rss_stat_trace_p3s
CATEGORY="mmap" run_test ./statm_cross_geometry_p3s
CATEGORY="mmap" run_test ./pci_mmap_p3s
CATEGORY="mmap" run_test ./anon_vma_name_p3s
CATEGORY="mmap" run_test ./print_vma_addr_p3s
CATEGORY="mmap" run_p3s_module_test pagewalk ./pagewalk_p3s
CATEGORY="mmap" run_p3s_module_test folio_within_range ./folio_within_range_p3s
CATEGORY="mmap" run_test ./remap_file_pages_p3s
CATEGORY="mmap" run_test ./shmem_swap_usage_p3s
CATEGORY="mmap" run_test ./swap_header_p3s
CATEGORY="mmap" run_test ./swap_header_badpages_p3s
CATEGORY="mmap" run_test ./swapoff_shared_slice_p3s
CATEGORY="mmap" run_test ./swap_pss_p3s
CATEGORY="mmap" run_test ./anon_rmap_split_window_p3s
CATEGORY="mmap" run_test ./fork_dontfork_slice_p3s
CATEGORY="mmap" run_test ./trace_ring_buffer_mmap_p3s --native
CATEGORY="mmap" run_test ./trace_ring_buffer_mmap_p3s
CATEGORY="mmap" run_test ./truncate_p3s

if $RUN_ALL; then
    run_gup_matrix
else
    # get_user_pages_fast() benchmark
    CATEGORY="gup_test" run_test ./gup_test -u -n 1
    CATEGORY="gup_test" run_test ./gup_test -u -n -1
    # pin_user_pages_fast() benchmark
    CATEGORY="gup_test" run_test ./gup_test -a -n 1
    CATEGORY="gup_test" run_test ./gup_test -a -n -1
fi
# Dump pages 0, 19, and 4096, using pin_user_pages:
CATEGORY="gup_test" run_test ./gup_test -ct -F 0x1 0 19 0x1000
CATEGORY="gup_test" run_test ./gup_longterm
CATEGORY="gup_test" run_p3s_module_test gup_retry ./gup_retry_p3s
CATEGORY="gup_test" run_p3s_module_test iov_iter ./iov_iter_p3s
CATEGORY="gup_test" run_p3s_module_test iov_range_retry ./iov_range_retry_p3s
CATEGORY="gup_test" run_p3s_module_test iov_range_retry ./iov_range_retry_p3s --native
CATEGORY="gup_test" run_p3s_module_test iov_iter ./page_range_p3s
CATEGORY="gup_test" run_p3s_module_test iov_iter ./page_range_p3s --native
CATEGORY="gup_test" run_p3s_module_test fault_in ./fault_in_p3s
CATEGORY="gup_test" run_test ./vmsplice_slice_p3s
CATEGORY="mmap" run_test ./futex_shared_p3s
CATEGORY="mmap" run_test ./futex_uffd_p3s
CATEGORY="mmap" run_test ./futex_uffd_p3s --native
CATEGORY="mmap" run_test ./xdp_umem_p3s --native
CATEGORY="mmap" run_test ./xdp_umem_p3s
CATEGORY="mmap" run_test ./tcp_zerocopy_align_p3s

CATEGORY="userfaultfd" run_test ./uffd-unit-tests
CATEGORY="userfaultfd" run_test ./userfaultfd_mixed_p3s
CATEGORY="userfaultfd" run_test ./userfaultfd_remap_p3s
CATEGORY="userfaultfd" run_test ./userfaultfd_shmem_p3s
CATEGORY="userfaultfd" run_test ./uffd_move_swapped_slice_p3s
CATEGORY="userfaultfd" run_test ./uffd_move_swapcache_lock_p3s
CATEGORY="userfaultfd" run_test ./uffd_move_policy_p3s
uffd_stress_bin=./uffd-stress
CATEGORY="userfaultfd" run_test ${uffd_stress_bin} anon 20 16
# Hugetlb tests require source and destination huge pages. Pass in almost half
# the size of the free pages we have, which is used for *each*. An adjustment
# of (nr_parallel - 1) is done (see nr_parallel in uffd-stress.c) to have some
# extra hugepages - this is done to prevent the test from failing by racily
# reserving more hugepages than strictly required.
# uffd-stress expects a region expressed in MiB, so we adjust
# half_ufd_size_MB accordingly.
adjustment=$(( (31 < (nr_cpus - 1)) ? 31 : (nr_cpus - 1) ))
half_ufd_size_MB=$((((freepgs - adjustment) * hpgsize_KB) / 1024 / 2))
CATEGORY="userfaultfd" run_test ${uffd_stress_bin} hugetlb "$half_ufd_size_MB" 32
CATEGORY="userfaultfd" run_test ${uffd_stress_bin} hugetlb-private "$half_ufd_size_MB" 32
CATEGORY="userfaultfd" run_test ${uffd_stress_bin} shmem 20 16
CATEGORY="userfaultfd" run_test ${uffd_stress_bin} shmem-private 20 16
# uffd-wp-mremap requires at least one page of each size.
have_all_size_hugepgs=true
declare -A nr_size_hugepgs
for f in /sys/kernel/mm/hugepages/**/nr_hugepages; do
	old=$(cat $f)
	nr_size_hugepgs["$f"]="$old"
	if [ "$old" == 0 ]; then
		echo 1 > "$f"
	fi
	if [ $(cat "$f") == 0 ]; then
		have_all_size_hugepgs=false
		break
	fi
done
if $have_all_size_hugepgs; then
	CATEGORY="userfaultfd" run_test ./uffd-wp-mremap
else
	echo "# SKIP ./uffd-wp-mremap"
fi

#cleanup
for f in "${!nr_size_hugepgs[@]}"; do
	echo "${nr_size_hugepgs["$f"]}" > "$f"
done
echo "$nr_hugepgs" > /proc/sys/vm/nr_hugepages

CATEGORY="compaction" run_test ./compaction_test

if command -v sudo &> /dev/null && sudo -u nobody ls ./on-fault-limit >/dev/null;
then
	CATEGORY="mlock" run_test sudo -u nobody ./on-fault-limit
else
	echo "# SKIP ./on-fault-limit"
fi

CATEGORY="mmap" run_test ./map_populate
CATEGORY="mempolicy" run_test ./mbind_p3s

CATEGORY="mlock" run_test ./mlock-random-test

CATEGORY="mlock" run_test ./mlock2-tests
CATEGORY="mlock" run_test ./mlock_onfault_p3s
CATEGORY="mlock" run_test ./zero_sibling_mlock_p3s
CATEGORY="mlock" run_test ./mlock_onfault_p3s --native

CATEGORY="mlock" run_test ./mlock_large_file_p3s
CATEGORY="mlock" run_test ./mlock_file_slices_p3s
CATEGORY="mlock" run_test ./mlock_file_slices_p3s --native
CATEGORY="mlock" run_test ./mlock_partial_file_p3s

CATEGORY="mlock" run_test ./mlock_reclaim_p3s

CATEGORY="process_mrelease" run_test ./mrelease_test

CATEGORY="mremap" run_test ./mremap_test
CATEGORY="mremap" run_test ./mremap_rlimit_p3s
CATEGORY="mremap" run_test ./mremap_commit_p3s
CATEGORY="mremap" run_test ./mremap_hint_p3s
CATEGORY="mremap" run_p3s_module_test mremap_pgoff ./mremap_pgoff_p3s
CATEGORY="mmap" run_test ./msync_offset_p3s
CATEGORY="mmap" run_test ./bpf_map_mmap_p3s
CATEGORY="mmap" run_test ./bpf_map_mmap_p3s --native
CATEGORY="mmap" run_test ./bpf_remote_access_p3s --native
CATEGORY="mmap" run_test ./fork_filemap_p3s
CATEGORY="mmap" run_test ./munmap_p3s
CATEGORY="mmap" run_test ./bpf_stack_build_id_p3s

CATEGORY="hugetlb" run_test ./thuge-gen
CATEGORY="hugetlb" run_test ./charge_reserved_hugetlb.sh -cgroup-v2
CATEGORY="hugetlb" run_test ./hugetlb_reparenting_test.sh -cgroup-v2
if $RUN_DESTRUCTIVE; then
nr_hugepages_tmp=$(cat /proc/sys/vm/nr_hugepages)
enable_soft_offline=$(cat /proc/sys/vm/enable_soft_offline)
echo 8 > /proc/sys/vm/nr_hugepages
CATEGORY="hugetlb" run_test ./hugetlb-soft-offline
echo "$nr_hugepages_tmp" > /proc/sys/vm/nr_hugepages
echo "$enable_soft_offline" > /proc/sys/vm/enable_soft_offline
CATEGORY="hugetlb" run_test ./hugetlb-read-hwpoison
fi

if [ $VADDR64 -ne 0 ]; then

	# set overcommit_policy as OVERCOMMIT_ALWAYS so that kernel
	# allows high virtual address allocation requests independent
	# of platform's physical memory.

	if [ -x ./virtual_address_range ]; then
		prev_policy=$(cat /proc/sys/vm/overcommit_memory)
		echo 1 > /proc/sys/vm/overcommit_memory
		CATEGORY="hugevm" run_test ./virtual_address_range
		echo $prev_policy > /proc/sys/vm/overcommit_memory
	fi

	# va high address boundary switch test
	ARCH_ARM64="arm64"
	prev_nr_hugepages=$(cat /proc/sys/vm/nr_hugepages)
	if [ "$ARCH" == "$ARCH_ARM64" ]; then
		echo 6 > /proc/sys/vm/nr_hugepages
	fi
	CATEGORY="hugevm" run_test bash ./va_high_addr_switch.sh
	if [ "$ARCH" == "$ARCH_ARM64" ]; then
		echo $prev_nr_hugepages > /proc/sys/vm/nr_hugepages
	fi
fi # VADDR64

# vmalloc stability smoke test
CATEGORY="vmalloc" run_test bash ./test_vmalloc.sh smoke

CATEGORY="mremap" run_test ./mremap_dontunmap

CATEGORY="hmm" run_test bash ./test_hmm.sh smoke

# MADV_GUARD_INSTALL and MADV_GUARD_REMOVE tests
CATEGORY="madv_guard" run_test ./guard-regions

# MADV_POPULATE_READ and MADV_POPULATE_WRITE tests
CATEGORY="madv_populate" run_test ./madv_populate
CATEGORY="madv_populate" run_test ./madvise_remove_p3s
CATEGORY="madv_populate" run_test ./partial_anon_reclaim_p3s
CATEGORY="madv_populate" run_test ./madvise_willneed_p3s "$TMPDIR/madvise-willneed-p3s.bin"

# PROCESS_MADV test
CATEGORY="process_madv" run_test ./process_madv

CATEGORY="vma_merge" run_test ./merge
CATEGORY="process_madv" run_test ./process_madvise_p3s
CATEGORY="process_madv" run_test ./process_vm_access_p3s
CATEGORY="process_madv" run_test ./remote_write_dontneed_p3s
CATEGORY="process_madv" run_test ./mixed_context_switch_p3s
CATEGORY="process_madv" run_test ./mixed_mm_access_p3s

if [ -x ./memfd_secret ]
then
if [ -f /proc/sys/kernel/yama/ptrace_scope ]; then
	(echo 0 > /proc/sys/kernel/yama/ptrace_scope 2>&1) | tap_prefix
fi
CATEGORY="memfd_secret" run_test ./memfd_secret
fi

# KSM KSM_MERGE_TIME_HUGE_PAGES test with size of 100
if [ "${HAVE_HUGEPAGES}" = "1" ]; then
	CATEGORY="ksm" run_test ./ksm_tests -H -s 100
fi
# KSM KSM_MERGE_TIME test with size of 100
CATEGORY="ksm" run_test ./ksm_tests -P -s 100
# KSM MADV_MERGEABLE test with 10 identical pages
CATEGORY="ksm" run_test ./ksm_tests -M -p 10
# KSM unmerge test
CATEGORY="ksm" run_test ./ksm_tests -U
# KSM test with 10 zero pages and use_zero_pages = 0
CATEGORY="ksm" run_test ./ksm_tests -Z -p 10 -z 0
# KSM test with 10 zero pages and use_zero_pages = 1
CATEGORY="ksm" run_test ./ksm_tests -Z -p 10 -z 1
# KSM test with 2 NUMA nodes and merge_across_nodes = 1
CATEGORY="ksm_numa" run_test ./ksm_tests -N -m 1
# KSM test with 2 NUMA nodes and merge_across_nodes = 0
CATEGORY="ksm_numa" run_test ./ksm_tests -N -m 0

CATEGORY="ksm" run_test ./ksm_functional_tests

# protection_keys tests
nr_hugepgs=$(cat /proc/sys/vm/nr_hugepages)
if [ -x ./protection_keys_32 ]
then
	CATEGORY="pkey" run_test ./protection_keys_32
fi

if [ -x ./protection_keys_64 ]
then
	CATEGORY="pkey" run_test ./protection_keys_64
fi
echo "$nr_hugepgs" > /proc/sys/vm/nr_hugepages

if [ -x ./soft-dirty ]
then
	CATEGORY="soft_dirty" run_test ./soft-dirty
fi

CATEGORY="pagemap" run_test ./pagemap_ioctl
CATEGORY="pagemap" run_test ./pagemap_ioctl_p3s
CATEGORY="pagemap" run_test ./pagemap_scan_p3s
CATEGORY="pagemap" run_test ./procfs_native_rmap_p3s

CATEGORY="pfnmap" run_test ./pfnmap
CATEGORY="pfnmap" run_test ./remap_pfn_range_p3s
CATEGORY="pfnmap" run_test ./btf_vmlinux_mmap_p3s
CATEGORY="pfnmap" run_test ./alsa_pcm_mmap_p3s
CATEGORY="pfnmap" run_test ./access_phys_p3s

# COW tests
CATEGORY="cow" run_test ./cow

CATEGORY="thp" run_test ./khugepaged

CATEGORY="thp" run_test ./khugepaged -s 2

CATEGORY="thp" run_test ./khugepaged all:shmem

CATEGORY="thp" run_test ./khugepaged -s 4 all:shmem

CATEGORY="thp" run_test ./transhuge-stress -d 20

# Try to create XFS if not provided
if [ -z "${SPLIT_HUGE_PAGE_TEST_XFS_PATH}" ]; then
    if [ "${HAVE_HUGEPAGES}" = "1" ]; then
	if test_selected "thp"; then
	    if grep xfs /proc/filesystems &>/dev/null; then
		XFS_IMG=$(mktemp /tmp/xfs_img_XXXXXX)
		SPLIT_HUGE_PAGE_TEST_XFS_PATH=$(mktemp -d /tmp/xfs_dir_XXXXXX)
		truncate -s 314572800 ${XFS_IMG}
		mkfs.xfs -q ${XFS_IMG}
		mount -o loop ${XFS_IMG} ${SPLIT_HUGE_PAGE_TEST_XFS_PATH}
		MOUNTED_XFS=1
	    fi
	fi
    fi
fi

CATEGORY="thp" run_test ./split_huge_page_test ${SPLIT_HUGE_PAGE_TEST_XFS_PATH}

if [ -n "${MOUNTED_XFS}" ]; then
    umount ${SPLIT_HUGE_PAGE_TEST_XFS_PATH}
    rmdir ${SPLIT_HUGE_PAGE_TEST_XFS_PATH}
    rm -f ${XFS_IMG}
fi

CATEGORY="migration" run_test ./migration

CATEGORY="mkdirty" run_test ./mkdirty

CATEGORY="mdwe" run_test ./mdwe_test

CATEGORY="page_frag" run_test ./test_page_frag.sh smoke

CATEGORY="page_frag" run_test ./test_page_frag.sh aligned

CATEGORY="page_frag" run_test ./test_page_frag.sh nonaligned

CATEGORY="rmap" run_test ./rmap

if [ "${HAVE_HUGEPAGES}" = 1 ]; then
	echo "$orig_nr_hugepgs" > /proc/sys/vm/nr_hugepages
fi

CATEGORY="mmap" run_test ./devzero_slice_p3s

CATEGORY="uprobes" run_test ./uprobe_p3s

CATEGORY="mmap" run_test ./perf_page_size_p3s

CATEGORY="pagemap" run_test ./pagemap_exclusive_p3s

CATEGORY="smaps" run_test ./smaps_pss_p3s

CATEGORY="userfaultfd" run_test ./uffd_move_swap_window_p3s
CATEGORY="userfaultfd" run_test ./uffd_move_swap_window_p3s --native


# Additional P3S regressions, also reachable through the standard runner.
CATEGORY="mmap" run_test ./ashmem_p3s
CATEGORY="mmap" run_test ./dontneed_refault_p3s
CATEGORY="mmap" run_test ./exec_env_p3s
CATEGORY="mmap" run_test ./exec_stack_p3s
CATEGORY="mmap" run_test ./mte_anon_p3s
CATEGORY="mmap" run_test ./exec_user_args_p3s
CATEGORY="mmap" run_test ./fadvise_dontneed_p3s
CATEGORY="mmap" run_test ./file_cow_p3s
CATEGORY="mmap" run_p3s_module_test iov_iter ./file_cow_refs_p3s
CATEGORY="mmap" run_test ./filemap_eof_p3s "${TMPDIR:-/tmp}"
CATEGORY="mmap" run_test ./hole_anon_p3s
CATEGORY="mmap" run_test ./kcov_p3s
CATEGORY="mmap" run_test ./madv_cold_large_file_p3s "${TMPDIR:-/tmp}/madv-cold-p3s.bin"
CATEGORY="mmap" run_p3s_module_test anon_exclusive_probe ./memory_failure_anon_p3s
CATEGORY="mmap" run_test ./mremap_file_slice_p3s
CATEGORY="mmap" run_test ./mremap_slice_cow_p3s
CATEGORY="mmap" run_test ./mremap_split_p3s
CATEGORY="mmap" run_test ./oom_score_p3s
CATEGORY="mmap" run_test ./oom_trace_p3s
CATEGORY="mmap" run_test ./packed_anon_p3s
CATEGORY="mmap" run_test ./packed_swap_p3s
CATEGORY="mmap" run_test ./proc_pagewalk_p3s
CATEGORY="mmap" run_test ./process_mrelease_p3s
CATEGORY="mmap" run_test ./rmap_file_referenced_p3s "${TMPDIR:-/tmp}/rmap-file-p3s.bin"
CATEGORY="mmap" run_test ./smaps_native_fallback_p3s
CATEGORY="mmap" run_test ./swap_badpages_loop_p3s
CATEGORY="mmap" run_test ./swap_slice_mremap_p3s
CATEGORY="mmap" run_test ./swap_vma_readahead_p3s
CATEGORY="mmap" run_p3s_module_test anon_exclusive_probe ./swapoff_p3s
CATEGORY="mmap" run_test ./tcp_zerocopy_reject_p3s
CATEGORY="mmap" run_test ./truncate_remote_p3s
CATEGORY="mmap" run_test ./truncate_tail_p3s "${TMPDIR:-/tmp}"
CATEGORY="mmap" run_test ./uffd_copy_tuple_p3s
CATEGORY="mmap" run_test ./uffd_sigbus_anon_p3s
CATEGORY="mmap" run_test ./userfaultfd_poison_remote_p3s
CATEGORY="mmap" run_test ./userfaultfd_remote_p3s
CATEGORY="mmap" run_test ./userfaultfd_unregister_remote_p3s
CATEGORY="mmap" run_test ./userfaultfd_wp_remote_p3s
CATEGORY="mmap" run_p3s_module_test vb2_userptr_reject ./vb2_userptr_reject_p3s
CATEGORY="mmap" run_test ./vhost_dirty_log_p3s

echo "SUMMARY: PASS=${count_pass} SKIP=${count_skip} FAIL=${count_fail}" | tap_prefix
echo "1..${count_total}" | tap_output

exit $exitcode
