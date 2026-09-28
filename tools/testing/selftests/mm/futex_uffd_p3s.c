// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <linux/futex.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#include "../kselftest_p3s.h"

struct fault_handler {
	int fd;
	void *source;
	size_t page_size;
	atomic_bool stop;
	unsigned int events;
	int error;
};

static void *resolve_fault(void *argument)
{
	struct fault_handler *h = argument;
	struct pollfd p = { .fd = h->fd, .events = POLLIN };

	while (!atomic_load(&h->stop)) {
		struct uffd_msg msg;
		struct uffdio_copy copy = {
			.src = (uintptr_t)h->source,
			.len = h->page_size,
		};
		int ret = poll(&p, 1, 100);

		if (ret < 0 && errno != EINTR) {
			h->error = errno;
			break;
		}
		if (ret <= 0)
			continue;
		if (read(h->fd, &msg, sizeof(msg)) != sizeof(msg) ||
		    msg.event != UFFD_EVENT_PAGEFAULT) {
			h->error = EPROTO;
			break;
		}
		h->events++;
		copy.dst = msg.arg.pagefault.address & ~(h->page_size - 1);
		if (ioctl(h->fd, UFFDIO_COPY, &copy)) {
			h->error = errno;
			break;
		}
	}
	return NULL;
}

static int run_test(void)
{
	struct uffdio_api api = { .api = UFFD_API };
	struct uffdio_register reg = { .mode = UFFDIO_REGISTER_MODE_MISSING };
	struct fault_handler h = { .page_size = getpagesize() };
	uint32_t *word;
	pthread_t thread;
	int result, error;

	ksft_print_header();
	word = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	h.source = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (word == MAP_FAILED || h.source == MAP_FAILED)
		ksft_exit_fail_msg("mmap: %s\n", strerror(errno));
	memset(h.source, 0, NATIVE_PAGE_SIZE);
	*word = 0;
	result = syscall(SYS_futex, word, FUTEX_WAKE, 1, NULL, NULL, 0);
	if (result)
		ksft_exit_fail_msg("prefaulted shared FUTEX_WAKE: %d/%d\n",
				   result, errno);
	h.fd = syscall(SYS_userfaultfd, O_NONBLOCK | O_CLOEXEC);
	if (h.fd < 0 || ioctl(h.fd, UFFDIO_API, &api))
		ksft_exit_skip("kernel-fault userfaultfd unavailable: %s\n",
			       strerror(errno));
	if (madvise(word, NATIVE_PAGE_SIZE, MADV_DONTNEED))
		ksft_exit_fail_msg("MADV_DONTNEED: %s\n", strerror(errno));
	reg.range.start = (uintptr_t)word;
	reg.range.len = NATIVE_PAGE_SIZE;
	if (ioctl(h.fd, UFFDIO_REGISTER, &reg))
		ksft_exit_fail_msg("UFFDIO_REGISTER: %s\n", strerror(errno));
	if (pthread_create(&thread, NULL, resolve_fault, &h))
		ksft_exit_fail_msg("pthread_create\n");
	errno = 0;
	result = syscall(SYS_futex, word, FUTEX_WAKE, 1, NULL, NULL, 0);
	error = errno;
	atomic_store(&h.stop, true);
	pthread_join(thread, NULL);
	ioctl(h.fd, UFFDIO_UNREGISTER, &reg.range);
	ksft_set_plan(2);
	ksft_test_result_pass("prefaulted shared FUTEX_WAKE control\n");
	ksft_print_msg("pagesize=%zu personality=%#x wake=%d errno=%d events=%u handler=%d\n",
		       h.page_size, personality(0xffffffffUL), result, error,
		       h.events, h.error);
	ksft_test_result(result == 0 && h.events && !h.error,
			 "shared FUTEX_WAKE retries a missing UFFD page\n");
	close(h.fd);
	munmap(word, NATIVE_PAGE_SIZE);
	munmap(h.source, NATIVE_PAGE_SIZE);
	ksft_finished();
}

int main(int argc, char **argv)
{
	return p3s_geometry_main(argc, argv, run_test);
}
