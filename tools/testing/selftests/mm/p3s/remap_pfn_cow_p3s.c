// SPDX-License-Identifier: GPL-2.0

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <setjmp.h>
#include <signal.h>

#include "p3s_test.h"
#include "remap_pfn_cow_p3s.h"

#define TEST_MARKER	0x93
#define TEST_SLICE	2U
#define RESERVATION_SIZE	(3 * NATIVE_PAGE_SIZE)

static sigjmp_buf fault_environment;

static void fault_handler(int signal_number)
{
	siglongjmp(fault_environment, signal_number);
}

static bool read_byte(const unsigned char *address, unsigned char *value)
{
	if (sigsetjmp(fault_environment, 1))
		return false;
	*value = *address;
	return true;
}

static int run_test(void)
{
	struct remap_pfn_cow_p3s_probe probe = {};
	struct sigaction action = {
		.sa_handler = fault_handler,
	};
	unsigned char *mapping, *reservation, *target;
	unsigned long page_size = getpagesize();
	bool compat = p3s_is_compat_process();
	bool guards_fault = true;
	unsigned char guard_value;
	unsigned int guard;
	int fd;

	ksft_print_header();
	ksft_set_plan(compat ? 12 : 7);

	fd = p3s_open_fixture_or_skip("/dev/remap_pfn_cow_p3s", O_RDWR);
	ksft_test_result(fd >= 0, "open private PFN remap fixture\n");

	mapping = mmap(NULL, page_size, PROT_READ, MAP_SHARED, fd, 0);
	ksft_test_result(mapping != MAP_FAILED, "map the valid zero-offset control\n");
	if (mapping == MAP_FAILED)
		ksft_exit_fail_msg("control mmap failed: %s\n", strerror(errno));
	ksft_test_result(mapping[0] == TEST_MARKER - TEST_SLICE,
			 "zero-offset control reads the first physical slice\n");
	munmap(mapping, page_size);

	mapping = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE, fd, 0);
	ksft_test_result(mapping != MAP_FAILED,
			 "map the private PFN range at native-page offset zero\n");
	if (mapping == MAP_FAILED)
		ksft_exit_fail_msg("mmap failed: %s\n", strerror(errno));
	ksft_test_result(mapping[0] == TEST_MARKER - TEST_SLICE,
			 "mapping points at the requested physical slice\n");

	probe.address = (uintptr_t)mapping;
	ksft_test_result(!ioctl(fd, REMAP_PFN_COW_P3S_IOCTL, &probe),
			 "query the COW PFN VMA offset metadata\n");
	ksft_test_result(!probe.slice && probe.offset_matches,
			 "COW PFN VMA stores the complete physical offset\n");

	munmap(mapping, page_size);
	if (compat) {
		sigemptyset(&action.sa_mask);
		if (sigaction(SIGSEGV, &action, NULL) ||
		    sigaction(SIGBUS, &action, NULL))
			ksft_exit_fail_msg("sigaction failed: %s\n",
					   strerror(errno));
		mapping = mmap(NULL, page_size, PROT_READ, MAP_SHARED,
			       fd, TEST_SLICE * PROCESS_PAGE_SIZE);
		ksft_test_result(mapping == MAP_FAILED && errno == EINVAL,
				 "reject a VMA with a non-native-aligned offset\n");
		if (mapping != MAP_FAILED)
			munmap(mapping, page_size);

		reservation = mmap(NULL, RESERVATION_SIZE, PROT_NONE,
				   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (reservation == MAP_FAILED)
			ksft_exit_fail_msg("tail reservation failed: %s\n",
					   strerror(errno));
		target = (void *)(((uintptr_t)reservation + NATIVE_PAGE_SIZE - 1) &
				  ~(NATIVE_PAGE_SIZE - 1));
		mapping = mmap(target, page_size, PROT_READ, MAP_SHARED | MAP_FIXED,
			       fd, NATIVE_PAGE_SIZE);
		ksft_test_result(mapping == target,
				 "truncate only the last native-page tail\n");
		if (mapping == target) {
			ksft_test_result(mapping[0] == TEST_MARKER - TEST_SLICE,
					 "the truncated mapping retains its data\n");
			for (guard = 1; guard < P3S_SLICES; guard++) {
				if (read_byte(target + guard * PROCESS_PAGE_SIZE,
					      &guard_value)) {
					ksft_print_msg("guard %u readable: %#x\n",
						       guard, guard_value);
					guards_fault = false;
				}
			}
			ksft_test_result(guards_fault,
					 "the last-page tail does not populate guard PTEs\n");
		} else {
			ksft_test_result_fail("the truncated mapping retains its data\n");
			ksft_test_result_fail("the last-page tail does not populate guard PTEs\n");
		}
		munmap(reservation, RESERVATION_SIZE);

		reservation = mmap(NULL, RESERVATION_SIZE, PROT_NONE,
				   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (reservation == MAP_FAILED)
			ksft_exit_fail_msg("overrun reservation failed: %s\n",
					   strerror(errno));
		target = (void *)(((uintptr_t)reservation + NATIVE_PAGE_SIZE - 1) &
				  ~(NATIVE_PAGE_SIZE - 1));
		mapping = mmap(target, page_size, PROT_READ, MAP_SHARED | MAP_FIXED,
			       fd, 2 * NATIVE_PAGE_SIZE);
		ksft_test_result(mapping == MAP_FAILED && errno == EINVAL,
				 "reject a remap beyond the last native page\n");
		if (mapping != MAP_FAILED)
			munmap(mapping, page_size);
		munmap(reservation, RESERVATION_SIZE);
	}
	close(fd);
	ksft_finished();
}

int main(int argc, char **argv)
{
	return p3s_geometry_main(argc, argv, run_test);
}
