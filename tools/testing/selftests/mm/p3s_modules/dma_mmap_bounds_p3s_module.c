// SPDX-License-Identifier: GPL-2.0
/* Exercise the public DMA mmap APIs, including a page-array DMA backend. */
#include <linux/dma-map-ops.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/uaccess.h>

#include "../dma_mmap_bounds_p3s.h"
#include "../../p3s/p3s_misc_module.h"

struct bounds_buffer {
	struct dma_bounds_request request;
	struct page *pages[DIV_ROUND_UP(DMA_BOUNDS_MAX_SIZE, PAGE_SIZE)];
	void *cpu_addr;
	dma_addr_t dma_addr;
	struct sg_table *sgt;
};

static struct platform_device *direct_device, *array_device;

/* Model a DMA backend with the normal native-page vm_map_pages() bound. */
static int array_mmap(struct device *dev, struct vm_area_struct *vma,
		      void *cpu_addr, dma_addr_t dma_addr, size_t size,
		      unsigned long attrs)
{
	struct bounds_buffer *buffer = cpu_addr;

	return vm_map_pages(vma, buffer->pages, PAGE_ALIGN(size) >> PAGE_SHIFT);
}

static const struct dma_map_ops array_ops = { .mmap = array_mmap };

static void free_buffer(struct bounds_buffer *buffer)
{
	size_t size = max(buffer->request.size, 1U);
	unsigned int i;

	switch (buffer->request.api) {
	case DMA_BOUNDS_ATTRS:
		if (buffer->cpu_addr)
			dma_free_attrs(&direct_device->dev, size, buffer->cpu_addr,
				       buffer->dma_addr, 0);
		break;
	case DMA_BOUNDS_PAGES:
		if (buffer->pages[0])
			__free_pages(buffer->pages[0], get_order(size));
		break;
	case DMA_BOUNDS_PAGE_ARRAY:
		for (i = 0; i < ARRAY_SIZE(buffer->pages); i++)
			if (buffer->pages[i])
				__free_page(buffer->pages[i]);
		break;
#ifdef P3S_DMA_NONCONTIG
	case DMA_BOUNDS_NONCONTIG:
		if (buffer->cpu_addr)
			dma_vunmap_noncontiguous(&direct_device->dev, buffer->cpu_addr);
		if (buffer->sgt)
			dma_free_noncontiguous(&direct_device->dev, size, buffer->sgt,
					       DMA_BIDIRECTIONAL);
		break;
#endif
	}
	kfree(buffer);
}

static long bounds_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct dma_bounds_request request;
	struct bounds_buffer *buffer;
	size_t size, offset;
	int error = -ENOMEM;

	if (cmd != DMA_BOUNDS_SETUP)
		return -ENOTTY;
	/* File references held by VMAs keep each allocation alive until unmap. */
	if (file->private_data)
		return -EBUSY;
	if (copy_from_user(&request, (void __user *)arg, sizeof(request)))
		return -EFAULT;
	if (request.api >= DMA_BOUNDS_API_COUNT || request.size > DMA_BOUNDS_MAX_SIZE)
		return -EINVAL;
#ifndef P3S_DMA_NONCONTIG
	if (request.api == DMA_BOUNDS_NONCONTIG)
		return -EOPNOTSUPP;
