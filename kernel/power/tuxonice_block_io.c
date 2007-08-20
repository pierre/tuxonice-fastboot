/*
 * kernel/power/tuxonice_block_io.c
 *
 * Copyright (C) 2004-2007 Nigel Cunningham (nigel at suspend2 net)
 *
 * Distributed under GPLv2.
 *
 * This file contains block io functions for TuxOnIce. These are
 * used by the swapwriter and it is planned that they will also
 * be used by the NFSwriter.
 *
 */

#include <linux/blkdev.h>
#include <linux/syscalls.h>
#include <linux/suspend.h>

#include "tuxonice.h"
#include "tuxonice_sysfs.h"
#include "tuxonice_modules.h"
#include "tuxonice_prepare_image.h"
#include "tuxonice_block_io.h"
#include "tuxonice_ui.h"

static int pr_index;

#if 0
#define PR_DEBUG(a, b...) do { if (pr_index < 20) printk(a, ##b); } while(0)
#else
#define PR_DEBUG(a, b...) do { } while(0)
#endif

#define MAX_OUTSTANDING_IO 2048
#define SUBMIT_BATCH_SIZE 128

static int max_outstanding_io = MAX_OUTSTANDING_IO;
static int submit_batch_size = SUBMIT_BATCH_SIZE;

struct io_info {
	struct bio *sys_struct;
	sector_t first_block;
	struct page *bio_page, *dest_page;
	int writing, readahead_index;
	struct block_device *dev;
	struct list_head list;
};

static LIST_HEAD(ioinfo_ready_for_cleanup);
static DEFINE_SPINLOCK(ioinfo_ready_lock);

static LIST_HEAD(ioinfo_submit_batch);
static DEFINE_SPINLOCK(ioinfo_submit_lock);

static LIST_HEAD(ioinfo_busy);
static DEFINE_SPINLOCK(ioinfo_busy_lock);

static struct io_info *waiting_on;

static atomic_t submit_batch;
static int submit_batched(void);

/* [Max] number of I/O operations pending */
static atomic_t outstanding_io;

static int extra_page_forward = 0;

static volatile unsigned long toi_readahead_flags[
	DIV_ROUND_UP(MAX_OUTSTANDING_IO, BITS_PER_LONG)];
static spinlock_t toi_readahead_flags_lock = SPIN_LOCK_UNLOCKED;
static struct page *toi_ra_pages[MAX_OUTSTANDING_IO];
static int readahead_index, ra_submit_index;

static int current_stream;
/* 0 = Header, 1 = Pageset1, 2 = Pageset2 */
struct extent_iterate_saved_state toi_writer_posn_save[3];

/* Pointer to current entry being loaded/saved. */
struct extent_iterate_state toi_writer_posn;

/* Not static, so that the allocators can setup and complete
 * writing the header */
char *toi_writer_buffer;
int toi_writer_buffer_posn;

int toi_read_fd;

static struct toi_bdev_info *toi_devinfo;

int toi_header_bytes_used = 0;

DEFINE_MUTEX(toi_bio_mutex);

static atomic_t toi_bio_waits;

/**
 * toi_bio_cleanup_one: Cleanup one bio.
 * @io_info : Struct io_info to be cleaned up.
 *
 * Cleanup the bio pointed to by io_info and record as appropriate that the
 * cleanup is done.
 */
static void toi_bio_cleanup_one(struct io_info *io_info)
{
	int readahead_index = io_info->readahead_index;
	unsigned long flags;

	if (!io_info->writing && readahead_index == -1) {
		char *to = (char *) kmap(io_info->dest_page);
		char *from = (char *) kmap(io_info->bio_page);
		memcpy(to, from, PAGE_SIZE);
		kunmap(io_info->dest_page);
		kunmap(io_info->bio_page);
	}

	put_page(io_info->bio_page);
	if (io_info->writing || readahead_index == -1)
		__free_page(io_info->bio_page);

	bio_put(io_info->sys_struct);
	io_info->sys_struct = NULL;

	if (readahead_index > -1) {
		int index = readahead_index/BITS_PER_LONG;
		int bit = readahead_index - (index * BITS_PER_LONG);
		spin_lock_irqsave(&toi_readahead_flags_lock, flags);
		set_bit(bit, &toi_readahead_flags[index]);
		spin_unlock_irqrestore(&toi_readahead_flags_lock, flags);
	}

	if (waiting_on == io_info)
		waiting_on = NULL;
	kfree(io_info);
	atomic_dec(&outstanding_io);
}

