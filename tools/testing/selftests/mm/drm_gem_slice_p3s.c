// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include "../kselftest_p3s.h"

static int run_test(void)
{
	struct drm_mode_create_dumb create = { .width = 64, .height = 64, .bpp = 32 };
	struct drm_mode_map_dumb map = {};
	struct drm_mode_destroy_dumb destroy = {};
	unsigned long size = getpagesize();
	char path[64];
	void *mapping;
	int fd = -1, err = 0;
	unsigned int i;

	ksft_print_header();
	for (i = 0; i < 16; i++) {
		snprintf(path, sizeof(path), "/dev/dri/card%u", i);
		fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;
		if (!ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create))
			break;
		close(fd);
		fd = -1;
	}
	if (fd < 0)
		ksft_exit_skip("no DRM device supports a test-owned dumb buffer\n");
	map.handle = create.handle;
	destroy.handle = create.handle;
	if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map)) {
		ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
		close(fd);
		ksft_exit_skip("DRM device cannot map its dumb buffer\n");
	}
	ksft_set_plan(6);
	ksft_test_result(size == PROCESS_PAGE_SIZE || size == NATIVE_PAGE_SIZE,
			 "process page size is supported (%lu)\n", size);
	mapping = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, map.offset);
	if (mapping != MAP_FAILED) {
		*(volatile unsigned int *)mapping = 0x6a39421b;
		err = *(volatile unsigned int *)mapping != 0x6a39421b;
		munmap(mapping, size);
	}
	ksft_test_result(mapping != MAP_FAILED && !err,
			 "aligned nonzero GEM cookie maps a test-owned buffer\n");
	for (i = 1; i < 4; i++) {
		int saved_errno;

		if (size != PROCESS_PAGE_SIZE) {
			ksft_test_result_skip("slice %u requires a 4KB process\n", i);
			continue;
		}
		errno = 0;
		mapping = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
			       fd, map.offset + i * PROCESS_PAGE_SIZE);
		saved_errno = errno;
		ksft_test_result(mapping == MAP_FAILED && saved_errno == EINVAL,
				 "reject GEM cookie with slice %u (errno=%d)\n",
				 i, saved_errno);
		if (mapping != MAP_FAILED)
			munmap(mapping, size);
	}
	ksft_test_result(!ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy),
			 "destroy the test-owned GEM object\n");
	close(fd);
	ksft_finished();
}

int main(int argc, char **argv)
{
	return p3s_geometry_main(argc, argv, run_test);
}
