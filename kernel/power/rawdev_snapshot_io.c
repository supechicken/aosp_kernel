// SPDX-License-Identifier: GPL-2.0-only
/*
 * Hibernation image writer that stores the image on a raw block device.
 *
 * The complete kernel side of this feature lives in this single translation
 * unit and is compiled only for CONFIG_HIBERNATION_RAWDEV.  kernel/power/
 * swap.c is NOT touched at all: the few swap.c helpers this writer needs (a
 * page level bio write, the bio completion batch and the two free page water
 * marks) are duplicated below under rawdev_* names instead of being
 * de-static'ed and exported.
 *
 * Image layout written to the hibernation partition, all offsets in pages:
 *
 *   0                     struct rawdev_header ("header page", rewritten last)
 *   1 .. npayload         page aligned blocks, one per pfn_merge_info[] entry,
 *                         in table order.  A block covers
 *                         entry.info.merged_cnt * PAGE_SIZE bytes of
 *                         uncompressed data and entry.info.compblock_len
 *                         bytes of payload, padded up to a page boundary.
 *   npayload + 1 ..       struct pfn_merge_info[hdr->pfn_merge_cnt], padded
 *                         up to a page boundary
 *
 * hdr->image_size counts the header page plus every page of payload and of
 * metadata, so it doubles as the total image size for the reader.
 *
 * The header page is written twice: a provisional copy first (so a crash in
 * the middle of the payload still leaves a recognisable header) and then the
 * final copy, seeked back to page 0, with image_size / pfn_merge_cnt /
 * image_crc filled in.
 *
 * Copyright (c) 2024 AmLogic, Inc. All rights reserved.
 */

#define pr_fmt(fmt) "PM: " fmt

#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/build_bug.h>
#include <linux/err.h>
#include <linux/file.h>
#include <linux/gfp.h>
#include <linux/init.h>
#include <linux/atomic.h>
#include <linux/cpumask.h>
#include <linux/crc32.h>
#include <linux/crypto.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/swap.h>
#include <linux/utsname.h>
#include <linux/vmalloc.h>
#include <linux/vmstat.h>
#include <asm/suspend.h>

#include "power.h"

/* ====================================================================== *
 * On-disk header ABI (page 0 of the image)
 *
 * struct swsusp_info in kernel/power/power.h is COMMON code and is left
 * completely untouched by this feature.  The STD trailer therefore lives in
 * its own struct below, which repeats the seven upstream members verbatim.
 *
 * Because the members and their order are identical, the compiler produces
 * exactly the same offsets as if the trailer had been appended to
 * struct swsusp_info (arm64, LP64):
 *
 *     off 000  struct new_utsname uts           (6 * 65 = 390 bytes)
 *     off 392  u32           version_code
 *     off 400  unsigned long num_physpages
 *     off 408  int           cpus
 *     off 416  unsigned long image_pages
 *     off 424  unsigned long pages
 *     off 432  unsigned long size
 *     off 440  AmLogic STD trailer              (72 bytes, ends at 511)
 *     .. 4095  padding up to one page
 *
 * Both structs are __aligned(PAGE_SIZE), i.e. exactly one page, so the image
 * layout is byte for byte identical to the one the U-Boot side expects and
 * no bootloader change is needed.  U-Boot has its own copy of this layout in
 * include/amlogic/cmd_resume.h; the two must be kept in sync by hand.
 * ======================================================================
 */

/*
 * Compression algorithm of the image, stored in rawdev_header.compress_algo.
 * The numeric values are part of the on-disk format, do not renumber them.
 */
enum rawdev_compress_algo {
	RAWDEV_COMP_NONE	= 0,	/* image written as is */
	RAWDEV_COMP_LZO		= 1,	/* not produced by this writer */
	RAWDEV_COMP_LZ4HC	= 2,	/* not produced by this writer */
	RAWDEV_COMP_LZ4		= 3,	/* what this writer emits */
};

/* Algorithm name as understood by the crypto API */
#define RAWDEV_COMP_NAME	"lz4"

/* Number of staging area candidates advertised to the resume side */
#define RAWDEV_STAGING_MAX	3

/* One block of physically adjacent pages, merged before compression */
struct merge_block_info {
	unsigned int	merged_cnt;	/* pages covered by this block */
	unsigned int	compblock_len;	/* payload length in bytes */
};

/* Metadata table entry, one per block, in image order */
struct pfn_merge_info {
	unsigned int		start_pfn;	/* first pfn of the block */
	unsigned int		compressed;	/* RAWDEV_BLK_* */
	struct merge_block_info	info;
};

/**
 * struct rawdev_header - on-disk header of a STD hibernation image
 * @compress_algo:	RAWDEV_COMP_* value describing the payload
 * @image_size:		total image size in bytes, header page included
 * @pfn_merge_cnt:	number of valid entries in the metadata table
 * @metadata_offset:	byte offset of the metadata table inside the image
 * @resume_pa:		physical address of cpu_resume(), i.e. the entry point
 *			the bootloader has to jump to
 * @image_crc:		CRC32 (big endian polynomial) over the payload
 * @staging_pfn:	candidate start pfns for the decompression staging
 *			area, best (largest) candidate first
 * @staging_size:	size in bytes of each staging area candidate
 *
 * The first seven members are a verbatim copy of the upstream struct
 * swsusp_info, so this struct can replace it without changing a single byte
 * of the on-disk layout.
 */
struct rawdev_header {
	/* ---- verbatim copy of the upstream struct swsusp_info members ---- */
	struct new_utsname	uts;
	u32			version_code;
	unsigned long		num_physpages;
	int			cpus;
	unsigned long		image_pages;
	unsigned long		pages;
	unsigned long		size;

	/* ---- AmLogic STD trailer, must stay right after ->size ---- */
	enum rawdev_compress_algo	compress_algo;
	unsigned int			image_size;
	unsigned int			pfn_merge_cnt;
	unsigned int			metadata_offset;
	unsigned int			resume_pa;
	unsigned int			image_crc;
	unsigned long			staging_pfn[RAWDEV_STAGING_MAX];
	unsigned long			staging_size[RAWDEV_STAGING_MAX];
} __aligned(PAGE_SIZE);