/**
 * toi_cleanup_some_completed_io: Cleanup completed TuxOnIce i/o.
 *
 * Cleanup i/o that has been completed. In the end_bio routine (below), we only
 * move the associated io_info struct from the busy list to the
 * ready_for_cleanup list. Now (no longer in an interrupt context), we can we
 * can do the real work.
 *
 * This routine is designed so that multiple callers can be in here
 * simultaneously.
 */
static void toi_cleanup_some_completed_io(void)
{
	int num_cleaned = 0;
	unsigned long flags;

	spin_lock_irqsave(&ioinfo_ready_lock, flags);
	while(!list_empty(&ioinfo_ready_for_cleanup)) {
		struct io_info *first = list_entry(
				ioinfo_ready_for_cleanup.next,
				struct io_info, list);

		list_del_init(&first->list);

		spin_unlock_irqrestore(&ioinfo_ready_lock, flags);
		toi_bio_cleanup_one(first);
		spin_lock_irqsave(&ioinfo_ready_lock, flags);

		num_cleaned++;
		if (num_cleaned == submit_batch_size)
			break;
	}
	spin_unlock_irqrestore(&ioinfo_ready_lock, flags);
}

/**
 * do_bio_wait: Wait for some TuxOnIce i/o to complete.
 *
 * Submit any I/O that's batched up (if we're not already doing
 * that, schedule and clean up whatever we can.
 */
static void do_bio_wait(void)
{
	struct backing_dev_info *bdi;

	submit_batched();

	atomic_inc(&toi_bio_waits);

	if (waiting_on) {
		bdi = waiting_on->dev->bd_inode->i_mapping->backing_dev_info;
		blk_run_backing_dev(bdi, waiting_on->bio_page);
	}

	io_schedule();
	toi_cleanup_some_completed_io();
}

/**
 * toi_finish_all_io: Complete all outstanding i/o.
 */
static void toi_finish_all_io(void)
{
	while (atomic_read(&outstanding_io))
		do_bio_wait();
}

/**
 * toi_readahead_ready: Is this readahead finished?
 *
 * Returns whether the readahead requested is ready.
 */
static int toi_readahead_ready(int readahead_index)
{
	int index = readahead_index / BITS_PER_LONG;
	int bit = readahead_index - (index * BITS_PER_LONG);

	return test_bit(bit, &toi_readahead_flags[index]);
}

/**
 * toi_wait_on_readahead: Wait on a particular page.
 *
 * @readahead_index: Index of the readahead to wait for.
 */
static void toi_wait_on_readahead(int readahead_index)
{
	while (!toi_readahead_ready(readahead_index))
		do_bio_wait();
}

static int toi_prepare_readahead(int index)
{
	unsigned long new_page = get_zeroed_page(TOI_ATOMIC_GFP);

	if(!new_page)
		return -ENOMEM;

	toi_ra_pages[index] = virt_to_page(new_page);
	return 0;
}

/* toi_readahead_cleanup
 * Clean up structures used for readahead */
static void toi_cleanup_readahead(int page)
{
	__free_page(toi_ra_pages[page]);
	toi_ra_pages[page] = 0;
	return;
}

/**
 * toi_end_bio: bio completion function.
 *
 * @bio: bio that has completed.
 * @bytes_done: Number of bytes written/read.
 * @err: Error value. Yes, like end_swap_bio_read, we ignore it.
 *
 * Function called by block driver from interrupt context when I/O is completed.
 * This is the reason we use spinlocks in manipulating the io_info lists. Nearly
 * the fs/buffer.c version, but we want to mark the page as done in our own
 * structures too.
 */
