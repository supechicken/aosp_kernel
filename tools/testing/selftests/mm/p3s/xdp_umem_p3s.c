// SPDX-License-Identifier: GPL-2.0
/* 4KB processes refuse UMEM registration; native mappings remain supported. */
#define _GNU_SOURCE

#include <linux/if_xdp.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include "kselftest_p3s.h"

#ifndef AF_XDP
#define AF_XDP 44
#endif
#ifndef SOL_XDP
#define SOL_XDP 283
#endif

#define TAG_MASK		(0xb4ULL << 56)

static int register_umem(uint64_t address, size_t len)
{
	struct xdp_umem_reg registration = {
		.addr = address,
		.len = len,
		.chunk_size = 2048,
	};
	int fd;
	int ret;

	fd = socket(AF_XDP, SOCK_RAW | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -errno;
	ret = setsockopt(fd, SOL_XDP, XDP_UMEM_REG, &registration,
			 sizeof(registration));
	if (ret)
		ret = -errno;
	close(fd);
	return ret;
}

static int run_test(void)
{
	unsigned char *mapping;
	unsigned int tag, large;
	bool compat = p3s_is_compat_process();
	int fd;

	ksft_print_header();
	ksft_set_plan(4);
	fd = memfd_create("xdp-umem-p3s", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, NATIVE_PAGE_SIZE))
		ksft_exit_fail_msg("memfd failed: %s\n", strerror(errno));
	mapping = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE,
		       MAP_SHARED, fd, 0);
	if (mapping == MAP_FAILED ||
	    ((uintptr_t)mapping & (NATIVE_PAGE_SIZE - 1)))
		ksft_exit_fail_msg("native-aligned mmap failed\n");
	memset(mapping, 0x5a, NATIVE_PAGE_SIZE);
	for (large = 0; large < 2; large++) {
		size_t len = large ? NATIVE_PAGE_SIZE : PROCESS_PAGE_SIZE;

		for (tag = 0; tag < 2; tag++) {
			uint64_t address = (uintptr_t)mapping | (tag ? TAG_MASK : 0);
			int ret = register_umem(address, len);

			if (ret == -EAFNOSUPPORT || ret == -EPROTONOSUPPORT ||
			    ret == -EPERM || ret == -EACCES)
				ksft_exit_skip("AF_XDP unavailable: %s\n", strerror(-ret));
			ksft_test_result(ret == (compat ? -EOPNOTSUPP : 0),
				"%s %zuK UMEM tagged=%u: %s\n",
				compat ? "reject compat" : "accept native", len / 1024,
				tag, ret ? strerror(-ret) : "ok");
		}
	}
	munmap(mapping, NATIVE_PAGE_SIZE);
	close(fd);
	ksft_finished();
}

int main(int argc, char **argv)
{
	return p3s_geometry_main(argc, argv, run_test);
}
