// SPDX-License-Identifier: GPL-2.0

#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/pagemap.h>
#include <linux/uaccess.h>

#include "../fault_in_p3s.h"

#include "../../p3s/p3s_misc_module.h"

static long fault_in_p3s_ioctl(struct file *file, unsigned int cmd,
				unsigned long arg)
{
	struct fault_in_p3s_args request;
	void __user *address;

	if (cmd != FAULT_IN_P3S_IOCTL)
		return -ENOTTY;
	if (copy_from_user(&request, (void __user *)arg, sizeof(request)))
		return -EFAULT;
	if (!request.length || request.reserved)
		return -EINVAL;

	address = u64_to_user_ptr(request.address);
	switch (request.operation) {
	case FAULT_IN_P3S_WRITEABLE:
		request.not_faulted = fault_in_writeable(address, request.length);
		break;
	case FAULT_IN_P3S_SAFE_WRITEABLE:
		request.not_faulted = fault_in_safe_writeable(address, request.length);
		break;
	case FAULT_IN_P3S_READABLE:
		request.not_faulted = fault_in_readable(address, request.length);
		break;
	default:
		return -EINVAL;
	}

	if (copy_to_user((void __user *)arg, &request, sizeof(request)))
		return -EFAULT;
	return 0;
}

static const struct file_operations fault_in_p3s_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = fault_in_p3s_ioctl,
	.compat_ioctl = fault_in_p3s_ioctl,
};

P3S_MISC_MODULE(FAULT_IN_P3S_DEVICE_NAME, &fault_in_p3s_fops, 0600,
		 p3s_misc_no_setup, p3s_misc_no_teardown,
		 "P3S fault-in helper regression test");
