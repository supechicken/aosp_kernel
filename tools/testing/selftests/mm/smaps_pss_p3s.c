// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

/*
 * A P3S file folio can be mapped by several 4K PTEs, one per slice.  Those
 * PTEs overlap at struct-page granularity but not necessarily in physical
 * bytes.  Check conservative page-level sharing, capped at each mapped slice.
 */

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
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "../kselftest_p3s.h"
#include "vm_util.h"

#define USER_PAGE_SIZE 4096UL
#define MAPPING_SIZE (4 * USER_PAGE_SIZE)

struct smaps_stats {
	unsigned long rss_kb;
	unsigned long pss_kb;
	unsigned long shared_clean_kb;
	unsigned long shared_dirty_kb;
	unsigned long private_clean_kb;
	unsigned long private_dirty_kb;
};

static bool read_smaps_stats(uintptr_t address, struct smaps_stats *stats)
{
	char *line = NULL;
	size_t line_size = 0;
	bool in_mapping = false;
	FILE *file;

	memset(stats, 0, sizeof(*stats));
	file = fopen("/proc/self/smaps", "re");
	if (!file)
		return false;

	while (getline(&line, &line_size, file) >= 0) {
		unsigned long start, end, value;

		if (sscanf(line, "%lx-%lx", &start, &end) == 2) {
			if (in_mapping)
				break;
			in_mapping = address >= start && address < end;
			continue;
		}
		if (!in_mapping)
			continue;
		if (sscanf(line, "Rss: %lu kB", &value) == 1)
			stats->rss_kb = value;
		else if (sscanf(line, "Pss: %lu kB", &value) == 1)
			stats->pss_kb = value;
		else if (sscanf(line, "Shared_Clean: %lu kB", &value) == 1)
			stats->shared_clean_kb = value;
		else if (sscanf(line, "Shared_Dirty: %lu kB", &value) == 1)
			stats->shared_dirty_kb = value;
		else if (sscanf(line, "Private_Clean: %lu kB", &value) == 1)
			stats->private_clean_kb = value;
		else if (sscanf(line, "Private_Dirty: %lu kB", &value) == 1)
			stats->private_dirty_kb = value;
	}

	free(line);
	fclose(file);
	return in_mapping && stats->rss_kb;
}

static bool stats_equal(const struct smaps_stats *stats,
			unsigned long rss, unsigned long pss,
			unsigned long shared, unsigned long private)
{
	return stats->rss_kb == rss && stats->pss_kb == pss &&
	       stats->shared_clean_kb + stats->shared_dirty_kb == shared &&
	       stats->private_clean_kb + stats->private_dirty_kb == private;
}

static void show_result(const char *name, const struct smaps_stats *stats,
			bool pass, int *test_no, int *failures)
{
	printf("%s %d - %s\n", pass ? "ok" : "not ok", ++*test_no, name);
	printf("# Rss=%lu Pss=%lu Shared=%lu Private=%lu kB\n",
	       stats->rss_kb, stats->pss_kb,
	       stats->shared_clean_kb + stats->shared_dirty_kb,
	       stats->private_clean_kb + stats->private_dirty_kb);
	if (!pass)
		(*failures)++;
}

static int run_test(void)
{
	struct smaps_stats stats;
	unsigned char *alias = MAP_FAILED;
	unsigned char *mapping = MAP_FAILED;
	unsigned long i;
	int failures = 0;
	int test_no = 0;
	int fd = -1;

	printf("TAP version 13\n1..5\n");
	printf("%s %d - process page size is 4K\n",
	       sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE ? "ok" : "not ok",
	       ++test_no);
	if (sysconf(_SC_PAGESIZE) != USER_PAGE_SIZE) {
		failures++;
		goto out;
	}

	fd = syscall(SYS_memfd_create, "smaps-pss-p3s", MFD_CLOEXEC);
	if (fd < 0) {
		perror("memfd_create");
		return EXIT_FAILURE;
	}
	if (ftruncate(fd, MAPPING_SIZE)) {
		perror("ftruncate");
		return EXIT_FAILURE;
	}
	mapping = mmap(NULL, MAPPING_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
		       fd, 0);
	if (mapping == MAP_FAILED) {
		perror("mmap");
		return EXIT_FAILURE;
	}
	for (i = 0; i < MAPPING_SIZE; i += USER_PAGE_SIZE)
		mapping[i] = (unsigned char)(0x40 + i / USER_PAGE_SIZE);

	if (!read_smaps_stats((uintptr_t)mapping, &stats)) {
		perror("read smaps");
		goto out;
	}
	show_result("disjoint slices are private and have full PSS", &stats,
		    stats_equal(&stats, 16, 16, 0, 16),
		    &test_no, &failures);

	alias = mmap(NULL, USER_PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
		     fd, 0);
	if (alias == MAP_FAILED) {
		perror("mmap alias");
		goto out;
	}
	alias[0] ^= 1;
	if (!read_smaps_stats((uintptr_t)mapping, &stats)) {
		perror("read target smaps with alias");
		goto out;
	}
	show_result("an alias shares the native page conservatively", &stats,
		    stats_equal(&stats, 16, 12, 16, 0),
		    &test_no, &failures);

	if (!read_smaps_stats((uintptr_t)alias, &stats)) {
		perror("read alias smaps");
		goto out;
	}
	show_result("the alias receives its native-page mapcount share", &stats,
		    stats_equal(&stats, 4, 3, 4, 0),
		    &test_no, &failures);

	munmap(alias, USER_PAGE_SIZE);
	alias = MAP_FAILED;
	if (!read_smaps_stats((uintptr_t)mapping, &stats)) {
		perror("read target smaps after unmap");
		goto out;
	}
	show_result("unmapping the alias restores exclusive accounting", &stats,
		    stats_equal(&stats, 16, 16, 0, 16),
		    &test_no, &failures);

out:
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
