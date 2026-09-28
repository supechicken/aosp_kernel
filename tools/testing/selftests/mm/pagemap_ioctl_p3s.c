// SPDX-License-Identifier: GPL-2.0
/* Run the generic PAGEMAP_SCAN regression suite under 4K P3S geometry. */
#define _GNU_SOURCE

#include <limits.h>

#include "kselftest_p3s.h"

static int run_test(void)
{
	char executable[PATH_MAX];
	char *name;
	ssize_t length;

	length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
	if (length < 0 || length >= (ssize_t)sizeof(executable) - 1)
		ksft_exit_fail_msg("read /proc/self/exe: %s\n", strerror(errno));
	executable[length] = '\0';
	name = strrchr(executable, '/');
	if (!name)
		ksft_exit_fail_msg("unexpected executable path: %s\n", executable);
	strcpy(name + 1, "pagemap_ioctl");
	execl(executable, executable, "--p3s-compat", NULL);
	ksft_exit_fail_msg("exec %s: %s\n", executable, strerror(errno));
}

P3S_COMPAT_MAIN(run_test)
