/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _P3S_TEST_H
#define _P3S_TEST_H

#include "../kselftest_p3s.h"
#include "vm_util.h"

/* Path of a /proc file of @pid; 0 means this process. */
static inline void p3s_proc_path(char *path, size_t size, pid_t pid,
				 const char *file)
{
	if (pid)
		snprintf(path, size, "/proc/%d/%s", (int)pid, file);
	else
		snprintf(path, size, "/proc/self/%s", file);
}

/* The pagemap entry of @address in this process. */
static inline bool p3s_pagemap_entry(const void *address, uint64_t *entry)
{
	int fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
	bool ok;

	if (fd < 0)
		return false;
	ok = pagemap_read_entry(fd, (uintptr_t)address, getpagesize(), entry);
	close(fd);
	return ok;
}

/* P3S pagemap reports compat PFNs in 4K units; compare native folios. */
static inline uint64_t p3s_native_pfn(uint64_t pfn)
{
	return getpagesize() == PROCESS_PAGE_SIZE ? pfn / P3S_SLICES : pfn;
}

/* The native PFN of a present page; false when absent or hidden. */
static inline bool p3s_pfn(const void *address, uint64_t *pfn)
{
	uint64_t entry;

	if (!p3s_pagemap_entry(address, &entry) || !(entry & PM_PRESENT))
		return false;
	*pfn = p3s_native_pfn(PAGEMAP_PFN(entry));
	return *pfn != 0;
}

static inline bool p3s_page_present(const void *address)
{
	uint64_t entry;

	return p3s_pagemap_entry(address, &entry) &&
	       (entry & PM_PRESENT);
}

static inline bool p3s_page_swapped(const void *address)
{
	uint64_t entry;

	return p3s_pagemap_entry(address, &entry) &&
	       !(entry & PM_PRESENT) && (entry & PM_SWAP);
}

/*
 * Sum of the "@field:" line, in bytes, over every VMA of @pid (0 = self)
 * that overlaps [@address, @address + @length).  False when no VMA matched.
 * With @length 1 this is the field of the single VMA containing @address.
 */
static inline bool p3s_smaps_sum(pid_t pid, uintptr_t address, size_t length,
				 const char *field, unsigned long *bytes)
{
	uintptr_t target_end = address + length;
	unsigned long start, end, value_kb;
	char path[64], format[64];
	char *line = NULL;
	size_t capacity = 0;
	bool found = false, in_target = false;
	FILE *smaps;

	*bytes = 0;
	snprintf(format, sizeof(format), "%s: %%lu kB", field);
	p3s_proc_path(path, sizeof(path), pid, "smaps");
	smaps = fopen(path, "re");
	if (!smaps)
		return false;
	while (getline(&line, &capacity, smaps) >= 0) {
		if (sscanf(line, "%lx-%lx", &start, &end) == 2) {
			in_target = address < end && target_end > start;
			continue;
		}
		if (in_target && sscanf(line, format, &value_kb) == 1) {
			*bytes += value_kb * 1024;
			found = true;
			in_target = false;
		}
	}
	free(line);
	fclose(smaps);
	return found;
}

/* The same for a range of this process. */
static inline bool p3s_smaps_bytes(const void *address, size_t length,
				   const char *field, unsigned long *bytes)
{
	return p3s_smaps_sum(0, (uintptr_t)address, length, field, bytes);
}

/* /proc/<pid>/status */

/* The "@field:" line of @pid's status (0 = self) in kB, or -1. */
static inline long p3s_status_kb(pid_t pid, const char *field)
{
	char path[64], line[256];
	size_t name_len = strlen(field);
	long value = -1;
	FILE *status;

	p3s_proc_path(path, sizeof(path), pid, "status");
	status = fopen(path, "re");
	if (!status)
		return -1;
	while (fgets(line, sizeof(line), status)) {
		if (!strncmp(line, field, name_len) && line[name_len] == ':') {
			if (sscanf(line + name_len + 1, " %ld kB", &value) != 1)
				value = -1;
			break;
		}
	}
	fclose(status);
	return value;
}

/* Test fixtures (device nodes, debugfs files, ...) */

/* An error from opening a fixture that means "not on this system". */
static inline bool p3s_fixture_unavailable(int error)
{
	return error == ENOENT || error == ENXIO || error == ENODEV ||
	       error == ENOTDIR || error == EACCES || error == EPERM;
}

/* open(2) a fixture with O_CLOEXEC; -1 with errno preserved on failure. */
static inline int p3s_open_fixture(const char *path, int flags)
{
	return open(path, flags | O_CLOEXEC);
}

/*
 * open(2) a fixture the test cannot run without.  A missing or inaccessible
 * fixture skips the test; any other failure fails it.
 */
static inline int p3s_open_fixture_or_skip(const char *path, int flags)
{
	int fd = p3s_open_fixture(path, flags);

	if (fd >= 0)
		return fd;
	if (p3s_fixture_unavailable(errno))
		ksft_exit_skip("%s is unavailable: %s\n", path, strerror(errno));
	ksft_exit_fail_msg("open %s failed: %s\n", path, strerror(errno));
}

#endif /* _P3S_TEST_H */
