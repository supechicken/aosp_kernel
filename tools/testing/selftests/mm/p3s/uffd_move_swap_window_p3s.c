// SPDX-License-Identifier: GPL-2.0
/* Move whole swapped windows and preserve progress at a destination conflict. */
#define _GNU_SOURCE
#include <linux/userfaultfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#include "p3s_test.h"

#define PAGEOUT_POLLS 1000
#define TEST_LENGTH (3 * NATIVE_PAGE_SIZE)

static bool pageout_reaches_swap(void *address, unsigned long length,
				 unsigned long minimum)
{
	unsigned long swap;
	unsigned int attempt;

	if (madvise(address, length, MADV_PAGEOUT))
		return false;
	for (attempt = 0; attempt < PAGEOUT_POLLS; attempt++) {
		if (!p3s_smaps_bytes(address, length, "Swap", &swap))
			return false;
		if (swap >= minimum)
			return true;
		usleep(10000);
	}
	return false;
}

static unsigned char *map_exact_edge(size_t size,
				     unsigned char **reservation)
{
	unsigned char *mapping, *base;
	size_t length = size + 2 * NATIVE_PAGE_SIZE;

	mapping = mmap(NULL, length, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS,
		       -1, 0);
	if (mapping == MAP_FAILED)
		return MAP_FAILED;
	base = (unsigned char *)(((uintptr_t)mapping + 2 * NATIVE_PAGE_SIZE - 1) &
				 ~(NATIVE_PAGE_SIZE - 1));
	if (mprotect(base, size, PROT_READ | PROT_WRITE)) {
		munmap(mapping, length);
		return MAP_FAILED;
	}
	*reservation = mapping;
	return base;
}

static bool check_bytes(const unsigned char *base, size_t length)
{
	for (size_t i = 0; i < length; i++)
		if (base[i] != (unsigned char)(0x31 + i / NATIVE_PAGE_SIZE))
			return false;
	return true;
}

static void test_move(int uffd, int pagemap, bool conflict)
{
	struct uffdio_move move = {};
	unsigned char *src_reservation, *dst_reservation;
	unsigned char *src, *dst;
	unsigned long page = sysconf(_SC_PAGESIZE);
	uint64_t entry;
	bool ok;
	int ret;

	src = map_exact_edge(TEST_LENGTH, &src_reservation);
	dst = map_exact_edge(TEST_LENGTH, &dst_reservation);
	if (src == MAP_FAILED || dst == MAP_FAILED)
		ksft_exit_fail_msg("mapping setup failed: %s\n", strerror(errno));
	for (size_t i = 0; i < TEST_LENGTH; i++)
		src[i] = 0x31 + i / NATIVE_PAGE_SIZE;
	if (conflict)
		memset(dst + 2 * NATIVE_PAGE_SIZE, 0xa5, NATIVE_PAGE_SIZE);
	if (uffd_register(uffd, dst, TEST_LENGTH, true, false, false))
		ksft_exit_fail_msg("register failed: %s\n", strerror(errno));

	ok = pageout_reaches_swap(src + NATIVE_PAGE_SIZE, NATIVE_PAGE_SIZE,
				  NATIVE_PAGE_SIZE);
	ksft_test_result(ok, "%s: page out the middle source folio\n",
			 conflict ? "partial" : "complete");
	if (!ok)
		ksft_exit_fail_msg("could not establish swapped-source precondition\n");
	ok = true;
	for (size_t off = NATIVE_PAGE_SIZE; off < 2 * NATIVE_PAGE_SIZE;
	     off += page) {
		entry = pagemap_get_entry(pagemap, (char *)src + off);
		ok &= (entry & PM_SWAP) && !(entry & PM_PRESENT);
	}
	ksft_test_result(ok, "source PTEs are swapped before MOVE\n");
	if (!ok)
		ksft_exit_fail_msg("source was not swapped\n");

	move.src = (uintptr_t)src;
	move.dst = (uintptr_t)dst;
	move.len = TEST_LENGTH;
	errno = 0;
	ret = ioctl(uffd, UFFDIO_MOVE, &move);
	ok = conflict ? ret == -1 && errno == EAGAIN &&
		move.move == 2 * NATIVE_PAGE_SIZE :
		!ret && move.move == TEST_LENGTH;
	ksft_test_result(ok, "%s MOVE reports progress across a swapped window\n",
			 conflict ? "partial" : "complete");
	if (!ok)
		ksft_exit_fail_msg("MOVE ret=%d errno=%d moved=%lld\n", ret, errno,
				   (long long)move.move);

	ksft_test_result(check_bytes(dst, conflict ? 2 * NATIVE_PAGE_SIZE :
				    TEST_LENGTH), "moved bytes preserve their source order\n");
	if (conflict) {
		ok = true;
		for (size_t i = 0; i < NATIVE_PAGE_SIZE; i++)
			ok &= src[2 * NATIVE_PAGE_SIZE + i] == 0x33 &&
			      dst[2 * NATIVE_PAGE_SIZE + i] == 0xa5;
	} else {
		ok = true;
		for (size_t off = 0; off < TEST_LENGTH; off += page) {
			entry = pagemap_get_entry(pagemap, (char *)src + off);
			ok &= !(entry & (PM_PRESENT | PM_SWAP));
		}
	}
	ksft_test_result(ok, "%s remains intact after MOVE\n",
			 conflict ? "unmoved suffix" : "empty source");

	if (uffd_unregister(uffd, dst, TEST_LENGTH))
		ksft_exit_fail_msg("unregister failed\n");
	munmap(src_reservation, TEST_LENGTH + 2 * NATIVE_PAGE_SIZE);
	munmap(dst_reservation, TEST_LENGTH + 2 * NATIVE_PAGE_SIZE);
}

