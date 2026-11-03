// SPDX-License-Identifier: GPL-2.0
/*
 * MADV_FREE over an isolated 32-page anonymous VMA of a 4K compat process
 * marks the pages lazy-free and they keep their data until reclaim.  Lazy
 * freeing requires coverage of every existing mapping of the folio;
 * unpopulated slices do not prevent lazyfree.
 */
#define _GNU_SOURCE

#include <sys/mman.h>
#include <sched.h>

#include "p3s_test.h"

#define TEST_PAGES	32
#define MAPPING_SIZE	(TEST_PAGES * PROCESS_PAGE_SIZE)
#define RESERVE_SIZE	(MAPPING_SIZE + 2 * NATIVE_PAGE_SIZE)

static bool vma_span(const void *address, unsigned long *span)
{
	unsigned long target = (unsigned long)address;
	unsigned long start, end;
	char *line = NULL;
	size_t capacity = 0;
	bool found = false;
	FILE *maps;

	maps = fopen("/proc/self/maps", "re");
	if (!maps)
		return false;
	while (getline(&line, &capacity, maps) >= 0) {
		if (sscanf(line, "%lx-%lx", &start, &end) != 2)
			continue;
		if (target >= start && target < end) {
			*span = end - start;
			found = true;
			break;
		}
	}
	free(line);
	fclose(maps);
	return found;
}

/* Observe the local lazyfree batch without changing production semantics. */
static void flush_lazyfree(void *scratch)
{
	if (madvise(scratch, NATIVE_PAGE_SIZE, MADV_COLD))
		ksft_exit_fail_msg("MADV_COLD scratch: %s\n", strerror(errno));
}

static void partial_mapping(unsigned int nr, void *scratch)
{
	unsigned char *area, *page;
	unsigned long lazyfree = 0;
	uintptr_t base;
	unsigned int i;
	bool preserved = true;
	int ret;

	area = mmap(NULL, 4 * NATIVE_PAGE_SIZE, PROT_NONE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (area == MAP_FAILED)
		ksft_exit_fail_msg("partial reserve: %s\n", strerror(errno));
	base = ((uintptr_t)area + 2 * NATIVE_PAGE_SIZE - 1) &
	       ~(uintptr_t)(NATIVE_PAGE_SIZE - 1);
	page = mmap((void *)base, NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	if (page == MAP_FAILED)
		ksft_exit_fail_msg("partial mapping: %s\n", strerror(errno));
	for (i = 0; i < nr; i++)
		page[i * PROCESS_PAGE_SIZE] = 0x60 + i;
	ret = madvise(page, NATIVE_PAGE_SIZE, MADV_FREE);
	flush_lazyfree(scratch);
	ksft_test_result(!ret &&
			 p3s_smaps_bytes(page, 1, "LazyFree", &lazyfree) &&
			 lazyfree == nr * PROCESS_PAGE_SIZE,
			 "lazyfree all %u populated slices (%lu bytes)\n",
			 nr, lazyfree);
	for (i = 0; i < nr; i++)
		if (page[i * PROCESS_PAGE_SIZE] != 0x60 + i)
			preserved = false;
	ksft_test_result(preserved,
			 "preserve %u populated slices before reclaim\n", nr);
	munmap(area, 4 * NATIVE_PAGE_SIZE);
}

static int run_test(void)
{
	unsigned long lazyfree = 0;
	unsigned long span = 0;
	unsigned char *mapping;
	unsigned char *reservation;
	uintptr_t base;
	bool preserved = true;
	unsigned int i;
	int ret;
	void *scratch;
	cpu_set_t cpus, one;
	int cpu;

	if (sched_getaffinity(0, sizeof(cpus), &cpus))
		ksft_exit_fail_msg("get affinity failed\n");
	CPU_ZERO(&one);
	for (cpu = 0; cpu < CPU_SETSIZE; cpu++)
		if (CPU_ISSET(cpu, &cpus)) {
			CPU_SET(cpu, &one);
			break;
		}
	if (sched_setaffinity(0, sizeof(one), &one))
		ksft_exit_fail_msg("set affinity failed\n");
	scratch = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (scratch == MAP_FAILED)
		ksft_exit_fail_msg("scratch mmap failed\n");
	memset(scratch, 1, NATIVE_PAGE_SIZE);

	ksft_print_header();
	ksft_set_plan(12);

	/* A native-aligned VMA with guard pages on both sides. */
	reservation = mmap(NULL, RESERVE_SIZE, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	base = ((uintptr_t)reservation + 2 * NATIVE_PAGE_SIZE - 1) &
	       ~(uintptr_t)(NATIVE_PAGE_SIZE - 1);
	mapping = reservation == MAP_FAILED ? MAP_FAILED :
		mmap((void *)base, MAPPING_SIZE, PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	if (mapping != MAP_FAILED) {
		madvise(mapping, MAPPING_SIZE, MADV_NOHUGEPAGE);
		for (i = 0; i < TEST_PAGES; i++)
			mapping[i * PROCESS_PAGE_SIZE] = 0x40 + i;
	}
	ksft_test_result(mapping != MAP_FAILED,
			 "map and populate anonymous process pages\n");
	if (mapping == MAP_FAILED)
		ksft_exit_fail_msg("mmap failed: %s\n", strerror(errno));
	ksft_test_result(vma_span(mapping, &span) && span == MAPPING_SIZE,
			 "target is an isolated %lu-byte VMA (%lu bytes)\n",
			 MAPPING_SIZE, span);

	ret = madvise(mapping, PROCESS_PAGE_SIZE, MADV_FREE);
	flush_lazyfree(scratch);
	ksft_test_result(!ret && p3s_smaps_bytes(mapping, 1, "LazyFree", &lazyfree) &&
			 lazyfree == 0,
			 "MADV_FREE on one slice leaves its 16K tuple resident (%lu bytes)\n",
			 lazyfree);

	ret = madvise(mapping, MAPPING_SIZE, MADV_FREE);
	flush_lazyfree(scratch);
	ksft_test_result(!ret, "mark the process pages MADV_FREE\n");
	ksft_test_result(p3s_smaps_bytes(mapping, 1, "LazyFree", &lazyfree) &&
			 lazyfree >= MAPPING_SIZE - PROCESS_PAGE_SIZE,
			 "account lazy-free process pages (%lu bytes)\n",
			 lazyfree);

	for (i = 0; i < TEST_PAGES; i++) {
		if (mapping[i * PROCESS_PAGE_SIZE] != (unsigned char)(0x40 + i)) {
			preserved = false;
			break;
		}
	}
	ksft_test_result(preserved,
			 "lazy-free pages retain data before reclaim\n");

	munmap(reservation, RESERVE_SIZE);
	for (i = 1; i < P3S_SLICES; i++)
		partial_mapping(i, scratch);
	munmap(scratch, NATIVE_PAGE_SIZE);
	ksft_finished();
}

P3S_COMPAT_MAIN(run_test)