/*
 * Compile time guards against silent ABI drift.  These offsets are what the
 * bootloader (include/amlogic/cmd_resume.h) relies on; if any assert fires
 * the on-disk format changed and both sides have to be updated together.
 */
static_assert(sizeof(struct swsusp_info) == PAGE_SIZE);
static_assert(offsetof(struct swsusp_info, size) + sizeof(unsigned long) ==
	      offsetof(struct rawdev_header, compress_algo));
/* the algorithm must stay a 32 bit on-disk field */
static_assert(sizeof(enum rawdev_compress_algo) == sizeof(u32));
/* trailer shape: six u32 scalars followed by two arrays of unsigned long */
static_assert(offsetof(struct rawdev_header, staging_pfn) ==
	      offsetof(struct rawdev_header, compress_algo) +
	      6 * sizeof(u32));
static_assert(offsetof(struct rawdev_header, staging_size) ==
	      offsetof(struct rawdev_header, staging_pfn) +
	      RAWDEV_STAGING_MAX * sizeof(unsigned long));
static_assert(sizeof(struct rawdev_header) == PAGE_SIZE);

/*
 * Size of the physical staging area excluded from the image by the
 * "hib_staging=" early parameter.  It must be at least as large as the
 * biggest decompression buffer the resume path (U-Boot snapshot_boot() or
 * the kernel's own restore path) will ever allocate, because the whole
 * point of the parameter is that this range can never overlap a restored
 * kernel page.  Keep it in sync with the resume side.
 */
#define HIB_STAGING_SIZE	(256UL << 20)

/* Number of pages/bytes compressed in one go. */
#define RAWDEV_UNC_PAGES	32
#define RAWDEV_UNC_SIZE		(RAWDEV_UNC_PAGES * PAGE_SIZE)

/*
 * Number of pages needed for compressed data in the worst case, same
 * formula as swap.c: size + size / 16 + 64 + 3 + 2.  The expression is
 * spelled out here instead of going through a helper macro or a static
 * inline function because the result is used as the bound of a struct
 * member array, and a function call is not an integer constant
 * expression: clang then reports the member as a variable length array
 * (-Wvla) or refuses it outright.  RAWDEV_UNC_SIZE is a macro, not a
 * macro argument, so repeating it here does not count as argument reuse.
 */
#define RAWDEV_CMP_PAGES	DIV_ROUND_UP(RAWDEV_UNC_SIZE + \
					       RAWDEV_UNC_SIZE / 16 + 64 + 3 + 2 + \
					       sizeof(size_t), PAGE_SIZE)
#define RAWDEV_CMP_SIZE		(RAWDEV_CMP_PAGES * PAGE_SIZE)

/* Maximum number of compression threads. */
#define RAWDEV_THREADS		3

/*
 * Marker for a zero page inside the packed pfn array.  It is produced by
 * pack_pfns() in snapshot.c (which defines the same value locally) and it
 * means "this page was not copied into copy_bm, materialise it as zeros".
 * The rawdev writer must therefore NOT consume a copy-page slot for it.
 */
#define RAWDEV_PFN_ZERO_FLAG	(1UL << (BITS_PER_LONG - 1))

/*
 * Encoding of pfn_merge_info[].compressed.  RAWDEV_BLK_ZERO is a
 * merge-stage-only marker: it stops a zero page from being merged with its
 * neighbours.  The compression stage overwrites the field with 0 or 1 for
 * every block it actually emits, because a zero page does end up as a real
 * (tiny) compressed block in the payload.
 */
#define RAWDEV_BLK_RAW		0
#define RAWDEV_BLK_COMPRESSED	1
#define RAWDEV_BLK_ZERO		2

/**
 * struct rawdev_bio_batch - batch of in-flight page writes
 *
 * Local copy of swap.c's hib_bio_batch, which is private to that file.
 */
struct rawdev_bio_batch {
	atomic_t		count;
	wait_queue_head_t	wait;
	blk_status_t		error;
	struct blk_plug		plug;
};

/**
 * struct rawdev_handle - file-alike handle for the raw hibernation device
 * @cur_pos:		page index of the next write
 * @reqd_free_pages:	free pages we insist on keeping while the image is
 *			being written
 */
struct rawdev_handle {
	sector_t	cur_pos;
	unsigned long	reqd_free_pages;
};

/**
 * struct rawdev_crc_data - state of the CRC32 thread
 */
struct rawdev_crc_data {
	struct task_struct	*thr;
	atomic_t		ready;
	atomic_t		stop;
	wait_queue_head_t	go;
	wait_queue_head_t	done;
	size_t			len;	/* current uncompressed length */
	unsigned char		*buf;	/* current uncompressed data */
	u32			*crc32;	/* points to header's crc */
};

/**
 * struct rawdev_cmp_data - state of one compression thread
 */
struct rawdev_cmp_data {
	struct task_struct	*thr;
	struct crypto_comp	*tfm;
	atomic_t		ready;
	atomic_t		stop;
	int			ret;
	wait_queue_head_t	go;
	wait_queue_head_t	done;
	size_t			unc_len;
	size_t			cmp_len;
	unsigned char		unc[RAWDEV_UNC_SIZE];
	unsigned char		cmp[RAWDEV_CMP_SIZE];
};

/*
 * Compression algorithm name.  It is written to the image header as an enum
 * (rawdev_header.compress_algo) and the resume side picks the same algorithm
 * there, so this string only has to agree with the crypto API name.
 */
static char compress_type[20] = RAWDEV_COMP_NAME;

/* Number of pages needed to save the original pfns of the image pages */
static unsigned int nr_meta_pages;

/* The raw hibernation device, opened for writing while an image is saved */
static struct file *rawdev_bdev_file;

/* Physical start pfn of the "hib_staging=" reserved area, 0 when unused */
static unsigned long hib_staging_pfn;

/*
 * Free page water marks.  Copies of swap.c's low_free_pages() /
 * reqd_free_pages(), which are file scope there.
 */
static inline unsigned long rawdev_low_free_pages(void)
{
	return nr_free_pages() - nr_free_highpages();
}

static inline unsigned long rawdev_reqd_free_pages(void)
{
	return rawdev_low_free_pages() / 2;
}

