// SPDX-License-Identifier: GPL-2.0
/*
 * Native and 4K compat exec keep the stack VMA end 16K-aligned while
 * retaining address randomization across repeated execs.
 */
#define _GNU_SOURCE

#include <limits.h>
#include <signal.h>
#include <sys/wait.h>

#include "kselftest_p3s.h"

#define SAMPLE_COUNT 64

static bool read_stack_end(pid_t pid, uintptr_t *stack_end)
{
	char path[64];
	char *line = NULL;
	size_t line_size = 0;
	bool found = false;
	FILE *file;

	if (snprintf(path, sizeof(path), "/proc/%d/maps", pid) >=
	    (int)sizeof(path))
		return false;
	file = fopen(path, "re");
	if (!file)
		return false;
	while (getline(&line, &line_size, file) >= 0) {
		unsigned long start, end;

		if (strstr(line, "[stack]") &&
		    sscanf(line, "%lx-%lx", &start, &end) == 2) {
			*stack_end = end;
			found = true;
			break;
		}
	}
	free(line);
	fclose(file);
	return found;
}

static bool collect_samples(bool compat, unsigned int *residue_mask,
			    bool *varied)
{
	uintptr_t first = 0;
	unsigned int i;

	*residue_mask = 0;
	*varied = false;
	for (i = 0; i < SAMPLE_COUNT; i++) {
		uintptr_t stack_end;
		unsigned char ready;
		int pipefd[2];
		int status;
		pid_t pid;

		if (pipe(pipefd))
			return false;
		pid = fork();
		if (pid < 0) {
			close(pipefd[0]);
			close(pipefd[1]);
			return false;
		}
		if (!pid) {
			close(pipefd[0]);
			if (pipefd[1] != 3) {
				if (dup2(pipefd[1], 3) < 0)
					_exit(126);
				close(pipefd[1]);
			}
			p3s_execl(compat, NULL,
				  compat ? "--sample-compat" : "--sample-native",
				  NULL);
			_exit(127);
		}
		close(pipefd[1]);
		if (read(pipefd[0], &ready, 1) != 1 ||
		    !read_stack_end(pid, &stack_end)) {
			close(pipefd[0]);
			kill(pid, SIGKILL);
			waitpid(pid, &status, 0);
			return false;
		}
		close(pipefd[0]);
		*residue_mask |= 1U << ((stack_end / PROCESS_PAGE_SIZE) & 3);
		if (!i)
			first = stack_end;
		else if (stack_end != first)
			*varied = true;
		kill(pid, SIGKILL);
		if (waitpid(pid, &status, 0) != pid)
			return false;
	}
	return true;
}

static bool aslr_enabled(void)
{
	int randomize_va_space;
	FILE *file = fopen("/proc/sys/kernel/randomize_va_space", "re");

	if (!file)
		return false;
	if (fscanf(file, "%d", &randomize_va_space) != 1)
		randomize_va_space = 0;
	fclose(file);
	return randomize_va_space > 0;
}

static int run_test(void)
{
	unsigned int residue_mask;
	bool collected, varied;
	unsigned int mode;

	p3s_require_compat();
	ksft_print_header();
	if (!aslr_enabled())
		ksft_exit_skip("address randomization is disabled\n");
	ksft_set_plan(6);

	for (mode = 0; mode < 2; mode++) {
		const char *name = mode ? "4K compat" : "native";

		collected = collect_samples(mode, &residue_mask, &varied);
		ksft_test_result(collected, "%s: collect stack VMA ends\n", name);
		if (!collected)
			ksft_exit_fail_msg("could not collect stack samples\n");
		ksft_print_msg("%s: 16K stack-end residue mask: %#x\n",
			       name, residue_mask);
		ksft_test_result(residue_mask == 1,
				 "%s: stack VMA end stays 16K-aligned\n", name);
		ksft_test_result(varied, "%s: stack ASLR remains enabled\n", name);
	}
	ksft_finished();
}

static int sample(bool compat)
{
	unsigned char ready = 1;

	if (compat ? !p3s_is_compat_process() :
	    getpagesize() != NATIVE_PAGE_SIZE)
		return EXIT_FAILURE;
	if (write(3, &ready, 1) != 1)
		return EXIT_FAILURE;
	for (;;)
		pause();
}

int main(int argc, char **argv)
{
	const char *mode = p3s_run_mode(argc, argv, NULL);

	if (!mode)
		exec_compat(argv[0], P3S_RUN_FLAG, NULL);
	if (argc == 2 && !strcmp(mode, P3S_RUN_FLAG))
		return run_test();
	if (argc == 2 && !strcmp(mode, "--sample-compat"))
		return sample(true);
	if (argc == 2 && !strcmp(mode, "--sample-native"))
		return sample(false);
	return EXIT_FAILURE;
}
