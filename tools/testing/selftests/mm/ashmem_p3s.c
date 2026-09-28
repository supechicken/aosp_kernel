// SPDX-License-Identifier: GPL-2.0
/* Pin a shared ashmem region using each caller's process-page geometry. */
#define _GNU_SOURCE

#include <limits.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include "../../../drivers/staging/android/uapi/ashmem.h"
#include "kselftest_p3s.h"

static int new_region(void)
{
	int fd = open("/dev/ashmem", O_RDWR | O_CLOEXEC);

	if (fd >= 0 && ioctl(fd, ASHMEM_SET_SIZE, 2 * NATIVE_PAGE_SIZE)) {
		close(fd);
		return -1;
	}
	return fd;
}

static bool pin_status(int fd, unsigned int command, unsigned int length,
		       int expected)
{
	struct ashmem_pin pin = { .offset = 0, .len = length };

	return ioctl(fd, command, &pin) == expected;
}

/* len == 0 covers the native-rounded extent accepted by ashmem mmap. */
static bool tail_range_checks(void)
{
	const unsigned long map_size = 2 * NATIVE_PAGE_SIZE;
	struct ashmem_pin whole = { .offset = 0, .len = 0 };
	struct ashmem_pin tail = {
		.offset = NATIVE_PAGE_SIZE, .len = NATIVE_PAGE_SIZE,
	};
	unsigned char resident = 0;
	unsigned char *mapping;
	int fd, unpin, status, purge, repin, cache;
	bool zero, ok;

	fd = open("/dev/ashmem", O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return false;
	if (ioctl(fd, ASHMEM_SET_SIZE, NATIVE_PAGE_SIZE + PROCESS_PAGE_SIZE)) {
		close(fd);
		return false;
	}
	mapping = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (mapping == MAP_FAILED) {
		close(fd);
		return false;
	}
	mapping[NATIVE_PAGE_SIZE] = 0x5a;
	ok = !mincore(mapping + NATIVE_PAGE_SIZE, getpagesize(), &resident) &&
	     (resident & 1);
	unpin = ioctl(fd, ASHMEM_UNPIN, &whole);
	status = ioctl(fd, ASHMEM_GET_PIN_STATUS, &tail);
	purge = ioctl(fd, ASHMEM_PURGE_ALL_CACHES);
	resident = 0xff;
	cache = mincore(mapping + NATIVE_PAGE_SIZE, getpagesize(), &resident);
	repin = ioctl(fd, ASHMEM_PIN, &whole);
	zero = mapping[NATIVE_PAGE_SIZE] == 0;
	ksft_print_msg("ashmem tail pagesz=%d unpin=%d status=%d purge=%d "
		       "mincore=%d resident=%u repin=%d zero=%d\n",
		       getpagesize(), unpin, status, purge, cache, resident, repin, zero);
	ok &= unpin == 0 && status == ASHMEM_IS_UNPINNED && purge >= 0 &&
	      cache == 0 && !(resident & 1) && repin == ASHMEM_WAS_PURGED && zero;
	munmap(mapping, map_size);
	close(fd);
	return ok;
}

static int native_checks(int fd)
{
	struct ashmem_pin first = { .offset = 0, .len = PROCESS_PAGE_SIZE };
	bool own = fd < 0;
	bool ok;
	void *mapping;

	if (getpagesize() != NATIVE_PAGE_SIZE)
		return EXIT_FAILURE;
	if (own)
		fd = new_region();
	if (fd < 0)
		return EXIT_FAILURE;
	mapping = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE,
		       MAP_SHARED, fd, 0);
	if (mapping == MAP_FAILED)
		return EXIT_FAILURE;
	munmap(mapping, NATIVE_PAGE_SIZE);
	errno = 0;
	ok = ioctl(fd, ASHMEM_UNPIN, &first) == -1 && errno == EINVAL;
	ok &= pin_status(fd, ASHMEM_UNPIN, NATIVE_PAGE_SIZE, 0);
	ok &= pin_status(fd, ASHMEM_GET_PIN_STATUS, NATIVE_PAGE_SIZE,
			 ASHMEM_IS_UNPINNED);
	ok &= pin_status(fd, ASHMEM_PIN, NATIVE_PAGE_SIZE, ASHMEM_NOT_PURGED);
	ok &= pin_status(fd, ASHMEM_GET_PIN_STATUS, NATIVE_PAGE_SIZE,
			 ASHMEM_IS_PINNED);
	if (own) {
		close(fd);
		ok &= tail_range_checks();
	}
	return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

static bool native_child(int fd)
{
	char fd_arg[16];
	int flags = fd < 0 ? 0 : fcntl(fd, F_GETFD);
	int status;
	pid_t pid;

	if (fd >= 0 && (flags < 0 || fcntl(fd, F_SETFD, flags & ~FD_CLOEXEC)))
		return false;
	snprintf(fd_arg, sizeof(fd_arg), "%d", fd);
	pid = fork();
	if (!pid) {
		p3s_execl(false, NULL, "--native-pin", fd_arg, NULL);
		_exit(EXIT_FAILURE);
	}
	if (fd >= 0 && fcntl(fd, F_SETFD, flags))
		return false;
	if (pid < 0 || waitpid(pid, &status, 0) != pid)
		return false;
	return WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS;
}

static int compat_checks(void)
{
	void *mapping;
	int fd = new_region();

	if (fd < 0) {
		if (errno == ENOENT || errno == ENODEV)
			ksft_exit_skip("/dev/ashmem is unavailable\n");
		ksft_exit_fail_msg("ashmem setup failed: %s\n", strerror(errno));
	}
	ksft_print_header();
	ksft_set_plan(10);
	mapping = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE,
		       MAP_SHARED, fd, 0);
	ksft_test_result(mapping != MAP_FAILED, "create the shared backing file\n");
	if (mapping == MAP_FAILED)
		ksft_exit_fail_msg("ashmem mmap failed: %s\n", strerror(errno));
	munmap(mapping, NATIVE_PAGE_SIZE);
	ksft_test_result(pin_status(fd, ASHMEM_UNPIN, PROCESS_PAGE_SIZE, 0),
			 "unpin a 4KB range\n");
	ksft_test_result(pin_status(fd, ASHMEM_GET_PIN_STATUS, PROCESS_PAGE_SIZE,
				    ASHMEM_IS_UNPINNED), "query an unpinned 4KB range\n");
	ksft_test_result(pin_status(fd, ASHMEM_PIN, PROCESS_PAGE_SIZE, ASHMEM_NOT_PURGED),
			 "pin a 4KB range\n");
	ksft_test_result(pin_status(fd, ASHMEM_GET_PIN_STATUS, PROCESS_PAGE_SIZE,
				    ASHMEM_IS_PINNED), "query a pinned 4KB range\n");
	ksft_test_result(pin_status(fd, ASHMEM_UNPIN, PROCESS_PAGE_SIZE, 0),
			 "unpin before passing the fd to a native process\n");
	ksft_test_result(native_child(fd), "native caller pins the same shared fd\n");
	ksft_test_result(pin_status(fd, ASHMEM_GET_PIN_STATUS, PROCESS_PAGE_SIZE,
				    ASHMEM_IS_PINNED), "observe the native caller's pin\n");
	close(fd);
	ksft_test_result(native_child(-1), "retain native pin/unpin/status semantics\n");
	ksft_test_result(tail_range_checks(),
			 "len=0 unpin covers and reclaims the native-rounded tail\n");
	ksft_finished();
}

int main(int argc, char **argv)
{
	const char *mode = p3s_run_mode(argc, argv, NULL);
	char *end;
	long fd;

	if (mode && argc == 3 && !strcmp(mode, "--native-pin")) {
		errno = 0;
		fd = strtol(argv[2], &end, 10);
		if (errno || *end || fd < -1 || fd > INT_MAX)
			return EXIT_FAILURE;
		return native_checks(fd);
	}
	return p3s_compat_main(argc, argv, compat_checks);
}
