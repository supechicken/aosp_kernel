// SPDX-License-Identifier: GPL-2.0
/* DMA object bounds follow the process page size, not the backing page size. */
#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "p3s_test.h"
#include "dma_mmap_bounds_p3s.h"

static const unsigned int sizes[] = {
	0, 4096, 4097, 8192, 8193, 12288, 16384, 16385, 20481,
};
static const char * const apis[] = { "attrs", "pages", "page-array callback",
				  "noncontiguous" };
static sigjmp_buf fault_env;

static void fault_handler(int sig)
{
	siglongjmp(fault_env, sig);
}

static bool read_byte(const volatile unsigned char *address, unsigned char *value)
{
	if (sigsetjmp(fault_env, 1))
		return false;
	*value = *address;
	return true;
}

static bool check_mapping(int fd, size_t length, off_t offset, bool allowed)
{
	size_t reserve_size = length + 2 * NATIVE_PAGE_SIZE, i;
	unsigned char *reservation, *target, *mapping, value;
	bool ok = true;
	int error;

	reservation = mmap(NULL, reserve_size, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (reservation == MAP_FAILED)
		return false;
	/* Use virtual slice 1 in compat mode, independently of the file offset. */
	target = (void *)(((uintptr_t)reservation + NATIVE_PAGE_SIZE - 1) &
			 ~(NATIVE_PAGE_SIZE - 1));
	if (p3s_is_compat_process())
		target += PROCESS_PAGE_SIZE;
	errno = 0;
	mapping = mmap(target, length, PROT_READ, MAP_SHARED | MAP_FIXED, fd, offset);
	error = errno;
	if (allowed) {
		ok = mapping == target;
		if (ok) {
			for (i = 0; i < length; i += PROCESS_PAGE_SIZE) {
				unsigned char want = 0x40 + (offset + i) / PROCESS_PAGE_SIZE;

				if (!read_byte(mapping + i, &value) || value != want ||
				    !read_byte(mapping + i + PROCESS_PAGE_SIZE - 1, &value) ||
				    value != want)
					ok = false;
			}
		}
	} else {
		ok = mapping == MAP_FAILED && error == ENXIO;
		/* No readable slice may survive a rejected fixed mapping. */
		for (i = 0; i < length; i += PROCESS_PAGE_SIZE)
			if (read_byte(target + i, &value))
				ok = false;
	}
	if (!ok)
		ksft_print_msg("offset=%lld length=%zu allowed=%d map=%p errno=%d\n",
			       (long long)offset, length, allowed, mapping, error);
	munmap(reservation, reserve_size);
	return ok;
}

static int run_test(void)
{
	struct sigaction action = { .sa_handler = fault_handler };
	unsigned int api, index, test;
	size_t step = getpagesize();

	ksft_print_header();
	ksft_set_plan(DMA_BOUNDS_API_COUNT * ARRAY_SIZE(sizes) * 5);
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGBUS, &action, NULL) || sigaction(SIGSEGV, &action, NULL))
		ksft_exit_fail_msg("sigaction: %s\n", strerror(errno));
	for (api = 0; api < DMA_BOUNDS_API_COUNT; api++) {
		for (index = 0; index < ARRAY_SIZE(sizes); index++) {
			struct dma_bounds_request request = { api, sizes[index] };
			size_t end = (request.size + step - 1) & ~(step - 1);
			size_t lengths[] = { end ?: step, step, (end ?: step) + step,
					     step, step };
			off_t offsets[] = { 0, end ? end - step : 0, 0, end, 1ULL << 40 };
			int fd = p3s_open_fixture_or_skip("/dev/dma_mmap_bounds_p3s", O_RDWR);
			int error = ioctl(fd, DMA_BOUNDS_SETUP, &request) ? errno : 0;

			for (test = 0; test < ARRAY_SIZE(lengths); test++) {
				if (error == EOPNOTSUPP) {
					ksft_test_result_skip("%s size=%u case=%u: required DMA exports unavailable\n",
							      apis[api], request.size, test);
					continue;
				}
				ksft_test_result(!error && check_mapping(fd, lengths[test],
						 offsets[test], end && test < 2),
						 "%s page=%zu size=%u offset=%lld length=%zu\n",
						 apis[api], step, request.size,
						 (long long)offsets[test], lengths[test]);
			}
			close(fd);
		}
	}
	ksft_finished();
}

int main(int argc, char **argv)
{
	return p3s_geometry_main(argc, argv, run_test);
}