static int toi_end_bio(struct bio *bio, unsigned int bytes_done, int err)
{
	struct io_info *io_info = bio->bi_private;
	unsigned long flags;

	spin_lock_irqsave(&ioinfo_busy_lock, flags);
	list_del_init(&io_info->list);
	spin_unlock_irqrestore(&ioinfo_busy_lock, flags);

	spin_lock_irqsave(&ioinfo_ready_lock, flags);
	list_add_tail(&io_info->list, &ioinfo_ready_for_cleanup);
	spin_unlock_irqrestore(&ioinfo_ready_lock, flags);
	return 0;
}

/**
 *	submit - submit BIO request.
 *	@writing: READ or WRITE.
 *	@io_info: IO info structure.
 *
 * 	Based on Patrick's pmdisk code from long ago:
 *	"Straight from the textbook - allocate and initialize the bio.
 *	If we're writing, make sure the page is marked as dirty.
 *	Then submit it and carry on."
 *
 *	With a twist, though - we handle block_size != PAGE_SIZE.
 *	Caller has already checked that our page is not fragmented.
 */
static int submit(struct io_info *io_info)
{
	struct bio *bio = NULL;
	unsigned long flags;

	while (!bio) {
		bio = bio_alloc(GFP_ATOMIC,1);
		if (!bio)
			do_bio_wait();
	}

	bio->bi_bdev = io_info->dev;
	bio->bi_sector = io_info->first_block;
	bio->bi_private = io_info;
	bio->bi_end_io = toi_end_bio;
	io_info->sys_struct = bio;

	if (bio_add_page(bio, io_info->bio_page, PAGE_SIZE, 0) < PAGE_SIZE) {
		printk("ERROR: adding page to bio at %lld\n",
				(unsigned long long) io_info->first_block);
		bio_put(bio);
		return -EFAULT;
	}

	if (io_info->writing)
		bio_set_pages_dirty(bio);

	spin_lock_irqsave(&ioinfo_busy_lock, flags);
	list_add_tail(&io_info->list, &ioinfo_busy);
	spin_unlock_irqrestore(&ioinfo_busy_lock, flags);

	submit_bio(io_info->writing, bio);

	return 0;
}

/**
 * submit_batched: Submit a batch of bios we've been saving up.
 *
 * Submit a batch. The submit function can wait on I/O, so we have
 * simple locking to avoid infinite recursion.
 */
static int submit_batched(void)
{
	static int running_already = 0;
	struct io_info *first = NULL;
	unsigned long flags;
	int num_submitted = 0;
	struct backing_dev_info *bdi = NULL;

	if (running_already)
		return 0;

	running_already = 1;
	spin_lock_irqsave(&ioinfo_submit_lock, flags);
	while(!list_empty(&ioinfo_submit_batch)) {
		first = list_entry(ioinfo_submit_batch.next, struct io_info,
				list);
		if (!num_submitted)
			bdi = first->dev->bd_inode->i_mapping->backing_dev_info;
		list_del_init(&first->list);
		atomic_dec(&submit_batch);
		spin_unlock_irqrestore(&ioinfo_submit_lock, flags);
		submit(first);
		spin_lock_irqsave(&ioinfo_submit_lock, flags);
		num_submitted++;
		if (num_submitted == submit_batch_size)
			break;
	}
	spin_unlock_irqrestore(&ioinfo_submit_lock, flags);
	running_already = 0;

	if (bdi)
		blk_run_backing_dev(bdi, NULL);
	return num_submitted;
}

/**
 * add_to_batch: Add a page of i/o to our batch for later submission.
 *
 * @io_info: Data structure describing a page of I/O to be done.
 */
static void add_to_batch(struct io_info *io_info)
{
	unsigned long flags;
	int waiting;

	/* Put our prepared I/O struct on the batch list. */
	spin_lock_irqsave(&ioinfo_submit_lock, flags);
	list_add_tail(&io_info->list, &ioinfo_submit_batch);
	waiting = atomic_add_return(1, &submit_batch);
	spin_unlock_irqrestore(&ioinfo_submit_lock, flags);

	if (waiting >= submit_batch_size)
		submit_batched();
}

/**
 * get_io_info_struct: Allocate a struct for recording info on i/o submitted.
 */
static struct io_info *get_io_info_struct(void)
{
	struct io_info *this = NULL;

