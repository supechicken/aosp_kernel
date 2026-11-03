// SPDX-License-Identifier: GPL-2.0

#include <linux/scatterlist.h>

__rust_helper int rust_helper_sg_alloc_table_from_pages(struct sg_table *sgt,
							struct page **pages,
							unsigned int n_pages,
							unsigned int offset,
							unsigned long size,
							gfp_t gfp_mask)
{
	return sg_alloc_table_from_pages(sgt, pages, n_pages, offset, size,
					 gfp_mask);
}
