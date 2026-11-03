// SPDX-License-Identifier: GPL-2.0
/*
 * Verify physical mlock state on an existing large file folio.  Ordinary
 * Android apps can obtain these from EROFS even with anonymous THP disabled.
 * The file is opened read-only and is never changed.  PFN observation needs
 * privilege; the mlock requests themselves fit Android's 64K app limit.
 */
#define _GNU_SOURCE

#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>

#include "p3s_test.h"

#define FOLIO_BYTES (64 * 1024UL)
#define MAX_FILE_BYTES (64 * 1024 * 1024UL)
#define KPF_HEAD (1ULL << 15)
#define KPF_TAIL (1ULL << 16)
#define KPF_UNEVICTABLE (1ULL << 18)
#define KPF_MLOCKED (1ULL << 33)

static int flags_fd, pagemap_fd;

static bool read_flags(uint64_t pfn, uint64_t *flags)
{
	return pread(flags_fd, flags, sizeof(*flags), pfn * sizeof(*flags)) ==
	       sizeof(*flags);
}

static bool mapped_pfn(const void *addr, uint64_t *pfn)
{
	uint64_t entry;
	off_t offset = (uintptr_t)addr / getpagesize() * sizeof(entry);

	if (pread(pagemap_fd, &entry, sizeof(entry), offset) != sizeof(entry) ||
	    !(entry & PM_PRESENT))
		return false;
	*pfn = p3s_native_pfn(PAGEMAP_PFN(entry));
	return *pfn != 0;
}

/* Require an entire, initially unlocked 64K folio, not merely contiguous PFNs. */
static bool candidate(const unsigned char *addr, uint64_t *head)
{
	uint64_t pfn, flags;
	size_t off;

	if (!mapped_pfn(addr, head) || !read_flags(*head, &flags) ||
	    !(flags & KPF_HEAD) || (flags & KPF_MLOCKED))
		return false;
	for (off = 0; off < FOLIO_BYTES; off += getpagesize()) {
		if (!mapped_pfn(addr + off, &pfn) ||
		    pfn != *head + off / NATIVE_PAGE_SIZE ||
		    !read_flags(pfn, &flags) || (flags & KPF_MLOCKED) ||
		    !(flags & (off < NATIVE_PAGE_SIZE ? KPF_HEAD : KPF_TAIL)))
			return false;
	}
	return read_flags(*head + FOLIO_BYTES / NATIVE_PAGE_SIZE, &flags) &&
	       !(flags & KPF_TAIL);
}

static bool physical_state(const unsigned char *addr, uint64_t head, bool locked)
{
	uint64_t pfn, flags;
	size_t off;

	for (off = 0; off < FOLIO_BYTES; off += getpagesize()) {
		if (!mapped_pfn(addr + off, &pfn) ||
		    pfn != head + off / NATIVE_PAGE_SIZE ||
		    !read_flags(pfn, &flags))
			return false;
		if (locked) {
			if ((flags & (KPF_MLOCKED | KPF_UNEVICTABLE)) !=
			    (KPF_MLOCKED | KPF_UNEVICTABLE))
				return false;
		} else if (flags & KPF_MLOCKED) {
			return false;
		}
	}
	return true;
}

