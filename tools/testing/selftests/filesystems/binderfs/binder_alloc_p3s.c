// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/android/binder.h>
#include <linux/android/binderfs.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/personality.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../../kselftest_p3s.h"

#define USER_PAGE_SIZE 4096UL
#define BINDER_VM_SIZE ((1UL * 1024 * 1024) - 2 * USER_PAGE_SIZE)
#define BINDER_VM_HINT ((void *)0x100000000ULL)
#define TEST_CODE 0x50505053U
#define TRANSACTION_PAYLOAD_SIZE (BINDER_VM_SIZE - USER_PAGE_SIZE)

struct server_ready {
	int error;
	uintptr_t mapping;
};

struct transaction_result {
	bool received;
	int active_pages;
	int lru_pages;
};

static unsigned char transaction_payload[TRANSACTION_PAYLOAD_SIZE];

static void init_transaction_payload(void)
{
	size_t i;

	for (i = 0; i < sizeof(transaction_payload); i++)
		transaction_payload[i] = (i * 131 + 17) & 0xff;
}

static unsigned long mapping_kernel_page_size(pid_t pid, uintptr_t address)
{
	char path[64];
	char *line = NULL;
	size_t line_size = 0;
	unsigned long page_size = 0;
	bool in_mapping = false;
	FILE *file;

	if (snprintf(path, sizeof(path), "/proc/%d/smaps", pid) >=
	    (int)sizeof(path))
		return 0;
	file = fopen(path, "re");
	if (!file)
		return 0;
	while (getline(&line, &line_size, file) >= 0) {
		unsigned long start, end, size_kb;

		if (sscanf(line, "%lx-%lx", &start, &end) == 2) {
			in_mapping = address >= start && address < end;
			continue;
		}
		if (in_mapping &&
		    sscanf(line, "KernelPageSize: %lu kB", &size_kb) == 1) {
			page_size = size_kb * 1024;
			break;
		}
	}
	free(line);
	fclose(file);
	return page_size;
}

static int binder_page_slots(const char *binderfs_dir, pid_t pid,
			     int *active_pages, int *lru_pages)
{
	char path[320];
	char *line = NULL;
	size_t line_size = 0;
	int slots = -1;
	bool in_proc = false;
	FILE *file;

	if (snprintf(path, sizeof(path), "%s/binder_logs/stats",
		     binderfs_dir) >= (int)sizeof(path))
		return -1;
	file = fopen(path, "re");
	if (!file) {
		ksft_print_msg("open %s failed: %s\n", path, strerror(errno));
		return -1;
	}
	while (getline(&line, &line_size, file) >= 0) {
		int active, lru, free_pages;
		int current_pid;

		if (sscanf(line, "proc %d", &current_pid) == 1) {
			in_proc = current_pid == pid;
			continue;
		}
		if (!in_proc)
			continue;
		if (sscanf(line, "  pages: %d:%d:%d", &active, &lru, &free_pages) == 3) {
			slots = active + lru + free_pages;
			if (active_pages)
				*active_pages = active;
			if (lru_pages)
				*lru_pages = lru;
			break;
		}
	}
	free(line);
	fclose(file);
	if (slots < 0)
		ksft_print_msg("no Binder page statistics in %s\n", path);
	return slots;
}

static int binder_write_commands(int fd, const void *commands, size_t size)
{
	struct binder_write_read bwr = {
		.write_size = size,
		.write_buffer = (binder_uintptr_t)(uintptr_t)commands,
	};

	return ioctl(fd, BINDER_WRITE_READ, &bwr);
}

static int binder_enter_looper(int fd)
{
	uint32_t command = BC_ENTER_LOOPER;

	return binder_write_commands(fd, &command, sizeof(command));
}

static int binder_free_transaction(int fd, binder_uintptr_t buffer)
{
	unsigned char request[sizeof(uint32_t) + sizeof(buffer)];
	uint32_t command = BC_FREE_BUFFER;

	memcpy(request, &command, sizeof(command));
	memcpy(request + sizeof(command), &buffer, sizeof(buffer));
	return binder_write_commands(fd, request, sizeof(request));
}

static int binder_reply_empty(int fd)
{
	struct binder_transaction_data transaction = {};
	unsigned char commands[sizeof(uint32_t) + sizeof(transaction)];
	uint32_t command = BC_REPLY;

	memcpy(commands, &command, sizeof(command));
	memcpy(commands + sizeof(command), &transaction, sizeof(transaction));
	return binder_write_commands(fd, commands, sizeof(commands));
}

