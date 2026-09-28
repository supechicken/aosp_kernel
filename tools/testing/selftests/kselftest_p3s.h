/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Shared helpers for the P3S (per-process page size) selftests.
 *
 * A P3S kernel is a native 16K arm64 kernel on which a process carrying the
 * ADDR_PAGE_SIZE_4KB personality bit sees 4K pages.  Nearly every test
 * in this corpus starts as whatever the runner is, re-execs itself as a 4K
 * compat process and only then runs its checks; a few also spawn a native
 * (16K) helper.  This header holds the one copy of that dance.
 * MM-specific readers live in mm/p3s_test.h.
 *
 * The 4K compat interface is deliberately minimal: whatever a test observes
 * that is not part of that interface is expected to behave as it does for a
 * native 16K process.
 */
#ifndef __KSELFTEST_P3S_H
#define __KSELFTEST_P3S_H

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/personality.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "kselftest.h"

/*
 * personality(2) bit selecting the 4K compat page size at the next exec.
 * The uapi <linux/personality.h> carries it as an enumerator, which the libc
 * <sys/personality.h> does not re-export, hence the macro.
 */
#define ADDR_PAGE_SIZE_4KB	0x40000000

/* Page size of a 4K compat process and of the native kernel. */
#define PROCESS_PAGE_SIZE		4096UL
#define NATIVE_PAGE_SIZE		16384UL

/* Number of 4K process pages ("slices") backed by one native page. */
#define P3S_SLICES			(NATIVE_PAGE_SIZE / PROCESS_PAGE_SIZE)

/* Mode flag the standard main passes to its 4K compat re-exec. */
#define P3S_RUN_FLAG			"--run"

/*
 * argv[0] of the running test, recorded by p3s_compat_main() and
 * p3s_run_mode() so helpers that fork and re-exec deep inside the test do
 * not need argv threaded through to them.
 */
static const char *p3s_argv0 __maybe_unused = "/proc/self/exe";

#define P3S_MAX_EXEC_ARGS 15

static inline bool p3s_is_compat_process(void)
{
	int persona = personality(0xffffffffUL);

	return getpagesize() == PROCESS_PAGE_SIZE && persona >= 0 &&
	       (persona & ADDR_PAGE_SIZE_4KB);
}

/* Hard precondition of every compat-side test body. */
static inline void p3s_require_compat(void)
{
	if (!p3s_is_compat_process())
		ksft_exit_skip("P3S 4K process unavailable: page size=%d (expected %lu)\n",
			       getpagesize(), PROCESS_PAGE_SIZE);
}

/* Re-exec /proc/self/exe with the compat bit set or cleared.  -1 with errno. */
static inline int p3s_execv(bool compat, char *const argv[])
{
	int persona = personality(0xffffffffUL);

	if (persona < 0)
		return -1;
	persona = compat ? persona | ADDR_PAGE_SIZE_4KB :
			   persona & ~ADDR_PAGE_SIZE_4KB;
	if (personality(persona) < 0)
		return -1;
	return execv("/proc/self/exe", argv);
}

/* Collect the NULL-terminated list after @argv0 into @argv. */
static inline void p3s_build_argv(const char *argv[P3S_MAX_EXEC_ARGS + 1],
				  const char *argv0, va_list ap)
{
	unsigned int i = 1;

	argv[0] = argv0 ? argv0 : p3s_argv0;
	while (i < P3S_MAX_EXEC_ARGS &&
	       (argv[i] = va_arg(ap, const char *)))
		i++;
	argv[i] = NULL;
}

/*
 * execl()-style re-exec that returns -1 (with errno) on failure, for forked
 * helpers that must fail quietly rather than bail out of the TAP stream.
 * @argv0 may be NULL to reuse the recorded argv[0].
 */
static inline int __attribute__((sentinel))
p3s_execl(bool compat, const char *argv0, ...)
{
	const char *argv[P3S_MAX_EXEC_ARGS + 1];
	va_list ap;

	va_start(ap, argv0);
	p3s_build_argv(argv, argv0, ap);
	va_end(ap);
	return p3s_execv(compat, (char *const *)argv);
}

