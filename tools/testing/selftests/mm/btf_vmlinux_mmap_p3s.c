// SPDX-License-Identifier: GPL-2.0
/*
 * Native processes can mmap() the BTF blob read-only.  A 4K compat process
 * can still read() it, but must not mmap() it: the native-page length check
 * does not reject a trailing 4K slice beyond the BTF allocation.
 */
#define _GNU_SOURCE

#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "kselftest_p3s.h"

#define BTF_PATH	"/sys/kernel/btf/vmlinux"

static sigjmp_buf fault_env;

static void fault_handler(int sig)
{
	siglongjmp(fault_env, sig);
}

static int probe_byte(const volatile unsigned char *addr)
{
	int sig = sigsetjmp(fault_env, 1);

	if (sig)
		return sig;
	(void)*addr;
	return 0;
}

static int run_case(bool compat)
{
	struct sigaction action = { .sa_handler = fault_handler };
	unsigned long page = getpagesize();
	unsigned char *buf, *reservation, *map;
	size_t size, map_len, off, mismatches = 0, first_bad = SIZE_MAX;
	struct stat st;
	int fd, sig, ret = KSFT_PASS;

	fd = open(BTF_PATH, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		ksft_print_msg("%s: %s\n", BTF_PATH, strerror(errno));
		return KSFT_SKIP;
	}
	if (fstat(fd, &st) || st.st_size <= 0)
		return KSFT_FAIL;
	if (compat) {
		unsigned char first;

		if (pread(fd, &first, 1, 0) != 1)
			return KSFT_FAIL;
		errno = 0;
		map = mmap(NULL, page, PROT_READ, MAP_PRIVATE, fd, 0);
		if (map != MAP_FAILED) {
			munmap(map, page);
			ksft_print_msg("compat BTF mmap unexpectedly succeeded\n");
			return KSFT_FAIL;
		}
		if (errno != EOPNOTSUPP) {
			ksft_print_msg("compat BTF mmap: expected EOPNOTSUPP, got %d\n",
				       errno);
			return KSFT_FAIL;
		}
		close(fd);
		return KSFT_PASS;
	}
	size = st.st_size;
	map_len = (size + page - 1) & ~(page - 1);
	buf = malloc(size);
	if (!buf)
		return KSFT_FAIL;
	/* sysfs returns binary attributes in chunks. */
	for (off = 0; off < size; ) {
		ssize_t got = pread(fd, buf + off, size - off, off);

		if (got <= 0) {
			ksft_print_msg("read BTF at %zu of %zu failed: %s\n",
				       off, size, got ? strerror(errno) : "EOF");
			return KSFT_FAIL;
		}
		off += got;
	}

	/* Map into the middle of a PROT_NONE reservation to see stray PTEs. */
	reservation = mmap(NULL, map_len + 2 * NATIVE_PAGE_SIZE + 4 * page,
			   PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (reservation == MAP_FAILED)
		return KSFT_FAIL;
	map = (unsigned char *)(((uintptr_t)reservation + NATIVE_PAGE_SIZE - 1) &
				~(NATIVE_PAGE_SIZE - 1));
	map = mmap(map, map_len, PROT_READ, MAP_PRIVATE | MAP_FIXED, fd, 0);
	if (map == MAP_FAILED) {
		ksft_print_msg("mmap %zu bytes of %s: %s\n", map_len, BTF_PATH,
			       strerror(errno));
		return errno == EINVAL || errno == ENODEV ? KSFT_SKIP : KSFT_FAIL;
	}

	sigemptyset(&action.sa_mask);
	if (sigaction(SIGSEGV, &action, NULL) || sigaction(SIGBUS, &action, NULL))
		return KSFT_FAIL;

	for (off = 0; off < size; off += page) {
		size_t len = size - off < page ? size - off : page;

		sig = probe_byte(map + off);
		if (sig) {
			ksft_print_msg("offset %zu raised signal %d\n", off, sig);
			ret = KSFT_FAIL;
			break;
		}
		if (memcmp(map + off, buf + off, len)) {
			if (first_bad == SIZE_MAX)
				first_bad = off;
			mismatches++;
		}
	}
	if (mismatches) {
		ksft_print_msg("%zu of %zu pages differ from read(); first at offset %zu\n",
			       mismatches, map_len / page, first_bad);
		ret = KSFT_FAIL;
	}
	for (off = map_len; off < map_len + 4 * page; off += page) {
		if (!probe_byte(map + off)) {
			ksft_print_msg("byte %zu past the %zu-byte mapping is readable\n",
				       off - map_len, map_len);
			ret = KSFT_FAIL;
			break;
		}
	}
	ksft_print_msg("%s geometry: BTF %zu bytes, mapped %zu bytes\n",
		       page == NATIVE_PAGE_SIZE ? "native" : "compat", size,
		       map_len);
	munmap(reservation, map_len + 2 * NATIVE_PAGE_SIZE + 4 * page);
	free(buf);
	close(fd);
	return ret;
}

static int run_child(bool compat)
{
	int status;
	pid_t pid = fork();

	if (pid < 0)
		ksft_exit_fail_msg("fork: %s\n", strerror(errno));
	if (!pid) {
		p3s_execl(compat, NULL, compat ? "--compat" : "--native", NULL);
		_exit(KSFT_SKIP);
	}
	if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status))
		return KSFT_FAIL;
	return WEXITSTATUS(status);
}

static void report(int ret, const char *what)
{
	if (ret == KSFT_SKIP)
		ksft_test_result_skip("%s\n", what);
	else
		ksft_test_result(ret == KSFT_PASS, "%s\n", what);
	fflush(stdout);
}

int main(int argc, char **argv)
{
	const char *mode = p3s_run_mode(argc, argv, NULL);

	if (mode && !strcmp(mode, "--native"))
		return getpagesize() == (int)NATIVE_PAGE_SIZE ? run_case(false) : KSFT_SKIP;
	if (mode && !strcmp(mode, "--compat"))
		return p3s_is_compat_process() ? run_case(true) : KSFT_SKIP;

	ksft_print_header();
	ksft_set_plan(2);
	fflush(stdout);
	report(run_child(false), "native 16K mmap of BTF matches read()");
	report(run_child(true), "4K compat read of BTF works but mmap is unsupported");
	ksft_finished();
}