static struct transaction_result read_transaction(int fd,
					  const char *binderfs_dir,
					  bool check_lru)
{
	struct transaction_result result = {
		.active_pages = -1,
		.lru_pages = -1,
	};
	unsigned char read_buffer[4096];
	struct pollfd pfd = {
		.fd = fd,
		.events = POLLIN,
	};
	unsigned int attempt;

	for (attempt = 0; attempt < 8; attempt++) {
		struct binder_write_read bwr = {
			.read_size = sizeof(read_buffer),
			.read_buffer = (binder_uintptr_t)(uintptr_t)read_buffer,
		};
		size_t offset = 0;

		if (poll(&pfd, 1, 500) <= 0)
			continue;
		if (ioctl(fd, BINDER_WRITE_READ, &bwr))
			return result;
		while (offset + sizeof(uint32_t) <= bwr.read_consumed) {
			struct binder_transaction_data transaction;
			uint32_t command;

			memcpy(&command, read_buffer + offset, sizeof(command));
			offset += sizeof(command);
			switch (command) {
			case BR_TRANSACTION:
				if (offset + sizeof(transaction) > bwr.read_consumed)
					return result;
				memcpy(&transaction, read_buffer + offset,
				       sizeof(transaction));
				if (transaction.code != TEST_CODE ||
				    transaction.data_size != sizeof(transaction_payload) ||
				    memcmp((void *)(uintptr_t)transaction.data.ptr.buffer,
					   transaction_payload,
					   sizeof(transaction_payload)))
					return result;
				if (check_lru)
					binder_page_slots(binderfs_dir, getpid(),
							  &result.active_pages,
							  &result.lru_pages);
				result.received = !binder_free_transaction(fd,
							 transaction.data.ptr.buffer) &&
						  !binder_reply_empty(fd);
				return result;
			case BR_NOOP:
			case BR_TRANSACTION_COMPLETE:
			case BR_SPAWN_LOOPER:
			case BR_FINISHED:
				break;
			case BR_ERROR:
				offset += sizeof(int32_t);
				break;
			default:
				return result;
			}
		}
	}
	return result;
}

static void binder_server(const char *device_path, const char *binderfs_dir,
			  int ready_fd, int result_fd)
{
	struct server_ready ready = {};
	struct transaction_result result = {
		.active_pages = -1,
		.lru_pages = -1,
	};
	void *mapping = MAP_FAILED;
	int manager = 0;
	int fd;

	fd = open(device_path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		ready.error = errno;
		goto report_ready;
	}
	mapping = mmap(BINDER_VM_HINT, BINDER_VM_SIZE, PROT_READ, MAP_SHARED,
		       fd, 0);
	if (mapping == MAP_FAILED) {
		ready.error = errno;
		goto report_ready;
	}
	ready.mapping = (uintptr_t)mapping;
	if (ioctl(fd, BINDER_SET_CONTEXT_MGR, &manager) ||
	    binder_enter_looper(fd)) {
		ready.error = errno;
		goto report_ready;
	}

report_ready:
	write_full(ready_fd, &ready, sizeof(ready));
	close(ready_fd);
	for (unsigned int round = 0; !ready.error && round < 2; round++) {
		result = read_transaction(fd, binderfs_dir, round == 1);
		write_full(result_fd, &result, sizeof(result));
		if (!result.received)
			break;
	}
	close(result_fd);
	if (mapping != MAP_FAILED)
		munmap(mapping, BINDER_VM_SIZE);
	if (fd >= 0)
		close(fd);
	_exit(result.received ? EXIT_SUCCESS : EXIT_FAILURE);
}

