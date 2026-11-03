// SPDX-License-Identifier: GPL-2.0
/* Private /dev/zero VMAs retain byte-accurate file offsets. */
#define _GNU_SOURCE
#include <sys/mman.h>
#include "kselftest_p3s.h"

static bool zero_range(unsigned char *address, unsigned char *start,
		       unsigned char *end, unsigned long expected_offset)
{
	unsigned long first, last, offset;
	char permissions[5], line[512];
	FILE *maps = fopen("/proc/self/maps", "r");
	bool match = false;

	if (!maps)
		return false;
	while (fgets(line, sizeof(line), maps)) {
		if (sscanf(line, "%lx-%lx %4s %lx", &first, &last,
			   permissions, &offset) != 4 ||
		    (unsigned long)address < first || (unsigned long)address >= last)
			continue;
		match = first == (unsigned long)start && last == (unsigned long)end &&
			offset == expected_offset;
		break;
	}
	fclose(maps);
	return match;
}

static void test_zero_offsets(void)
{
	int fd = open("/dev/zero", O_RDWR);
	unsigned long len = 2 * NATIVE_PAGE_SIZE;
	unsigned char *map = MAP_FAILED;
	bool split = false, merged = false;

	if (fd >= 0)
		map = mmap(NULL, len, PROT_READ | PROT_WRITE,
			   MAP_PRIVATE, fd, 2 * PROCESS_PAGE_SIZE);
	if (map != MAP_FAILED) {
		for (unsigned long i = 0; i < len; i += PROCESS_PAGE_SIZE)
			map[i] = (unsigned char)(0x51 + i / PROCESS_PAGE_SIZE);
		split = !mprotect(map + PROCESS_PAGE_SIZE, len - PROCESS_PAGE_SIZE,
				  PROT_READ) &&
			zero_range(map + PROCESS_PAGE_SIZE, map + PROCESS_PAGE_SIZE,
				   map + len, 3 * PROCESS_PAGE_SIZE);
		merged = !mprotect(map, len, PROT_READ | PROT_WRITE) &&
			zero_range(map, map, map + len, 2 * PROCESS_PAGE_SIZE);
		for (unsigned long i = 0; i < len; i += PROCESS_PAGE_SIZE)
			merged &= map[i] == (unsigned char)(0x51 + i / PROCESS_PAGE_SIZE);
		munmap(map, len);
	}
	if (fd >= 0)
		close(fd);
	ksft_test_result(split, "/dev/zero split retains byte-accurate offsets\n");
	ksft_test_result(merged, "/dev/zero restores one VMA and preserves all slices\n");
}

static int run_test(void)
{
	ksft_print_header();
	ksft_set_plan(2);
	test_zero_offsets();
	ksft_finished();
}

P3S_COMPAT_MAIN(run_test)
