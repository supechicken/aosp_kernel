// SPDX-License-Identifier: GPL-2.0
/* Public page-size contract on native and P3S processes. */
#define _GNU_SOURCE

#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#include "kselftest_p3s.h"

static int run_test(void)
{
	const size_t page = getpagesize();
	unsigned char *mapping;
	unsigned char marker = 0x69;
	void *file_map;
	void *bad_map;
	int fd;

	ksft_print_header();
	ksft_set_plan(8);
	ksft_test_result(page == PROCESS_PAGE_SIZE || page == NATIVE_PAGE_SIZE,
			 "arm64 process reports a supported page size (%zu)\n", page);
	ksft_test_result(sysconf(_SC_PAGESIZE) == (long)page &&
			 getauxval(AT_PAGESZ) == page,
			 "libc and AT_PAGESZ agree with getpagesize()\n");

	fd = syscall(SYS_memfd_create, "page-size-contract", 0);
	if (fd < 0 || ftruncate(fd, 3 * page) ||
	    pwrite(fd, &marker, 1, page) != 1)
		ksft_exit_fail_msg("memfd setup: %s\n", strerror(errno));
	file_map = mmap(NULL, page, PROT_READ, MAP_SHARED, fd, page);
	ksft_test_result(file_map != MAP_FAILED &&
			 *(unsigned char *)file_map == marker,
			 "mmap accepts a file offset of one reported page\n");
	if (file_map != MAP_FAILED)
		munmap(file_map, page);
	errno = 0;
	bad_map = mmap(NULL, page, PROT_READ, MAP_SHARED, fd, page / 2);
	ksft_test_result(bad_map == MAP_FAILED && errno == EINVAL,
			 "mmap rejects a sub-page file offset\n");
	if (bad_map != MAP_FAILED)
		munmap(bad_map, page);
	close(fd);

	mapping = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ksft_test_result(mapping != MAP_FAILED &&
			 !((uintptr_t)mapping & (page - 1)),
			 "anonymous mapping starts on a reported-page boundary\n");
	if (mapping == MAP_FAILED)
		ksft_exit_fail_msg("anonymous mmap: %s\n", strerror(errno));
	mapping[0] = marker;
	ksft_test_result(!mprotect(mapping + page, page, PROT_NONE),
			 "mprotect accepts a reported-page boundary\n");
	errno = 0;
	ksft_test_result(mprotect(mapping + page / 2, page, PROT_NONE) == -1 &&
			 errno == EINVAL,
			 "mprotect rejects a sub-page address\n");
	ksft_test_result(!munmap(mapping + page, page) && mapping[0] == marker,
			 "munmap removes exactly one reported page\n");
	munmap(mapping, page);
	ksft_finished();
}

int main(int argc, char **argv)
{
	const char *mode = p3s_run_mode(argc, argv, NULL);

	if (argc == 1)
		return run_test();
	if (argc == 2 && !strcmp(mode, "--compat")) {
		if (getpagesize() != NATIVE_PAGE_SIZE)
			ksft_exit_skip("requires a native 16K launcher\n");
		exec_compat(argv[0], "--run", NULL);
	}
	if (argc == 2 && !strcmp(mode, "--run")) {
		p3s_require_compat();
		return run_test();
	}
	return KSFT_FAIL;
}
