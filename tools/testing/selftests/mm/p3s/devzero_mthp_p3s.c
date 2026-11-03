// SPDX-License-Identifier: GPL-2.0
/*
 * A private /dev/zero VMA retains vm_file, but a write fault installs an
 * anonymous folio.  With native 16K pages and 64K mTHP enabled, its PTEs
 * must map that folio, not page frames derived from a file offset.
 *
 * Run only on a disposable or fixed kernel: an unfixed kernel can map and
 * write unrelated physical pages.  Restore the global THP controls on exit.
 */
#define _GNU_SOURCE

#include <limits.h>
#include <sys/mman.h>

#include "p3s_test.h"

#define THP_ROOT	"/sys/kernel/mm/transparent_hugepage"
#define MTHP_SIZE	(64 * 1024UL)
#define BLOCKS		16
#define KPF_ANON	(1ULL << 12)
#define KPF_THP		(1ULL << 22)

static char saved_global[32], saved_size[32];

static bool read_setting(const char *path, char *saved, size_t size)
{
	char line[128];
	const char *open, *close;
	FILE *file = fopen(path, "re");

	if (!file)
		return false;
	if (!fgets(line, sizeof(line), file)) {
		fclose(file);
		return false;
	}
	fclose(file);
	open = strchr(line, '[');
	close = open ? strchr(open, ']') : NULL;
	if (!close || close - open <= 1 || (size_t)(close - open) >= size)
		return false;
	snprintf(saved, size, "%.*s", (int)(close - open - 1), open + 1);
	return true;
}

static bool write_setting(const char *path, const char *value)
{
	size_t len = strlen(value);
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	bool ok = fd >= 0 && write(fd, value, len) == (ssize_t)len;

	if (fd >= 0)
		close(fd);
	return ok;
}

static void restore_settings(void)
{
	if (saved_size[0])
		write_setting(THP_ROOT "/hugepages-64kB/enabled", saved_size);
	if (saved_global[0])
		write_setting(THP_ROOT "/enabled", saved_global);
}

static bool page_flags(uint64_t pfn, uint64_t *flags)
{
	int fd = open("/proc/kpageflags", O_RDONLY | O_CLOEXEC);
	bool ok;

	if (fd < 0)
		return false;
	ok = pread(fd, flags, sizeof(*flags), pfn * sizeof(*flags)) ==
	     sizeof(*flags);
	close(fd);
	return ok;
}

struct outcome {
	unsigned int checked, anon, huge, data_errors;
	bool page_info;
};

static struct outcome check_mapping(bool devzero)
{
	const size_t len = BLOCKS * MTHP_SIZE;
	struct outcome result = { .page_info = true };
	unsigned char *raw, *base;
	int fd = -1, flags = MAP_PRIVATE;
	unsigned int block, page;

	if (devzero)
		fd = open("/dev/zero", O_RDWR | O_CLOEXEC);
	else
		flags |= MAP_ANONYMOUS;
	if (devzero && fd < 0) {
		result.page_info = false;
		return result;
	}
	raw = mmap(NULL, len + MTHP_SIZE, PROT_READ | PROT_WRITE,
		   flags, fd, 0);
	if (fd >= 0)
		close(fd);
	if (raw == MAP_FAILED) {
		result.page_info = false;
		return result;
	}
	base = (void *)(((uintptr_t)raw + MTHP_SIZE - 1) &
			 ~(uintptr_t)(MTHP_SIZE - 1));
	if (base > raw)
		munmap(raw, base - raw);
	if (raw + len + MTHP_SIZE > base + len)
		munmap(base + len, raw + len + MTHP_SIZE - (base + len));
	if (madvise(base, len, MADV_HUGEPAGE)) {
		result.page_info = false;
		goto out;
	}
	for (block = 0; block < BLOCKS; block++) {
		unsigned char *p = base + block * MTHP_SIZE;
		uint64_t pfn, bits;

		for (page = 0; page < MTHP_SIZE / NATIVE_PAGE_SIZE; page++)
			p[page * NATIVE_PAGE_SIZE] = block + page + 1;
		for (page = 0; page < MTHP_SIZE / NATIVE_PAGE_SIZE; page++)
			if (p[page * NATIVE_PAGE_SIZE] != block + page + 1)
				result.data_errors++;
		if (!p3s_pfn(p, &pfn) || !page_flags(pfn, &bits)) {
			result.page_info = false;
			break;
		}
		result.checked++;
		result.anon += !!(bits & KPF_ANON);
		result.huge += !!(bits & KPF_THP);
		if (block < 4)
			ksft_print_msg("%s block %u: PFN %#llx flags %#llx\n",
				       devzero ? "devzero" : "control", block,
				       (unsigned long long)pfn,
				       (unsigned long long)bits);
	}
out:
	munmap(base, len);
	return result;
}

int main(void)
{
	struct outcome control, devzero;

	ksft_print_header();
	if (getpagesize() != (int)NATIVE_PAGE_SIZE) {
		if (p3s_is_compat_process())
			exec_native(NULL, NULL);
		ksft_exit_skip("requires native 16K pages\n");
	}
	if (geteuid())
		ksft_exit_skip("root is required to read PFNs and set mTHP policy\n");
	if (!read_setting(THP_ROOT "/enabled", saved_global,
			  sizeof(saved_global)) ||
	    !read_setting(THP_ROOT "/hugepages-64kB/enabled", saved_size,
			  sizeof(saved_size)))
		ksft_exit_skip("64K anonymous mTHP is unavailable\n");
	atexit(restore_settings);
	if (!write_setting(THP_ROOT "/enabled", "always") ||
	    !write_setting(THP_ROOT "/hugepages-64kB/enabled", "always"))
		ksft_exit_skip("cannot enable 64K mTHP\n");
	/* Probe PFN visibility before starting the corruption-sensitive case. */
	control = check_mapping(false);
	if (!control.page_info || control.checked != BLOCKS || !control.huge)
		ksft_exit_skip("control did not allocate visible 64K anon folios\n");
	ksft_set_plan(2);
	ksft_test_result(control.anon == BLOCKS && !control.data_errors,
			 "anonymous mTHP control maps its own folios\n");
	devzero = check_mapping(true);
	restore_settings();
	ksft_print_msg("private /dev/zero: checked=%u anon=%u mTHP=%u data_errors=%u\n",
		       devzero.checked, devzero.anon, devzero.huge,
		       devzero.data_errors);
	ksft_test_result(devzero.page_info && devzero.checked == BLOCKS &&
			 devzero.anon == BLOCKS && devzero.huge &&
			 !devzero.data_errors,
			 "private /dev/zero maps anonymous folios, not unrelated PFNs\n");
	ksft_finished();
}
