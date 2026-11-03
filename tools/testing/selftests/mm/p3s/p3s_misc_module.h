/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Boilerplate shared by the P3S selftest fixture modules.
 *
 * Nearly every fixture is a dynamic-minor misc device whose file_operations
 * exercise one kernel mmap/GUP/iov path under a 4K compat process.  The
 * module init/exit, the miscdevice and the MODULE_* tags are identical
 * across them, so a fixture is:
 *
 *	static const struct file_operations foo_fops = { ... };
 *	P3S_MISC_MODULE("foo_p3s", &foo_fops, 0600,
 *			 p3s_misc_no_setup, p3s_misc_no_teardown,
 *			 "what this fixture exercises");
 *
 * @setup runs before the device is registered and returns 0 or -errno;
 * @teardown undoes it, both on a failed registration and at module exit.
 * P3S_MISC_MODULE_POST() additionally takes a hook that runs after a
 * successful misc_register() with the registered miscdevice, for fixtures
 * that need the device it creates (its DMA mask, for instance).
 */
#ifndef __P3S_MISC_MODULE_H
#define __P3S_MISC_MODULE_H

#include <linux/miscdevice.h>
#include <linux/module.h>

static inline int p3s_misc_no_setup(void)
{
	return 0;
}

static inline void p3s_misc_no_teardown(void)
{
}

static inline int p3s_misc_no_post(struct miscdevice *device)
{
	return 0;
}

#define P3S_MISC_MODULE_POST(devname, fops_ptr, devmode, setup_fn,	\
			      post_fn, teardown_fn, desc)		\
static struct miscdevice p3s_misc_device = {				\
	.minor = MISC_DYNAMIC_MINOR,					\
	.name = devname,						\
	.fops = fops_ptr,						\
	.mode = devmode,						\
};									\
									\
static int __init p3s_misc_module_init(void)				\
{									\
	int error = setup_fn();						\
									\
	if (error)							\
		return error;						\
	error = misc_register(&p3s_misc_device);			\
	if (!error) {							\
		error = post_fn(&p3s_misc_device);			\
		if (error)						\
			misc_deregister(&p3s_misc_device);		\
	}								\
	if (error)							\
		teardown_fn();						\
	return error;							\
}									\
									\
static void __exit p3s_misc_module_exit(void)				\
{									\
	misc_deregister(&p3s_misc_device);				\
	teardown_fn();							\
}									\
									\
module_init(p3s_misc_module_init);					\
module_exit(p3s_misc_module_exit);					\
MODULE_LICENSE("GPL");							\
MODULE_DESCRIPTION(desc)

#define P3S_MISC_MODULE(devname, fops_ptr, devmode, setup_fn, teardown_fn, \
			 desc)						\
	P3S_MISC_MODULE_POST(devname, fops_ptr, devmode, setup_fn,	\
			      p3s_misc_no_post, teardown_fn, desc)

#endif /* __P3S_MISC_MODULE_H */
