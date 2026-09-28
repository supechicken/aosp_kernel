// SPDX-License-Identifier: GPL-2.0

#include <linux/fs.h>
#include <linux/file.h>
#include <linux/highmem.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/pagemap.h>
#include <linux/shmem_fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/uio.h>

#include "../iov_iter_p3s.h"
#include "../page_range_p3s.h"

#include "../../p3s/p3s_misc_module.h"

#define PROCESS_PAGE_SIZE 4096UL

static const u8 expected[] = { 0x31, 0x72, 0x93, 0xb4 };

struct page_range_backing {
	struct page *pages[4];
};

static int page_range_open(struct inode *inode, struct file *file)
{
	struct page_range_backing *backing;
	unsigned int i, j;

	backing = kzalloc(sizeof(*backing), GFP_KERNEL);
	if (!backing)
		return -ENOMEM;
	for (i = 0; i < ARRAY_SIZE(backing->pages); i++) {
		void *base;

		backing->pages[i] = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (!backing->pages[i])
			goto fail;
		base = kmap_local_page(backing->pages[i]);
		if (mm_is_p3s_4k(current->mm)) {
			for (j = 0; j < ARRAY_SIZE(expected); j++)
				memset(base + j * PROCESS_PAGE_SIZE, expected[j],
				       PROCESS_PAGE_SIZE);
		} else {
			memset(base, expected[i], PAGE_SIZE);
		}
		kunmap_local(base);
	}
	file->private_data = backing;
	return 0;
fail:
	while (i)
		put_page(backing->pages[--i]);
	kfree(backing);
	return -ENOMEM;
}

static int page_range_release(struct inode *inode, struct file *file)
{
	struct page_range_backing *backing = file->private_data;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(backing->pages); i++)
		put_page(backing->pages[i]);
	kfree(backing);
	return 0;
}

/* The caller selects slices with normal mmap offsets, not a new export. */
static int page_range_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct page_range_backing *backing = file->private_data;

	return vm_map_pages(vma, backing->pages, ARRAY_SIZE(backing->pages));
}

static int check_page_value(struct page *page, size_t offset, u8 value)
{
	void *base;
	int ret;

	if (offset >= PAGE_SIZE)
		return -ERANGE;
	base = kmap_local_page(page);
	ret = *((u8 *)base + offset) == value ? 0 : -ENODATA;
	kunmap_local(base);
	return ret;
}

static int check_page(struct page *page, size_t offset, u8 value,
		      bool expect_packed, struct page **first_page)
{
	if (expect_packed) {
		if (*first_page && page != *first_page)
			return -ENXIO;
		*first_page = page;
	}
	return check_page_value(page, offset, value);
}

static int run_get_pages(unsigned long address, size_t length,
			 bool expect_packed)
{
	struct page *first_page = NULL;
	struct iov_iter iter;
	unsigned int nr_slices = length / PROCESS_PAGE_SIZE;
	unsigned int i;

	iov_iter_ubuf(&iter, ITER_SOURCE, (void __user *)address, length);
	for (i = 0; i < nr_slices; i++) {
		struct page *page = NULL;
		size_t offset = 0;
		ssize_t ret;
		int err;

		ret = iov_iter_get_pages2(&iter, &page, PROCESS_PAGE_SIZE, 1,
					  &offset);
		if (ret != PROCESS_PAGE_SIZE)
			return ret < 0 ? ret : -EMSGSIZE;
		err = check_page(page, offset, expected[i], expect_packed,
				 &first_page);
		put_page(page);
		if (err)
			return err;
	}
	return iov_iter_count(&iter) ? -EMSGSIZE : 0;
}

