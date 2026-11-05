// SPDX-License-Identifier: GPL-2.0
/* Huge shmem policy must preserve bytes and process-page address hints. */
#define _GNU_SOURCE
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include "p3s_test.h"

#define TEST_LENGTH (64UL * 1024 * 1024)

static bool correct_bytes(const unsigned char *mapping)
{
	return mapping[0] == (getpagesize() == PROCESS_PAGE_SIZE ? 0x59 : 0x5a) &&
	       mapping[TEST_LENGTH - getpagesize()] == 0x6d;
}

static int run_test(int fd, const char *mount_path)
{
	unsigned char *mapping, *reservation, *hint;
	unsigned long pmd_mapped = ~0UL;
	size_t reserve_length = TEST_LENGTH + 2 * NATIVE_PAGE_SIZE;
	const size_t page = getpagesize();
	const bool mixed = p3s_is_compat_process();

	ksft_print_header();
	ksft_set_plan(6);
	mapping = mmap(NULL, TEST_LENGTH, PROT_READ, MAP_SHARED, fd, page);
	ksft_test_result(mapping != MAP_FAILED,
			 "map huge-enabled tmpfs from one process-page offset\n");
	if (mapping == MAP_FAILED)
		ksft_exit_fail_msg("file mmap: %s\n", strerror(errno));
	ksft_test_result(!((uintptr_t)mapping &
			   ((mixed ? NATIVE_PAGE_SIZE : page) - 1)),
			 "default placement obeys the active page geometry\n");
	ksft_test_result(correct_bytes(mapping), "retain bytes at the requested file slices\n");
	if (mixed)
		ksft_test_result(p3s_smaps_bytes(mapping, TEST_LENGTH,
					       "ShmemPmdMapped", &pmd_mapped) &&
				 !pmd_mapped,
				 "P3S shmem mappings do not use PMD entries\n");
	else
		ksft_test_result_skip("PMD exclusion is P3S-specific\n");
	munmap(mapping, TEST_LENGTH);

	reservation = mmap(NULL, reserve_length, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (reservation == MAP_FAILED)
		ksft_exit_fail_msg("hint reservation: %s\n", strerror(errno));
	hint = (unsigned char *)(((uintptr_t)reservation + NATIVE_PAGE_SIZE - 1) &
				~(NATIVE_PAGE_SIZE - 1)) +
	       (page == NATIVE_PAGE_SIZE ? 0 : page);
	munmap(reservation, reserve_length);
	mapping = mmap(hint, TEST_LENGTH, PROT_READ, MAP_SHARED, fd, page);
	ksft_test_result(mapping == hint,
			 "honor a free process-page-aligned nonfixed hint\n");
	ksft_test_result(mapping != MAP_FAILED && correct_bytes(mapping),
			 "hint placement retains the same file bytes\n");
	if (mapping != MAP_FAILED)
		munmap(mapping, TEST_LENGTH);
	close(fd);
	if (umount(mount_path) || rmdir(mount_path))
		ksft_exit_fail_msg("tmpfs cleanup: %s\n", strerror(errno));
	ksft_finished();
}

int main(int argc, char **argv)
{
	const char *mode = p3s_run_mode(argc, argv, NULL);
	char mount_path[] = "./shmem-mmap-offset-p3s-XXXXXX";
	char file_path[sizeof(mount_path) + 16], fd_string[32];
	unsigned char head4k = 0x59, head16k = 0x5a, tail = 0x6d;
	int fd;

	if (mode && argc == 4 && !strcmp(mode, P3S_RUN_FLAG))
		return run_test(atoi(argv[2]), argv[3]);
	if (argc != 1)
		return EXIT_FAILURE;
	if (!mkdtemp(mount_path))
		ksft_exit_fail_msg("mkdtemp: %s\n", strerror(errno));
	if (mount("tmpfs", mount_path, "tmpfs", 0, "size=128m,huge=always")) {
		rmdir(mount_path);
		ksft_exit_skip("huge=always tmpfs: %s\n", strerror(errno));
	}
	snprintf(file_path, sizeof(file_path), "%s/test", mount_path);
	fd = open(file_path, O_CREAT | O_EXCL | O_RDWR, 0600);
	if (fd < 0 || ftruncate(fd, NATIVE_PAGE_SIZE + TEST_LENGTH))
		ksft_exit_fail_msg("backing file: %s\n", strerror(errno));
	unlink(file_path);
	if (pwrite(fd, &head4k, 1, PROCESS_PAGE_SIZE) != 1 ||
	    pwrite(fd, &head16k, 1, NATIVE_PAGE_SIZE) != 1 ||
	    pwrite(fd, &tail, 1, TEST_LENGTH) != 1)
		ksft_exit_fail_msg("backing bytes: %s\n", strerror(errno));
	snprintf(fd_string, sizeof(fd_string), "%d", fd);
	if (getpagesize() == NATIVE_PAGE_SIZE)
		exec_compat(argv[0], P3S_RUN_FLAG, fd_string, mount_path, NULL);
	else {
		char *const args[] = { argv[0], P3S_RUN_FLAG, fd_string,
				       mount_path, NULL };

		execv("/proc/self/exe", args);
		ksft_exit_fail_msg("re-exec failed: %s\n", strerror(errno));
	}
}
