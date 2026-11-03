// SPDX-License-Identifier: GPL-2.0
/* MADV_FREE must cover mapped slices across holes, not just one PTE run. */
#define _GNU_SOURCE

#include <sched.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include "p3s_test.h"

#define RECLAIM_ROUNDS 8

struct window {
	unsigned char *area;
	unsigned char *base;
	unsigned int mask;
};

static size_t page_size;
static unsigned int slots;
static void *scratch;

static void drain(void)
{
	if (madvise(scratch, NATIVE_PAGE_SIZE, MADV_COLD))
		ksft_exit_fail_msg("scratch MADV_COLD: %s\n", strerror(errno));
}

static unsigned int first_slice(unsigned int mask)
{
	return __builtin_ctz(mask);
}

static unsigned int last_slice(unsigned int mask)
{
	return 31 - __builtin_clz(mask);
}

static bool data_matches(struct window *w, bool zero, int redirty)
{
	unsigned int i;
	size_t j;

	for (i = 0; i < slots; i++) {
		unsigned char expected = zero ? 0 : (int)i == redirty ? 0xa5 : 0x40 + i;

		if (!(w->mask & (1U << i)))
			continue;
		for (j = 0; j < page_size; j++)
			if (((volatile unsigned char *)w->base)[i * page_size + j] != expected)
				return false;
	}
	return true;
}

static struct window setup(unsigned int mask, bool zap)
{
	struct window w = { .mask = mask };
	uintptr_t address;
	uint64_t first = 0, pfn = 0, entry;
	unsigned int i;
	bool valid = true;