	do {
		while (atomic_read(&outstanding_io) >= max_outstanding_io)
			do_bio_wait();

		this = kmalloc(sizeof(struct io_info), GFP_ATOMIC);
	} while (!this);

	INIT_LIST_HEAD(&this->list);
	return this;
}

/**
 * toi_do_io: Prepare to do some i/o on a page and submit or batch it.
 *
 * @writing: Whether reading or writing.
 * @bdev: The block device which we're using.
 * @block0: The first sector we're reading or writing.
 * @page: The page on which I/O is being done.
 * @readahead_index: If doing readahead, the index (reset this flag when done).
 * @syncio: Whether the i/o is being done synchronously.
 *
 * Prepare and start a read or write operation.
 *
 * Note that we always work with our own page. If writing, we might be given a
 * compression buffer that will immediately be used to start compressing the
 * next page. For reading, we do readahead and therefore don't know the final
 * address where the data needs to go.
 *
 * Failure? What's that?
 */
static void toi_do_io(int writing, struct block_device *bdev, long block0,
	struct page *page, int readahead_index, int syncio)
{
	struct io_info *io_info = get_io_info_struct();
	unsigned long buffer_virt = 0;
	char *to, *from;

	/* Done before submitting to avoid races. */
	if (syncio)
		waiting_on = io_info;

	/* Copy settings to the io_info struct */
	io_info->writing = writing;
	io_info->dev = bdev;
	io_info->first_block = block0;
	io_info->dest_page = page;
	io_info->readahead_index = readahead_index;

	if (io_info->readahead_index == -1) {
		while (!(buffer_virt = get_zeroed_page(TOI_ATOMIC_GFP)))
			do_bio_wait();

		io_info->bio_page = virt_to_page(buffer_virt);
	} else {
		unsigned long flags;
		int index = io_info->readahead_index / BITS_PER_LONG;
		int bit = io_info->readahead_index - index * BITS_PER_LONG;

		spin_lock_irqsave(&toi_readahead_flags_lock, flags);
		clear_bit(bit, &toi_readahead_flags[index]);
		spin_unlock_irqrestore(&toi_readahead_flags_lock, flags);

		io_info->bio_page = page;
	}

	/*
	 * If writing, copy our data. The data is probably in lowmem, but we cannot be
	 * certain. If there is no compression, we might be passed the actual source
	 * page's address.
	 */
	if (writing) {
		to = (char *) buffer_virt;
		from = kmap_atomic(page, KM_USER1);
		memcpy(to, from, PAGE_SIZE);
		kunmap_atomic(from, KM_USER1);
	}

	/* Submit the page */
	get_page(io_info->bio_page);

	if (syncio)
	 	submit(io_info);
	else
		add_to_batch(io_info);

	atomic_inc(&outstanding_io);

	if (syncio)
		do { do_bio_wait(); } while (waiting_on);
}

/**
 * toi_bdev_page_io: Simpler interface to do directly i/o on a single page.
 *
 * @writing: Whether reading or writing.
 * @bdev: Block device on which we're operating.
 * @pos: Sector at which page to read starts.
 * @page: Page to be read/written.
 *
 * We used to use bread here, but it doesn't correctly handle
 * blocksize != PAGE_SIZE. Now we create a submit_info to get the data we
 * want and use our normal routines (synchronously).
 */
static void toi_bdev_page_io(int writing, struct block_device *bdev,
		long pos, struct page *page)
{
	toi_do_io(writing, bdev, pos, page, -1, 1);
}

/**
 * toi_bio_memory_needed: Report amount of memory needed for block i/o.
 *
 * We want to have at least enough memory so as to have max_outstanding_io
 * transactions on the fly at once. If we can do more, fine.
 */
static int toi_bio_memory_needed(void)
{
	return (max_outstanding_io * (PAGE_SIZE + sizeof(struct request) +
				sizeof(struct bio) + sizeof(struct io_info)));
}

/**
 * toi_set_devinfo: Set the bdev info used for i/o.
 *
 * @info: Pointer to array of struct toi_bdev_info - the list of
 * bdevs and blocks on them in which the image is stored.
 *
 * Set the list of bdevs and blocks in which the image will be stored.
 * Sort of like putting a tape in the cassette player.
 */