static void test_occupied_slice(int uffd, int pagemap, unsigned int busy)
{
	unsigned char *src_reservation, *dst_reservation;
	unsigned char *src, *dst;
	unsigned long page = sysconf(_SC_PAGESIZE);
	struct uffdio_move move = {};
	bool ok, copied = true;
	int ret, saved_errno;

	src = map_exact_edge(NATIVE_PAGE_SIZE, &src_reservation);
	dst = map_exact_edge(NATIVE_PAGE_SIZE, &dst_reservation);
	if (src == MAP_FAILED || dst == MAP_FAILED)
		ksft_exit_fail_msg("occupied-slice mapping setup failed\n");
	memset(src, 0x31, NATIVE_PAGE_SIZE);
	memset(dst + busy * page, 0xa5, page);
	if (uffd_register(uffd, dst, NATIVE_PAGE_SIZE, true, false, false))
		ksft_exit_fail_msg("occupied-slice register failed\n");
	ok = pageout_reaches_swap(src, NATIVE_PAGE_SIZE, NATIVE_PAGE_SIZE);
	for (size_t off = 0; off < NATIVE_PAGE_SIZE; off += page) {
		uint64_t entry = pagemap_get_entry(pagemap, (char *)src + off);

		ok &= (entry & PM_SWAP) && !(entry & PM_PRESENT);
		entry = pagemap_get_entry(pagemap, (char *)dst + off);
		ok &= off == busy * page ? !!(entry & PM_PRESENT) :
			!(entry & (PM_PRESENT | PM_SWAP));
	}
	ksft_test_result(ok, "slice %u: swapped source and only one occupied destination\n",
			 busy);
	if (!ok)
		ksft_exit_fail_msg("occupied-slice preconditions failed\n");
	move.src = (uintptr_t)src;
	move.dst = (uintptr_t)dst;
	move.len = NATIVE_PAGE_SIZE;
	errno = 0;
	ret = ioctl(uffd, UFFDIO_MOVE, &move);
	saved_errno = errno;
	ok = ret == -1 && saved_errno == (busy ? EBUSY : EEXIST) &&
		move.move == -(busy ? EBUSY : EEXIST);
	ksft_test_result(ok, "slice %u: MOVE distinguishes a busy sibling from its target\n",
			 busy);
	if (!ok)
		ksft_print_msg("MOVE ret=%d errno=%d moved=%lld, expected %d\n",
			       ret, saved_errno, (long long)move.move,
			       busy ? EBUSY : EEXIST);
	ok = true;
	for (size_t off = 0; off < NATIVE_PAGE_SIZE; off += page) {
		uint64_t entry = pagemap_get_entry(pagemap, (char *)src + off);

		ok &= (entry & PM_SWAP) && !(entry & PM_PRESENT);
		entry = pagemap_get_entry(pagemap, (char *)dst + off);
		ok &= off == busy * page ? !!(entry & PM_PRESENT) :
			!(entry & (PM_PRESENT | PM_SWAP));
	}
	ksft_test_result(ok, "slice %u: rejected MOVE leaves source and destination PTEs intact\n",
			 busy);
	for (size_t off = 0; off < NATIVE_PAGE_SIZE; off += page) {
		struct uffdio_copy copy = {
			.src = (uintptr_t)(src + off),
			.dst = (uintptr_t)(dst + off),
			.len = page,
		};

		errno = 0;
		ret = ioctl(uffd, UFFDIO_COPY, &copy);
		copied &= off == busy * page ?
			ret == -1 && errno == EEXIST && copy.copy == -EEXIST :
			!ret && copy.copy == (__s64)page;
	}
	ksft_test_result(copied, "slice %u: explicit COPY fills only missing destination slices\n",
			 busy);
	ok = true;
	for (size_t off = 0; off < NATIVE_PAGE_SIZE; off++)
		ok &= src[off] == 0x31 && dst[off] ==
			(off / page == busy ? 0xa5 : 0x31);
	ksft_test_result(ok, "slice %u: COPY preserves source and occupied-slice data\n",
			 busy);
	if (uffd_unregister(uffd, dst, NATIVE_PAGE_SIZE))
		ksft_exit_fail_msg("occupied-slice unregister failed\n");
	munmap(src_reservation, 3 * NATIVE_PAGE_SIZE);
	munmap(dst_reservation, 3 * NATIVE_PAGE_SIZE);
}