	w.area = mmap(NULL, 4 * NATIVE_PAGE_SIZE, PROT_NONE,
		      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (w.area == MAP_FAILED)
		ksft_exit_fail_msg("reserve: %s\n", strerror(errno));
	address = ((uintptr_t)w.area + 2 * NATIVE_PAGE_SIZE - 1) &
		  ~(uintptr_t)(NATIVE_PAGE_SIZE - 1);
	w.base = mmap((void *)address, NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	if (w.base == MAP_FAILED ||
	    (slots == 1 && madvise(w.base, NATIVE_PAGE_SIZE, MADV_NOHUGEPAGE)))
		ksft_exit_fail_msg("map: %s\n", strerror(errno));
	for (i = 0; i < slots; i++) {
		if (zap || (mask & (1U << i)))
			memset(w.base + i * page_size, 0x40 + i, page_size);
	}
	if (zap) {
		for (i = 0; i < slots; i++)
			if (!(mask & (1U << i)) &&
			    madvise(w.base + i * page_size, page_size, MADV_DONTNEED))
				ksft_exit_fail_msg("make hole: %s\n", strerror(errno));
	}
	for (i = 0; i < slots; i++) {
		if (!p3s_pagemap_entry(w.base + i * page_size, &entry))
			ksft_exit_fail_msg("pagemap read failed\n");
		if (!(mask & (1U << i))) {
			valid &= !(entry & (PM_PRESENT | PM_SWAP));
			continue;
		}
		valid &= p3s_pfn(w.base + i * page_size, &pfn);
		if (!first)
			first = pfn;
		valid &= pfn == first;
	}
	ksft_test_result(valid && first, "mask %#x %s: one folio, exact PTE holes\n",
			 mask, zap ? "zapped" : "untouched");
	if (!valid || !first)
		ksft_exit_fail_msg("fixture is not the requested sliced folio\n");
	return w;
}

static bool lazy_bytes(struct window *w, unsigned long expected)
{
	unsigned long value;

	drain();
	return p3s_smaps_bytes(w->base, 1, "LazyFree", &value) && value == expected;
}

static bool discarded(struct window *w)
{
	uint64_t entry;
	unsigned int i;

	for (i = 0; i < slots; i++)
		if ((w->mask & (1U << i)) &&
		    (!p3s_pagemap_entry(w->base + i * page_size, &entry) ||
		     (entry & (PM_PRESENT | PM_SWAP))))
			return false;
	return true;
}

static void reclaim(struct window *w)
{
	unsigned int round;

	for (round = 0; round < RECLAIM_ROUNDS; round++) {
		if (madvise(w->base, NATIVE_PAGE_SIZE, MADV_PAGEOUT))
			ksft_exit_fail_msg("MADV_PAGEOUT: %s\n", strerror(errno));
		if (discarded(w))
			break;
		usleep(10000);
	}
}

static void release(struct window *w)
{
	munmap(w->area, 4 * NATIVE_PAGE_SIZE);
}

static void positive(unsigned int mask, bool zap)
{
	struct window w = setup(mask, zap);
	unsigned int first = zap ? first_slice(mask) : 0;
	unsigned int end = zap ? last_slice(mask) + 1 : slots;
	int ret = madvise(w.base + first * page_size,
			  (end - first) * page_size, MADV_FREE);

	ksft_test_result(!ret && lazy_bytes(&w, __builtin_popcount(mask) * page_size),
			 "mask %#x %s: all mapped slices are lazyfree\n",
			 mask, zap ? "bounded/zapped" : "whole/untouched");
	ksft_test_result(data_matches(&w, false, -1),
			 "mask %#x: data retained before reclaim\n", mask);
	reclaim(&w);
	ksft_test_result(discarded(&w), "mask %#x: PTEs discarded without swap\n", mask);
	ksft_test_result(data_matches(&w, true, -1),
			 "mask %#x: zero-filled on refault\n", mask);
	release(&w);
}

static void partial(unsigned int first)
{
	struct window w = setup(5, false);
	int ret = madvise(w.base + first * page_size, page_size, MADV_FREE);

	ksft_test_result(!ret && lazy_bytes(&w, 0),
			 "slice %u only: exclude mapping outside request\n", first);
	reclaim(&w);
	ksft_test_result(data_matches(&w, false, -1),
			 "slice %u only: preserve both slices after reclaim\n", first);
	release(&w);
}

static void shared(unsigned int mask)
{
	struct window w = setup(mask, false);
	int ready[2], done[2], status;
	char byte;
	pid_t child;

	if (pipe(ready) || pipe(done))
		ksft_exit_fail_msg("pipe: %s\n", strerror(errno));
	child = fork();
	if (child < 0)
		ksft_exit_fail_msg("fork: %s\n", strerror(errno));
	if (!child) {
		close(ready[0]);
		close(done[1]);
		byte = 1;
		if (write(ready[1], &byte, 1) != 1 || read(done[0], &byte, 1) != 1)
			_exit(2);
		_exit(data_matches(&w, false, -1) ? 0 : 1);
	}
	close(ready[1]);
	close(done[0]);
	if (read(ready[0], &byte, 1) != 1)
		ksft_exit_fail_msg("child readiness failed\n");
	status = madvise(w.base, NATIVE_PAGE_SIZE, MADV_FREE);
	/* Native MADV_FREE may mark a clean shared folio, but cannot discard it. */
	ksft_test_result(!status && (slots == 1 || lazy_bytes(&w, 0)),
			 "fork: apply native or slice coverage policy\n");
	reclaim(&w);
	ksft_test_result(data_matches(&w, false, -1), "fork: preserve parent data\n");
	if (write(done[1], &byte, 1) != 1 || waitpid(child, &status, 0) != child)
		ksft_exit_fail_msg("child completion failed\n");
	ksft_test_result(WIFEXITED(status) && !WEXITSTATUS(status), "fork: preserve child data\n");
	close(ready[0]);
	close(done[1]);
	release(&w);
}

static void redirty(unsigned int mask, unsigned int dirty)
{
	struct window w = setup(mask, false);
	bool preserved = true;
	unsigned int i;
	size_t j;
	int ret = madvise(w.base, NATIVE_PAGE_SIZE, MADV_FREE);

	ksft_test_result(!ret && lazy_bytes(&w, __builtin_popcount(mask) * page_size),
			 "redirty: establish lazyfree folio first\n");
	memset(w.base + dirty * page_size, 0xa5, page_size);
	reclaim(&w);
	/* MADV_FREE is cancelled for written process pages, not sibling slices. */
	for (i = 0; i < slots; i++) {
		volatile unsigned char *data = w.base + i * page_size;
		unsigned char expected;

		if (!(mask & (1U << i)))
			continue;
		expected = i == dirty ? 0xa5 : data[0];
		if (i != dirty && expected != 0 && expected != 0x40 + i)
			preserved = false;
		for (j = 0; j < page_size; j++)
			if (data[j] != expected)
				preserved = false;
	}
	ksft_test_result(preserved,
			 "written slice %u survives; unwritten slices stay intact or zero\n",
			 dirty);
	release(&w);
}

static int run_test(void)
{
	static const unsigned int masks[] = { 5, 9, 10, 11, 13 };
	cpu_set_t allowed, one;
	unsigned int i;
	int cpu;

	page_size = getpagesize();
	slots = NATIVE_PAGE_SIZE / page_size;
	ksft_print_header();
	if (geteuid())
		ksft_exit_skip("root is required to verify physical folio identity\n");
	if (sched_getaffinity(0, sizeof(allowed), &allowed))
		ksft_exit_fail_msg("get affinity failed\n");
	CPU_ZERO(&one);
	for (cpu = 0; cpu < CPU_SETSIZE; cpu++)
		if (CPU_ISSET(cpu, &allowed)) {
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
	ksft_set_plan(slots == P3S_SLICES ? 66 : 17);
	if (slots == P3S_SLICES) {
		for (i = 0; i < ARRAY_SIZE(masks); i++) {
			positive(masks[i], false);
			positive(masks[i], true);
		}
		partial(0);
		partial(2);
		shared(5);
		redirty(5, 0);
		redirty(5, 2);
	} else {
		positive(1, false);
		positive(1, true);
		shared(1);
		redirty(1, 0);
	}
	munmap(scratch, NATIVE_PAGE_SIZE);
	ksft_finished();
}

int main(int argc, char **argv)
{
	return p3s_geometry_main(argc, argv, run_test);
}