#endif
	buffer = kzalloc(sizeof(*buffer), GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;
	buffer->request = request;
	size = max(request.size, 1U);
	switch (request.api) {
	case DMA_BOUNDS_ATTRS:
		buffer->cpu_addr = dma_alloc_attrs(&direct_device->dev, size,
						   &buffer->dma_addr, GFP_KERNEL, 0);
		if (!buffer->cpu_addr)
			goto fail;
		break;
	case DMA_BOUNDS_PAGES:
		buffer->pages[0] = alloc_pages(GFP_KERNEL | __GFP_ZERO, get_order(size));
		if (!buffer->pages[0])
			goto fail;
		buffer->cpu_addr = page_address(buffer->pages[0]);
		break;
	case DMA_BOUNDS_PAGE_ARRAY:
		for (offset = 0; offset < PAGE_ALIGN(size) / PAGE_SIZE; offset++) {
			buffer->pages[offset] = alloc_page(GFP_KERNEL | __GFP_ZERO);
			if (!buffer->pages[offset])
				goto fail;
		}
		break;
#ifdef P3S_DMA_NONCONTIG
	case DMA_BOUNDS_NONCONTIG:
		buffer->sgt = dma_alloc_noncontiguous(&direct_device->dev, size,
						     DMA_BIDIRECTIONAL, GFP_KERNEL, 0);
		if (!buffer->sgt)
			goto fail;
		buffer->cpu_addr = dma_vmap_noncontiguous(&direct_device->dev,
							 size, buffer->sgt);
		if (!buffer->cpu_addr)
			goto fail;
		break;
#endif
	}
	/* Initialize native padding too, so the native control can read it. */
	for (offset = 0; offset < PAGE_ALIGN(size); offset += 4096) {
		void *address = request.api == DMA_BOUNDS_PAGE_ARRAY ?
			page_address(buffer->pages[offset / PAGE_SIZE]) + offset % PAGE_SIZE :
			buffer->cpu_addr + offset;

		memset(address, 0x40 + offset / 4096, 4096);
	}
	file->private_data = buffer;
	return 0;
fail:
	free_buffer(buffer);
	return error;
}

static int bounds_open(struct inode *inode, struct file *file)
{
	file->private_data = NULL;
	return 0;
}

static int bounds_release(struct inode *inode, struct file *file)
{
	if (file->private_data)
		free_buffer(file->private_data);
	return 0;
}

static int bounds_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct bounds_buffer *buffer = file->private_data;
	size_t size;

	if (!buffer)
		return -EINVAL;
	size = buffer->request.size;
	switch (buffer->request.api) {
	case DMA_BOUNDS_ATTRS:
		return dma_mmap_attrs(&direct_device->dev, vma, buffer->cpu_addr,
				      buffer->dma_addr, size, 0);
	case DMA_BOUNDS_PAGES:
		return dma_mmap_pages(&direct_device->dev, vma, size, buffer->pages[0]);
	case DMA_BOUNDS_PAGE_ARRAY:
		return dma_mmap_attrs(&array_device->dev, vma, buffer, 0, size, 0);
#ifdef P3S_DMA_NONCONTIG
	case DMA_BOUNDS_NONCONTIG:
		return dma_mmap_noncontiguous(&direct_device->dev, vma, size, buffer->sgt);
#endif
	default:
		return -EINVAL;
	}
}

static const struct file_operations bounds_fops = {
	.owner = THIS_MODULE,
	.open = bounds_open,
	.release = bounds_release,
	.unlocked_ioctl = bounds_ioctl,
	.mmap = bounds_mmap,
};

static int bounds_setup(void)
{
	int error;

	direct_device = platform_device_register_simple("dma-bounds-direct",
							PLATFORM_DEVID_NONE, NULL, 0);
	if (IS_ERR(direct_device))
		return PTR_ERR(direct_device);
	direct_device->dev.dma_mask = &direct_device->dev.coherent_dma_mask;
	direct_device->dev.dma_coherent = true;
	error = dma_set_mask_and_coherent(&direct_device->dev, DMA_BIT_MASK(64));
	if (error)
		goto free_direct;
	array_device = platform_device_register_simple("dma-bounds-array",
						       PLATFORM_DEVID_NONE, NULL, 0);
	if (IS_ERR(array_device)) {
		error = PTR_ERR(array_device);
		goto free_direct;
	}
	set_dma_ops(&array_device->dev, &array_ops);
	return 0;
free_direct:
	platform_device_unregister(direct_device);
	return error;
}

static void bounds_teardown(void)
{
	platform_device_unregister(array_device);
	platform_device_unregister(direct_device);
}

P3S_MISC_MODULE("dma_mmap_bounds_p3s", &bounds_fops, 0, bounds_setup,
		 bounds_teardown, "P3S DMA mmap logical-size bounds fixture");
