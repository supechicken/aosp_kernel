// SPDX-License-Identifier: GPL-2.0
/* MOVE must preserve physical pages; unsupported slices fall back to COPY. */
#define _GNU_SOURCE
#include "p3s_tuple_test.h"

struct move_case {
	const char *name;
	unsigned int present;
	unsigned int first;
	unsigned int dst_first;
	unsigned int count;
	bool reject;
};

static bool run_case(const struct move_case *test)
{
	unsigned char *src_res = NULL, *dst_res = NULL;
	unsigned char *src, *dst;
	uint64_t before[P3S_SLICES] = {}, after;
	struct uffdio_api api = { .api = UFFD_API };
	struct uffdio_register reg = { .mode = UFFDIO_REGISTER_MODE_MISSING };
	struct uffdio_move move = {};
	struct uffdio_copy copy = {};
	bool passed = false;
	unsigned int i;
	int fd = -1, ret, error;

	src = map_exact_edge(NATIVE_PAGE_SIZE, &src_res);
	dst = map_exact_edge(NATIVE_PAGE_SIZE, &dst_res);
	if (src == MAP_FAILED || dst == MAP_FAILED)
		ksft_exit_fail_msg("mmap: %s\n", strerror(errno));
	for (i = 0; i < P3S_SLICES; i++) {
		if (!(test->present & (1U << i)))
			continue;
		memset(src + i * PROCESS_PAGE_SIZE, 0x31 + i, PROCESS_PAGE_SIZE);
		if (!p3s_pagemap_entry(src + i * PROCESS_PAGE_SIZE, &before[i]) ||
		    !PAGEMAP_PFN(before[i]))
			ksft_exit_skip("visible pagemap PFNs required\n");
	}
	fd = syscall(__NR_userfaultfd, O_CLOEXEC | UFFD_USER_MODE_ONLY);
	if (fd < 0)
		ksft_exit_skip("userfaultfd: %s\n", strerror(errno));
	if (ioctl(fd, UFFDIO_API, &api))
		ksft_exit_fail_msg("UFFDIO_API: %s\n", strerror(errno));
	if (!(api.features & UFFD_FEATURE_MOVE))
		ksft_exit_skip("UFFD_FEATURE_MOVE unavailable\n");
	reg.range.start = (uintptr_t)dst;
	reg.range.len = NATIVE_PAGE_SIZE;
	if (ioctl(fd, UFFDIO_REGISTER, &reg))
		ksft_exit_fail_msg("UFFDIO_REGISTER: %s\n", strerror(errno));
	move.src = (uintptr_t)(src + test->first * PROCESS_PAGE_SIZE);
	move.dst = (uintptr_t)(dst + test->dst_first * PROCESS_PAGE_SIZE);
	move.len = test->count * PROCESS_PAGE_SIZE;
	errno = 0;
	ret = ioctl(fd, UFFDIO_MOVE, &move);
	error = errno;
	if (test->reject) {
		if (ret != -1 || error != EBUSY || move.move != -EBUSY)
			goto out;
		for (i = 0; i < P3S_SLICES; i++) {
			if (p3s_page_present(dst + i * PROCESS_PAGE_SIZE))
				goto out;
			if (!(test->present & (1U << i)))
				continue;
			if (!p3s_pagemap_entry(src + i * PROCESS_PAGE_SIZE, &after) ||
			    !(after & PM_PRESENT) ||
			    PAGEMAP_PFN(after) != PAGEMAP_PFN(before[i]) ||
			    !all_bytes_are(src + i * PROCESS_PAGE_SIZE,
					   PROCESS_PAGE_SIZE, 0x31 + i))
				goto out;
		}
		/* ART handles EBUSY by explicitly requesting a COPY. */
		copy.src = move.src;
		copy.dst = move.dst;
		copy.len = PROCESS_PAGE_SIZE;
		passed = !ioctl(fd, UFFDIO_COPY, &copy) &&
			 copy.copy == PROCESS_PAGE_SIZE &&
			 all_bytes_are((void *)(uintptr_t)copy.dst,
				       PROCESS_PAGE_SIZE, 0x31 + test->first);
	} else {
		if (ret || move.move != (long long)move.len)
			goto out;
		for (i = 0; i < test->count; i++) {
			unsigned int s = test->first + i;
			unsigned int d = test->dst_first + i;

			if (p3s_page_present(src + s * PROCESS_PAGE_SIZE) ||
			    !p3s_pagemap_entry(dst + d * PROCESS_PAGE_SIZE, &after) ||
			    !(after & PM_PRESENT) ||
			    PAGEMAP_PFN(after) != PAGEMAP_PFN(before[s]) ||
			    !all_bytes_are(dst + d * PROCESS_PAGE_SIZE,
					   PROCESS_PAGE_SIZE, 0x31 + s))
				goto out;
		}
		passed = true;
	}
out:
	if (!passed)
		ksft_print_msg("%s: ret=%d errno=%d moved=%lld\n", test->name,
			       ret, error, (long long)move.move);
	close(fd);
	munmap(src_res, 3 * NATIVE_PAGE_SIZE);
	munmap(dst_res, 3 * NATIVE_PAGE_SIZE);
	return passed;
}

