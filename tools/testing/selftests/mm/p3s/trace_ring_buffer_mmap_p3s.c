// SPDX-License-Identifier: GPL-2.0
/* Keep native trace mmap and the read/splice interfaces for 4KB processes. */
#define _GNU_SOURCE

#include <linux/trace_mmap.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "p3s_test.h"

#define TRACE_PIPE_RAW "/sys/kernel/tracing/per_cpu/cpu0/trace_pipe_raw"

static int read_checks(int fd)
{
	char buffer[NATIVE_PAGE_SIZE];
	int pipes[2];
	ssize_t ret;

	errno = 0;
	ret = read(fd, buffer, sizeof(buffer));
	ksft_test_result(ret >= 0 || (ret < 0 && errno == EAGAIN),
			 "read the ring or report no data without blocking\n");
	if (pipe2(pipes, O_CLOEXEC | O_NONBLOCK))
		ksft_exit_fail_msg("pipe2 failed: %s\n", strerror(errno));
	errno = 0;
	ret = splice(fd, NULL, pipes[1], NULL, sizeof(buffer), SPLICE_F_NONBLOCK);
	ksft_test_result(ret >= 0 || (ret < 0 && errno == EAGAIN),
			 "splice the ring or report no data without blocking\n");
	close(pipes[0]);
	close(pipes[1]);
	return 0;
}

static int run_test(void)
{
	struct trace_buffer_meta *meta;
	unsigned long page_size = getpagesize();
	unsigned long data_len, meta_len, offset;
	unsigned char checksum = 0;
	unsigned char *data;
	void *subpage;
	bool layout_ok;
	int fd;

	ksft_print_header();
	ksft_set_plan(page_size == PROCESS_PAGE_SIZE ? 5 : 9);
	fd = p3s_open_fixture_or_skip(TRACE_PIPE_RAW, O_RDONLY | O_NONBLOCK);
	ksft_test_result(true, "open the CPU0 trace ring buffer\n");
	if (page_size == PROCESS_PAGE_SIZE) {
		errno = 0;
		meta = mmap(NULL, page_size, PROT_READ, MAP_SHARED, fd, 0);
		ksft_test_result(meta == MAP_FAILED && errno == EOPNOTSUPP,
				 "reject trace mmap from a 4KB process\n");
		if (meta != MAP_FAILED)
			munmap(meta, page_size);
		errno = 0;
		data = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ, MAP_SHARED,
			    fd, NATIVE_PAGE_SIZE);
		ksft_test_result(data == MAP_FAILED && errno == EOPNOTSUPP,
				 "reject 4KB-process mmap at a kernel-page offset\n");
		if (data != MAP_FAILED)
			munmap(data, NATIVE_PAGE_SIZE);
		close(fd);
		fd = p3s_open_fixture_or_skip(TRACE_PIPE_RAW, O_RDONLY | O_NONBLOCK);
		read_checks(fd);
		close(fd);
		ksft_finished();
	}

	meta = mmap(NULL, page_size, PROT_READ, MAP_SHARED, fd, 0);
	if (meta == MAP_FAILED)
		ksft_exit_fail_msg("metadata mmap failed: %s\n", strerror(errno));
	ksft_test_result(true, "map the ring-buffer metadata page\n");
	meta_len = meta->meta_page_size;
	data_len = (unsigned long)meta->subbuf_size * meta->nr_subbufs;
	layout_ok = meta->meta_struct_len >= sizeof(*meta) &&
		meta->meta_struct_len <= meta_len && meta_len >= page_size &&
		!(meta_len % page_size) && meta->subbuf_size &&
		meta->nr_subbufs && data_len;
	ksft_test_result(layout_ok, "metadata describes a valid kernel-page ring\n");
	if (!layout_ok)
		ksft_exit_fail_msg("invalid trace ring-buffer metadata\n");
	subpage = mmap(NULL, page_size, PROT_READ, MAP_SHARED, fd, page_size);
	ksft_test_result(meta_len == page_size ? subpage != MAP_FAILED :
			 subpage == MAP_FAILED,
			 "handle a page offset according to the ring layout\n");
	if (subpage != MAP_FAILED)
		munmap(subpage, page_size);
	data = mmap(NULL, data_len, PROT_READ, MAP_SHARED, fd, meta_len);
	ksft_test_result(data != MAP_FAILED, "map the complete native ring\n");
	if (data == MAP_FAILED)
		ksft_exit_fail_msg("full data mmap failed: %s\n", strerror(errno));
	for (offset = 0; offset < data_len; offset += page_size)
		checksum ^= data[offset];
	ksft_test_result(true, "access every mapped page without SIGBUS\n");
	ksft_print_msg("mapped data checksum=%u\n", checksum);
	ksft_test_result(ioctl(fd, TRACE_MMAP_IOCTL_GET_READER) == 0,
			 "swap a reader sub-buffer through the mmap ioctl\n");
	munmap(data, data_len);
	munmap(meta, page_size);
	close(fd);
	fd = p3s_open_fixture_or_skip(TRACE_PIPE_RAW, O_RDONLY | O_NONBLOCK);
	read_checks(fd);
	close(fd);
	ksft_finished();
}

int main(int argc, char **argv)
{
	return p3s_geometry_main(argc, argv, run_test);
}