static void toi_set_devinfo(struct toi_bdev_info *info)
{
	toi_devinfo = info;
}

/**
 * dump_block_chains: Print the contents of the bdev info array.
 */
static void dump_block_chains(void)
{
	int i;

	for (i = 0; i < toi_writer_posn.num_chains; i++) {
		struct extent *this;

		printk("Chain %d:", i);

		this = (toi_writer_posn.chains + i)->first;

		if (!this)
			printk(" (Empty)");

		while (this) {
			printk(" [%lu-%lu]%s", this->minimum, this->maximum,
					this->next ? "," : "");
			this = this->next;
		}

		printk("\n");
	}

	for (i = 0; i < 3; i++)
		printk("Posn %d: Chain %d, extent %d, offset %lu.\n", i,
				toi_writer_posn_save[i].chain_num,
				toi_writer_posn_save[i].extent_num,
				toi_writer_posn_save[i].offset);
}

/**
 * go_next_page: Skip blocks to the start of the next page.
 *
 * Go forward one page, or two if extra_page_forward is set. It only gets
 * set at the start of reading the image header, to skip the first page
 * of the header, which is read without using the extent chains.
 */
static int go_next_page(void)
{
	int i, max = (toi_writer_posn.current_chain == -1) ? 1 :
	  toi_devinfo[toi_writer_posn.current_chain].blocks_per_page;

	for (i = 0; i < max; i++)
		toi_extent_state_next(&toi_writer_posn);

	if (toi_extent_state_eof(&toi_writer_posn)) {
		printk("Extent state eof. "
			"Expected compression ratio too optimistic?\n");
		dump_block_chains();
		return -ENODATA;
	}

	if (extra_page_forward) {
		extra_page_forward = 0;
		return go_next_page();
	}

	return 0;
}

/**
 * set_extra_page_forward: Make us skip an extra page on next go_next_page.
 *
 * Used in reading header, to jump to 2nd page after getting 1st page
 * direct from image header.
 */
static void set_extra_page_forward(void)
{
	extra_page_forward = 1;
}

/**
 * toi_bio_rw_page: Do i/o on the next disk page in the image.
 *
 * @writing: Whether reading or writing.
 * @page: Page to do i/o on.
 * @readahead_index: -1 or the index in the readahead ring.
 *
 * Submit a page for reading or writing, possibly readahead.
 */
static int toi_bio_rw_page(int writing, struct page *page,
		int readahead_index)
{
	struct toi_bdev_info *dev_info;

	if (test_action_state(TOI_TEST_FILTER_SPEED))
		return 0;
		
	if (go_next_page()) {
		printk("Failed to advance a page in the extent data.\n");
		return -ENODATA;
	}

	if (current_stream == 0 && writing &&
		toi_writer_posn.current_chain == toi_writer_posn_save[2].chain_num &&
		toi_writer_posn.current_offset == toi_writer_posn_save[2].offset) {
		dump_block_chains();
		BUG();
	}

	dev_info = &toi_devinfo[toi_writer_posn.current_chain];

	toi_do_io(writing, dev_info->bdev,
		toi_writer_posn.current_offset <<
			dev_info->bmap_shift,
		page, readahead_index, 0);

	return 0;
}

/**
 * toi_rw_init: Prepare to read or write a stream in the image.
 *
 * @writing: Whether reading or writing.
 * @stream number: Section of the image being processed.
 */
static int toi_rw_init(int writing, int stream_number)
{
	toi_header_bytes_used = 0;

	toi_extent_state_restore(&toi_writer_posn,
			&toi_writer_posn_save[stream_number]);

	toi_writer_buffer_posn = writing ? 0 : PAGE_SIZE;

	current_stream = stream_number;

	readahead_index = ra_submit_index = -1;

	pr_index = 0;

	return 0;
}

/**
 * toi_read_header_init: Prepare to read the image header.
 *
 * Reset readahead indices prior to starting to read a section of the image.
 */
static void toi_read_header_init(void)
{
	readahead_index = ra_submit_index = -1;
}

/**
 * toi_rw_cleanup: Cleanup after i/o.
 *
 * @writing: Whether we were reading or writing.
 */
