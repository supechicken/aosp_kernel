// SPDX-License-Identifier: GPL-2.0
/* Private /dev/zero keeps file-offset units even though its faults are anonymous. */
#define _GNU_SOURCE
#include <sys/mman.h>

#include "kselftest_p3s.h"

#define AREA_SIZE (10 * NATIVE_PAGE_SIZE)

static int zero_fd;

static unsigned char *reserve(unsigned char **raw)
{
	*raw = mmap(NULL, AREA_SIZE, PROT_NONE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (*raw == MAP_FAILED)
		ksft_exit_fail_msg("reserve: %s\n", strerror(errno));
	return (void *)(((uintptr_t)*raw + 2 * NATIVE_PAGE_SIZE - 1) &
			~(NATIVE_PAGE_SIZE - 1));
}

static void map_zero(unsigned char *addr, size_t len, off_t offset)
{
	if (mmap(addr, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED,
		 zero_fd, offset) != addr)
		ksft_exit_fail_msg("map /dev/zero: %s\n", strerror(errno));
}

static bool bytes_are(unsigned char *addr, size_t len, unsigned char value)
{
	size_t i;

	for (i = 0; i < len; i++)
		if (addr[i] != value)
			return false;
	return true;
}

static int count_vmas(unsigned char *addr, size_t len)
{
	unsigned long lo, hi, start = (unsigned long)addr;
	char line[512];
	FILE *maps = fopen("/proc/self/maps", "re");
	int count = 0;

	if (!maps)
		ksft_exit_fail_msg("open maps: %s\n", strerror(errno));
	while (fgets(line, sizeof(line), maps))
		if (sscanf(line, "%lx-%lx", &lo, &hi) == 2 &&
		    lo < start + len && hi > start)
			count++;
	fclose(maps);
	return count;
}

static bool grow(unsigned int offset, bool populated, int flags)
{
	size_t unit = getpagesize();
	unsigned char *raw, *base = reserve(&raw), *dst;
	bool ok;

	map_zero(base, 2 * unit, offset * unit);
	if (munmap(base + 2 * unit, unit))
		ksft_exit_fail_msg("unmap growth gap: %s\n", strerror(errno));
	if (populated)
		memset(base, 0x63, 2 * unit);
	dst = mremap(base, 2 * unit, 3 * unit, flags);
	ok = dst == base;
	if (!ok)
		ksft_print_msg("grow offset=%u populated=%d flags=%d: %s\n",
			       offset, populated, flags,
			       dst == MAP_FAILED ? strerror(errno) : "unexpected move");
	if (dst != MAP_FAILED) {
		ok &= bytes_are(dst, 2 * unit, populated ? 0x63 : 0);
		ok &= bytes_are(dst + 2 * unit, unit, 0);
		memset(dst + 2 * unit, 0x97, unit);
		ok &= bytes_are(dst, 2 * unit, populated ? 0x63 : 0);
		ok &= count_vmas(dst, 3 * unit) == 1;
		if (dst != base)
			munmap(dst, 3 * unit);
	}
	munmap(raw, AREA_SIZE);
	return ok;
}

static bool merge_right(bool linear, bool populated)
{
	unsigned char *raw, *base = reserve(&raw);
	size_t len = NATIVE_PAGE_SIZE;
	int expected = linear ? 1 : 2;
	bool ok;

	/* The right neighbour exists first, so mmap tests the merge-next path. */
	map_zero(base + len, len, linear ? len : 4 * len);
	if (populated)
		memset(base + len, 0x83, len);
	map_zero(base, len, 0);
	ok = count_vmas(base, 2 * len) == expected;
	if (!ok)
		ksft_print_msg("right merge linear=%d populated=%d: %d VMAs, expected %d\n",
			       linear, populated, count_vmas(base, 2 * len), expected);
	ok &= bytes_are(base, len, 0);
	ok &= bytes_are(base + len, len, populated ? 0x83 : 0);
	memset(base, 0x51, len);
	memset(base + len, 0x83, len);
	ok &= bytes_are(base, len, 0x51) && bytes_are(base + len, len, 0x83);
	munmap(raw, AREA_SIZE);
	return ok;
}

/* Moving a populated range beside its source must not merge it with itself. */
static bool move_beside_source(unsigned int offset, bool populated)
{
	size_t unit = getpagesize();
	unsigned char *raw, *base = reserve(&raw), *dst;
	bool ok;

	map_zero(base, unit, offset * unit);
	if (populated)
		memset(base, 0x69, unit);
	dst = mremap(base, unit, unit, MREMAP_MAYMOVE | MREMAP_FIXED,
		     base + unit);
	ok = dst == base + unit;
	if (ok) {
		ok &= bytes_are(dst, unit, populated ? 0x69 : 0);
		memset(dst, 0x69, unit);
		ok &= bytes_are(dst, unit, 0x69);
	}
	munmap(raw, AREA_SIZE);
	return ok;
}

static int run_test(void)
{
	unsigned int offsets = p3s_is_compat_process() ? P3S_SLICES : 1;
	unsigned int offset, populated, movable, linear;

	ksft_print_header();
	ksft_set_plan(6 * offsets + 4);
	zero_fd = open("/dev/zero", O_RDWR | O_CLOEXEC);
	if (zero_fd < 0)
		ksft_exit_fail_msg("open /dev/zero: %s\n", strerror(errno));
	for (offset = 0; offset < offsets; offset++)
		for (populated = 0; populated < 2; populated++) {
			for (movable = 0; movable < 2; movable++)
				ksft_test_result(grow(offset, populated,
						     movable ? MREMAP_MAYMOVE : 0),
					"%dK grow offset=%u populated=%u maymove=%u\n",
					getpagesize() / 1024, offset, populated, movable);
			ksft_test_result(move_beside_source(offset, populated),
				"%dK move beside source offset=%u populated=%u\n",
				getpagesize() / 1024, offset, populated);
		}
	for (linear = 0; linear < 2; linear++)
		for (populated = 0; populated < 2; populated++)
			ksft_test_result(merge_right(linear, populated),
				"%dK merge-right linear=%u populated=%u\n",
				getpagesize() / 1024, linear, populated);
	close(zero_fd);
	ksft_finished();
}

int main(int argc, char **argv)
{
	return p3s_geometry_main(argc, argv, run_test);
}