static int run_test(void)
{
	struct uffdio_api api = { .api = UFFD_API };
	FILE *swaps;
	char line[256];
	bool have_swap;
	int uffd, pagemap;

	ksft_print_header();
	swaps = fopen("/proc/swaps", "re");
	have_swap = swaps && fgets(line, sizeof(line), swaps) &&
		fgets(line, sizeof(line), swaps);
	if (swaps)
		fclose(swaps);
	if (!have_swap)
		ksft_exit_skip("active swap is required\n");
	uffd = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK);
	if (uffd < 0)
		ksft_exit_skip("userfaultfd unavailable: %s\n", strerror(errno));
	if (ioctl(uffd, UFFDIO_API, &api))
		ksft_exit_fail_msg("UFFDIO_API failed\n");
	pagemap = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
	if (pagemap < 0)
		ksft_exit_fail_msg("pagemap open failed: %s\n", strerror(errno));
	ksft_set_plan(sysconf(_SC_PAGESIZE) == PROCESS_PAGE_SIZE ? 25 : 15);
	test_move(uffd, pagemap, false);
	test_move(uffd, pagemap, true);
	if (sysconf(_SC_PAGESIZE) == PROCESS_PAGE_SIZE) {
		for (unsigned int busy = 1; busy < 4; busy++)
			test_occupied_slice(uffd, pagemap, busy);
	} else {
		test_occupied_slice(uffd, pagemap, 0);
	}
	close(pagemap);
	close(uffd);
	ksft_finished();
}

int main(int argc, char **argv)
{
	return p3s_geometry_main(argc, argv, run_test);
}
