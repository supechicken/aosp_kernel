/* SPDX-License-Identifier: GPL-2.0 */
#ifndef PAGE_RANGE_P3S_H
#define PAGE_RANGE_P3S_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define PAGE_RANGE_P3S_MAX 4
struct page_range_p3s_args {
	__u64 address;
	__u64 length;
	__u32 capacity;
	__u32 pin;
	__s32 result;
	__u32 reserved;
	struct {
		__u32 offset;
		__u32 length;
		__u32 first;
		__u32 last;
	} spans[PAGE_RANGE_P3S_MAX];
};

#define PAGE_RANGE_P3S_IOCTL _IOWR('i', 0x72, struct page_range_p3s_args)

struct file_refs_p3s_args {
	__s32 fd;
	__s32 refs;
};

#define FILE_REFS_P3S_IOCTL _IOWR('i', 0x73, struct file_refs_p3s_args)
#ifndef __KERNEL__
#include <sys/mman.h>
#include <unistd.h>

/* Map one owned page in deliberately non-linear slice order. */
static inline unsigned char *page_range_map(int fd)
{
	static const unsigned int slices[] = { 3, 1, 0, 2 };
	size_t size = sysconf(_SC_PAGESIZE);
	unsigned char *area;
	unsigned int i;

	area = mmap(NULL, 4 * size, PROT_NONE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (area == MAP_FAILED)
		return MAP_FAILED;
	for (i = 0; i < 4; i++) {
		off_t offset = (size == 4096 ? slices[i] : i) * size;

		if (mmap(area + i * size, size, PROT_READ | PROT_WRITE,
			 MAP_SHARED | MAP_FIXED, fd, offset) == MAP_FAILED) {
			munmap(area, 4 * size);
			return MAP_FAILED;
		}
	}
	return area;
}
#endif

#endif
