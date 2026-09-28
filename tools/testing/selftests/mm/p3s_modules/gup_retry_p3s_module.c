// SPDX-License-Identifier: GPL-2.0

#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/p3s.h>
#include <linux/module.h>
#include <linux/p3s.h>
#include <linux/uaccess.h>

#include "../gup_retry_p3s.h"

#include "../../p3s/p3s_misc_module.h"

static int mmap_cleanup_fail(struct file *file, struct vm_area_struct *vma)
{
	return -EIO;
}

static long gup_retry_p3s_ioctl(struct file *file, unsigned int cmd,
				 unsigned long arg)
{
	struct gup_retry_p3s_args request;
	struct page *expected = NULL;
	struct page *pages[2];
	long expected_count = 0;
	long count;

	if (cmd != GUP_RETRY_P3S_IOCTL)
		return -EINVAL;
	if (copy_from_user(&request, (void __user *)arg, sizeof(request)))
		return -EFAULT;

	count = pin_user_pages_unlocked(request.address, ARRAY_SIZE(pages),
					pages, 0);
	request.nr_pinned = count;
	request.second_matches_expected = 0;
	request.reserved = 0;
	if (count == ARRAY_SIZE(pages)) {
		expected_count = pin_user_pages_unlocked(request.address +
				mm_pte_size(current->mm), 1, &expected, 0);
		if (expected_count == 1)
			request.second_matches_expected = pages[1] == expected;
	}

	if (expected_count == 1)
		unpin_user_page(expected);
	if (count > 0)
		unpin_user_pages(pages, count);
	if (copy_to_user((void __user *)arg, &request, sizeof(request)))
		return -EFAULT;
	return 0;
}

static const struct file_operations gup_retry_p3s_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = gup_retry_p3s_ioctl,
	.compat_ioctl = gup_retry_p3s_ioctl,
	.mmap = mmap_cleanup_fail,
};

P3S_MISC_MODULE(GUP_RETRY_P3S_DEVICE_NAME, &gup_retry_p3s_fops, 0,
		 p3s_misc_no_setup, p3s_misc_no_teardown,
		 "P3S GUP retry regression test helper");