static int run_test(void)
{
	const char *file = getenv("P3S_MLOCK_FILE");
	struct rlimit limit;
	struct stat st;
	unsigned char *map, *folio = NULL;
	uint64_t head = 0;
	size_t size, off, page = getpagesize();
	unsigned long checksum = 0;
	unsigned int i;
	int fd;
	struct {
		const char *name;
		size_t offset, length;
		bool onfault, locked;
	} cases[] = {
		{ "full folio", 0, FOLIO_BYTES, false, true },
		{ "first process page", 0, page, false, false },
		{ "last process page", FOLIO_BYTES - page, page, false, false },
		{ "interior process page", page, page, false, false },
		{ "prefaulted MLOCK_ONFAULT folio", 0, FOLIO_BYTES, true, true },
	};

	ksft_print_header();
	if (!file)
		file = "/system/framework/framework-res.apk";
	fd = p3s_open_fixture_or_skip(file, O_RDONLY);
	flags_fd = p3s_open_fixture_or_skip("/proc/kpageflags", O_RDONLY);
	pagemap_fd = p3s_open_fixture_or_skip("/proc/self/pagemap", O_RDONLY);
	if (fstat(fd, &st) || getrlimit(RLIMIT_MEMLOCK, &limit))
		ksft_exit_fail_msg("fixture metadata: %s\n", strerror(errno));
	if (limit.rlim_cur < FOLIO_BYTES)
		ksft_exit_skip("RLIMIT_MEMLOCK must allow 64K\n");
	if (st.st_size < 2 * (off_t)FOLIO_BYTES)
		ksft_exit_skip("fixture is too small\n");
	size = st.st_size < (off_t)MAX_FILE_BYTES ? st.st_size : MAX_FILE_BYTES;
	size &= ~(NATIVE_PAGE_SIZE - 1);
	if (size < 2 * FOLIO_BYTES)
		ksft_exit_skip("fixture is too small\n");
	/* Exercise a compat VMA whose virtual base is not native-page aligned. */
	map = mmap((void *)(p3s_is_compat_process() ? 0x50001000UL : 0x50000000UL),
		   size, PROT_READ, MAP_PRIVATE | MAP_FIXED_NOREPLACE, fd, 0);
	if (map == MAP_FAILED)
		ksft_exit_fail_msg("mmap: %s\n", strerror(errno));
	for (off = 0; off < size; off += page)
		checksum += map[off];
	/* Avoid the file's hot header; never drop another process's page cache. */
	for (off = FOLIO_BYTES; off + FOLIO_BYTES < size; off += FOLIO_BYTES) {
		if (candidate(map + off, &head)) {
			folio = map + off;
			break;
		}
	}
	if (!folio)
		ksft_exit_skip("no visible, unlocked 64K file folio; use P3S_MLOCK_FILE\n");
	ksft_print_msg("page=%zu file=%s offset=%zu pfn=%llu checksum=%lu\n",
		       page, file, off, (unsigned long long)head, checksum);
	ksft_set_plan(2 * (sizeof(cases) / sizeof(cases[0])) + 4);
	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		unsigned char *start = folio + cases[i].offset;
		int rc;

		if (!physical_state(folio, head, false))
			ksft_exit_fail_msg("folio changed or remained locked before %s\n",
					   cases[i].name);
		errno = 0;
		rc = cases[i].onfault ? mlock2(start, cases[i].length, MLOCK_ONFAULT) :
				       mlock(start, cases[i].length);
		ksft_print_msg("%s rc=%d errno=%d\n", cases[i].name, rc, errno);
		ksft_test_result(!rc && physical_state(folio, head, cases[i].locked),
				"%s has expected physical mlock state\n", cases[i].name);
		rc = munlock(start, cases[i].length);
		ksft_test_result(!rc && physical_state(folio, head, false),
				"%s unlock clears physical mlock state\n", cases[i].name);
	}
	/* Start with no PTEs in this alias, with the same file folio cached. */
	for (i = 0; i < 2; i++) {
		unsigned char *cold;
		const char *name = i ? "cold MLOCK_ONFAULT" : "cold mlock";
		int rc;

		cold = mmap((void *)(p3s_is_compat_process() ? 0x60003000UL : 0x60000000UL),
			    FOLIO_BYTES, PROT_READ, MAP_PRIVATE | MAP_FIXED_NOREPLACE,
			    fd, folio - map);
		if (cold == MAP_FAILED)
			ksft_exit_fail_msg("cold mmap: %s\n", strerror(errno));
		rc = i ? mlock2(cold, FOLIO_BYTES, MLOCK_ONFAULT) : mlock(cold, FOLIO_BYTES);
		if (!rc)
			for (off = 0; off < FOLIO_BYTES; off += page)
				checksum += ((volatile unsigned char *)cold)[off];
		ksft_test_result(!rc && physical_state(folio, head, true),
				"%s marks the complete faulted folio\n", name);
		rc = munlock(cold, FOLIO_BYTES);
		ksft_test_result(!rc && physical_state(folio, head, false),
				"%s unlock clears physical mlock state\n", name);
		munmap(cold, FOLIO_BYTES);
	}
	munmap(map, size);
	close(fd);
	close(pagemap_fd);
	close(flags_fd);
	ksft_finished();
}

int main(int argc, char **argv)
{
	return p3s_geometry_main(argc, argv, run_test);
}
