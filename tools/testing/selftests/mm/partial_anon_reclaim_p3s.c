// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <stdint.h>
#include <sys/mman.h>

#include "kselftest_p3s.h"

static unsigned long field(void *address, const char *name)
{
	unsigned long a, b, value = 0;
	bool selected = false;
	char line[512], key[64];
	FILE *f = fopen("/proc/self/smaps", "re");

	if (!f)
		ksft_exit_fail_msg("smaps: %s\n", strerror(errno));
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "%lx-%lx", &a, &b) == 2) {
			if (selected)
				break;
			selected = a <= (uintptr_t)address && (uintptr_t)address < b;
		} else if (selected && sscanf(line, "%63[^:]: %lu kB", key, &value) == 2 &&
			   !strcmp(key, name)) {
			fclose(f);
			return value;
		}
	}
	fclose(f);
	ksft_exit_fail_msg("smaps field %s missing\n", name);
}

static unsigned char *mapping(unsigned char **area)
{
	unsigned char *p;

	*area = mmap(NULL, 4 * NATIVE_PAGE_SIZE, PROT_NONE,
		     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (*area == MAP_FAILED)
		ksft_exit_fail_msg("mmap: %s\n", strerror(errno));
	p = (void *)(((uintptr_t)*area + 2 * NATIVE_PAGE_SIZE - 1) &
		     ~(NATIVE_PAGE_SIZE - 1));
	if (mprotect(p, NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE))
		ksft_exit_fail_msg("mprotect: %s\n", strerror(errno));
	memset(p, 0x57, NATIVE_PAGE_SIZE);
	return p;
}

static bool evict(void *p, size_t size)
{
	unsigned int i;

	if (madvise(p, size, MADV_PAGEOUT))
		ksft_exit_fail_msg("MADV_PAGEOUT: %s\n", strerror(errno));
	for (i = 0; i < 100; i++) {
		if (field(p, "Swap") == size / 1024)
			return true;
		usleep(10000);
	}
	return false;
}

static int run_test(void)
{
	unsigned char *area, *p;
	unsigned long before, after;
	bool bytes = true;
	unsigned int i;

	p3s_require_compat();
	ksft_print_header();
	p = mapping(&area);
	if (!evict(p, NATIVE_PAGE_SIZE))
		ksft_exit_skip("whole-folio PAGEOUT positive control unavailable\n");
	for (i = 0; i < NATIVE_PAGE_SIZE; i++)
		bytes &= p[i] == 0x57;
	munmap(area, 4 * NATIVE_PAGE_SIZE);
	ksft_set_plan(4);
	ksft_test_result(bytes, "whole-folio swap-out/in preserves initialized bytes\n");
	p = mapping(&area);
	if (munmap(p + 3 * PROCESS_PAGE_SIZE, PROCESS_PAGE_SIZE))
		ksft_exit_fail_msg("make partial folio: %s\n", strerror(errno));
	before = field(p, "Referenced");
	if (madvise(p, 3 * PROCESS_PAGE_SIZE, MADV_COLD))
		ksft_exit_fail_msg("MADV_COLD: %s\n", strerror(errno));
	after = field(p, "Referenced");
	ksft_print_msg("partial folio Referenced: before=%lu after=%lu kB\n", before, after);
	ksft_test_result(before && !after, "MADV_COLD deactivates a partially mapped small folio\n");
	ksft_test_result(evict(p, 3 * PROCESS_PAGE_SIZE),
			 "MADV_PAGEOUT evicts a partially mapped small folio\n");
	bytes = true;
	for (i = 0; i < 3 * PROCESS_PAGE_SIZE; i++)
		bytes &= p[i] == 0x57;
	ksft_test_result(bytes, "partial-folio swap-in preserves all remaining slices\n");
	munmap(area, 4 * NATIVE_PAGE_SIZE);
	ksft_finished();
}

P3S_COMPAT_MAIN(run_test)