/**
 * enough_rawdev - is there room for an uncompressed image?
 * @nr_pages:	number of pages the image will need
 */
static int enough_rawdev(unsigned int nr_pages)
{
	/* get_capacity() is in 512 byte sectors, the requirement is in pages */
	unsigned long free_rawdev =
		get_capacity(file_bdev(rawdev_bdev_file)->bd_disk) >>
		(PAGE_SHIFT - SECTOR_SHIFT);
	unsigned long required;

	pr_debug("Free raw dev pages: %lu\n", free_rawdev);

	required = PAGES_FOR_IO + nr_pages;

	return free_rawdev > required;
}

/**
 * get_compress_algo - map compress_type to the header encoding
 */
static enum rawdev_compress_algo get_compress_algo(void)
{
	if (!strcmp(compress_type, "lzo"))
		return RAWDEV_COMP_LZO;
	else if (!strcmp(compress_type, "lz4hc"))
		return RAWDEV_COMP_LZ4HC;
	else if (!strcmp(compress_type, "lz4"))
		return RAWDEV_COMP_LZ4;

	return RAWDEV_COMP_NONE;
}

/*
 * Bio batch handling.  Copies of swap.c's hib_init_batch() /
 * hib_finish_batch() / hib_wait_io() plus a write-only hib_end_io().
 */
static void rawdev_init_batch(struct rawdev_bio_batch *hb)
{
	atomic_set(&hb->count, 0);
	init_waitqueue_head(&hb->wait);
	hb->error = BLK_STS_OK;
	blk_start_plug(&hb->plug);
}

static void rawdev_finish_batch(struct rawdev_bio_batch *hb)
{
	blk_finish_plug(&hb->plug);
}

static void rawdev_end_io(struct bio *bio)
{
	struct rawdev_bio_batch *hb = bio->bi_private;
	struct page *page = bio_first_page_all(bio);

	if (bio->bi_status)
		pr_alert("Write-error on hibernation device (%u:%u:%llu)\n",
			 MAJOR(bio_dev(bio)), MINOR(bio_dev(bio)),
			 (unsigned long long)bio->bi_iter.bi_sector);

	/* This file only ever submits WRITE bios. */
	put_page(page);

	if (bio->bi_status && !hb->error)
		hb->error = bio->bi_status;
	if (atomic_dec_and_test(&hb->count))
		wake_up(&hb->wait);

	bio_put(bio);
}

/**
 * rawdev_submit_io - submit one page write against the raw device
 * @page_off:	page index inside the hibernation partition
 * @addr:	address of the page to write
 * @hb:		completion batch, or %NULL to wait for the write
 */
static int rawdev_submit_io(pgoff_t page_off, void *addr,
			    struct rawdev_bio_batch *hb)
{
	struct page *page = virt_to_page(addr);
	struct bio *bio;
	int error;

	bio = bio_alloc(file_bdev(rawdev_bdev_file), 1,
			REQ_OP_WRITE | REQ_SYNC, GFP_NOIO | __GFP_HIGH);
	bio->bi_iter.bi_sector = page_off * (PAGE_SIZE >> 9);

	if (bio_add_page(bio, page, PAGE_SIZE, 0) < PAGE_SIZE) {
		pr_err("Adding page to bio failed at %llu\n",
		       (unsigned long long)bio->bi_iter.bi_sector);
		bio_put(bio);
		return -EFAULT;
	}

	if (hb) {
		bio->bi_end_io = rawdev_end_io;
		bio->bi_private = hb;
		atomic_inc(&hb->count);
		submit_bio(bio);
		return 0;
	}

	error = submit_bio_wait(bio);
	bio_put(bio);
	return error;
}

static int rawdev_wait_io(struct rawdev_bio_batch *hb)
{
	/*
	 * We rely on the behaviour of blk_plug: a thread that has a plug
	 * flushes the plug list before sleeping.
	 */
	wait_event(hb->wait, atomic_read(&hb->count) == 0);
	return blk_status_to_errno(hb->error);
}

/**
 * rawdev_bio_write_page - write one page at @offset
 * @buf:	address of the page to write
 * @offset:	page index; the LBA is offset * (PAGE_SIZE >> 9)
 * @hb:		completion batch, or %NULL to write synchronously
 *
 * This is a copy of swap.c's write_page() with the "offset 0 means no swap
 * slot" guard removed: a STD image legitimately starts at page 0, which is
 * exactly why the original patch had to punch a hole into write_page().
 */
static int rawdev_bio_write_page(void *buf, sector_t offset,
				 struct rawdev_bio_batch *hb)
{
	void *src;
	int ret;

	if (hb) {
		src = (void *)__get_free_page(GFP_NOIO | __GFP_NOWARN |
					      __GFP_NORETRY);
		if (src) {
			copy_page(src, buf);
		} else {
			ret = rawdev_wait_io(hb);	/* free pages */
			if (ret)
				return ret;
			src = (void *)__get_free_page(GFP_NOIO | __GFP_NOWARN |
						      __GFP_NORETRY);
			if (src) {
				copy_page(src, buf);
			} else {
				WARN_ON_ONCE(1);
				hb = NULL;	/* go synchronous */
				src = buf;
			}
		}
	} else {
		src = buf;
	}

	return rawdev_submit_io(offset, src, hb);
}

/**
 * rawdev_write_page - append one page to the image
 * @handle:	raw device handle
 * @buf:	address of the page to write
 * @hb:		completion batch, or %NULL to write synchronously
 */
static int rawdev_write_page(struct rawdev_handle *handle, void *buf,
			     struct rawdev_bio_batch *hb)
{
	int error = 0;

	error = rawdev_bio_write_page(buf, handle->cur_pos, hb);
	if (error)
		return error;

	handle->cur_pos += 1;

	if (hb && rawdev_low_free_pages() <= handle->reqd_free_pages) {
		error = rawdev_wait_io(hb);
		if (error)
			goto out;
		/*
		 * Recalculate the number of required free pages, to make sure
		 * we never take more than half.
		 */
		handle->reqd_free_pages = rawdev_reqd_free_pages();
	}

out:
	return error;
}

/**
 * rawdev_open - open the raw hibernation device for writing
 * @handle:	handle to initialise
 */
