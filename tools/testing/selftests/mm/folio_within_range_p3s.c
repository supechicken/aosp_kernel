// SPDX-License-Identifier: GPL-2.0
/*
 * The in-kernel folio_within_range() fixture, driven from a 4K compat
 * process, recognizes a fully covered native folio and rejects truncated
 * query ranges. The rmap check separately covers a nonzero VMA slice offset.
 */
#define _GNU_SOURCE

#include <sys/ioctl.h>

#include "p3s_test.h"
#include "folio_within_range_p3s.h"

static int run_test(void)
{
	int fd;

	ksft_print_header();
	ksft_set_plan(5);

	fd = p3s_open_fixture_or_skip("/dev/folio_within_range_p3s",
				       O_RDONLY);
	ksft_test_result(fd >= 0, "open the folio range fixture\n");

	ksft_test_result(ioctl(fd,
			       FOLIO_WITHIN_RANGE_P3S_CHECK_ALIGNED) == 0,
			 "recognize a completely covered native folio\n");
	ksft_test_result(ioctl(fd,
			       FOLIO_WITHIN_RANGE_P3S_CHECK_SUBPAGE) == 0,
			 "reject a query range starting inside the native folio\n");
	ksft_test_result(ioctl(fd,
			       FOLIO_WITHIN_RANGE_P3S_CHECK_SHORT_END) == 0,
			 "reject a query range ending inside the native folio\n");
	ksft_test_result(ioctl(fd,
			       FOLIO_WITHIN_RANGE_P3S_CHECK_ADDRESS_END) == 0,
			 "stop a reverse-map walk at the end of the native folio\n");

	close(fd);
	ksft_finished();
}

P3S_COMPAT_MAIN(run_test)
