// SPDX-License-Identifier: GPL-2.0
/* Native BPF producer mappings retain remote byte access. */
#define _GNU_SOURCE
#include <linux/bpf.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>

#include "../kselftest_p3s.h"

static int run_test(void)
{
	union bpf_attr attr = {
		.map_type = BPF_MAP_TYPE_USER_RINGBUF,
		.max_entries = NATIVE_PAGE_SIZE,
	};
	const uint64_t marker = 0x1234567887654321ULL;
	uint64_t *producer, value = 0;
	struct iovec local = { .iov_base = &value, .iov_len = sizeof(value) };
	struct iovec remote;
	ssize_t bytes;
	int fd, mem;

	ksft_print_header();
	if (p3s_is_compat_process())
		ksft_exit_skip("BPF map mmap is native-only; use --native\n");
	fd = syscall(__NR_bpf, BPF_MAP_CREATE, &attr, sizeof(attr));
	if (fd < 0) {
		if (errno == EPERM || errno == EACCES)
			ksft_exit_skip("BPF map creation unavailable: %s\n", strerror(errno));
		ksft_exit_fail_msg("BPF map creation: %s\n", strerror(errno));
	}
	ksft_set_plan(6);
	ksft_test_result_pass("create test-owned user ring buffer\n");
	producer = mmap(NULL, getpagesize(), PROT_READ | PROT_WRITE,
			MAP_SHARED, fd, getpagesize());
	if (producer == MAP_FAILED)
		ksft_exit_fail_msg("producer mmap: %s\n", strerror(errno));
	*producer = marker;
	ksft_test_result(*producer == marker, "ordinary mapped-pointer control\n");
	remote.iov_base = producer;
	remote.iov_len = sizeof(value);
	bytes = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
	ksft_test_result(bytes == sizeof(value) && value == marker,
			 "process_vm_readv reads the producer slice (%zd, %#llx)\n",
			 bytes, (unsigned long long)value);
	value = marker + 1;
	bytes = process_vm_writev(getpid(), &local, 1, &remote, 1, 0);
	ksft_test_result(bytes == sizeof(value) && *producer == value,
			 "process_vm_writev writes the producer slice (%zd, %#llx)\n",
			 bytes, (unsigned long long)*producer);
	*producer = marker;
	mem = open("/proc/self/mem", O_RDWR);
	if (mem < 0) {
		ksft_test_result_skip("proc/self/mem unavailable: %s\n", strerror(errno));
		ksft_test_result_skip("proc/self/mem write unavailable\n");
	} else {
		value = 0;
		bytes = pread(mem, &value, sizeof(value), (uintptr_t)producer);
		ksft_test_result(bytes == sizeof(value) && value == marker,
				 "proc/self/mem reads the producer slice (%zd, %#llx)\n",
				 bytes, (unsigned long long)value);
		value = marker + 2;
		bytes = pwrite(mem, &value, sizeof(value), (uintptr_t)producer);
		ksft_test_result(bytes == sizeof(value) && *producer == value,
				 "proc/self/mem writes the producer slice (%zd, %#llx)\n",
				 bytes, (unsigned long long)*producer);
		close(mem);
	}
	munmap(producer, getpagesize());
	close(fd);
	ksft_finished();
}

int main(int argc, char **argv)
{
	return p3s_geometry_main(argc, argv, run_test);
}
