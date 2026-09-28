// SPDX-License-Identifier: GPL-2.0
/* Existing iov APIs must preserve bytes, runs and UFFD retries in both modes. */
#define _GNU_SOURCE
#include <linux/userfaultfd.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#include "../kselftest_p3s.h"
#include "iov_range_retry_p3s.h"

#define BULK_SIZE (1024 * 1024)

struct fault_handler {
	int fd;
	void *source;
	size_t page_size;
	atomic_bool stop;
	unsigned int events;
	int error;
};

static uint32_t checksum(const unsigned char *data, size_t length)
{
	uint32_t value = 2166136261U;
	size_t i;

	for (i = 0; i < length; i++) {
		value ^= data[i];
		value *= 16777619U;
	}
	return value;
}

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

static void check_range(int fd, void *address, size_t length, bool pin,
			const char *description)
{
	struct iov_range_retry_p3s_args r = {
		.address = (uintptr_t)address, .length = length, .pin = pin,
	};
	uint32_t expected = checksum(address, length);
	int ret = ioctl(fd, IOV_RANGE_RETRY_P3S_IOCTL, &r);

	ksft_test_result(!ret && r.result == (int64_t)length &&
			 r.checksum == expected,
			 "%s %s: bytes=%lld/%zu offset=%llu checksum=%#x/%#x\n",
			 pin ? "pin" : "get", description,
			 (long long)r.result, length, (unsigned long long)r.offset,
			 r.checksum, expected);
}

static void check_uffd(int fd, void *destination, void *source, bool pin)
{
	struct uffdio_api api = { .api = UFFD_API };
	struct uffdio_register reg = { .mode = UFFDIO_REGISTER_MODE_MISSING };
	struct fault_handler h = { .source = source, .page_size = getpagesize() };
	struct iov_range_retry_p3s_args r = {
		.address = (uintptr_t)destination, .length = h.page_size, .pin = pin,
	};
	pthread_t thread;
	int ret;

	h.fd = syscall(SYS_userfaultfd, O_NONBLOCK | O_CLOEXEC);
	if (h.fd < 0 || ioctl(h.fd, UFFDIO_API, &api)) {
		if (h.fd >= 0)
			close(h.fd);
		ksft_test_result_skip("kernel-fault userfaultfd unavailable\n");
		return;
	}
	if (madvise(destination, NATIVE_PAGE_SIZE, MADV_DONTNEED))
		ksft_exit_fail_msg("MADV_DONTNEED: %s\n", strerror(errno));
	reg.range.start = (uintptr_t)destination;
	reg.range.len = NATIVE_PAGE_SIZE;
	if (ioctl(h.fd, UFFDIO_REGISTER, &reg))
		ksft_exit_fail_msg("UFFDIO_REGISTER: %s\n", strerror(errno));
	if (pthread_create(&thread, NULL, resolve_fault, &h))
		ksft_exit_fail_msg("pthread_create\n");
	ret = ioctl(fd, IOV_RANGE_RETRY_P3S_IOCTL, &r);
	atomic_store(&h.stop, true);
	pthread_join(thread, NULL);
	ioctl(h.fd, UFFDIO_UNREGISTER, &reg.range);
	ksft_test_result(!ret && r.result == (int64_t)h.page_size &&
			 h.events && !h.error &&
			 r.checksum == checksum(source, h.page_size),
			 "%s UFFD retry: bytes=%lld events=%u handler=%d\n",
			 pin ? "pin" : "get", (long long)r.result,
			 h.events, h.error);
	close(h.fd);
}

static int run_test(void)
{
	unsigned char *allocation, *bulk, *destination;
	unsigned int pin;
	size_t i;
	int fd = open("/dev/iov_range_retry_p3s", O_RDWR);

	ksft_print_header();
	if (fd < 0)
		ksft_exit_skip("iov_range_retry_p3s fixture unavailable\n");
	allocation = mmap(NULL, BULK_SIZE + NATIVE_PAGE_SIZE,
			  PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	destination = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (allocation == MAP_FAILED || destination == MAP_FAILED)
		ksft_exit_fail_msg("mmap: %s\n", strerror(errno));
	bulk = (void *)(((uintptr_t)allocation + NATIVE_PAGE_SIZE - 1) &
		       ~(NATIVE_PAGE_SIZE - 1));
	for (i = 0; i < BULK_SIZE; i++)
		bulk[i] = (i * 31 + (i >> 12) * 17 + (i >> 14)) & 0xff;
	ksft_set_plan(8);
	for (pin = 0; pin < 2; pin++) {
		check_range(fd, bulk, BULK_SIZE, pin, "1MiB in one extraction");
		check_range(fd, bulk + 17, 2 * PROCESS_PAGE_SIZE + 46,
			    pin, "nonaligned multi-page range");
		check_range(fd, bulk + PROCESS_PAGE_SIZE - 1,
			    3 * PROCESS_PAGE_SIZE + 17, pin, "slice boundary range");
		check_uffd(fd, destination, bulk, pin);
	}
	close(fd);
	munmap(allocation, BULK_SIZE + NATIVE_PAGE_SIZE);
	munmap(destination, NATIVE_PAGE_SIZE);
	ksft_finished();
}

int main(int argc, char **argv)
{
	return p3s_geometry_main(argc, argv, run_test);
}
