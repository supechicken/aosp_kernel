// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <fcntl.h>
#include <linux/dma-heap.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "p3s_test.h"

#define BUFFER_SIZE (4 * NATIVE_PAGE_SIZE)

static unsigned char marker(size_t offset)
{
	return 0x40 + 17 * (offset / PROCESS_PAGE_SIZE) + 37 * offset;
}

static bool check_mapping(const unsigned char *mapping, size_t offset,
			  size_t length)
{
	size_t i;

	for (i = 0; i < length; i++) {
		if (mapping[i] != marker(offset + i))
			return false;
	}
	return true;
}

static int run_test(void)
{
	struct dma_heap_allocation_data allocation = {
		.len = BUFFER_SIZE,
		.fd_flags = O_RDWR | O_CLOEXEC,
	};
	unsigned long page_size = getpagesize();
	struct {
		size_t offset;
		size_t length;
	} ranges[] = {
		{ 0, page_size },
		{ page_size, page_size },
		{ 2 * page_size, page_size },
		{ 3 * page_size, page_size },
		{ NATIVE_PAGE_SIZE - page_size, 2 * page_size },
		{ NATIVE_PAGE_SIZE + page_size,
		  page_size < NATIVE_PAGE_SIZE ? NATIVE_PAGE_SIZE - page_size : page_size },
		{ BUFFER_SIZE - page_size, page_size },
	};
	unsigned char *reference, *mapping;
	size_t i;
	int heap_fd, error;

	ksft_print_header();
	ksft_set_plan(13);
	heap_fd = p3s_open_fixture_or_skip("/dev/dma_heap/system", O_RDWR);
	ksft_test_result(heap_fd >= 0, "open the system DMA heap\n");
	if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &allocation))
		ksft_exit_fail_msg("DMA heap allocation failed: %s\n", strerror(errno));
	ksft_test_result(allocation.fd >= 0, "allocate four native pages\n");
	reference = mmap(NULL, BUFFER_SIZE, PROT_READ | PROT_WRITE,
			 MAP_SHARED, allocation.fd, 0);
	ksft_test_result(reference != MAP_FAILED, "map the whole test-owned buffer\n");
	if (reference == MAP_FAILED)
		ksft_exit_fail_msg("reference mmap failed: %s\n", strerror(errno));
	for (i = 0; i < BUFFER_SIZE; i++)
		reference[i] = marker(i);
	ksft_test_result(check_mapping(reference, 0, BUFFER_SIZE),
			 "distinct slice patterns survive the full-buffer control\n");

	for (i = 0; i < ARRAY_SIZE(ranges); i++) {
		bool matches = false;

		mapping = mmap(NULL, ranges[i].length, PROT_READ, MAP_SHARED,
			       allocation.fd, ranges[i].offset);
		if (mapping != MAP_FAILED) {
			matches = check_mapping(mapping, ranges[i].offset, ranges[i].length);
			munmap(mapping, ranges[i].length);
		}
		ksft_test_result(matches, "read offset=%zu length=%zu exactly\n",
				 ranges[i].offset, ranges[i].length);
	}
	errno = 0;
	mapping = mmap(NULL, page_size, PROT_READ, MAP_SHARED,
		       allocation.fd, BUFFER_SIZE);
	error = errno;
	ksft_test_result(mapping == MAP_FAILED && error == EINVAL,
			 "reject a range starting past the buffer\n");
	if (mapping != MAP_FAILED)
		munmap(mapping, page_size);
	errno = 0;
	mapping = mmap(NULL, 2 * page_size, PROT_READ, MAP_SHARED,
		       allocation.fd, BUFFER_SIZE - page_size);
	error = errno;
	ksft_test_result(mapping == MAP_FAILED && error == EINVAL,
			 "reject a range ending past the buffer\n");
	if (mapping != MAP_FAILED)
		munmap(mapping, 2 * page_size);
	munmap(reference, BUFFER_SIZE);
	close(allocation.fd);
	close(heap_fd);
	ksft_finished();
}

int main(int argc, char **argv)
{
	return p3s_geometry_main(argc, argv, run_test);
}