static int run_extract_pages(unsigned long address, size_t length,
			     bool expect_packed)
{
	struct page *first_page = NULL;
	struct iov_iter iter;
	unsigned int nr_slices = length / PROCESS_PAGE_SIZE;
	unsigned int i;

	iov_iter_ubuf(&iter, ITER_SOURCE, (void __user *)address, length);
	for (i = 0; i < nr_slices; i++) {
		struct page *storage[1];
		struct page **pages = storage;
		size_t offset = 0;
		ssize_t ret;
		int err;

		ret = iov_iter_extract_pages(&iter, &pages,
					     PROCESS_PAGE_SIZE, 1, 0, &offset);
		if (ret != PROCESS_PAGE_SIZE)
			return ret < 0 ? ret : -EMSGSIZE;
		err = check_page(pages[0], offset, expected[i], expect_packed,
				 &first_page);
		unpin_user_page(pages[0]);
		if (err)
			return err;
	}
	return iov_iter_count(&iter) ? -EMSGSIZE : 0;
}

static ssize_t run_bulk_extract(unsigned long address, size_t length)
{
	struct page *storage[2];
	struct page **pages = storage;
	struct iov_iter iter;
	size_t offset = 0;
	ssize_t ret;
	int i;

	iov_iter_ubuf(&iter, ITER_SOURCE, (void __user *)address, length);
	ret = iov_iter_extract_pages(&iter, &pages, length,
				     ARRAY_SIZE(storage), 0, &offset);
	if (ret > 0) {
		int nr_pages = DIV_ROUND_UP(offset + ret, PAGE_SIZE);

		for (i = 0; i < nr_pages; i++)
			unpin_user_page(pages[i]);
	}
	return ret;
}

static int run_npages(unsigned long address, size_t length)
{
	unsigned int expected = length / PROCESS_PAGE_SIZE;
	struct iovec iov[2] = {
		{
			.iov_base = (void __user *)address,
			.iov_len = PROCESS_PAGE_SIZE,
		},
		{
			.iov_base = (void __user *)(address + PROCESS_PAGE_SIZE),
			.iov_len = length - PROCESS_PAGE_SIZE,
		},
	};
	struct iov_iter iter;

	iov_iter_ubuf(&iter, ITER_SOURCE, (void __user *)(address + 1),
		      length - 1);
	if (iov_iter_npages(&iter, INT_MAX) != expected ||
	    iov_iter_npages(&iter, expected - 1) != expected - 1)
		return -ERANGE;

	iov_iter_init(&iter, ITER_SOURCE, iov, ARRAY_SIZE(iov), length);
	iov_iter_advance(&iter, PROCESS_PAGE_SIZE / 2);
	if (iov_iter_npages(&iter, INT_MAX) != expected ||
	    iov_iter_npages(&iter, expected - 1) != expected - 1)
		return -ERANGE;
	return 0;
}

/* Exercise byte offsets through the existing exported iov APIs. */
static long page_range_ioctl(unsigned long arg)
{
	struct page_range_p3s_args request;
	struct iov_iter iter;
	unsigned long size = mm_pte_size(current->mm);
	long nr = 0;
	unsigned int i;

	if (copy_from_user(&request, (void __user *)arg, sizeof(request)))
		return -EFAULT;
	if (request.capacity > PAGE_RANGE_P3S_MAX || request.pin > 1 ||
	    request.address > ULONG_MAX || request.length > SIZE_MAX)
		return -EINVAL;
	memset(request.spans, 0, sizeof(request.spans));
	request.reserved = 0;
	if (request.address + request.length < request.address) {
		nr = -EOVERFLOW;
		goto out;
	}
	/* The overflow check above makes the rounded last-byte count safe. */
	if (request.length &&
	    ((request.address + request.length - 1) / size -
	     request.address / size + 1 > request.capacity)) {
		nr = -ENOSPC;
		goto out;
	}
	iov_iter_ubuf(&iter, ITER_DEST, (void __user *)request.address,
		      request.length);
	for (i = 0; i < request.capacity && iov_iter_count(&iter); i++) {
		struct page *page, **pages = &page;
		unsigned long address = request.address + request.length -
					iov_iter_count(&iter);
		size_t length = min_t(size_t, iov_iter_count(&iter),
				      size - mm_offset_in_page(current->mm, address));
		size_t offset = 0;
		ssize_t bytes;
		u8 *base;

		if (request.pin)
			bytes = iov_iter_extract_pages(&iter, &pages, length,
						       1, 0, &offset);
		else
			bytes = iov_iter_get_pages2(&iter, &page, length,
						    1, &offset);
		if (bytes <= 0) {
			if (!nr)
				nr = bytes;
			break;
		}
		base = kmap_local_page(page);
		request.spans[i].offset = offset;
		request.spans[i].length = bytes;
		request.spans[i].first = base[offset];
		request.spans[i].last = base[offset + bytes - 1];
		kunmap_local(base);
		if (request.pin)
			unpin_user_page(page);
		else
			put_page(page);
		nr++;
	}
out:
	request.result = nr;
	return copy_to_user((void __user *)arg, &request, sizeof(request)) ?
		-EFAULT : 0;
}

