/* SPDX-License-Identifier: GPL-2.0 */
#ifndef MMU_NOTIFIER_P3S_H
#define MMU_NOTIFIER_P3S_H

#include <linux/ioctl.h>
#include <linux/types.h>

struct mmu_notifier_p3s_range {
	__u64 start;
	__u64 end;
	__u64 page_size;
};

struct mmu_notifier_p3s_stats {
	__u64 total_callbacks;
	__u64 callbacks;
	__u64 bad_ranges;
	__u64 last_start;
	__u64 last_end;
	__u64 max_span;
	__u64 invalidate_starts;
	__u64 invalidate_ends;
	__u64 last_invalidate_start;
	__u64 last_invalidate_end;
};

#define MMU_NOTIFIER_P3S_IOC_MAGIC	'N'
#define MMU_NOTIFIER_P3S_SET_RANGE	_IOW(MMU_NOTIFIER_P3S_IOC_MAGIC, 0, \
					     struct mmu_notifier_p3s_range)
#define MMU_NOTIFIER_P3S_GET_STATS	_IOR(MMU_NOTIFIER_P3S_IOC_MAGIC, 1, \
					     struct mmu_notifier_p3s_stats)

#endif
