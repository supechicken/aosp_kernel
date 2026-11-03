// SPDX-License-Identifier: GPL-2.0
/* TCP mmap refusal must leave read() fallback usable in 4KB processes. */
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include "kselftest_p3s.h"

#ifndef TCP_ZEROCOPY_RECEIVE
#define TCP_ZEROCOPY_RECEIVE 35
#endif

/* Only the original three fields are needed for the alignment control. */
struct zc_receive {
	uint64_t address;
	uint32_t length;
	uint32_t recv_skip_hint;
};

static int connected_loopback_fd(int *peer)
{
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	};
	socklen_t alen = sizeof(addr);
	int lfd, cfd;

	lfd = socket(AF_INET, SOCK_STREAM, 0);
	if (lfd < 0)
		ksft_exit_fail_perror("socket");
	if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) ||
	    listen(lfd, 1) ||
	    getsockname(lfd, (struct sockaddr *)&addr, &alen))
		ksft_exit_fail_perror("listen");
	cfd = socket(AF_INET, SOCK_STREAM, 0);
	if (cfd < 0 || connect(cfd, (struct sockaddr *)&addr, sizeof(addr)))
		ksft_exit_fail_perror("connect");
	*peer = accept(lfd, NULL, NULL);
	if (*peer < 0)
		ksft_exit_fail_perror("accept");
	close(lfd);
	return cfd;
}

/* With an empty receive queue, only the native address-alignment gate runs. */
static int zc_probe(int fd, unsigned long address)
{
	struct zc_receive zc = {
		.address = address,
		.length = NATIVE_PAGE_SIZE,
	};
	socklen_t len = sizeof(zc);

	if (getsockopt(fd, IPPROTO_TCP, TCP_ZEROCOPY_RECEIVE, &zc, &len))
		return -errno;
	return 0;
}

static int run_case(bool compat)
{
	static const char payload[] = "TCP read fallback after mmap refusal";
	char received[sizeof(payload)];
	void *map;
	int fd, peer, saved_errno, ret = KSFT_PASS;
	size_t done = 0;
	ssize_t n;

	fd = connected_loopback_fd(&peer);
	errno = 0;
	map = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ, MAP_SHARED, fd, 0);
	saved_errno = errno;
	if (compat) {
		if (map != MAP_FAILED || saved_errno != EOPNOTSUPP) {
			ksft_print_msg("4KB TCP mmap: expected EOPNOTSUPP, got %s (%d)\n",
				map == MAP_FAILED ? strerror(saved_errno) : "success",
				saved_errno);
			ret = KSFT_FAIL;
		}
	} else if (map == MAP_FAILED) {
		ksft_print_msg("native TCP mmap failed: %s\n", strerror(saved_errno));
		ret = KSFT_FAIL;
	} else if (zc_probe(fd, (unsigned long)map) != 0 ||
		   zc_probe(fd, (unsigned long)map + PROCESS_PAGE_SIZE) != -EINVAL) {
		ksft_print_msg("native TCP zero-copy alignment control failed\n");
		ret = KSFT_FAIL;
	}
	if (map != MAP_FAILED)
		munmap(map, NATIVE_PAGE_SIZE);

	/* A refused mmap is recoverable: normal socket data must still arrive. */
	if (send(peer, payload, sizeof(payload), MSG_NOSIGNAL) != sizeof(payload)) {
		ksft_print_msg("send fallback payload: %s\n", strerror(errno));
		ret = KSFT_FAIL;
		goto out;
	}
	shutdown(peer, SHUT_WR);
	while (done < sizeof(received)) {
		n = read(fd, received + done, sizeof(received) - done);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		done += n;
	}
	if (done != sizeof(received) || memcmp(received, payload, sizeof(payload))) {
		ksft_print_msg("TCP read fallback lost or changed data\n");
		ret = KSFT_FAIL;
	}
out:
	close(peer);
	close(fd);
	return ret;
}

static int run_child(bool compat)
{
	int status;
	pid_t pid = fork();

	if (pid < 0)
		ksft_exit_fail_perror("fork");
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
	report(run_child(false), "native TCP mmap, alignment and read controls");
	report(run_child(true), "4KB TCP mmap refusal preserves read fallback");
	ksft_finished();
}
