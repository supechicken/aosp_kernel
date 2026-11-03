/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _IOV_RANGE_RETRY_P3S_H
#define _IOV_RANGE_RETRY_P3S_H

#include <linux/ioctl.h>
#include <linux/types.h>

struct iov_range_retry_p3s_args {
	__u64 address;
	__u64 length;
	__s64 result;
	__u64 offset;
	__u32 pin;
	__u32 checksum;
};

#define IOV_RANGE_RETRY_P3S_IOCTL \
	_IOWR('p', 0x71, struct iov_range_retry_p3s_args)

#endif /* _IOV_RANGE_RETRY_P3S_H */