/* Observe only the test's small shmem backing, with no persistent reference. */
static long file_refs_ioctl(unsigned long arg)
{
	struct file_refs_p3s_args request;
	struct folio *folio;
	struct file *file;
	long ret = 0;

	if (copy_from_user(&request, (void __user *)arg, sizeof(request)))
		return -EFAULT;
	file = fget(request.fd);
	if (!file)
		return -EBADF;
	if (!shmem_file(file) || i_size_read(file_inode(file)) != PAGE_SIZE) {
		ret = -EINVAL;
		goto out_file;
	}
	folio = filemap_get_folio(file->f_mapping, 0);
	if (IS_ERR(folio)) {
		ret = PTR_ERR(folio);
		goto out_file;
	}
	request.refs = folio_ref_count(folio);
	folio_put(folio);
	if (copy_to_user((void __user *)arg, &request, sizeof(request)))
		ret = -EFAULT;
out_file:
	fput(file);
	return ret;
}

static long iov_iter_p3s_ioctl(struct file *file, unsigned int cmd,
				unsigned long arg)
{
	struct iov_iter_p3s_args request;
	bool expect_packed;

	if (cmd == PAGE_RANGE_P3S_IOCTL)
		return page_range_ioctl(arg);
	if (cmd == FILE_REFS_P3S_IOCTL)
		return file_refs_ioctl(arg);
	if (cmd != IOV_ITER_P3S_IOCTL)
		return -EINVAL;
	if (copy_from_user(&request, (void __user *)arg, sizeof(request)))
		return -EFAULT;
	if (!request.length || request.length % PROCESS_PAGE_SIZE ||
	    request.length > ARRAY_SIZE(expected) * PROCESS_PAGE_SIZE ||
	    request.flags & ~IOV_ITER_P3S_F_EXPECT_PACKED)
		return -EINVAL;
	expect_packed = request.flags & IOV_ITER_P3S_F_EXPECT_PACKED;

	request.native_page_size = PAGE_SIZE;
	request.get_pages_result = run_get_pages(request.address,
						 request.length, expect_packed);
	request.extract_pages_result = run_extract_pages(request.address,
							 request.length,
							 expect_packed);
	request.bulk_first_len = run_bulk_extract(request.address,
						  request.length);
	request.npages_result = run_npages(request.address, request.length);
	if (copy_to_user((void __user *)arg, &request, sizeof(request)))
		return -EFAULT;
	return 0;
}

static const struct file_operations iov_iter_p3s_fops = {
	.owner = THIS_MODULE,
	.open = page_range_open,
	.release = page_range_release,
	.mmap = page_range_mmap,
	.unlocked_ioctl = iov_iter_p3s_ioctl,
	.compat_ioctl = iov_iter_p3s_ioctl,
};

P3S_MISC_MODULE(IOV_ITER_P3S_DEVICE_NAME, &iov_iter_p3s_fops, 0,
		 p3s_misc_no_setup, p3s_misc_no_teardown,
		 "P3S user-backed iov_iter page extraction regression helper");
