// SPDX-License-Identifier: GPL-2.0

#include <linux/bitmap.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/uaccess.h>

#include "../pagewalk_p3s.h"

#include "../../p3s/p3s_misc_module.h"

static long pagewalk_p3s_ioctl(struct file *unused, unsigned int command,
				unsigned long argument)
{
	struct pagewalk_p3s_args request;
	unsigned long *bitmap = NULL;
	struct file *target;
	pgoff_t first, last, nr;
	u64 end;
	long error = 0;

	if (copy_from_user(&request, (void __user *)argument, sizeof(request)))
		return -EFAULT;
	if (!request.length ||
	    check_add_overflow(request.offset, request.length, &end))
		return -EINVAL;

	first = request.offset >> PAGE_SHIFT;
	last = DIV_ROUND_UP_ULL(end, PAGE_SIZE);
	nr = last - first;
	if (!nr)
		return -EINVAL;

	target = fget(request.fd);
	if (!target)
		return -EBADF;

	request.count = 0;
	request.bitmap_weight = 0;
	switch (command) {
	case PAGEWALK_P3S_IOCTL_WRITE_PROTECT:
		request.count = wp_shared_mapping_range(target->f_mapping,
							first, nr);
		break;
	case PAGEWALK_P3S_IOCTL_CLEAN: {
		pgoff_t start = nr;
		pgoff_t finish = 0;

		bitmap = bitmap_zalloc(nr, GFP_KERNEL);
		if (!bitmap) {
			error = -ENOMEM;
			break;
		}
		request.count = clean_record_shared_mapping_range(target->f_mapping,
								  first, nr, first, bitmap,
								  &start, &finish);
		request.bitmap_weight = bitmap_weight(bitmap, nr);
		break;
	}
	default:
		error = -ENOTTY;
	}

	bitmap_free(bitmap);
	fput(target);
	if (error)
		return error;
	if (copy_to_user((void __user *)argument, &request, sizeof(request)))
		return -EFAULT;
	return 0;
}

static const struct file_operations pagewalk_p3s_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = pagewalk_p3s_ioctl,
};

P3S_MISC_MODULE(PAGEWALK_P3S_DEVICE_NAME, &pagewalk_p3s_fops, 0,
		 p3s_misc_no_setup, p3s_misc_no_teardown,
		 "P3S address-space page-walk regression test helper");
