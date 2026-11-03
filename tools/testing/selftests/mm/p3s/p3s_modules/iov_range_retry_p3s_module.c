// SPDX-License-Identifier: GPL-2.0
#include <linux/fs.h>
#include <linux/highmem.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/sizes.h>
#include <linux/uaccess.h>
#include <linux/uio.h>

#include "../iov_range_retry_p3s.h"

/* Exercise existing exported iov APIs without adding test-only MM exports. */
static long iov_range_retry_ioctl(struct file *file, unsigned int cmd,
				 unsigned long arg)
{
	struct iov_range_retry_p3s_args request;
	struct page *storage[64], **pages = storage;
	struct iov_iter iter;
	size_t offset = 0, remaining;
	unsigned int i, nr;
	ssize_t bytes;

	if (cmd != IOV_RANGE_RETRY_P3S_IOCTL)
		return -EINVAL;
	if (copy_from_user(&request, (void __user *)arg, sizeof(request)))
		return -EFAULT;
	if (!request.length || request.length > SZ_1M || request.pin > 1)
		return -EINVAL;
	iov_iter_ubuf(&iter, ITER_DEST, (void __user *)request.address,
		      request.length);
	if (request.pin)
		bytes = iov_iter_extract_pages(&iter, &pages, request.length,
					       ARRAY_SIZE(storage), 0, &offset);
	else
		bytes = iov_iter_get_pages2(&iter, storage, request.length,
					   ARRAY_SIZE(storage), &offset);
	request.result = bytes;
	request.offset = offset;
	request.checksum = 2166136261U;
	if (bytes > 0) {
		remaining = bytes;
		nr = DIV_ROUND_UP(offset + bytes, PAGE_SIZE);
		for (i = 0; i < nr; i++) {
			const unsigned char *mapping = kmap_local_page(pages[i]);
			size_t chunk = min(remaining, PAGE_SIZE - offset);
			size_t j;

			for (j = 0; j < chunk; j++) {
				request.checksum ^= mapping[offset + j];
				request.checksum *= 16777619U;
			}
			kunmap_local(mapping);
			remaining -= chunk;
			offset = 0;
			if (request.pin)
				unpin_user_page(pages[i]);
			else
				put_page(pages[i]);
		}
	}
	if (copy_to_user((void __user *)arg, &request, sizeof(request)))
		return -EFAULT;
	return 0;
}

static const struct file_operations iov_range_retry_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = iov_range_retry_ioctl,
};

static struct miscdevice iov_range_retry_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "iov_range_retry_p3s",
	.fops = &iov_range_retry_fops,
	.mode = 0600,
};

static int __init iov_range_retry_init(void)
{
	return misc_register(&iov_range_retry_device);
}

static void __exit iov_range_retry_exit(void)
{
	misc_deregister(&iov_range_retry_device);
}

module_init(iov_range_retry_init);
module_exit(iov_range_retry_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("P3S iov range, batching and userfaultfd retry regression fixture");
