// SPDX-License-Identifier: GPL-2.0
/* Huge shmem policy must preserve file bytes and valid 4KB address hints. */
#define _GNU_SOURCE
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include "p3s_test.h"

#define TEST_LENGTH (64UL * 1024 * 1024)

static bool correct_bytes(const unsigned char *mapping)
{
	return mapping[0] == 0x59 &&
	       mapping[TEST_LENGTH - PROCESS_PAGE_SIZE] == 0x6d;
}

static int run_test(int fd, const char *mount_path)
{
	unsigned char *mapping, *reservation, *hint;
	unsigned long pmd_mapped = ~0UL;
	size_t reserve_length = TEST_LENGTH + 2 * NATIVE_PAGE_SIZE;

	p3s_require_compat();
	ksft_print_header();
	ksft_set_plan(6);
	mapping = mmap(NULL, TEST_LENGTH, PROT_READ, MAP_SHARED, fd, PROCESS_PAGE_SIZE);
	ksft_test_result(mapping != MAP_FAILED,
			 "map huge-enabled tmpfs from file offset 4KB\n");
	if (mapping == MAP_FAILED)
		ksft_exit_fail_msg("file mmap: %s\n", strerror(errno));
	ksft_test_result(!((uintptr_t)mapping & (NATIVE_PAGE_SIZE - 1)),
			 "default placement remains native-folio aligned\n");
	ksft_test_result(correct_bytes(mapping), "retain bytes at the requested file slices\n");
	ksft_test_result(p3s_smaps_bytes(mapping, TEST_LENGTH, "ShmemPmdMapped", &pmd_mapped) &&
			 !pmd_mapped, "4KB shmem mappings do not use PMD entries\n");
	munmap(mapping, TEST_LENGTH);

	reservation = mmap(NULL, reserve_length, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (reservation == MAP_FAILED)
		ksft_exit_fail_msg("hint reservation: %s\n", strerror(errno));
	hint = (unsigned char *)(((uintptr_t)reservation + NATIVE_PAGE_SIZE - 1) &
				~(NATIVE_PAGE_SIZE - 1)) + PROCESS_PAGE_SIZE;
	munmap(reservation, reserve_length);
	mapping = mmap(hint, TEST_LENGTH, PROT_READ, MAP_SHARED, fd, PROCESS_PAGE_SIZE);
	ksft_test_result(mapping == hint, "honor a free 4KB-aligned nonfixed hint\n");
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
	unsigned char head = 0x59, tail = 0x6d;
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
	if (fd < 0 || ftruncate(fd, PROCESS_PAGE_SIZE + TEST_LENGTH))
		ksft_exit_fail_msg("backing file: %s\n", strerror(errno));
	unlink(file_path);
	if (pwrite(fd, &head, 1, PROCESS_PAGE_SIZE) != 1 ||
	    pwrite(fd, &tail, 1, TEST_LENGTH) != 1)
		ksft_exit_fail_msg("backing bytes: %s\n", strerror(errno));
	snprintf(fd_string, sizeof(fd_string), "%d", fd);
	exec_compat(argv[0], P3S_RUN_FLAG, fd_string, mount_path, NULL);
}