static int rawdev_open(struct rawdev_handle *handle)
{
	int error;

	rawdev_bdev_file = bdev_file_open_by_dev(swsusp_resume_device,
						 BLK_OPEN_WRITE, NULL, NULL);
	if (IS_ERR(rawdev_bdev_file)) {
		error = PTR_ERR(rawdev_bdev_file);
		rawdev_bdev_file = NULL;
		return error;
	}

	handle->cur_pos = 0;
	handle->reqd_free_pages = rawdev_reqd_free_pages();

	return 0;
}

/**
 * rawdev_close - close the raw hibernation device
 *
 * Only rawdev_snapshot_write() uses this, so it stays file local and power.h
 * carries no prototype for it.
 */
static void rawdev_close(void)
{
	if (!rawdev_bdev_file || IS_ERR(rawdev_bdev_file)) {
		pr_debug("Image device not initialised\n");
		return;
	}

	bdev_fput(rawdev_bdev_file);
	rawdev_bdev_file = NULL;
}

/**
 * rawdev_crc_threadfn - CRC32 update thread
 * @data:	struct rawdev_crc_data
 */
static int rawdev_crc_threadfn(void *data)
{
	struct rawdev_crc_data *d = data;

	while (1) {
		wait_event(d->go, atomic_read_acquire(&d->ready) ||
			   kthread_should_stop());
		/*
		 * Paired with the main loop's ->go wake_up: the crc payload
		 * fields must not be pre-loaded before the ->ready handshake.
		 */
		smp_rmb();
		if (kthread_should_stop()) {
			d->thr = NULL;
			/* publish ->thr before ->stop is set */
			smp_wmb();
			atomic_set_release(&d->stop, 1);
			wake_up(&d->done);
			break;
		}
		atomic_set(&d->ready, 0);
		*d->crc32 = crc32_be(*d->crc32, d->buf, d->len);
		/*
		 * The updated crc32 must be visible before ->stop is set: the
		 * main loop reads it right after the handshake.
		 */
		smp_wmb();
		atomic_set_release(&d->stop, 1);
		wake_up(&d->done);
	}

	return 0;
}

/**
 * rawdev_compress_threadfn - compression thread
 * @data:	struct rawdev_cmp_data
 */
static int rawdev_compress_threadfn(void *data)
{
	unsigned int temp_len;
	struct rawdev_cmp_data *d = data;

	while (1) {
		wait_event(d->go, atomic_read_acquire(&d->ready) ||
			   kthread_should_stop());
		/*
		 * Paired with the main loop's ->go wake_up: unc/unc_len must
		 * not be pre-loaded before the ->ready handshake.
		 */
		smp_rmb();
		if (kthread_should_stop()) {
			d->thr = NULL;
			d->ret = -1;
			/* publish ->thr/->ret before ->stop is set */
			smp_wmb();
			atomic_set_release(&d->stop, 1);
			wake_up(&d->done);
			break;
		}
		temp_len = RAWDEV_CMP_SIZE;
		atomic_set(&d->ready, 0);
		d->ret = crypto_comp_compress(d->tfm, d->unc, d->unc_len,
					      d->cmp, &temp_len);
		d->cmp_len = temp_len;
		/*
		 * d->ret/cmp_len must be visible before d->stop is set.
		 *
		 * LINK: SMP_MB_PAIR_2
		 */
		smp_wmb();
		atomic_set_release(&d->stop, 1);

		wake_up(&d->done);
	}

	return 0;
}

/**
 * hib_staging_setup - parse "hib_staging=<pa>"
 * @p:	physical address of the staging area, in the usual memparse syntax
 *
 * Reserves the range so it is never handed out as a page that goes into the
 * image.  The resume path uses the very same range as its decompression
 * buffer; keeping it out of the image is what removes the
 * "comp_buffer overlaps a restored page" corruption of kernel-image blocks
 * (e.g. cpu_resume).
 */
static int __init hib_staging_setup(char *p)
{
	unsigned long pa;

	if (!p || !*p)
		return 0;

	pa = memparse(p, NULL);
	if (pa) {
		hib_staging_pfn = pa >> PAGE_SHIFT;
		memblock_reserve(pa, HIB_STAGING_SIZE);
		pr_info("HIB: staging region reserved @0x%lx (%lu MB)\n",
			pa, HIB_STAGING_SIZE >> 20);
	}

	return 0;
}

early_param("hib_staging", hib_staging_setup);

/**
 * track_largest_gaps - remember the RAWDEV_STAGING_MAX largest holes
 * @gaps:	array of hole start pfns, largest hole first
 * @bytes:	array of hole sizes in bytes, largest hole first
 * @pfn:	start pfn of the hole that was just found
 * @size:	size of that hole in bytes
 *
 * Holes are the runs of physical pages that are not part of the image.  The
 * resume side uses one of them as the staging area for decompression, so
 * merge_adjacent_pfn() keeps the largest ones and stores them in the image
 * header.
 */
static void track_largest_gaps(unsigned long *gaps, size_t *bytes,
			       unsigned long pfn, size_t size)
{
	int i;

	for (i = 0; i < RAWDEV_STAGING_MAX; i++) {
		if (size <= bytes[i])
			continue;

		/* shift the shorter entries one slot down */
		if (i < RAWDEV_STAGING_MAX - 1) {
			memmove(&bytes[i + 1], &bytes[i],
				(RAWDEV_STAGING_MAX - 1 - i) *
				sizeof(bytes[0]));
			memmove(&gaps[i + 1], &gaps[i],
				(RAWDEV_STAGING_MAX - 1 - i) *
				sizeof(gaps[0]));
		}
		bytes[i] = size;
		gaps[i] = pfn;
		break;
	}
}

/**
 * merge_adjacent_pfn - build the pfn_merge_info[] table
 * @info:	image header being filled in
 * @snapshot:	snapshot handle positioned at the first metadata page
 *
 * Walks every pfn the image must restore, not only the copied ones:
 * info->image_pages is nr_copy_pages and excludes the zero pages, while the
 * packed pfn array (built from orig_bm) does contain them, flagged with
 * RAWDEV_PFN_ZERO_FLAG.  Every zero page also starts its own merge block,
 * so the block count is bounded by the total pfn count as well.
 *
 * While walking, the three largest holes in the physical page map are
 * recorded; when "hib_staging=" is not given they are handed to the resume
 * side as candidate staging areas.
 *
 * Returns the table (to be freed with vfree()) or %NULL on allocation
 * failure.
 */