static inline void __noreturn p3s_vexec_or_die(bool compat, const char *argv0,
					       va_list ap)
{
	const char *argv[P3S_MAX_EXEC_ARGS + 1];

	p3s_build_argv(argv, argv0, ap);
	p3s_execv(compat, (char *const *)argv);
	ksft_exit_fail_msg("re-exec as a %s process failed: %s\n",
			   compat ? "4K compat" : "native", strerror(errno));
}

/*
 * Re-exec ourselves as a 4K compat process.  The NULL-terminated argument
 * list follows @argv0, as for execl(); @argv0 may be NULL to reuse the
 * recorded argv[0].  Never returns.
 */
static inline void __noreturn __attribute__((sentinel))
exec_compat(const char *argv0, ...)
{
	va_list ap;

	va_start(ap, argv0);
	p3s_vexec_or_die(true, argv0, ap);
}

/* Re-exec ourselves as a native (16K) process.  Never returns. */
static inline void __noreturn __attribute__((sentinel))
exec_native(const char *argv0, ...)
{
	va_list ap;

	va_start(ap, argv0);
	p3s_vexec_or_die(false, argv0, ap);
}

/* Re-exec once; unsupported kernels and architectures are SKIP, not FAIL. */
static inline int p3s_compat_main(int argc, char **argv, int (*run)(void))
{
	p3s_argv0 = argv[0];
#ifndef __aarch64__
	ksft_print_header();
	ksft_exit_skip("requires arm64 16K native / P3S 4K geometry\n");
#else
	char *const next_argv[] = { argv[0], P3S_RUN_FLAG, NULL };

	if (argc == 2 && !strcmp(argv[1], P3S_RUN_FLAG)) {
		p3s_require_compat();
		return run();
	}
	if (argc != 1)
		return EXIT_FAILURE;
	if (p3s_is_compat_process())
		return run();
	if (getpagesize() != NATIVE_PAGE_SIZE) {
		ksft_print_header();
		ksft_exit_skip("native exec does not use 16K pages\n");
	}
	p3s_execv(true, next_argv);
	if (errno == EINVAL || errno == EPERM) {
		ksft_print_header();
		ksft_exit_skip("P3S personality unavailable: %s\n", strerror(errno));
	}
	ksft_exit_fail_msg("P3S re-exec failed: %s\n", strerror(errno));
#endif
}

/*
 * Opt-in entry point for tests with the same assertions in both geometries.
 * Default to compat; --native selects the 16K control.  Always prove native
 * 16K exec first: getpagesize() == 4096 alone also matches a native 4K kernel.
 * Existing single-mode tests keep their current entry point.
 */
static inline void __noreturn
p3s_geometry_exec(bool compat, const char *stage, const char *mode)
{
	int error;

	p3s_execl(compat, "/proc/self/exe", stage, mode, NULL);
	error = errno;
	ksft_print_header();
	if (error == EINVAL || error == EPERM)
		ksft_exit_skip("P3S personality/exec unavailable: %s\n",
			       strerror(error));
	ksft_exit_fail_msg("P3S re-exec failed: %s\n", strerror(error));
}

