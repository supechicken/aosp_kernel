// SPDX-License-Identifier: GPL-2.0
/* A native perf reader observes byte offsets and page sizes of a 4KB child. */
#define _GNU_SOURCE
#include <linux/memfd.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include "../kselftest_p3s.h"

static void copy_ring(void *to, const void *data, size_t size,
		      uint64_t position, size_t length)
{
	size_t offset = position % size;
	size_t first = size - offset;

	if (first > length)
		first = length;
	memcpy(to, data + offset, first);
	if (first < length)
		memcpy(to + first, data, length - first);
}

static int child(void)
{
	unsigned char *memory;
	char command;
	int fd;
	void *text;

	if (!p3s_is_compat_process())
		return KSFT_SKIP;
	memory = mmap(NULL, PROCESS_PAGE_SIZE, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (memory == MAP_FAILED ||
	    write(STDOUT_FILENO, &memory, sizeof(memory)) != sizeof(memory) ||
	    read(STDIN_FILENO, &command, 1) != 1)
		return KSFT_FAIL;
	fd = memfd_create("perf-observer-p3s", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, NATIVE_PAGE_SIZE))
		return KSFT_FAIL;
	text = mmap(NULL, PROCESS_PAGE_SIZE, PROT_READ | PROT_EXEC,
		    MAP_PRIVATE, fd, PROCESS_PAGE_SIZE);
	if (text == MAP_FAILED)
		return KSFT_FAIL;
	(void)__atomic_load_n(memory, __ATOMIC_RELAXED);
	munmap(text, PROCESS_PAGE_SIZE);
	munmap(memory, PROCESS_PAGE_SIZE);
	close(fd);
	return KSFT_PASS;
}

static int run_test(void)
{
	struct perf_event_attr attr = {
		.size = sizeof(attr), .type = PERF_TYPE_SOFTWARE,
		.config = PERF_COUNT_SW_PAGE_FAULTS_MIN, .sample_period = 1,
		.sample_type = PERF_SAMPLE_ADDR | PERF_SAMPLE_DATA_PAGE_SIZE |
			       PERF_SAMPLE_CODE_PAGE_SIZE,
		.disabled = 1, .exclude_kernel = 1, .exclude_hv = 1,
		.mmap = 1, .mmap2 = 1, .mmap_data = 1,
	};
	struct perf_event_mmap_page *metadata;
	unsigned char *data;
	uint64_t address, head, position = 0, sizes[3] = {};
	uint64_t file_offset = UINT64_MAX;
	int ready[2], go[2], fd, status;
	size_t ring_size = 17 * NATIVE_PAGE_SIZE;
	pid_t pid;
	bool sample = false, record = false;

	ksft_print_header();
	ksft_set_plan(7);
	if (pipe(ready) || pipe(go))
		ksft_exit_fail_msg("pipe failed: %s\n", strerror(errno));
	pid = fork();
	if (!pid) {
		close(ready[0]); close(go[1]);
		if (dup2(ready[1], STDOUT_FILENO) < 0 ||
		    dup2(go[0], STDIN_FILENO) < 0)
			_exit(KSFT_FAIL);
		close(ready[1]); close(go[0]);
		p3s_execl(true, NULL, "--child", NULL);
		_exit(KSFT_FAIL);
	}
	close(ready[1]); close(go[0]);
	if (pid < 0 || read(ready[0], &address, sizeof(address)) != sizeof(address)) {
		if (pid > 0 && waitpid(pid, &status, 0) == pid &&
		    WIFEXITED(status) && WEXITSTATUS(status) == KSFT_SKIP)
			ksft_exit_skip("4KB child unavailable\n");
		ksft_exit_fail_msg("child setup failed\n");
	}
	close(ready[0]);
	ksft_test_result_pass("native reader has a real 4KB child\n");
	fd = syscall(__NR_perf_event_open, &attr, pid, -1, -1, 0);
	if (fd < 0)
		ksft_exit_fail_msg("perf_event_open: %s\n", strerror(errno));
	metadata = mmap(NULL, ring_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	ksft_test_result(metadata != MAP_FAILED, "map a native perf ring\n");
	if (metadata == MAP_FAILED || ioctl(fd, PERF_EVENT_IOC_ENABLE, 0))
		ksft_exit_fail_msg("ring setup: %s\n", strerror(errno));
	if (write(go[1], "g", 1) != 1 || waitpid(pid, &status, 0) != pid)
		ksft_exit_fail_msg("child synchronization failed\n");
	close(go[1]);
	ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);
	ksft_test_result(WIFEXITED(status) && !WEXITSTATUS(status),
			 "4KB child maps file offset 4KB and faults an anonymous page\n");
	data = (unsigned char *)metadata + metadata->data_offset;
	head = __atomic_load_n(&metadata->data_head, __ATOMIC_ACQUIRE);
	while (position < head) {
		struct perf_event_header h;

		copy_ring(&h, data, metadata->data_size, position, sizeof(h));
		if (h.size < sizeof(h) || h.size > metadata->data_size)
			break;
		if (h.type == PERF_RECORD_SAMPLE &&
		    h.size >= sizeof(h) + sizeof(sizes)) {
			uint64_t s[3];

			copy_ring(s, data, metadata->data_size, position + sizeof(h), sizeof(s));
			if (s[0] == address) {
				memcpy(sizes, s, sizeof(s));
				sample = true;
			}
		}
		if (h.type == PERF_RECORD_MMAP2 && h.size > 72) {
			char *r = malloc(h.size);

			if (!r)
				ksft_exit_fail_msg("allocation failed\n");
			copy_ring(r, data, metadata->data_size, position, h.size);
			if (memmem(r + 72, h.size - 72, "perf-observer-p3s", 17)) {
				memcpy(&file_offset, r + 32, sizeof(file_offset)); record = true;
			}
			free(r);
		}
		position += h.size;
	}
	ksft_test_result(sample, "capture the child's minor-fault sample\n");
	ksft_test_result(sample && sizes[1] == PROCESS_PAGE_SIZE,
			 "sampled data page size is 4KB\n");
	ksft_test_result(sample && sizes[2] == PROCESS_PAGE_SIZE,
			 "sampled code page size is 4KB\n");
	ksft_test_result(record && file_offset == PROCESS_PAGE_SIZE,
			 "PERF_RECORD_MMAP2 includes the file slice offset\n");
	munmap(metadata, ring_size); close(fd);
	ksft_finished();
}

int main(int argc, char **argv)
{
	p3s_argv0 = argv[0];
	if (argc == 2 && !strcmp(argv[1], "--child"))
		return child();
	if (p3s_is_compat_process()) {
		p3s_execl(false, NULL, "--native", NULL);
		ksft_exit_fail_msg("native re-exec failed: %s\n", strerror(errno));
	}
	if (getpagesize() != NATIVE_PAGE_SIZE)
		ksft_exit_skip("requires native 16KB reader\n");
	return run_test();
}
