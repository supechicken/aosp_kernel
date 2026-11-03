// SPDX-License-Identifier: GPL-2.0
#ifndef __SELFTEST_MM_PAGEWALK_P3S_H
#define __SELFTEST_MM_PAGEWALK_P3S_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define PAGEWALK_P3S_DEVICE_NAME "pagewalk_p3s"

struct pagewalk_p3s_args {
	__s32 fd;
	__u32 reserved;
	__u64 offset;
	__u64 length;
	__u64 count;
	__u64 bitmap_weight;
};

#define PAGEWALK_P3S_IOCTL_WRITE_PROTECT \
	_IOWR('w', 1, struct pagewalk_p3s_args)
#define PAGEWALK_P3S_IOCTL_CLEAN \
	_IOWR('w', 2, struct pagewalk_p3s_args)

#endif