static struct pfn_merge_info *merge_adjacent_pfn(struct rawdev_header *info,
						 struct snapshot_handle *snapshot)
{
	int ret;
	int nr_pages = 0, pfn_mi_index = 0;
	int in_page_index;
	unsigned long pfn_diff;
	unsigned long *pfn_ptr, *compare_pfn_ptr;
	unsigned long seed_pfn = 0, nr_walked_pfn = 0;
	unsigned long total_pfns;
	struct pfn_merge_info *pfn_merge_info;
	unsigned long gap_pfn[RAWDEV_STAGING_MAX] = {0};
	size_t gap_size[RAWDEV_STAGING_MAX] = {0};
	size_t cur_gap_size;
	unsigned long cur_gap_start;
	int i;

	total_pfns = info->image_pages + snapshot_get_zero_pages();

	pfn_merge_info = vmalloc_array(total_pfns, sizeof(*pfn_merge_info));
	if (!pfn_merge_info)
		return NULL;

	while (nr_pages < nr_meta_pages) {
		ret = snapshot_read_next(snapshot);
		if (ret <= 0)
			break;

		pfn_ptr = (unsigned long *)data_of(*snapshot);

		if (nr_pages == 0) {
			seed_pfn = *pfn_ptr & ~RAWDEV_PFN_ZERO_FLAG;
			pfn_merge_info[pfn_mi_index].start_pfn = seed_pfn;
			pfn_merge_info[pfn_mi_index].info.merged_cnt = 1;
			/* Must test the RAW entry: seed_pfn is masked above. */
			pfn_merge_info[pfn_mi_index].compressed =
				(*pfn_ptr & RAWDEV_PFN_ZERO_FLAG) ?
				RAWDEV_BLK_ZERO : RAWDEV_BLK_RAW;
			in_page_index = 1;
			nr_walked_pfn++;
		} else {
			in_page_index = 0;
		}

		while (in_page_index < (PAGE_SIZE / sizeof(long))) {
			compare_pfn_ptr = pfn_ptr + in_page_index;
			pfn_diff = (*compare_pfn_ptr & ~RAWDEV_PFN_ZERO_FLAG) -
				   (seed_pfn & ~RAWDEV_PFN_ZERO_FLAG);

			if (pfn_diff > 1) {
				cur_gap_size = (size_t)(pfn_diff - 1) * PAGE_SIZE;
				cur_gap_start =
					(seed_pfn & ~RAWDEV_PFN_ZERO_FLAG) + 1;
				track_largest_gaps(gap_pfn, gap_size,
						   cur_gap_start, cur_gap_size);
			}

			if (pfn_diff == 1 &&
			    !(*compare_pfn_ptr & RAWDEV_PFN_ZERO_FLAG) &&
			    pfn_merge_info[pfn_mi_index].compressed !=
					RAWDEV_BLK_ZERO &&
			    pfn_merge_info[pfn_mi_index].info.merged_cnt <
					RAWDEV_UNC_PAGES) {
				pfn_merge_info[pfn_mi_index].info.merged_cnt++;
			} else {
				pfn_mi_index++;
				pfn_merge_info[pfn_mi_index].start_pfn =
					*compare_pfn_ptr & ~RAWDEV_PFN_ZERO_FLAG;
				pfn_merge_info[pfn_mi_index].info.merged_cnt = 1;
				pfn_merge_info[pfn_mi_index].compressed =
					(*compare_pfn_ptr &
					 RAWDEV_PFN_ZERO_FLAG) ?
					RAWDEV_BLK_ZERO : RAWDEV_BLK_RAW;
			}

			seed_pfn = *compare_pfn_ptr & ~RAWDEV_PFN_ZERO_FLAG;

			nr_walked_pfn++;
			if (nr_walked_pfn == total_pfns)
				break;

			in_page_index++;
		}

		nr_pages++;
	}

	info->pfn_merge_cnt = pfn_mi_index + 1;

	if (hib_staging_pfn) {
		/*
		 * Dedicated, image-excluded staging region (see hib_staging=).
		 * Removes the comp_buffer/startpfn physical overlap that
		 * corrupts LZ4 decompression of kernel-image blocks.
		 */
		info->staging_pfn[0] = hib_staging_pfn;
		info->staging_size[0] = HIB_STAGING_SIZE;
		for (i = 1; i < RAWDEV_STAGING_MAX; i++) {
			info->staging_pfn[i] = 0;
			info->staging_size[i] = 0;
		}
	} else {
		for (i = 0; i < RAWDEV_STAGING_MAX; i++) {
			info->staging_pfn[i] = gap_pfn[i];
			info->staging_size[i] = gap_size[i];
		}
	}

	return pfn_merge_info;
}

/**
 * preset_snapshot_header - fill the STD specific header fields
 * @header:	header page copied out of the snapshot stream
 */
static void preset_snapshot_header(struct rawdev_header *header)
{
	header->compress_algo = get_compress_algo();
	if (header->compress_algo)
		header->image_size = PAGE_SIZE;
	else
		header->image_size = 0;

	header->pfn_merge_cnt = 0;
	header->metadata_offset = PAGE_SIZE;
	header->image_crc = ~0;
	/*
	 * cpu_resume lives in .idmap.text, which the linker places inside
	 * .rodata.text (arch/arm64/kernel/vmlinux.lds.S), i.e. outside the
	 * linear map on a VA_BITS=39/KASLR image.  Use __pa_symbol(), the
	 * canonical kimage_voffset based conversion for kernel image symbols.
	 */
	pr_info("HIB: cpu_resume=%pS __pa=0x%lx __pa_symbol=0x%lx\n",
		cpu_resume, (unsigned long)__pa(cpu_resume),
		(unsigned long)__pa_symbol(cpu_resume));
	header->resume_pa = (u32)__pa_symbol(cpu_resume);
	/*
	 * Bringup instrumentation: dump the page U-Boot will jump to, so the
	 * boot log can be compared against the vmlinux disassembly of
	 * cpu_resume().  Harmless, drop it once bringup is done.
	 */
	{
		unsigned long rpa = (unsigned long)header->resume_pa;
		unsigned long off = (rpa & ~PAGE_MASK) / 4;
		u32 *vp = __va(rpa & PAGE_MASK);

		off = min(off, (PAGE_SIZE / 4) - 8);
		pr_info("HIB: resume phys page dump @0x%lx:\n", rpa);
		pr_info("  %08x %08x %08x %08x %08x %08x %08x %08x\n",
			vp[off + 0], vp[off + 1], vp[off + 2], vp[off + 3],
			vp[off + 4], vp[off + 5], vp[off + 6], vp[off + 7]);
	}
}

