// SPDX-License-Identifier: GPL-2.0
/* BPF map mmap is native-only, including FDs inherited by a 4KB process. */
#define _GNU_SOURCE
#include <linux/bpf.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#include "../kselftest_p3s.h"

static const enum bpf_map_type types[] = {
	BPF_MAP_TYPE_ARRAY, BPF_MAP_TYPE_RINGBUF, BPF_MAP_TYPE_USER_RINGBUF,
};
static int map_fds[3];

static int sys_bpf(enum bpf_cmd cmd, union bpf_attr *attr)
{
	return syscall(__NR_bpf, cmd, attr, sizeof(*attr));
}

static int create_map(enum bpf_map_type type, unsigned int size)
{
	union bpf_attr attr = { .map_type = type, .max_entries = size };

	if (type == BPF_MAP_TYPE_ARRAY) {
		attr.key_size = sizeof(unsigned int);
		attr.value_size = sizeof(uint64_t);
		attr.map_flags = BPF_F_MMAPABLE;
	}
	return sys_bpf(BPF_MAP_CREATE, &attr);
}

static bool check_mapping(int fd, size_t size, int prot, off_t offset,
			  int native_errno, const uint64_t *expected)
{
	bool compat = p3s_is_compat_process();
	int wanted = compat ? EOPNOTSUPP : native_errno;
	void *p;
	bool ok;

	errno = 0;
	p = mmap(NULL, size, prot, MAP_SHARED, fd, offset);
	if (wanted) {
		ok = p == MAP_FAILED && errno == wanted;
	} else {
		ok = p != MAP_FAILED;
		if (ok && expected)
			ok = *(uint64_t *)p == *expected;
	}
	if (!ok)
		ksft_print_msg("mmap size=%zu prot=%#x offset=%lld: %p errno=%d expected=%d\n",
			       size, prot, (long long)offset, p, errno, wanted);
	if (p != MAP_FAILED)
		munmap(p, size);
	return ok;
}

static int run_test(void)
{
	uint64_t marker = 0x1234567887654321ULL, value = 0;
	unsigned int key = 0;
	union bpf_attr attr = {
		.map_fd = map_fds[0], .key = (unsigned long)&key,
		.value = (unsigned long)&marker, .flags = BPF_ANY,
	};
	size_t page = getpagesize();
	int i, fd;
	bool ok;

	ksft_print_header();
	ksft_set_plan(13);
	ksft_print_msg("map FDs were created in a native 16KB process; caller page size=%zu\n",
		       page);
	ok = !sys_bpf(BPF_MAP_UPDATE_ELEM, &attr);
	attr.value = (unsigned long)&value;
	ok = ok && !sys_bpf(BPF_MAP_LOOKUP_ELEM, &attr) && value == marker;
	ksft_test_result(ok, "map update and lookup remain usable without mmap\n");
	ksft_test_result(check_mapping(map_fds[0], page,
			 PROT_READ | PROT_WRITE, 0, 0, &marker),
			 "array: native mapping or 4KB EOPNOTSUPP\n");
	ksft_test_result(check_mapping(map_fds[0], page, PROT_READ, page,
			 EINVAL, NULL), "array: bounds or 4KB refusal\n");
	ksft_test_result(check_mapping(map_fds[1], page,
			 PROT_READ | PROT_WRITE, 0, 0, NULL),
			 "kernel ring: consumer mapping or 4KB refusal\n");
	ksft_test_result(check_mapping(map_fds[1], page, PROT_READ, page,
			 0, NULL), "kernel ring: producer mapping or 4KB refusal\n");
	ksft_test_result(check_mapping(map_fds[1], 2 * NATIVE_PAGE_SIZE,
			 PROT_READ, 2 * page, 0, NULL),
			 "kernel ring: data aliases or 4KB refusal\n");
	ksft_test_result(check_mapping(map_fds[1], page,
			 PROT_READ | PROT_WRITE, page, EPERM, NULL),
			 "kernel ring: writable producer protection or 4KB refusal\n");
	ksft_test_result(check_mapping(map_fds[2], page, PROT_READ, 0,
			 0, NULL), "user ring: consumer mapping or 4KB refusal\n");
	ksft_test_result(check_mapping(map_fds[2], page,
			 PROT_READ | PROT_WRITE, page, 0, NULL),
			 "user ring: producer mapping or 4KB refusal\n");
	ksft_test_result(check_mapping(map_fds[2], 2 * NATIVE_PAGE_SIZE,
			 PROT_READ | PROT_WRITE, 2 * page, 0, NULL),
			 "user ring: data aliases or 4KB refusal\n");
	ksft_test_result(check_mapping(map_fds[2], page,
			 PROT_READ | PROT_WRITE, 0, EPERM, NULL),
			 "user ring: consumer write protection or 4KB refusal\n");
	for (i = 1; i <= 2; i++) {
		errno = 0;
		fd = create_map(BPF_MAP_TYPE_RINGBUF, i * PROCESS_PAGE_SIZE);
		ksft_test_result(fd < 0 && errno == EINVAL,
				 "retain kernel-page alignment for a %dKB ring\n", i * 4);
		if (fd >= 0)
			close(fd);
	}
	for (i = 0; i < 3; i++)
		close(map_fds[i]);
	ksft_finished();
}

int main(int argc, char **argv)
{
	const char *saved = getenv("P3S_BPF_NATIVE_FDS");
	char buffer[96];
	int i;

	if (saved) {
		if (sscanf(saved, "%d,%d,%d", &map_fds[0], &map_fds[1],
			   &map_fds[2]) != 3)
			ksft_exit_fail_msg("invalid inherited map FDs\n");
	} else if (getpagesize() == NATIVE_PAGE_SIZE) {
		for (i = 0; i < 3; i++) {
			map_fds[i] = create_map(types[i], i ? NATIVE_PAGE_SIZE : 1);
			if (map_fds[i] < 0) {
				if (errno == EPERM || errno == EACCES)
					ksft_exit_skip("BPF maps unavailable: %s\n", strerror(errno));
				ksft_exit_fail_msg("create map type %u: %s\n", types[i], strerror(errno));
			}
			if (fcntl(map_fds[i], F_SETFD, 0))
				ksft_exit_fail_msg("retain map FD across exec\n");
		}
		snprintf(buffer, sizeof(buffer), "%d,%d,%d", map_fds[0],
			 map_fds[1], map_fds[2]);
		if (setenv("P3S_BPF_NATIVE_FDS", buffer, 1))
			ksft_exit_fail_msg("save map FDs\n");
	}
	return p3s_geometry_main(argc, argv, run_test);
}
