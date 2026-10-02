/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * /dev/android_futex - ioctl interface to the PING futex operations
 *
 * The arguments mirror sys_futex():
 *
 *   long futex(u32 *uaddr, int op, u32 val,
 *              const struct timespec *timeout, u32 *uaddr2, u32 val3);
 *
 * Supported ops (optionally OR'ed with FUTEX_PRIVATE_FLAG and, for
 * FUTEX_LOCK_PING, FUTEX_CLOCK_REALTIME):
 *
 *   FUTEX_LOCK_PING
 *   FUTEX_UNLOCK_PING
 *   FUTEX_TRYLOCK_PING
 */
#ifndef _UAPI_LINUX_ANDROID_FUTEX_H
#define _UAPI_LINUX_ANDROID_FUTEX_H

#include <linux/ioctl.h>
#include <linux/types.h>
#include <linux/futex.h>


/*
 * For convieniene these match the FUTEX_LOCK_PING values,
 * used in early versions of Suleiman's patch series.
 * But do not assume these values will match what eventually
 * goes upstream to be used via sys_futex()!!!
 */
#define ANDROID_FUTEX_LOCK_PING		14
#define ANDROID_FUTEX_UNLOCK_PING	15
#define ANDROID_FUTEX_TRYLOCK_PING	16

/**
 * struct android_futex_args - sys_futex() style arguments
 * @uaddr:	user address of the 32-bit futex word
 * @op:		FUTEX_*_PING, optionally OR'ed with FUTEX_PRIVATE_FLAG /
 *		FUTEX_CLOCK_REALTIME
 * @val:	unused by the PING ops (must be zero)
 * @timeout:	user pointer to a struct __kernel_timespec, or 0 for no
 *		timeout. Only used by FUTEX_LOCK_PING, where it is an
 *		absolute time on CLOCK_MONOTONIC, or on CLOCK_REALTIME if
 *		FUTEX_CLOCK_REALTIME is set in @op; must be zero for other ops.
 * @uaddr2:	unused by the PING ops (must be zero)
 * @val3:	unused by the PING ops (must be zero)
 * @__reserved:	explicit padding (must be zero)
 *
 * All pointers are passed as __u64 so the layout is identical for 32-bit
 * and 64-bit userspace.
 */
struct android_futex_args {
	__u64 uaddr;
	__u32 op;
	__u32 val;
	__u64 timeout;
	__u64 uaddr2;
	__u32 val3;
	__u32 __reserved;
};

#define ANDROID_FUTEX_IOC_MAGIC		0xFA

/*
 * Returns the same value sys_futex() would for the given op, or a
 * negative errno.
 */
#define ANDROID_FUTEX_IOC_FUTEX		_IOW(ANDROID_FUTEX_IOC_MAGIC, 0x00, \
					     struct android_futex_args)

#endif /* _UAPI_LINUX_ANDROID_FUTEX_H */