/**
 * rawdev_report_progress - print the save progress in 10 percent steps
 * @reported:	last percentage printed, updated in place
 * @done:	pages handled so far
 * @total:	number of pages the loop will handle in total
 *
 * Pages are consumed one merge block at a time (up to RAWDEV_UNC_PAGES at
 * once), and a block of zero pages advances the counter whole-block as well.
 * Testing "done % (total / 10) == 0" therefore misses most of the ten percent
 * boundaries: with 82941 pages to write the old code printed
 * 0/10/20/40/70/80/90/100 and then, because zero pages were counted in the
 * numerator but not in the denominator, finished at 120%.
 *
 * Remembering the last reported step and emitting as soon as the next one is
 * reached gives a monotonic 10/20/.../100 sequence no matter how the loop
 * happens to stride.
 */
static void rawdev_report_progress(unsigned int *reported, unsigned long done,
				   unsigned long total)
{
	unsigned int pct;

	if (!total)
		return;

	pct = (unsigned int)(done * 100 / total);
	if (pct < *reported + 10)
		return;

	*reported = pct - pct % 10;
	pr_info("Image saving progress: %3u%%\n", *reported);
}

/**
 * save_image_dir - save the image data without compression
 * @handle:	raw device handle
 * @snapshot:	image to read data from
 * @nr_to_write:	number of pages to save
 */
static int save_image_dir(struct rawdev_handle *handle,
			  struct snapshot_handle *snapshot,
			  unsigned int nr_to_write)
{
	unsigned int progress = 0;
	unsigned long nr_pages;
	int ret;
	int err2;
	struct rawdev_bio_batch hb;
	ktime_t start;
	ktime_t stop;

	rawdev_init_batch(&hb);

	pr_info("Saving image data pages (%u pages)...\n", nr_to_write);
	nr_pages = 0;
	start = ktime_get();
	while (1) {
		ret = snapshot_read_next(snapshot);
		if (ret <= 0)
			break;
		ret = rawdev_write_page(handle, data_of(*snapshot), &hb);
		if (ret)
			break;
		nr_pages++;
		rawdev_report_progress(&progress, nr_pages, nr_to_write);
	}
	err2 = rawdev_wait_io(&hb);
	rawdev_finish_batch(&hb);
	stop = ktime_get();
	if (!ret)
		ret = err2;
	if (!ret)
		pr_info("Image saving done\n");
	swsusp_show_speed(start, stop, nr_to_write, "Wrote");
	return ret;
}

/**
 * save_image_compress - save the image data with compression
 * @handle:	raw device handle
 * @snapshot:	image to read data from, positioned at the first data page
 * @nr_to_write:	number of pages to save
 * @info:	header page, updated in place with image_size, image_crc and
 *		pfn_merge_cnt
 */
static int save_image_compress(struct rawdev_handle *handle,
			       struct snapshot_handle *snapshot,
			       unsigned int nr_to_write,
			       struct rawdev_header *info)
{
	unsigned int progress = 0;
	int ret = 0;
	int net = 0;
	unsigned long nr_pages, nr_total;
	int err2;
	struct rawdev_bio_batch hb;
	ktime_t start;
	ktime_t stop;
	size_t off, pgoff = 0;
	size_t left_len, len, slice;
	unsigned char *buf;
	unsigned int thr, run_threads, nr_threads;
	unsigned char *page = NULL;
	struct rawdev_cmp_data *data = NULL;
	struct rawdev_crc_data *crc = NULL;
	struct pfn_merge_info *pfn_merge_info = NULL;
	size_t *check_cmp_len = NULL;
	int pfn_mi_index;

	rawdev_init_batch(&hb);
	/*
	 * We limit the number of threads for compression to limit the memory
	 * footprint.  One extra thread compared to swap.c's
	 * num_online_cpus() - 1 for compression performance.
	 */
	nr_threads = num_online_cpus();
	nr_threads = clamp_val(nr_threads, 1, RAWDEV_THREADS);

	/*
	 * Get the pfns from orig_bm to create pfn_merge_info.
	 */
	pfn_merge_info = merge_adjacent_pfn(info, snapshot);
	if (!pfn_merge_info) {
		ret = -ENOMEM;
		goto out_clean;
	}

	/*
	 * Allocate memory for the writer, the compression and CRC threads.
	 */
	page = (void *)__get_free_page(GFP_NOIO | __GFP_HIGH);
	if (!page) {
		pr_err("Failed to allocate output page\n");
		ret = -ENOMEM;
		goto out_clean;
	}

	data = vmalloc_array(nr_threads, sizeof(*data));
	if (!data) {
		ret = -ENOMEM;
		goto out_clean;
	}
	for (thr = 0; thr < nr_threads; thr++)
		memset(&data[thr], 0, offsetof(struct rawdev_cmp_data, go));

	crc = kmalloc(sizeof(*crc), GFP_KERNEL);
	if (!crc) {
		ret = -ENOMEM;
		goto out_clean;
	}
	memset(crc, 0, offsetof(struct rawdev_crc_data, go));

	check_cmp_len = kmalloc_array(nr_threads, sizeof(*check_cmp_len),
				      GFP_KERNEL);
	if (!check_cmp_len) {
		ret = -ENOMEM;
		goto out_clean;
	}