static int toi_rw_cleanup(int writing)
{
	if (writing && toi_bio_rw_page(WRITE,
			virt_to_page(toi_writer_buffer), -1))
		return -EIO;

	if (writing && current_stream == 2)
		toi_extent_state_save(&toi_writer_posn,
				&toi_writer_posn_save[1]);

	toi_finish_all_io();

	if (!writing)
		while (readahead_index != ra_submit_index) {
			toi_cleanup_readahead(readahead_index);
			readahead_index++;
			if (readahead_index == max_outstanding_io)
				readahead_index = 0;
		}

	current_stream = 0;

	printk("Waited on I/O %d times.\n", atomic_read(&toi_bio_waits));
	atomic_set(&toi_bio_waits, 0);
	return 0;
}

/**
 * toi_bio_read_page_with_readahead: Read a disk page with readahead.
 *
 * Read a page from disk, submitting readahead and cleaning up finished i/o
 * while we wait for the page we're after.
 */
static int toi_bio_read_page_with_readahead(void)
{
	static int last_result;
	unsigned long *virt;

	if (readahead_index == -1) {
		last_result = 0;
		readahead_index = ra_submit_index = 0;
	}

	/* Start a new readahead? */
	if (last_result) {
		/* We failed to submit a read, and have cleaned up
		 * all the readahead previously submitted */
		if (ra_submit_index == readahead_index) {
			abort_hibernate(TOI_FAILED_IO, "Failed to submit"
				" a read and no readahead left.");
			return -EIO;
		}
		goto wait;
	}

	do {
		if (toi_prepare_readahead(ra_submit_index))
			break;

		last_result = toi_bio_rw_page(READ,
			toi_ra_pages[ra_submit_index],
			ra_submit_index);

		if (last_result) {
			printk("Begin read chunk for page %d returned %d.\n",
				ra_submit_index, last_result);
			toi_cleanup_readahead(ra_submit_index);
			break;
		}

		ra_submit_index++;

		if (ra_submit_index == max_outstanding_io)
			ra_submit_index = 0;

	} while((!last_result) && (ra_submit_index != readahead_index) &&
			(!toi_readahead_ready(readahead_index)));

wait:
	toi_wait_on_readahead(readahead_index);

	virt = kmap_atomic(toi_ra_pages[readahead_index], KM_USER1);
	memcpy(toi_writer_buffer, virt, PAGE_SIZE);
	kunmap_atomic(virt, KM_USER1);

	toi_cleanup_readahead(readahead_index);

	readahead_index++;
	if (readahead_index == max_outstanding_io)
		readahead_index = 0;

	return 0;
}

/*
 * toi_rw_buffer: Combine smaller buffers into PAGE_SIZE I/O.
 *
 * @writing: Bool - whether writing (or reading).
 * @buffer: The start of the buffer to write or fill.
 * @buffer_size: The size of the buffer to write or fill.
 */
static int toi_rw_buffer(int writing, char *buffer, int buffer_size)
{
	int bytes_left = buffer_size;

	while (bytes_left) {
		char *source_start = buffer + buffer_size - bytes_left;
		char *dest_start = toi_writer_buffer + toi_writer_buffer_posn;
		int capacity = PAGE_SIZE - toi_writer_buffer_posn;
		char *to = writing ? dest_start : source_start;
		char *from = writing ? source_start : dest_start;

		if (bytes_left <= capacity) {
			memcpy(to, from, bytes_left);
			toi_writer_buffer_posn += bytes_left;
			toi_header_bytes_used += bytes_left;
			return 0;
		}

		/* Complete this page and start a new one */
		memcpy(to, from, capacity);
		bytes_left -= capacity;
		toi_header_bytes_used += capacity;

		if (!writing) {
			if (toi_bio_read_page_with_readahead())
				return -EIO;
		} else if (toi_bio_rw_page(WRITE,
					virt_to_page(toi_writer_buffer),
					-1))
				return -EIO;

		toi_writer_buffer_posn = 0;
		toi_cond_pause(0, NULL);
	}

	return 0;
}