static bool invalid_area_returns_einval(bool registered, bool cross_phase)
{
	unsigned char *src_res = NULL, *dst_res = NULL;
	unsigned char *src, *dst;
	struct uffdio_api api = { .api = UFFD_API };
	struct uffdio_register reg = { .mode = UFFDIO_REGISTER_MODE_MISSING };
	struct uffdio_move move = {};
	bool passed;
	int fd, ret, error;

	src = map_exact_edge(NATIVE_PAGE_SIZE, &src_res);
	dst = map_exact_edge(NATIVE_PAGE_SIZE, &dst_res);
	if (src == MAP_FAILED || dst == MAP_FAILED)
		ksft_exit_fail_msg("mmap: %s\n", strerror(errno));
	memset(src, 0x5a, NATIVE_PAGE_SIZE);
	fd = syscall(__NR_userfaultfd, O_CLOEXEC | UFFD_USER_MODE_ONLY);
	if (fd < 0)
		ksft_exit_skip("userfaultfd: %s\n", strerror(errno));
	if (ioctl(fd, UFFDIO_API, &api))
		ksft_exit_fail_msg("UFFDIO_API: %s\n", strerror(errno));
	if (!(api.features & UFFD_FEATURE_MOVE))
		ksft_exit_skip("UFFD_FEATURE_MOVE unavailable\n");
	if (registered) {
		/* A registered destination with protections unlike the source. */
		if (mprotect(dst, NATIVE_PAGE_SIZE, PROT_READ))
			ksft_exit_fail_msg("mprotect: %s\n", strerror(errno));
		reg.range.start = (uintptr_t)dst;
		reg.range.len = NATIVE_PAGE_SIZE;
		if (ioctl(fd, UFFDIO_REGISTER, &reg))
			ksft_exit_fail_msg("UFFDIO_REGISTER: %s\n", strerror(errno));
	}
	move.src = (uintptr_t)src;
	move.dst = (uintptr_t)(dst + (cross_phase ? PROCESS_PAGE_SIZE : 0));
	move.len = getpagesize();
	errno = 0;
	ret = ioctl(fd, UFFDIO_MOVE, &move);
	error = errno;
	passed = ret == -1 && error == EINVAL && move.move == -EINVAL &&
		 !p3s_page_present((void *)(uintptr_t)move.dst) &&
		 all_bytes_are(src, NATIVE_PAGE_SIZE, 0x5a);
	if (!passed)
		ksft_print_msg("registered=%d cross_phase=%d: ret=%d errno=%d moved=%lld\n",
			       registered, cross_phase, ret, error,
			       (long long)move.move);
	close(fd);
	munmap(src_res, 3 * NATIVE_PAGE_SIZE);
	munmap(dst_res, 3 * NATIVE_PAGE_SIZE);
	return passed;
}

static int run_tests(void)
{
	static const struct move_case cases[] = {
		{ "whole folio keeps every physical page", 15, 0, 0, 4, false },
		{ "sole mapped slice keeps its physical page", 2, 1, 1, 1, false },
		{ "all three mapped siblings move without copying", 14, 1, 1, 3, false },
		{ "partial folio returns EBUSY and COPY works", 15, 0, 0, 1, true },
		{ "cross-phase MOVE returns EBUSY and COPY works", 15, 0, 1, 1, true },
		{ "holey folio returns EBUSY and COPY works", 13, 0, 0, 4, true },
	};
	bool native = getpagesize() == NATIVE_PAGE_SIZE;
	unsigned int i, count = native ? 1 : 6;

	ksft_print_header();
	ksft_set_plan(count + (native ? 2 : 4));
	for (i = 0; i < count; i++)
		ksft_test_result(run_case(&cases[i]), "%s\n", cases[i].name);
	ksft_test_result(invalid_area_returns_einval(false, false),
			 "unregistered same-phase destination returns EINVAL\n");
	ksft_test_result(invalid_area_returns_einval(true, false),
			 "same-phase protection mismatch returns EINVAL\n");
	if (!native) {
		ksft_test_result(invalid_area_returns_einval(false, true),
				 "unregistered cross-phase destination returns EINVAL\n");
		ksft_test_result(invalid_area_returns_einval(true, true),
				 "cross-phase protection mismatch returns EINVAL\n");
	}
	ksft_finished();
}

int main(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "--native")) {
		if (getpagesize() != NATIVE_PAGE_SIZE)
			ksft_exit_skip("native 16KB control required\n");
		return run_tests();
	}
	return p3s_compat_main(argc, argv, run_tests);
}