	/*
	 * Start the compression threads.  data[] is zeroed up to ->go above,
	 * so an entry that was never reached keeps ->thr and ->tfm NULL and
	 * the cleanup path can walk all nr_threads entries safely.
	 */
	for (thr = 0; thr < nr_threads; thr++) {
		data[thr].tfm = crypto_alloc_comp(compress_type, 0, 0);
		if (IS_ERR(data[thr].tfm)) {
			data[thr].tfm = NULL;
			pr_err("Can't allocate compressor transform\n");
			ret = -ENOMEM;
			goto out_clean;
		}

		init_waitqueue_head(&data[thr].go);
		init_waitqueue_head(&data[thr].done);

		data[thr].thr = kthread_run(rawdev_compress_threadfn,
					    &data[thr],
					    "image_compress/%u", thr);
		if (IS_ERR(data[thr].thr)) {
			data[thr].thr = NULL;
			pr_err("Cannot start compression threads\n");
			ret = -ENOMEM;
			goto out_clean;
		}
	}

	/*
	 * Start the CRC32 thread.
	 */
	init_waitqueue_head(&crc->go);
	init_waitqueue_head(&crc->done);

	crc->crc32 = &info->image_crc;
	crc->thr = kthread_run(rawdev_crc_threadfn, crc, "image_crc32");
	if (IS_ERR(crc->thr)) {
		crc->thr = NULL;
		pr_err("Cannot start CRC32 thread\n");
		ret = -ENOMEM;
		goto out_clean;
	}