/**
 * toi_bio_read_page - read a page of the image.
 *
 * @pfn: The pfn where the data belongs.
 * @buffer_page: The page containing the (possibly compressed) data.
 * @buf_size: The number of bytes on @buffer_page used.
 *
 * Read a (possibly compressed) page from the image, into buffer_page,
 * returning its pfn and the buffer size.
 */
static int toi_bio_read_page(unsigned long *pfn, struct page *buffer_page,
		unsigned int *buf_size)
{
	int result = 0;
	char *buffer_virt = kmap(buffer_page);

	pr_index++;

	while (!mutex_trylock(&toi_bio_mutex))
		do_bio_wait();

	if (toi_rw_buffer(READ, (char *) pfn, sizeof(unsigned long)) ||
	    toi_rw_buffer(READ, (char *) buf_size, sizeof(int)) ||
	    toi_rw_buffer(READ, buffer_virt, *buf_size)) {
		abort_hibernate(TOI_FAILED_IO, "Read of data failed.");
		result = 1;
	} else
		PR_DEBUG("%d: PFN %ld, %d bytes.\n", pr_index, *pfn, *buf_size);

	mutex_unlock(&toi_bio_mutex);
	kunmap(buffer_page);
	return result;
}

/**
 * toi_bio_write_page - Write a page of the image.
 *
 * @pfn: The pfn where the data belongs.
 * @buffer_page: The page containing the (possibly compressed) data.
 * @buf_size: The number of bytes on @buffer_page used.
 *
 * Write a (possibly compressed) page to the image from the buffer, together
 * with it's index and buffer size.
 */
static int toi_bio_write_page(unsigned long pfn, struct page *buffer_page,
		unsigned int buf_size)
{
	char *buffer_virt = kmap(buffer_page);
	int result = 0;

	pr_index++;

	while (!mutex_trylock(&toi_bio_mutex))
		do_bio_wait();

	if (toi_rw_buffer(WRITE, (char *) &pfn, sizeof(unsigned long)) ||
	    toi_rw_buffer(WRITE, (char *) &buf_size, sizeof(int)) ||
	    toi_rw_buffer(WRITE, buffer_virt, buf_size))
		result = -EIO;

	PR_DEBUG("%d: Index %ld, %d bytes. Result %d.\n", pr_index, pfn,
			buf_size, result);

	mutex_unlock(&toi_bio_mutex);
	kunmap(buffer_page);
	return result;
}

/**
 * toi_rw_header_chunk: Read or write a portion of the image header.
 *
 * @writing: Whether reading or writing.
 * @owner: The module for which we're writing. Used for confirming that modules
 * don't use more header space than they asked for.
 * @buffer: Address of the data to write.
 * @buffer_size: Size of the data buffer.
 */
static int toi_rw_header_chunk(int writing,
		struct toi_module_ops *owner,
		char *buffer, int buffer_size)
{
	if (owner) {
		owner->header_used += buffer_size;
		if (owner->header_used > owner->header_requested) {
			printk(KERN_EMERG "TuxOnIce module %s is using more"
				"header space (%u) than it requested (%u).\n",
				owner->name,
				owner->header_used,
				owner->header_requested);
			return buffer_size;
		}
	}

	return toi_rw_buffer(writing, buffer, buffer_size);
}

/**
 * write_header_chunk_finish: Flush any buffered header data.
 */
static int write_header_chunk_finish(void)
{
	if (!toi_writer_buffer_posn)
		return 0;

	return toi_bio_rw_page(WRITE, virt_to_page(toi_writer_buffer),
		-1) ? -EIO : 0;
}

/**
 * toi_bio_storage_needed: Get the amount of storage needed for my fns.
 */
static int toi_bio_storage_needed(void)
{
	return 2 * sizeof(int);
}

/**
 * toi_bio_save_config_info: Save block i/o config to image header.
 *
 * @buf: PAGE_SIZE'd buffer into which data should be saved.
 */
static int toi_bio_save_config_info(char *buf)
{
	int *ints = (int *) buf;
	ints[0] = max_outstanding_io;
	ints[1] = submit_batch_size;
	return 2 * sizeof(int);
}

/**
 * toi_bio_load_config_info: Restore block i/o config.
 *
 * @buf: Data to be reloaded.
 * @size: Size of the buffer saved.
 */
