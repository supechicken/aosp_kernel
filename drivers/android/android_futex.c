// SPDX-License-Identifier: GPL-2.0
/*
 * /dev/android_futex - ioctl interface to the PING futex operations
 *
 * Provides a sys_futex()-like ioctl that dispatches
 * ANDROID_FUTEX_LOCK_PING, ANDROID_FUTEX_UNLOCK_PING and
 * ANDROID_FUTEX_TRYLOCK_PING into kernel/futex/ping.c.
 */

#include <linux/compat.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/time_namespace.h>
#include <linux/uaccess.h>

#include <uapi/linux/android_futex.h>

/* Internal futex helpers: futex_to_flags(), futex_lock_ping(), ... */
#include "../../kernel/futex/futex.h"

static long android_futex_do_op(struct android_futex_args *args)
{
	u32 __user *uaddr = u64_to_user_ptr(args->uaddr);
	struct __kernel_timespec __user *utime;
	unsigned int flags = futex_to_flags(args->op);
	int cmd = args->op & FUTEX_CMD_MASK;
	ktime_t t, *tp = NULL;
	struct timespec64 ts;

	if (args->val || args->uaddr2 || args->val3 || args->__reserved)
		return -EINVAL;

	/* Timeout and FUTEX_CLOCK_REALTIME only apply to FUTEX_LOCK_PING. */
	if ((args->timeout || (flags & FLAGS_CLOCKRT)) &&
	    cmd != ANDROID_FUTEX_LOCK_PING)
		return -EINVAL;

	switch (cmd) {
	case ANDROID_FUTEX_LOCK_PING:
		utime = u64_to_user_ptr(args->timeout);
		if (utime) {
			if (get_timespec64(&ts, utime))
				return -EFAULT;
			if (!timespec64_valid(&ts))
				return -EINVAL;
			t = timespec64_to_ktime(ts);
			if (!(flags & FLAGS_CLOCKRT))
				t = timens_ktime_to_host(CLOCK_MONOTONIC, t);
			tp = &t;
		}
		return futex_lock_ping(uaddr, flags, tp, 0);
	case ANDROID_FUTEX_UNLOCK_PING:
		return futex_unlock_ping(uaddr, flags);
	case ANDROID_FUTEX_TRYLOCK_PING:
		return futex_lock_ping(uaddr, flags, NULL, 1);
	}

	return -EINVAL;
}

static long android_futex_ioctl(struct file *file, unsigned int cmd,
				unsigned long arg)
{
	struct android_futex_args args;

	switch (cmd) {
	case ANDROID_FUTEX_IOC_FUTEX:
		if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
			return -EFAULT;
		return android_futex_do_op(&args);
	}

	return -ENOTTY;
}

static const struct file_operations android_futex_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl	= android_futex_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
	.llseek		= noop_llseek,
};

static struct miscdevice android_futex_misc = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "android_futex",
	.fops	= &android_futex_fops,
	.mode	= 0666,
};

module_misc_device(android_futex_misc);

MODULE_DESCRIPTION("ioctl interface for PING futex operations");
MODULE_LICENSE("GPL");