static int send_transaction(const char *device_path, void **mapping_out)
{
	struct binder_transaction_data transaction = {
		.target.handle = 0,
		.code = TEST_CODE,
		.data_size = sizeof(transaction_payload),
		.data.ptr.buffer = (binder_uintptr_t)(uintptr_t)transaction_payload,
	};
	unsigned char commands[sizeof(uint32_t) + sizeof(transaction)];
	uint32_t command = BC_TRANSACTION;
	int fd;
	int ret;
	void *mapping;

	memcpy(commands, &command, sizeof(command));
	memcpy(commands + sizeof(command), &transaction, sizeof(transaction));
	fd = open(device_path, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return -1;
	mapping = mmap(NULL, BINDER_VM_SIZE, PROT_READ, MAP_SHARED, fd, 0);
	if (mapping == MAP_FAILED) {
		close(fd);
		return -1;
	}
	ret = binder_write_commands(fd, commands, sizeof(commands));
	if (ret) {
		munmap(mapping, BINDER_VM_SIZE);
		close(fd);
		return -1;
	}
	*mapping_out = mapping;
	return fd;
}

static bool wait_readable(int fd, int timeout_ms)
{
	struct pollfd pfd = {
		.fd = fd,
		.events = POLLIN,
	};

	return poll(&pfd, 1, timeout_ms) > 0;
}

static void check_mapping_policy(const char *device, void *hint,
				 unsigned long offset, bool reject)
{
	int fd = open(device, O_RDWR | O_CLOEXEC);
	void *mapping;
	int error;

	if (fd < 0)
		ksft_exit_fail_msg("open Binder policy fixture: %s\n", strerror(errno));
	errno = 0;
	mapping = mmap(hint, BINDER_VM_SIZE, PROT_READ, MAP_SHARED, fd, offset);
	error = errno;
	ksft_test_result(reject ? mapping == MAP_FAILED && error == EINVAL :
			 mapping != MAP_FAILED &&
			 !((uintptr_t)mapping & (NATIVE_PAGE_SIZE - 1)),
			 "Binder hint=%p offset=%lu reject=%d errno=%d\n",
			 hint, offset, reject, error);
	if (mapping != MAP_FAILED)
		munmap(mapping, BINDER_VM_SIZE);
	close(fd);
}

static int run_test(const char *binderfs_dir)
{
	char control_path[256];
	char device_path[256];
	struct binderfs_device device = {};
	struct server_ready ready = {};
	unsigned long kernel_page_size;
	unsigned long expected_slots;
	struct transaction_result result = {
		.active_pages = -1,
		.lru_pages = -1,
	};
	bool first_received;
	void *sender_mapping = NULL;
	int page_slots;
	int sender_fd;
	int ready_pipe[2];
	int result_pipe[2];
	pid_t server;
	int control_fd;
	int status;

	ksft_print_header();
	p3s_require_compat();
	ksft_set_plan(11);
	init_transaction_payload();
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");
	if (snprintf(control_path, sizeof(control_path), "%s/binder-control",
		     binderfs_dir) >= (int)sizeof(control_path) ||
	    snprintf(device_path, sizeof(device_path), "%s/p3s-binder",
		     binderfs_dir) >= (int)sizeof(device_path))
		ksft_exit_fail_msg("binderfs path is too long\n");
	strcpy(device.name, "p3s-binder");
	control_fd = open(control_path, O_RDONLY | O_CLOEXEC);
	if (control_fd < 0 || ioctl(control_fd, BINDER_CTL_ADD, &device))
		ksft_exit_fail_msg("create binder device failed: %s\n",
				   strerror(errno));
	close(control_fd);
	ksft_test_result(access(device_path, R_OK | W_OK) == 0,
			 "create a binderfs device\n");

	check_mapping_policy(device_path, BINDER_VM_HINT + USER_PAGE_SIZE, 0, true);
	check_mapping_policy(device_path, NULL, USER_PAGE_SIZE, true);
	check_mapping_policy(device_path, NULL, 3 * USER_PAGE_SIZE, true);
	check_mapping_policy(device_path, NULL, NATIVE_PAGE_SIZE, false);

	if (pipe(ready_pipe) || pipe(result_pipe))
		ksft_exit_fail_msg("create binder test pipes failed: %s\n",
				   strerror(errno));
	server = fork();
	if (server < 0)
		ksft_exit_fail_msg("fork binder server failed: %s\n",
				   strerror(errno));
	if (!server) {
		close(ready_pipe[0]);
		close(result_pipe[0]);
		binder_server(device_path, binderfs_dir, ready_pipe[1],
			      result_pipe[1]);
	}
	close(ready_pipe[1]);
	close(result_pipe[1]);
	if (wait_readable(ready_pipe[0], 2000))
		read_full(ready_pipe[0], &ready, sizeof(ready));
	close(ready_pipe[0]);
	ksft_print_msg("binder mapping=%#lx error=%d\n",
		       (unsigned long)ready.mapping, ready.error);
	ksft_test_result(!ready.error && ready.mapping == (uintptr_t)BINDER_VM_HINT &&
			 !(ready.mapping & (NATIVE_PAGE_SIZE - 1)),
			 "map Binder at a native-page-aligned hint with a partial tail\n");
	kernel_page_size = mapping_kernel_page_size(server, ready.mapping);
	page_slots = binder_page_slots(binderfs_dir, server, NULL, NULL);
	expected_slots = kernel_page_size ?
		(BINDER_VM_SIZE + kernel_page_size - 1) / kernel_page_size : 0;
	ksft_print_msg("binder size=%lu kernel_page_size=%lu slots=%d expected=%lu\n",
		       BINDER_VM_SIZE, kernel_page_size, page_slots, expected_slots);
	if (page_slots < 0)
		ksft_test_result_skip("Binder page statistics unavailable\n");
	else
		ksft_test_result(kernel_page_size &&
				 page_slots == (int)expected_slots,
				 "track the Android Binder mapping's partial backing page\n");

	sender_fd = ready.error ? -1 : send_transaction(device_path, &sender_mapping);
	if (sender_fd >= 0 &&
	    wait_readable(result_pipe[0], 5000))
		read_full(result_pipe[0], &result, sizeof(result));
	if (sender_fd >= 0) {
		munmap(sender_mapping, BINDER_VM_SIZE);
		close(sender_fd);
	}
	ksft_test_result(result.received,
			 "deliver a transaction through the subpage mapping\n");
	first_received = result.received;
	result = (struct transaction_result) {
		.active_pages = -1,
		.lru_pages = -1,
	};
	sender_fd = first_received ? send_transaction(device_path,
						      &sender_mapping) : -1;
	if (sender_fd >= 0 &&
	    wait_readable(result_pipe[0], 5000))
		read_full(result_pipe[0], &result, sizeof(result));
	if (sender_fd >= 0) {
		munmap(sender_mapping, BINDER_VM_SIZE);
		close(sender_fd);
	}
	close(result_pipe[0]);
	ksft_test_result(result.received,
			 "repeat the large transaction in the same Binder mapping\n");
	if (result.lru_pages < 0)
		ksft_test_result_skip("Binder page statistics unavailable\n");
	else
		ksft_test_result(result.active_pages == (int)expected_slots &&
				 result.lru_pages == 0,
				 "no in-use Binder tail page remains on the LRU\n");
	if (!result.received)
		kill(server, SIGKILL);
	waitpid(server, &status, 0);
	unlink(device_path);
	ksft_finished();
}

static int run_in_private_binderfs(void)
{
	char mountpoint[] = P_tmpdir "/binder-alloc-p3s.XXXXXX";
	int status;
	pid_t child;

	if (unshare(CLONE_NEWNS) ||
	    mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL))
		ksft_exit_skip("private mount namespace unavailable: %s\n",
			       strerror(errno));
	if (!mkdtemp(mountpoint))
		ksft_exit_fail_msg("create binderfs mountpoint failed: %s\n",
				   strerror(errno));
	if (mount(NULL, mountpoint, "binder", 0, "stats=global")) {
		int saved_errno = errno;

		rmdir(mountpoint);
		if (saved_errno == ENODEV)
			ksft_exit_skip("binderfs is unavailable\n");
		errno = saved_errno;
		ksft_exit_fail_msg("mount binderfs failed: %s\n",
				   strerror(errno));
	}

	child = fork();
	if (child < 0)
		ksft_exit_fail_msg("fork compatibility process failed: %s\n",
				   strerror(errno));
	if (!child)
		exec_compat("binder_alloc_p3s", "--run", mountpoint, NULL);
	if (waitpid(child, &status, 0) < 0)
		ksft_exit_fail_msg("wait for compatibility process failed: %s\n",
				   strerror(errno));
	if (umount2(mountpoint, MNT_DETACH) || rmdir(mountpoint))
		ksft_exit_fail_msg("clean up binderfs failed: %s\n",
				   strerror(errno));
	if (!WIFEXITED(status))
		return EXIT_FAILURE;
	return WEXITSTATUS(status);
}

int main(int argc, char **argv)
{
	p3s_run_mode(argc, argv, NULL);
	if (argc < 3)
		p3s_require_native_compat();

	if (argc == 1)
		return run_in_private_binderfs();
	if (argc == 2)
		exec_compat("binder_alloc_p3s", "--run", argv[1], NULL);
	if (argc == 3 && !strcmp(argv[1], "--run"))
		return run_test(argv[2]);
	return EXIT_FAILURE;
}