static void toi_bio_load_config_info(char *buf, int size)
{
	int *ints = (int *) buf;
	max_outstanding_io  = ints[0];
	submit_batch_size = ints[1];
}

/**
 * toi_bio_initialise: Initialise bio code at start of some action.
 *
 * @starting_cycle: Whether starting a hibernation cycle, or just reading or
 * writing a sysfs value.
 */
static int toi_bio_initialise(int starting_cycle)
{
	toi_writer_buffer = (char *) get_zeroed_page(TOI_ATOMIC_GFP);

	return toi_writer_buffer ? 0 : -ENOMEM;
}

/**
 * toi_bio_cleanup: Cleanup after some action.
 *
 * @finishing_cycle: Whether completing a cycle.
 */
static void toi_bio_cleanup(int finishing_cycle)
{
	if (toi_writer_buffer) {
		free_page((unsigned long) toi_writer_buffer);
		toi_writer_buffer = NULL;
	}
}

struct toi_bio_ops toi_bio_ops = {
	.bdev_page_io = toi_bdev_page_io,
	.finish_all_io = toi_finish_all_io,
	.forward_one_page = go_next_page,
	.set_extra_page_forward = set_extra_page_forward,
	.set_devinfo = toi_set_devinfo,
	.read_page = toi_bio_read_page,
	.write_page = toi_bio_write_page,
	.rw_init = toi_rw_init,
	.rw_cleanup = toi_rw_cleanup,
	.read_header_init = toi_read_header_init,
	.rw_header_chunk = toi_rw_header_chunk,
	.write_header_chunk_finish = write_header_chunk_finish,
};

static struct toi_sysfs_data sysfs_params[] = {
	{ TOI_ATTR("max_outstanding_io", SYSFS_RW),
	  SYSFS_INT(&max_outstanding_io, 16, MAX_OUTSTANDING_IO, 0),
	},

	{ TOI_ATTR("submit_batch_size", SYSFS_RW),
	  SYSFS_INT(&submit_batch_size, 16, SUBMIT_BATCH_SIZE, 0),
	}
};

static struct toi_module_ops toi_blockwriter_ops =
{
	.name					= "lowlevel i/o",
	.type					= MISC_HIDDEN_MODULE,
	.directory				= "block_io",
	.module					= THIS_MODULE,
	.memory_needed				= toi_bio_memory_needed,
	.storage_needed				= toi_bio_storage_needed,
	.save_config_info			= toi_bio_save_config_info,
	.load_config_info			= toi_bio_load_config_info,
	.initialise				= toi_bio_initialise,
	.cleanup				= toi_bio_cleanup,

	.sysfs_data		= sysfs_params,
	.num_sysfs_entries	= sizeof(sysfs_params) / sizeof(struct toi_sysfs_data),
};

/**
 * toi_block_io_load: Load time routine for block i/o module.
 *
 * Register block i/o ops and sysfs entries.
 */
static __init int toi_block_io_load(void)
{
	return toi_register_module(&toi_blockwriter_ops);
}

#ifdef CONFIG_TOI_FILE_EXPORTS
EXPORT_SYMBOL_GPL(toi_read_fd);
#endif
#if defined(CONFIG_TOI_FILE_EXPORTS) || defined(CONFIG_TOI_SWAP_EXPORTS)
EXPORT_SYMBOL_GPL(toi_writer_posn);
EXPORT_SYMBOL_GPL(toi_writer_posn_save);
EXPORT_SYMBOL_GPL(toi_writer_buffer);
EXPORT_SYMBOL_GPL(toi_writer_buffer_posn);
EXPORT_SYMBOL_GPL(toi_header_bytes_used);
EXPORT_SYMBOL_GPL(toi_bio_ops);
#endif
#ifdef MODULE
static __exit void toi_block_io_unload(void)
{
	toi_unregister_module(&toi_blockwriter_ops);
}

module_init(toi_block_io_load);
module_exit(toi_block_io_unload);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Nigel Cunningham");
MODULE_DESCRIPTION("TuxOnIce block io functions");
#else
late_initcall(toi_block_io_load);
#endif
