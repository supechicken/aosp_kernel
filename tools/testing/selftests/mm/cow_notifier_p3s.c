// SPDX-License-Identifier: GPL-2.0
/*
 * COW of one 4K slice can replace every mapped PTE of its 16K folio.
 * Start/end MMU notifier invalidations must cover the whole replaced range.
 * The fixture module is loaded by cow_notifier_p3s.sh before this test.
 */
#define _GNU_SOURCE

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include "p3s_test.h"
#include "mmu_notifier_p3s.h"

static int run_test(void)
{
	struct mmu_notifier_p3s_range range;
	struct mmu_notifier_p3s_stats stats = {};
	unsigned char *raw, *base;
	uint64_t before, after;
	int fd, ready[2], status;
	pid_t child;
	char done = 1;
	bool cow_ok, range_ok;
	unsigned int i;

	ksft_print_header();
	p3s_require_compat();
	raw = mmap(NULL, 2 * NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (raw == MAP_FAILED)
		ksft_exit_fail_msg("mmap: %s\n", strerror(errno));
	base = (void *)(((uintptr_t)raw + NATIVE_PAGE_SIZE - 1) &
			 ~(uintptr_t)(NATIVE_PAGE_SIZE - 1));
	for (i = 0; i < P3S_SLICES; i++)
		base[i * PROCESS_PAGE_SIZE] = 0x31 + i;
	if (!p3s_pfn(base, &before))
		ksft_exit_skip("pagemap PFNs are unavailable\n");
	if (pipe(ready))
		ksft_exit_fail_msg("pipe: %s\n", strerror(errno));
	child = fork();
	if (child < 0)
		ksft_exit_fail_msg("fork: %s\n", strerror(errno));
	if (!child) {
		close(ready[1]);
		if (!read_full(ready[0], &done, 1))
			_exit(1);
		_exit(base[PROCESS_PAGE_SIZE] == 0x32 ? 0 : 1);
	}
	close(ready[0]);
	fd = p3s_open_fixture_or_skip("/dev/mmu_notifier_p3s", O_RDWR);
	range.start = (uintptr_t)base;
	range.end = range.start + NATIVE_PAGE_SIZE;
	range.page_size = PROCESS_PAGE_SIZE;
	if (ioctl(fd, MMU_NOTIFIER_P3S_SET_RANGE, &range))
		ksft_exit_fail_msg("set notifier range: %s\n", strerror(errno));
	base[PROCESS_PAGE_SIZE] = 0x77;
	if (!p3s_pfn(base, &after) ||
	    ioctl(fd, MMU_NOTIFIER_P3S_GET_STATS, &stats))
		ksft_exit_fail_msg("read COW PFN/notifier stats: %s\n",
				   strerror(errno));
	if (!write_full(ready[1], &done, 1))
		ksft_exit_fail_msg("release child: %s\n", strerror(errno));
	close(ready[1]);
	if (waitpid(child, &status, 0) != child)
		ksft_exit_fail_msg("waitpid: %s\n", strerror(errno));
	cow_ok = before != after && WIFEXITED(status) && !WEXITSTATUS(status);
	range_ok = stats.invalidate_starts == 1 &&
		   stats.invalidate_ends == 1 &&
		   stats.last_invalidate_start == (uintptr_t)base &&
		   stats.last_invalidate_end == (uintptr_t)base + NATIVE_PAGE_SIZE;
	ksft_print_msg("COW PFNs %#llx/%#llx; notifier %llu/%llu [%#llx,%#llx)\n",
		       (unsigned long long)before, (unsigned long long)after,
		       (unsigned long long)stats.invalidate_starts,
		       (unsigned long long)stats.invalidate_ends,
		       (unsigned long long)stats.last_invalidate_start,
		       (unsigned long long)stats.last_invalidate_end);
	ksft_set_plan(2);
	ksft_test_result(cow_ok, "fork actually triggered COW without child data loss\n");
	ksft_test_result(range_ok, "MMU notifier covers all replaced 4K PTEs\n");
	close(fd);
	munmap(raw, 2 * NATIVE_PAGE_SIZE);
	ksft_finished();
}

P3S_COMPAT_MAIN(run_test)
