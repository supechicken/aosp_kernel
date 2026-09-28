/* SPDX-License-Identifier: GPL-2.0 */
#ifndef IOV_ITER_P3S_H
#define IOV_ITER_P3S_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define IOV_ITER_P3S_DEVICE_NAME "iov_iter_p3s"
#define IOV_ITER_P3S_F_EXPECT_PACKED 1U

struct iov_iter_p3s_args {
	__u64 address;
	__u32 length;
	__u32 native_page_size;
	__s32 get_pages_result;
	__s32 extract_pages_result;
	__s32 bulk_first_len;
	__s32 npages_result;
	__u32 flags;
};

#define IOV_ITER_P3S_IOCTL \
	_IOWR('i', 0x71, struct iov_iter_p3s_args)

#endif