static inline int p3s_geometry_main(int argc, char **argv, int (*run)(void))
{
	p3s_argv0 = argv[0];
#ifndef __aarch64__
	ksft_print_header();
	ksft_exit_skip("requires arm64 16K native / 4K compat geometry\n");
#else
	if (argc == 1 || (argc == 2 && !strcmp(argv[1], "--native")))
		p3s_geometry_exec(false, "--p3s-native",
				  argc == 1 ? "compat" : "native");
	if (argc == 3 && !strcmp(argv[1], "--p3s-native")) {
		if (getpagesize() != NATIVE_PAGE_SIZE) {
			ksft_print_header();
			ksft_exit_skip("native exec does not use 16K pages\n");
		}
		if (!strcmp(argv[2], "native"))
			return run();
		if (!strcmp(argv[2], "compat"))
			p3s_geometry_exec(true, "--p3s-compat", NULL);
	}
	if (argc == 2 && !strcmp(argv[1], "--p3s-compat")) {
		int persona = personality(0xffffffffUL);

		if (getpagesize() != PROCESS_PAGE_SIZE || persona < 0 ||
		    !(persona & ADDR_PAGE_SIZE_4KB)) {
			ksft_print_header();
			ksft_exit_skip("4K compat exec is unavailable\n");
		}
		return run();
	}
	ksft_print_header();
	ksft_exit_fail_msg("usage: %s [--native]\n", argv[0]);
#endif
}

/*
 * The mode of a multi-role test: the first "--flag" argument, or, when
 * @envvar is given and no flag is present, the value of that environment
 * variable.  NULL means a top-level invocation.  Records argv[0].
 */
static inline const char *p3s_run_mode(int argc, char **argv,
				       const char *envvar)
{
	if (argc > 0 && argv)
		p3s_argv0 = argv[0];
	if (argc == 2 && !strcmp(argv[1], "--p3s-probe"))
		_exit(p3s_is_compat_process() ? KSFT_PASS : KSFT_SKIP);
	if (argc >= 2 && !strncmp(argv[1], "--", 2))
		return argv[1];
	return envvar ? getenv(envvar) : NULL;
}

/* Call after p3s_run_mode(), before a native launcher creates resources. */
static inline void p3s_require_native_compat(void)
{
	pid_t child;
	int status;

	if (getpagesize() != NATIVE_PAGE_SIZE) {
		ksft_print_header();
		ksft_exit_skip("requires native 16K pages\n");
	}
	child = fork();
	if (child < 0)
		ksft_exit_fail_msg("compat probe fork: %s\n", strerror(errno));
	if (!child) {
		p3s_execl(true, NULL, "--p3s-probe", NULL);
		_exit(errno == EINVAL || errno == EPERM ? KSFT_SKIP : KSFT_FAIL);
	}
	while (waitpid(child, &status, 0) < 0) {
		if (errno != EINTR)
			ksft_exit_fail_msg("compat probe wait: %s\n", strerror(errno));
	}
	if (WIFEXITED(status) && WEXITSTATUS(status) == KSFT_SKIP) {
		ksft_print_header();
		ksft_exit_skip("4K compat exec is unavailable\n");
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status))
		ksft_exit_fail_msg("compat probe failed: status=%d\n", status);
}

/* Whole-buffer I/O, retrying short transfers and EINTR. */

static inline bool read_full(int fd, void *buffer, size_t size)
{
	unsigned char *position = buffer;

	while (size) {
		ssize_t bytes = read(fd, position, size);

		if (bytes < 0 && errno == EINTR)
			continue;
		if (bytes <= 0)
			return false;
		position += bytes;
		size -= bytes;
	}
	return true;
}

static inline bool write_full(int fd, const void *buffer, size_t size)
{
	const unsigned char *position = buffer;

	while (size) {
		ssize_t bytes = write(fd, position, size);

		if (bytes < 0 && errno == EINTR)
			continue;
		if (bytes <= 0)
			return false;
		position += bytes;
		size -= bytes;
	}
	return true;
}

static inline bool pwrite_full(int fd, const void *buffer, size_t size,
			       off_t offset)
{
	const unsigned char *position = buffer;

	while (size) {
		ssize_t bytes = pwrite(fd, position, size, offset);

		if (bytes < 0 && errno == EINTR)
			continue;
		if (bytes <= 0)
			return false;
		position += bytes;
		offset += bytes;
		size -= bytes;
	}
	return true;
}

#define P3S_COMPAT_MAIN(run_fn)				\
int main(int argc, char **argv)					\
{								\
	return p3s_compat_main(argc, argv, run_fn);		\
}

#endif /* __KSELFTEST_P3S_H */
