// SPDX-License-Identifier: GPL-2.0

#include <linux/mm.h>
#include <linux/module.h>
#include <linux/uaccess.h>

#include "../pfn_insert_p3s.h"
#include "../p3s_misc_module.h"

#define USER_PAGE_SIZE	4096UL
#define FIRST_MARKER	0x51
#define WRONG_MARKER	0x61

static void *test_buffer;

static vm_fault_t pfn_insert_p3s_fault(struct vm_fault *vmf)
{
	return vmf_insert_pfn_prot(vmf->vma, vmf->address,
				   virt_to_phys(test_buffer) >> PAGE_SHIFT,
				   vmf->vma->vm_page_prot);
}

static const struct vm_operations_struct pfn_insert_p3s_vm_ops = {
	.fault = pfn_insert_p3s_fault,
};

static int pfn_insert_p3s_mmap(struct file *file, struct vm_area_struct *vma)
{
	u64 offset = vma_file_offset(vma);
	unsigned long pfn;

	if (vma->vm_end - vma->vm_start > PAGE_SIZE ||
	    offset > 2 * PAGE_SIZE ||
	    (offset >= PAGE_SIZE && vma_slice_off(vma)))
		return -EINVAL;
	vm_flags_set(vma, VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);
	vma->vm_ops = &pfn_insert_p3s_vm_ops;
	if (!offset)
		return 0;

	pfn = virt_to_phys(test_buffer) >> PAGE_SHIFT;
	if (offset == PAGE_SIZE)
		return remap_pfn_range(vma, vma->vm_start, pfn,
				       USER_PAGE_SIZE, vma->vm_page_prot);
	return remap_pfn_range(vma, vma->vm_start, pfn + 1,
			       PAGE_SIZE, vma->vm_page_prot);
}

static long pfn_insert_p3s_ioctl(struct file *file, unsigned int cmd,
				  unsigned long arg)
{
	struct pfn_insert_p3s_request request;
	struct vm_area_struct *vma;
	vm_fault_t ret;
	unsigned int i;

	if (cmd != PFN_INSERT_P3S_IOCTL && cmd != PFN_INSERT_P3S_INSPECT)
		return -ENOTTY;
	if (copy_from_user(&request, (void __user *)arg, sizeof(request)))
		return -EFAULT;

	mmap_read_lock(current->mm);
	vma = vma_lookup(current->mm, request.address);
	if (!vma || vma->vm_file != file) {
		mmap_read_unlock(current->mm);
		return -ENOENT;
	}
	if (cmd == PFN_INSERT_P3S_INSPECT) {
		request.present_mask = 0;
		for (i = 0; i < min_t(unsigned long,
					PAGE_SIZE / USER_PAGE_SIZE,
					(vma->vm_end - request.address) /
					USER_PAGE_SIZE); i++) {
			struct follow_pfnmap_args args = {
				.vma = vma,
				.address = request.address + i * USER_PAGE_SIZE,
			};

			if (!follow_pfnmap_start(&args)) {
				request.present_mask |= 1U << i;
				follow_pfnmap_end(&args);
			}
		}
		mmap_read_unlock(current->mm);
		return copy_to_user((void __user *)arg, &request,
				    sizeof(request)) ? -EFAULT : 0;
	}
	ret = vmf_insert_pfn_prot(vma, request.address,
				  virt_to_phys(test_buffer) >> PAGE_SHIFT,
				  vma->vm_page_prot);
	mmap_read_unlock(current->mm);
	if (ret & VM_FAULT_OOM)
		return -ENOMEM;
	if (ret & VM_FAULT_ERROR)
		return -EBUSY;
	return 0;
}

static const struct file_operations pfn_insert_p3s_fops = {
	.owner = THIS_MODULE,
	.mmap = pfn_insert_p3s_mmap,
	.unlocked_ioctl = pfn_insert_p3s_ioctl,
	.compat_ioctl = pfn_insert_p3s_ioctl,
};

static int pfn_insert_p3s_setup(void)
{
	unsigned int i;

	test_buffer = alloc_pages_exact(2 * PAGE_SIZE, GFP_KERNEL | __GFP_ZERO);
	if (!test_buffer)
		return -ENOMEM;
	for (i = 0; i < PAGE_SIZE / USER_PAGE_SIZE; i++) {
		memset(test_buffer + i * USER_PAGE_SIZE, FIRST_MARKER + i,
		       USER_PAGE_SIZE);
		memset(test_buffer + PAGE_SIZE + i * USER_PAGE_SIZE,
		       WRONG_MARKER + i, USER_PAGE_SIZE);
	}
	return 0;
}

static void pfn_insert_p3s_teardown(void)
{
	free_pages_exact(test_buffer, 2 * PAGE_SIZE);
}

P3S_MISC_MODULE("pfn_insert_p3s", &pfn_insert_p3s_fops, 0,
		 pfn_insert_p3s_setup, pfn_insert_p3s_teardown,
		 "P3S vmf_insert_pfn_prot whole-page regression fixture");
