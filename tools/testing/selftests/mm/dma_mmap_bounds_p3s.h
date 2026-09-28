/* SPDX-License-Identifier: GPL-2.0 */
#ifndef DMA_MMAP_BOUNDS_P3S_H
#define DMA_MMAP_BOUNDS_P3S_H

#include <linux/ioctl.h>
#include <linux/types.h>

enum dma_bounds_api {
	DMA_BOUNDS_ATTRS,
	DMA_BOUNDS_PAGES,
	DMA_BOUNDS_PAGE_ARRAY,
	DMA_BOUNDS_NONCONTIG,
	DMA_BOUNDS_API_COUNT,
};

struct dma_bounds_request {
	__u32 api;
	__u32 size;
};

#define DMA_BOUNDS_MAX_SIZE	32768U
#define DMA_BOUNDS_SETUP	_IOW('D', 0, struct dma_bounds_request)

#endif
