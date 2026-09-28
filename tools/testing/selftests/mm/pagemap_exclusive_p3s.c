// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

/* Check that pagemap conservatively detects aliases of a native file page. */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/personality.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "../kselftest_p3s.h"
#include "vm_util.h"

#define USER_PAGE_SIZE 4096UL
#define NR_SLICES 4
#define MAPPING_SIZE (NR_SLICES * USER_PAGE_SIZE)
static int failures;
static int test_no;

static void result(bool pass, const char *name)
{
	printf("%s %d - %s\n", pass ? "ok" : "not ok", ++test_no, name);
	if (!pass)
		failures++;
}

static int pagemap_fd = -1;

static int read_pagemap(void *address, uint64_t *entry)
{
	if (pagemap_fd < 0)
		pagemap_fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
	if (pagemap_fd < 0)
		return -1;
	*entry = pagemap_get_entry(pagemap_fd, address);
	return 0;
}

static bool check_exclusive(unsigned char *mapping, unsigned int exclusive_mask)
{
	unsigned int observed = 0;
	unsigned int i;

	for (i = 0; i < NR_SLICES; i++) {
		uint64_t entry;

		if (read_pagemap(mapping + i * USER_PAGE_SIZE, &entry)) {
			perror("read pagemap");
			return false;
		}
		if ((entry & (PM_PRESENT | PM_FILE)) != (PM_PRESENT | PM_FILE)) {
			printf("# slice %u missing present/file flags: %#llx\n", i,
			       (unsigned long long)entry);
			return false;
		}
		if (entry & PM_MMAP_EXCLUSIVE)
			observed |= 1U << i;
	}
	printf("# exclusive mask: expected=%#x observed=%#x\n",
	       exclusive_mask, observed);
	return observed == exclusive_mask;
}

static int run_test(void)
{
	unsigned char *mapping = MAP_FAILED;
	unsigned char *alias = MAP_FAILED;
	unsigned int i;
	uint64_t alias_entry;
	int fd = -1;

	printf("TAP version 13\n1..5\n");
	result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
	       "process page size is 4K");
	if (failures)
		goto out;

	fd = syscall(SYS_memfd_create, "pagemap-exclusive-p3s", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, MAPPING_SIZE)) {
		perror("prepare memfd");
		goto out;
	}
	mapping = mmap(NULL, MAPPING_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
		       fd, 0);
	if (mapping == MAP_FAILED) {
		perror("mmap target");
		goto out;
	}
	for (i = 0; i < NR_SLICES; i++)
		mapping[i * USER_PAGE_SIZE] = (unsigned char)i;
	result(check_exclusive(mapping, 0xf),
	       "disjoint file slices are exclusively mapped");

	alias = mmap(NULL, USER_PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
		     fd, 0);
	if (alias == MAP_FAILED) {
		perror("mmap alias");
		goto out;
	}
	alias[0] ^= 1;
	result(check_exclusive(mapping, 0),
	       "a file-page alias clears exclusivity for the slice window");

	if (read_pagemap(alias, &alias_entry)) {
		perror("read alias pagemap");
		goto out;
	}
	result(!(alias_entry & PM_MMAP_EXCLUSIVE),
	       "the second mapping of a slice is not exclusive");

	munmap(alias, USER_PAGE_SIZE);
	alias = MAP_FAILED;
	result(check_exclusive(mapping, 0xf),
	       "unmapping the alias restores exclusivity");

out:
	if (pagemap_fd >= 0)
		close(pagemap_fd);
	if (test_no < 5)
		failures += 5 - test_no;
	if (alias != MAP_FAILED)
		munmap(alias, USER_PAGE_SIZE);
	if (mapping != MAP_FAILED)
		munmap(mapping, MAPPING_SIZE);
	if (fd >= 0)
		close(fd);
	printf("# Totals: pass:%d fail:%d\n", 5 - failures, failures);
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}

P3S_COMPAT_MAIN(run_test)
