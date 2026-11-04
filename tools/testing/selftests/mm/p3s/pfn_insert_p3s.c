// SPDX-License-Identifier: GPL-2.0
/* vmf_insert_pfn_prot() atomically inserts/verifies the in-VMA PFN slices. */

#include <sys/ioctl.h>
#include <sys/mman.h>

#include "p3s_test.h"
#include "pfn_insert_p3s.h"

#define RESERVATION_SIZE	(3 * NATIVE_PAGE_SIZE)
#define COMPAT_PMD_SIZE	(512 * PROCESS_PAGE_SIZE)
#define FIRST_MARKER	0x51
#define WRONG_MARKER	0x61

static unsigned char *fixture_map_len(int fd, off_t offset,
				      size_t virtual_shift, size_t length,
				      void **reservation)
{
	unsigned char *target;

	*reservation = mmap(NULL, RESERVATION_SIZE, PROT_NONE,
			    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (*reservation == MAP_FAILED)
		ksft_exit_fail_msg("reserve aligned VMA: %s\n", strerror(errno));
	target = (void *)(((uintptr_t)*reservation + NATIVE_PAGE_SIZE - 1) &
			  ~(NATIVE_PAGE_SIZE - 1));
	target += virtual_shift;
	return mmap(target, length, PROT_READ,
		    MAP_SHARED | MAP_FIXED, fd, offset);
}

static unsigned char *fixture_map(int fd, off_t offset, size_t virtual_shift,
				  void **reservation)
{
	return fixture_map_len(fd, offset, virtual_shift, NATIVE_PAGE_SIZE,
			       reservation);
}

static bool inspect_mask(int fd, const unsigned char *mapping,
			 unsigned int expected)
{
	struct pfn_insert_p3s_request request = {
		.address = (uintptr_t)mapping,
	};

	if (ioctl(fd, PFN_INSERT_P3S_INSPECT, &request))
		return false;
	if (request.present_mask != expected)
		ksft_print_msg("present mask %#x, expected %#x\n",
			       request.present_mask, expected);
	return request.present_mask == expected;
}

static bool page_markers(const unsigned char *mapping, unsigned char first,
			 unsigned int count)
{
	unsigned int i;

	for (i = 0; i < count; i++) {
		if (mapping[i * PROCESS_PAGE_SIZE] != first + i)
			return false;
	}
	return true;
}

static int run_test(void)
{
	struct pfn_insert_p3s_request request = {};
	unsigned char *mapping;
	void *reservation;
	bool compat = p3s_is_compat_process();
	int fd;

	ksft_print_header();
	ksft_set_plan(compat ? 46 : 6);
	fd = p3s_open_fixture_or_skip("/dev/pfn_insert_p3s", O_RDWR);
	ksft_test_result(fd >= 0, "open PFN insertion fixture\n");

	mapping = fixture_map(fd, 0, 0, &reservation);
	ksft_test_result(mapping != MAP_FAILED, "map an empty native-page VMA\n");
	if (mapping == MAP_FAILED)
		ksft_exit_fail_msg("empty fixture mmap: %s\n", strerror(errno));
	ksft_test_result(mapping[0] == FIRST_MARKER,
			 "the first 4K access faults in the target PFN\n");
	ksft_test_result(inspect_mask(fd, mapping, (1U << P3S_SLICES) - 1),
			 "one fault installs all four 4K PTEs\n");
	ksft_test_result(page_markers(mapping, FIRST_MARKER, P3S_SLICES),
			 "all four PTEs select their matching physical slices\n");
	request.address = (uintptr_t)mapping + PROCESS_PAGE_SIZE;
	ksft_test_result(!ioctl(fd, PFN_INSERT_P3S_IOCTL, &request),
			 "a complete matching quartet is a no-op\n");
	munmap(reservation, RESERVATION_SIZE);

	if (compat) {
		bool inserted;

		mapping = fixture_map(fd, 0, PROCESS_PAGE_SIZE, &reservation);
		ksft_test_result(mapping != MAP_FAILED,
				 "map a full native page at a 4K-only virtual address\n");
		if (mapping == MAP_FAILED)
			ksft_exit_fail_msg("unaligned virtual mmap: %s\n",
					   strerror(errno));
		request.address = (uintptr_t)mapping + PROCESS_PAGE_SIZE;
		inserted = !ioctl(fd, PFN_INSERT_P3S_IOCTL, &request);
		ksft_test_result(inserted,
				 "insert a whole PFN at a 4K-only virtual address\n");
		ksft_test_result(inspect_mask(fd, mapping,
					      (1U << P3S_SLICES) - 1),
				 "the shifted VMA has all four PTEs\n");
		if (inserted)
			ksft_test_result(page_markers(mapping, FIRST_MARKER, P3S_SLICES),
					 "shifted PTEs select matching physical slices\n");
		else
			ksft_test_result_fail("shifted PTEs select matching physical slices\n");
		munmap(reservation, RESERVATION_SIZE);

		reservation = mmap(NULL, 2 * COMPAT_PMD_SIZE, PROT_NONE,
				   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (reservation == MAP_FAILED)
			ksft_exit_fail_msg("cross-table reservation: %s\n",
					   strerror(errno));
		mapping = (void *)((((uintptr_t)reservation + COMPAT_PMD_SIZE - 1) &
				  ~(COMPAT_PMD_SIZE - 1)) - PROCESS_PAGE_SIZE);
		mapping = mmap(mapping, NATIVE_PAGE_SIZE, PROT_READ,
			       MAP_SHARED | MAP_FIXED, fd, 0);
		ksft_test_result(mapping != MAP_FAILED,
				 "map one PFN across two 4K PTE tables\n");
		if (mapping == MAP_FAILED)
			ksft_exit_fail_msg("cross-table mmap: %s\n",
					   strerror(errno));
		request.address = (uintptr_t)mapping + PROCESS_PAGE_SIZE;
		inserted = !ioctl(fd, PFN_INSERT_P3S_IOCTL, &request);
		ksft_test_result(inserted,
				 "insert a whole PFN across the table boundary\n");
		ksft_test_result(inspect_mask(fd, mapping,
					      (1U << P3S_SLICES) - 1),
				 "both PTE tables contain their slices\n");
		if (inserted)
			ksft_test_result(page_markers(mapping, FIRST_MARKER, P3S_SLICES),
					 "cross-table PTEs select matching slices\n");
		else
			ksft_test_result_fail("cross-table PTEs select matching slices\n");
		munmap(reservation, 2 * COMPAT_PMD_SIZE);

		mapping = fixture_map(fd, NATIVE_PAGE_SIZE, 0, &reservation);
		ksft_test_result(mapping != MAP_FAILED,
				 "prepopulate only the first 4K PTE\n");
		if (mapping == MAP_FAILED)
			ksft_exit_fail_msg("partial fixture mmap: %s\n",
					   strerror(errno));
		ksft_test_result(inspect_mask(fd, mapping, 1),
				 "partial prepopulation leaves three empty PTEs\n");
		request.address = (uintptr_t)mapping + PROCESS_PAGE_SIZE;
		ksft_test_result(ioctl(fd, PFN_INSERT_P3S_IOCTL, &request) == -1 &&
				 errno == EBUSY,
				 "reject a partly populated native page\n");
		ksft_test_result(inspect_mask(fd, mapping, 1),
				 "partial rejection changes no PTEs\n");
		munmap(reservation, RESERVATION_SIZE);

		mapping = fixture_map(fd, 2 * NATIVE_PAGE_SIZE, 0,
				      &reservation);
		ksft_test_result(mapping != MAP_FAILED,
				 "prepopulate all four PTEs with a different PFN\n");
		if (mapping == MAP_FAILED)
			ksft_exit_fail_msg("wrong-frame fixture mmap: %s\n",
					   strerror(errno));
		ksft_test_result(inspect_mask(fd, mapping, (1U << P3S_SLICES) - 1),
				 "wrong-frame setup populates every PTE\n");
		request.address = (uintptr_t)mapping + PROCESS_PAGE_SIZE;
		ksft_test_result(ioctl(fd, PFN_INSERT_P3S_IOCTL, &request) == -1 &&
				 errno == EBUSY,
				 "reject a complete but wrong PFN quartet\n");
		ksft_test_result(page_markers(mapping, WRONG_MARKER, P3S_SLICES),
				 "wrong-frame rejection preserves every old PTE\n");
		munmap(reservation, RESERVATION_SIZE);

		mapping = fixture_map_len(fd, 0, 0, 3 * PROCESS_PAGE_SIZE,
					  &reservation);
		ksft_test_result(mapping != MAP_FAILED,
				 "map a VMA ending after three slices\n");
		if (mapping == MAP_FAILED)
			ksft_exit_fail_msg("tail fixture mmap: %s\n", strerror(errno));
		request.address = (uintptr_t)mapping + PROCESS_PAGE_SIZE;
		inserted = !ioctl(fd, PFN_INSERT_P3S_IOCTL, &request);
		ksft_test_result(inserted,
				 "insert the in-VMA slices of a tail window\n");
		ksft_test_result(inspect_mask(fd, mapping, 7),
				 "tail insertion fills only three PTEs\n");
		ksft_test_result(inserted &&
				 page_markers(mapping, FIRST_MARKER, 3),
				 "tail PTEs select the matching physical slices\n");
		ksft_test_result(inserted &&
				 !ioctl(fd, PFN_INSERT_P3S_IOCTL, &request),
				 "a complete matching tail is a no-op\n");
		ksft_test_result(!p3s_page_present(mapping + 3 * PROCESS_PAGE_SIZE),
				 "tail insertion leaves the adjacent guard unmapped\n");
		munmap(reservation, RESERVATION_SIZE);

		mapping = fixture_map(fd, 0, 0, &reservation);
		if (mapping != MAP_FAILED) {
			munmap(mapping, PROCESS_PAGE_SIZE);
			munmap(mapping + 3 * PROCESS_PAGE_SIZE,
			       PROCESS_PAGE_SIZE);
			mapping += PROCESS_PAGE_SIZE;
		}
		ksft_test_result(mapping != MAP_FAILED,
				 "trim a full mapping to file slices one and two\n");
		if (mapping == MAP_FAILED)
			ksft_exit_fail_msg("offset fixture mmap: %s\n", strerror(errno));
		request.address = (uintptr_t)mapping;
		inserted = !ioctl(fd, PFN_INSERT_P3S_IOCTL, &request);
		ksft_test_result(inserted,
				 "insert only the in-VMA offset slices\n");
		ksft_test_result(inspect_mask(fd, mapping, 3),
				 "offset insertion fills two PTEs\n");
		ksft_test_result(inserted &&
				 page_markers(mapping, FIRST_MARKER + 1, 2),
				 "offset PTEs select physical slices one and two\n");
		ksft_test_result(inserted &&
				 !ioctl(fd, PFN_INSERT_P3S_IOCTL, &request),
				 "a complete matching offset pair is a no-op\n");
		ksft_test_result(!p3s_page_present(mapping + 2 * PROCESS_PAGE_SIZE),
				 "offset insertion leaves the adjacent guard unmapped\n");
		munmap(reservation, RESERVATION_SIZE);

		mapping = fixture_map(fd, 0, 0, &reservation);
		if (mapping != MAP_FAILED) {
			munmap(mapping, 3 * PROCESS_PAGE_SIZE);
			mapping += 3 * PROCESS_PAGE_SIZE;
		}
		ksft_test_result(mapping != MAP_FAILED,
				 "trim a full mapping to its last slice\n");
		if (mapping == MAP_FAILED)
			ksft_exit_fail_msg("last-slice mmap: %s\n", strerror(errno));
		request.address = (uintptr_t)mapping;
		inserted = !ioctl(fd, PFN_INSERT_P3S_IOCTL, &request);
		ksft_test_result(inserted,
				 "insert the last slice without the other three\n");
		ksft_test_result(inspect_mask(fd, mapping, 1),
				 "last-slice insertion fills one PTE\n");
		ksft_test_result(inserted && mapping[0] == FIRST_MARKER + 3,
				 "last-slice PTE selects physical slice three\n");
		munmap(reservation, RESERVATION_SIZE);

		mapping = fixture_map_len(fd, NATIVE_PAGE_SIZE, 0,
					  3 * PROCESS_PAGE_SIZE, &reservation);
		ksft_test_result(mapping != MAP_FAILED,
				 "prepopulate one PTE in a tail VMA\n");
		if (mapping == MAP_FAILED)
			ksft_exit_fail_msg("partial tail mmap: %s\n", strerror(errno));
		ksft_test_result(inspect_mask(fd, mapping, 1),
				 "tail setup leaves two PTEs empty\n");
		request.address = (uintptr_t)mapping + PROCESS_PAGE_SIZE;
		ksft_test_result(ioctl(fd, PFN_INSERT_P3S_IOCTL, &request) == -1 &&
				 errno == EBUSY,
				 "reject a partly populated tail window\n");
		ksft_test_result(inspect_mask(fd, mapping, 1),
				 "partial tail rejection changes no PTEs\n");
		munmap(reservation, RESERVATION_SIZE);

		mapping = fixture_map_len(fd, 2 * NATIVE_PAGE_SIZE, 0,
					  3 * PROCESS_PAGE_SIZE, &reservation);
		ksft_test_result(mapping != MAP_FAILED,
				 "prepopulate a tail VMA with the wrong PFN\n");
		if (mapping == MAP_FAILED)
			ksft_exit_fail_msg("wrong tail mmap: %s\n", strerror(errno));
		ksft_test_result(inspect_mask(fd, mapping, 7),
				 "wrong tail setup fills all in-VMA PTEs\n");
		request.address = (uintptr_t)mapping + PROCESS_PAGE_SIZE;
		ksft_test_result(ioctl(fd, PFN_INSERT_P3S_IOCTL, &request) == -1 &&
				 errno == EBUSY,
				 "reject a complete but wrong tail PFN\n");
		ksft_test_result(page_markers(mapping, WRONG_MARKER, 3),
				 "wrong tail rejection preserves the old PTEs\n");
		munmap(reservation, RESERVATION_SIZE);
	}
	close(fd);
	ksft_finished();
}

int main(int argc, char **argv)
{
	return p3s_geometry_main(argc, argv, run_test);
}
