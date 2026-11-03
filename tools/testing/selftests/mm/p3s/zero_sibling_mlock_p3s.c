// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <stdint.h>
#include <sys/mman.h>

#include "kselftest_p3s.h"

#define PM_PFN_MASK ((1ULL << 55) - 1)
#define KPF_UNEVICTABLE (1ULL << 18)

static uint64_t native_pfn(void *address)
{
	uint64_t entry;
	int fd = open("/proc/self/pagemap", O_RDONLY);
	ssize_t n;

	if (fd < 0)
		ksft_exit_skip("pagemap unavailable\n");
	n = pread(fd, &entry, sizeof(entry),
		  (uintptr_t)address / getpagesize() * sizeof(entry));
	close(fd);
	if (n != sizeof(entry) || !(entry & (1ULL << 63)) ||
	    !(entry & PM_PFN_MASK))
		ksft_exit_skip("backing PFN unavailable\n");
	return (entry & PM_PFN_MASK) / P3S_SLICES;
}

static bool unevictable(uint64_t pfn)
{
	uint64_t flags = 0;
	int fd = open("/proc/kpageflags", O_RDONLY);
	ssize_t n;

	if (fd < 0)
		ksft_exit_skip("kpageflags unavailable\n");
	n = pread(fd, &flags, sizeof(flags), pfn * sizeof(flags));
	close(fd);
	if (n != sizeof(flags))
		ksft_exit_fail_msg("read kpageflags\n");
	return flags & KPF_UNEVICTABLE;
}

/* Flush the per-CPU mlock batch using only test-owned mappings. */
static void drain_mlock_batch(void)
{
	size_t bytes = 128 * NATIVE_PAGE_SIZE;
	unsigned char *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
			       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (p == MAP_FAILED)
		ksft_exit_fail_msg("drain mapping: %s\n", strerror(errno));
	memset(p, 0x69, bytes);
	if (mlock(p, bytes))
		ksft_exit_skip("mlock limit: %s\n", strerror(errno));
	munlock(p, bytes);
	munmap(p, bytes);
}

static int run_test(void)
{
	unsigned char *area, *map;
	uint64_t pfn;
	volatile unsigned char zero;
	bool packed = true;
	unsigned int i;

	p3s_require_compat();
	ksft_print_header();
	ksft_set_plan(4);
	area = mmap(NULL, 4 * NATIVE_PAGE_SIZE, PROT_NONE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (area == MAP_FAILED)
		ksft_exit_fail_msg("mmap: %s\n", strerror(errno));
	map = (void *)(((uintptr_t)area + 2 * NATIVE_PAGE_SIZE - 1) &
		       ~(NATIVE_PAGE_SIZE - 1));
	if (mprotect(map, NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE))
		ksft_exit_fail_msg("mprotect: %s\n", strerror(errno));
	for (i = 0; i < P3S_SLICES; i++)
		zero = map[i * PROCESS_PAGE_SIZE];
	(void)zero;
	if (mlock(map, NATIVE_PAGE_SIZE))
		ksft_exit_skip("mlock: %s\n", strerror(errno));
	drain_mlock_batch();
	pfn = native_pfn(map);
	for (i = 1; i < P3S_SLICES; i++)
		packed &= native_pfn(map + i * PROCESS_PAGE_SIZE) == pfn;
	ksft_test_result(packed, "zero-page write faults reuse one sibling folio\n");
	ksft_test_result(unevictable(pfn), "fully locked folio is unevictable\n");
	if (munlock(map, PROCESS_PAGE_SIZE))
		ksft_exit_fail_msg("partial munlock: %s\n", strerror(errno));
	drain_mlock_batch();
	ksft_test_result(unevictable(pfn),
			 "unlocking one slice keeps locked siblings unevictable\n");
	if (munlock(map + PROCESS_PAGE_SIZE,
		    NATIVE_PAGE_SIZE - PROCESS_PAGE_SIZE))
		ksft_exit_fail_msg("final munlock: %s\n", strerror(errno));
	drain_mlock_batch();
	ksft_test_result(!unevictable(pfn), "last munlock restores evictability\n");
	munmap(area, 4 * NATIVE_PAGE_SIZE);
	ksft_finished();
}

P3S_COMPAT_MAIN(run_test)
