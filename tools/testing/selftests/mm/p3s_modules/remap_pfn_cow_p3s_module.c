// SPDX-License-Identifier: GPL-2.0

#include <linux/mm.h>
#include <linux/module.h>
#include <linux/uaccess.h>

#include "../remap_pfn_cow_p3s.h"
#include "../../p3s/p3s_misc_module.h"

#define PROCESS_PAGE_SIZE	4096UL
#define TEST_SLICE		2U
#define TEST_MARKER		0x93

static void *test_buffer;

static int
remap_pfn_cow_p3s_mmap(struct file *file, struct vm_area_struct *vma)
{
	unsigned long size = vma->vm_end - vma->vm_start;
	u64 offset = vma_file_offset(vma);

	if (offset >= PAGE_SIZE || size > PAGE_SIZE - offset)
		return -EINVAL;
	return remap_pfn_range(vma, vma->vm_start,
			       page_to_pfn(virt_to_page(test_buffer)), size,
			       vma->vm_page_prot);
}

static long remap_pfn_cow_p3s_ioctl(struct file *file, unsigned int cmd,
				     unsigned long arg)
{
	struct remap_pfn_cow_p3s_probe probe;
	struct vm_area_struct *vma;
	phys_addr_t expected;

	if (cmd != REMAP_PFN_COW_P3S_IOCTL)
		return -ENOTTY;
	if (copy_from_user(&probe, (void __user *)arg, sizeof(probe)))
		return -EFAULT;

	mmap_read_lock(current->mm);
	vma = vma_lookup(current->mm, probe.address);
	if (!vma || vma->vm_file != file) {
		mmap_read_unlock(current->mm);
		return -ENOENT;
	}
	expected = virt_to_phys(test_buffer) +
		((phys_addr_t)(mm_is_p3s_4k(current->mm) ? TEST_SLICE : 0) <<
		 PAGE_SHIFT_4KB);
	probe.slice = vma_slice_off(vma);
	probe.offset_matches = vma_file_offset(vma) == expected;
	mmap_read_unlock(current->mm);

	if (copy_to_user((void __user *)arg, &probe, sizeof(probe)))
		return -EFAULT;
	return 0;
}

static const struct file_operations remap_pfn_cow_p3s_fops = {
	.owner = THIS_MODULE,
	.mmap = remap_pfn_cow_p3s_mmap,
	.unlocked_ioctl = remap_pfn_cow_p3s_ioctl,
	.compat_ioctl = remap_pfn_cow_p3s_ioctl,
};

static int remap_pfn_cow_p3s_setup(void)
{
	unsigned int slice;

	test_buffer = alloc_pages_exact(PAGE_SIZE, GFP_KERNEL | __GFP_ZERO);
	if (!test_buffer)
		return -ENOMEM;
	for (slice = 0; slice < PAGE_SIZE / PROCESS_PAGE_SIZE; slice++)
		memset(test_buffer + slice * PROCESS_PAGE_SIZE,
		       TEST_MARKER - TEST_SLICE + slice, PROCESS_PAGE_SIZE);
	return 0;
}

static void remap_pfn_cow_p3s_teardown(void)
{
	free_pages_exact(test_buffer, PAGE_SIZE);
}

P3S_MISC_MODULE("remap_pfn_cow_p3s", &remap_pfn_cow_p3s_fops, 0,
		 remap_pfn_cow_p3s_setup, remap_pfn_cow_p3s_teardown,
		 "P3S private PFN remap offset regression test helper");
