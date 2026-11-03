// SPDX-License-Identifier: GPL-2.0
/*
 * Fork must write-protect every parent PTE of a shared 16K anonymous folio,
 * including a 4K slice in a MADV_DONTFORK VMA.  Test both a sibling in the
 * same virtual window and one moved by mremap() to a different window.
 *
 * Compaction is the migration trigger.  A run with no observed PFN change is
 * inconclusive, not a pass.  The child writes its COW mapping only after the
 * parent has observed a real migration; that write must never reach parent.
 */
#define _GNU_SOURCE

#include <sys/mman.h>
#include <sys/wait.h>

#include "p3s_test.h"

#define WINDOWS		4096
#define FILLER_SIZE	(720UL * 1024 * 1024)

static bool write_text(const char *path, const char *text)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	size_t len = strlen(text);
	bool ok = fd >= 0 && write(fd, text, len) == (ssize_t)len;

	if (fd >= 0)
		close(fd);
	return ok;
}

/* Return 0 on pass, 1 on failure, 4 if no physical migration took place. */
static int run_trial(bool moved)
{
	const size_t len = WINDOWS * NATIVE_PAGE_SIZE;
	unsigned char *raw = MAP_FAILED, *dst_raw = MAP_FAILED;
	unsigned char *base, *dst = NULL, *filler = MAP_FAILED;
	uint64_t *old = NULL, pfn;
	int to_child[2] = { -1, -1 }, from_child[2] = { -1, -1 };
	int migrated = 0, shared = 0, status = 0, result = 1;
	pid_t child = -1;
	unsigned int i, slice, round;

	raw = mmap(NULL, len + NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (raw == MAP_FAILED)
		goto out;
	base = (void *)(((uintptr_t)raw + NATIVE_PAGE_SIZE - 1) &
			 ~(uintptr_t)(NATIVE_PAGE_SIZE - 1));
	if (moved) {
		dst_raw = mmap(NULL, len + NATIVE_PAGE_SIZE, PROT_NONE,
			       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (dst_raw == MAP_FAILED)
			goto out;
		dst = (void *)(((uintptr_t)dst_raw + NATIVE_PAGE_SIZE - 1) &
				~(uintptr_t)(NATIVE_PAGE_SIZE - 1));
	}
	old = calloc(WINDOWS, sizeof(*old));
	if (!old)
		goto out;
	for (i = 0; i < WINDOWS; i++) {
		unsigned char *nochild = base + i * NATIVE_PAGE_SIZE +
					  PROCESS_PAGE_SIZE;

		for (slice = 0; slice < P3S_SLICES; slice++)
			base[i * NATIVE_PAGE_SIZE + slice * PROCESS_PAGE_SIZE] =
				0x40 + slice;
		if (moved) {
			unsigned char *target = dst + i * NATIVE_PAGE_SIZE +
						PROCESS_PAGE_SIZE;

			if (mremap(nochild, PROCESS_PAGE_SIZE, PROCESS_PAGE_SIZE,
				   MREMAP_MAYMOVE | MREMAP_FIXED, target) != target)
				goto out;
			nochild = target;
		}
		if (madvise(nochild, PROCESS_PAGE_SIZE, MADV_DONTFORK) ||
		    !p3s_pfn(base + i * NATIVE_PAGE_SIZE, &old[i]))
			goto out;
		/* The moved PTE must still map the same physical folio. */
		if (moved && (!p3s_pfn(nochild, &pfn) || pfn != old[i]))
			goto out;
	}
	/* Fragment physical memory so compact_memory really moves the folios. */
	filler = mmap(NULL, FILLER_SIZE, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (filler == MAP_FAILED)
		goto out;
	for (size_t off = 0; off < FILLER_SIZE; off += NATIVE_PAGE_SIZE)
		filler[off] = 0x5a;
	if (madvise(filler, FILLER_SIZE, MADV_DONTFORK))
		goto out;
	if (pipe(to_child) || pipe(from_child))
		goto out;
	child = fork();
	if (child < 0)
		goto out;
	if (!child) {
		int idx;

		close(to_child[1]);
		close(from_child[0]);
		while (read_full(to_child[0], &idx, sizeof(idx))) {
			if (idx < 0)
				break;
			base[idx * NATIVE_PAGE_SIZE] = 0xa5;
			if (!write_full(from_child[1], &idx, sizeof(idx)))
				_exit(1);
		}
		_exit(0);
	}
	close(to_child[0]);
	to_child[0] = -1;
	close(from_child[1]);
	from_child[1] = -1;
	for (size_t off = 0; off < FILLER_SIZE; off += 2 * 1024 * 1024)
		if (madvise(filler + off, 1024 * 1024, MADV_DONTNEED))
			goto out;
	for (round = 0; round < 6; round++) {
		if (!write_text("/proc/sys/vm/compact_memory", "1"))
			goto out;
		for (i = 0; i < WINDOWS; i++) {
			int ack = -1;

			if (!p3s_pfn(base + i * NATIVE_PAGE_SIZE, &pfn))
				goto out;
			if (pfn == old[i])
				continue;
			old[i] = pfn;
			if (!write_full(to_child[1], &i, sizeof(i)) ||
			    !read_full(from_child[0], &ack, sizeof(ack)) || ack != (int)i)
				goto out;
			migrated++;
			if (base[i * NATIVE_PAGE_SIZE] == 0xa5)
				shared++;
		}
		if (migrated >= 30)
			break;
	}
	result = migrated ? !!shared : 4;
out:
	if (child > 0) {
		int stop = -1;

		if (to_child[1] >= 0)
			write_full(to_child[1], &stop, sizeof(stop));
		close(to_child[1]);
		to_child[1] = -1;
		if (waitpid(child, &status, 0) != child ||
		    !WIFEXITED(status) || WEXITSTATUS(status))
			result = 1;
	}
	if (to_child[0] >= 0)
		close(to_child[0]);
	if (to_child[1] >= 0)
		close(to_child[1]);
	if (from_child[0] >= 0)
		close(from_child[0]);
	if (from_child[1] >= 0)
		close(from_child[1]);
	if (filler != MAP_FAILED)
		munmap(filler, FILLER_SIZE);
	if (dst_raw != MAP_FAILED)
		munmap(dst_raw, len + NATIVE_PAGE_SIZE);
	if (raw != MAP_FAILED)
		munmap(raw, len + NATIVE_PAGE_SIZE);
	free(old);
	ksft_print_msg("%s: migrated=%d shared=%d\n",
		       moved ? "moved sibling" : "same-window sibling",
		       migrated, shared);
	return result;
}

static int run_test(void)
{
	int result;

	ksft_print_header();
	p3s_require_compat();
	if (geteuid() || access("/proc/sys/vm/compact_memory", W_OK))
		ksft_exit_skip("root and compact_memory are required\n");
	ksft_set_plan(2);
	result = run_trial(false);
	if (result == 4)
		ksft_test_result_skip("compaction did not migrate same-window folios\n");
	else
		ksft_test_result(!result, "fork write-protects a DONTFORK sibling\n");
	result = run_trial(true);
	if (result == 4)
		ksft_test_result_skip("compaction did not migrate moved folios\n");
	else
		ksft_test_result(!result, "migration cannot restore a moved sibling as writable\n");
	ksft_finished();
}

P3S_COMPAT_MAIN(run_test)
