/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __FAULT_IN_P3S_H
#define __FAULT_IN_P3S_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define FAULT_IN_P3S_DEVICE_NAME "fault_in_p3s"

enum fault_in_p3s_operation {
	FAULT_IN_P3S_WRITEABLE = 0,
	FAULT_IN_P3S_SAFE_WRITEABLE,
	FAULT_IN_P3S_READABLE,
};

struct fault_in_p3s_args {
	__u64 address;
	__u64 length;
	__u32 operation;
	__u32 reserved;
	__u64 not_faulted;
};

#define FAULT_IN_P3S_IOCTL \
	_IOWR('f', 0x71, struct fault_in_p3s_args)

#endif
