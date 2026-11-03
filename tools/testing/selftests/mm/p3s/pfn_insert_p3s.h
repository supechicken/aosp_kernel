/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PFN_INSERT_P3S_H
#define __PFN_INSERT_P3S_H

#include <linux/ioctl.h>
#include <linux/types.h>

struct pfn_insert_p3s_request {
	__u64 address;
	__u32 present_mask;
	__u32 reserved;
};

#define PFN_INSERT_P3S_IOCTL \
	_IOW(0xa5, 3, struct pfn_insert_p3s_request)
#define PFN_INSERT_P3S_INSPECT \
	_IOWR(0xa5, 4, struct pfn_insert_p3s_request)

#endif /* __PFN_INSERT_P3S_H */