	pr_info("Using %u thread(s) for compression\n", nr_threads);
	/*
	 * Progress is measured against the number of pfns the loop below
	 * walks, which is image_pages plus the zero pages: zero pages have a
	 * block of their own and are counted here too.  nr_to_write is only
	 * image_pages + nr_meta_pages, i.e. the number of pages READ from the
	 * snapshot stream, so using it as the denominator made the percentage
	 * overshoot to 120%.
	 */
	nr_total = info->image_pages + snapshot_get_zero_pages();
	pr_info("Compressing and saving image data (%lu pages)...\n",
		nr_total);
	nr_pages = 0;
	start = ktime_get();
	pfn_mi_index = 0;
	for (;;) {
		for (thr = 0; thr < nr_threads; thr++) {
			if (pfn_mi_index >= info->pfn_merge_cnt)
				break;

			for (off = 0;
			     off < PAGE_SIZE *
				pfn_merge_info[pfn_mi_index].info.merged_cnt;
			     off += PAGE_SIZE) {
				if (pfn_merge_info[pfn_mi_index].compressed ==
				    RAWDEV_BLK_ZERO) {
					/*
					 * Zero page: it was not copied into
					 * copy_bm (copy_data_pages skips it),
					 * so the data stream must NOT consume
					 * a snapshot page here -- fill with
					 * zeros to keep the block layout
					 * aligned.
					 */
					memset(data[thr].unc + off, 0, PAGE_SIZE);
					nr_pages++;
					rawdev_report_progress(&progress,
							       nr_pages,
							       nr_total);
					continue;
				}

				ret = snapshot_read_next(snapshot);
				if (ret < 0)
					goto out_finish;

				if (!ret)
					break;

				memcpy(data[thr].unc + off,
				       data_of(*snapshot), PAGE_SIZE);

				nr_pages++;
				rawdev_report_progress(&progress, nr_pages,
						       nr_total);
			}
			if (!off)
				break;

			data[thr].unc_len = off;

			/*
			 * Main loop modifies data[thr].unc and unc_len, and
			 * rawdev_compress_threadfn() uses them to calculate the
			 * crc32 value.  The barrier orders those stores before
			 * ->ready.
			 *
			 * LINK: SMP_MB_PAIR_1
			 */
			smp_wmb();

			atomic_set_release(&data[thr].ready, 1);
			wake_up(&data[thr].go);
			pfn_mi_index++;
		}

		if (!thr)
			break;

		for (run_threads = thr, thr = 0; thr < run_threads; thr++) {
			wait_event(data[thr].done,
				   atomic_read_acquire(&data[thr].stop));
			/*
			 * SMP_MB_PAIR_2: data[thr].ret/cmp_len are updated
			 * before ->stop is set; do not pre-load them.
			 */
			smp_rmb();
			atomic_set(&data[thr].stop, 0);

			ret = data[thr].ret;

			/*
			 * Memory barrier check point #1: copy cmp_len.
			 */
			check_cmp_len[thr] = data[thr].cmp_len;

			/*
			 * If lz4_compress() sets cmp_len to 0 the compressed
			 * buffer would have exceeded the maximum output size,
			 * so raw data is written when one of these holds:
			 * (1) ret < 0, (2) cmp_len == 0, (3) cmp_len >= unc_len.
			 */
			if (ret < 0 || !data[thr].cmp_len ||
			    data[thr].cmp_len >= data[thr].unc_len) {
				len = data[thr].unc_len;
				buf = data[thr].unc;
				pfn_merge_info[pfn_mi_index - run_threads +
					       thr].compressed = RAWDEV_BLK_RAW;
			} else {
				/*
				 * Memory barrier check point #2: copy cmp_len.
				 */
				len = data[thr].cmp_len;
				buf = data[thr].cmp;
				pfn_merge_info[pfn_mi_index - run_threads +
					       thr].compressed =
					RAWDEV_BLK_COMPRESSED;
			}

			if (thr != 0) {
				wait_event(crc->done,
					   atomic_read(&crc->stop));
				/*
				 * SMP_MB_PAIR_4: crc32 is updated before
				 * ->done is set; do not pre-load it.
				 */
				smp_rmb();
				atomic_set(&crc->stop, 0);
			}

			crc->buf = data[thr].unc;
			crc->len = data[thr].unc_len;

			/*
			 * crc->buf/len must be updated before crc->ready.
			 *
			 * LINK: SMP_MB_PAIR_3
			 */
			smp_wmb();

			atomic_set_release(&crc->ready, 1);
			wake_up(&crc->go);

			/* Update the metadata with the compressed block size. */
			pfn_merge_info[pfn_mi_index - run_threads +
				       thr].info.compblock_len = len;

			/*
			 * Memory barrier check point #3: all cmp_len equal?
			 */
			if (pfn_merge_info[pfn_mi_index - run_threads +
					   thr].compressed) {
				if (check_cmp_len[thr] != data[thr].cmp_len ||
				    check_cmp_len[thr] != len ||
				    len != data[thr].cmp_len) {
					pr_err("Invalid compressed length\n");
					ret = -EAGAIN;
					goto out_finish;
				}
			}

			/* Write the payload (one compressed block). */
			left_len = len;
			left_len = (left_len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
			info->image_size += left_len;
			info->metadata_offset += left_len;

			while (left_len > 0) {
				if (left_len > PAGE_SIZE - pgoff) {
					slice = PAGE_SIZE - pgoff;
					left_len -= slice;
				} else {
					slice = left_len;
					left_len = 0;
				}
				memcpy((void *)(page + pgoff), buf, slice);
				pgoff += slice;
				buf += slice;

				if (pgoff == PAGE_SIZE) {
					net = rawdev_write_page(handle, page, &hb);
					if (net) {
						ret = net;
						goto out_finish;
					}
					pgoff = 0;
				}
			}
		}
		wait_event(crc->done, atomic_read_acquire(&crc->stop));
		/*
		 * SMP_MB_PAIR_4: crc32 is updated before ->done is set; do
		 * not pre-load it.
		 */
		smp_rmb();
		atomic_set(&crc->stop, 0);
	}

	/*
	 * merge_adjacent_pfn() counts every pfn the snapshot must save,
	 * including zero pages.  Zero pages do have a (tiny) payload here,
	 * but when the data stream ends early the loop above leaves entries
	 * past the last written block untouched; U-Boot walks the metadata
	 * table using pfn_merge_cnt and would read uninitialised vmalloc garbage
	 * as compblock_len.  Record the real block count.
	 */
	info->pfn_merge_cnt = pfn_mi_index;

	left_len = sizeof(struct pfn_merge_info) * info->pfn_merge_cnt;
	left_len = (left_len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
	info->image_size += left_len;
	buf = (unsigned char *)pfn_merge_info;

	while (left_len > 0) {
		if (left_len > PAGE_SIZE - pgoff) {
			slice = PAGE_SIZE - pgoff;
			left_len -= slice;
		} else {
			slice = left_len;
			left_len = 0;
		}

		memcpy((void *)(page + pgoff), buf, slice);
		pgoff += slice;
		buf += slice;

		if (pgoff == PAGE_SIZE) {
			net = rawdev_write_page(handle, page, &hb);
			if (net) {
				ret = net;
				goto out_finish;
			}
			pgoff = 0;
		}
	}

	/*
	 * Both payload and metadata lengths are rounded up to a page, so
	 * pgoff is 0 here.  Flush anyway rather than silently dropping a
	 * partial page if that ever stops being true.
	 */
	if (pgoff) {
		memset((void *)(page + pgoff), 0, PAGE_SIZE - pgoff);
		net = rawdev_write_page(handle, page, &hb);
		if (net && !ret)
			ret = net;
	}

out_finish:
	err2 = rawdev_wait_io(&hb);
	stop = ktime_get();
	if (!ret)
		ret = err2;
	if (!ret)
		pr_info("Image saving done\n");
	swsusp_show_speed(start, stop, nr_to_write, "Wrote");
out_clean:
	rawdev_finish_batch(&hb);

	kfree(check_cmp_len);

	if (crc) {
		if (crc->thr)
			kthread_stop(crc->thr);
		kfree(crc);
	}

	if (data) {
		for (thr = 0; thr < nr_threads; thr++) {
			if (data[thr].thr)
				kthread_stop(data[thr].thr);
			if (data[thr].tfm)
				crypto_free_comp(data[thr].tfm);
		}
		vfree(data);
	}

	if (page)
		free_page((unsigned long)page);
	if (pfn_merge_info)
		vfree(pfn_merge_info);

	return ret;
}

/**
 * rawdev_snapshot_write - write the whole hibernation image to the raw device
 * @flags:	image flags (SF_*)
 *
 * Called instead of swsusp_write() when CONFIG_HIBERNATION_RAWDEV is enabled.
 * The raw resume device must already have been resolved into
 * swsusp_resume_device by the caller.
 */
int rawdev_snapshot_write(unsigned int flags)
{
	struct rawdev_handle handle;
	struct snapshot_handle snapshot;
	struct rawdev_header *info = NULL;
	unsigned long pages;
	int error;
	int err2;

	pages = snapshot_get_image_size();

	error = rawdev_open(&handle);
	if (error) {
		pr_err("Cannot get raw device writer\n");
		return error;
	}

	/* Check that the device is big enough for an uncompressed image. */
	if (flags & SF_NOCOMPRESS_MODE) {
		if (!enough_rawdev(pages)) {
			pr_err("Not enough raw device space\n");
			error = -ENOSPC;
			goto out_finish;
		}
	}

	/* Read swsusp_info first. */
	memset(&snapshot, 0, sizeof(snapshot));
	error = snapshot_read_next(&snapshot);
	if (error < (int)PAGE_SIZE) {
		if (error >= 0)
			error = -EFAULT;

		goto out_finish;
	}

	/* Get a page for swsusp_info so it can be updated later. */
	info = (void *)__get_free_page(GFP_NOIO | __GFP_HIGH);
	if (!info) {
		pr_err("Cannot allocate a page for swsusp_info\n");
		error = -ENOMEM;
		goto out_finish;
	}
	memcpy((void *)info, data_of(snapshot), sizeof(struct rawdev_header));

	preset_snapshot_header(info);

	/* -1 is the header page; image_pages excludes the metadata pages. */
	nr_meta_pages = info->pages - info->image_pages - 1;

	error = rawdev_write_page(&handle, info, NULL);
	if (error)
		goto out_finish;

	/* Actual image write. */
	if (!info->compress_algo)
		error = save_image_dir(&handle, &snapshot, pages - 1);
	else
		error = save_image_compress(&handle, &snapshot, pages - 1,
					    info);
	if (error)
		goto out_finish;

	/* Rewrite the header page at offset 0 with the final values. */
	handle.cur_pos = 0;
	err2 = rawdev_write_page(&handle, info, NULL);
	if (err2)
		error = err2;

	pr_info("Start of resume sequence after making hibernation image\n");

out_finish:
	rawdev_close();
	if (info)
		free_page((unsigned long)info);

	return error;
}
