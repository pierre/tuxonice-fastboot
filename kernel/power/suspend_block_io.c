/*
 * kernel/power/suspend_block_io.c
 *
 * Copyright (C) 2004-2007 Nigel Cunningham (nigel at suspend2 net)
 *
 * Distributed under GPLv2.
 * 
 * This file contains block io functions for suspend2. These are
 * used by the swapwriter and it is planned that they will also
 * be used by the NFSwriter.
 *
 */

#include <linux/blkdev.h>
#include <linux/syscalls.h>
#include <linux/suspend.h>

#include "suspend.h"
#include "sysfs.h"
#include "modules.h"
#include "prepare_image.h"
#include "block_io.h"
#include "ui.h"

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

static volatile unsigned long suspend_readahead_flags[
	DIV_ROUND_UP(MAX_OUTSTANDING_IO, BITS_PER_LONG)];
static spinlock_t suspend_readahead_flags_lock = SPIN_LOCK_UNLOCKED;
static struct page *suspend_readahead_pages[MAX_OUTSTANDING_IO];
static int readahead_index, readahead_submit_index;

static int current_stream;
/* 0 = Header, 1 = Pageset1, 2 = Pageset2 */
struct extent_iterate_saved_state suspend_writer_posn_save[3];

/* Pointer to current entry being loaded/saved. */
struct extent_iterate_state suspend_writer_posn;

/* Not static, so that the allocators can setup and complete
 * writing the header */
char *suspend_writer_buffer;
int suspend_writer_buffer_posn;

int suspend_read_fd;

static struct suspend_bdev_info *suspend_devinfo;

int suspend_header_bytes_used = 0;

DEFINE_MUTEX(suspend_bio_mutex);

/*
 * __suspend_bio_cleanup_one
 * 
 * Description: Clean up after completing I/O on a page.
 * Arguments:	struct io_info:	Data for I/O to be completed.
 */
static void __suspend_bio_cleanup_one(struct io_info *io_info)
{
	suspend_message(SUSPEND_WRITER, SUSPEND_HIGH, 0,
		"Cleanup IO: [%p]\n", io_info);

	if (!io_info->writing && io_info->readahead_index == -1) {
		char *from, *to;
		/*
		 * Copy the page we read into the buffer our caller provided.
		 */
		to = (char *) kmap(io_info->dest_page);
		from = (char *) kmap(io_info->bio_page);
		memcpy(to, from, PAGE_SIZE);
		kunmap(io_info->dest_page);
		kunmap(io_info->bio_page);
	}

	put_page(io_info->bio_page);
	if (io_info->writing || io_info->readahead_index == -1)
		__free_page(io_info->bio_page);
	
	bio_put(io_info->sys_struct);
	io_info->sys_struct = NULL;
}

/* __suspend_io_cleanup
 */

static void suspend_bio_cleanup_one(void *data)
{
	struct io_info *io_info = (struct io_info *) data;
	int readahead_index;
	unsigned long flags;

	readahead_index = io_info->readahead_index;
	list_del_init(&io_info->list);
	__suspend_bio_cleanup_one(io_info);

	if (readahead_index > -1) {
		int index = readahead_index/BITS_PER_LONG;
		int bit = readahead_index - (index * BITS_PER_LONG);
		spin_lock_irqsave(&suspend_readahead_flags_lock, flags);
		set_bit(bit, &suspend_readahead_flags[index]);
		spin_unlock_irqrestore(&suspend_readahead_flags_lock, flags);
	}

	if (waiting_on == io_info)
		waiting_on = NULL;
	kfree(io_info);
	atomic_dec(&outstanding_io);
}

/* suspend_cleanup_some_completed_io
 *
 * NB: This is designed so that multiple callers can be in here simultaneously.
 */

static void suspend_cleanup_some_completed_io(void)
{
	int num_cleaned = 0;
	struct io_info *first;
	unsigned long flags;

	spin_lock_irqsave(&ioinfo_ready_lock, flags);
	while(!list_empty(&ioinfo_ready_for_cleanup)) {
		first = list_entry(ioinfo_ready_for_cleanup.next,
				struct io_info, list);

		list_del_init(&first->list);

		spin_unlock_irqrestore(&ioinfo_ready_lock, flags);
		suspend_bio_cleanup_one((void *) first);
		spin_lock_irqsave(&ioinfo_ready_lock, flags);

		num_cleaned++;
		if (num_cleaned == submit_batch_size)
			break;
	}
	spin_unlock_irqrestore(&ioinfo_ready_lock, flags);
}

/* do_bio_wait
 *
 * Actions taken when we want some I/O to get run.
 * 
 * Submit any I/O that's batched up (if we're not already doing
 * that, unplug queues, schedule and clean up whatever we can.
 */
static void do_bio_wait(void)
{
	submit_batched();
	io_schedule();
	suspend_cleanup_some_completed_io();
}

/*
 * suspend_finish_all_io
 *
 * Description:	Finishes all IO and frees all IO info struct pages.
 */
static void suspend_finish_all_io(void)
{
	/* Wait for all I/O to complete. */
	while (atomic_read(&outstanding_io))
		do_bio_wait();
}

/*
 * wait_on_readahead
 *
 * Wait until a particular readahead is ready.
 */
static void suspend_wait_on_readahead(int readahead_index)
{
	int index = readahead_index / BITS_PER_LONG;
	int bit = readahead_index - index * BITS_PER_LONG;

	/* read_ahead_index is the one we want to return */
	while (!test_bit(bit, &suspend_readahead_flags[index]))
		do_bio_wait();
}

/*
 * readahead_done
 *
 * Returns whether the readahead requested is ready.
 */

static int suspend_readahead_ready(int readahead_index)
{
	int index = readahead_index / BITS_PER_LONG;
	int bit = readahead_index - (index * BITS_PER_LONG);

	return test_bit(bit, &suspend_readahead_flags[index]);
}

/* suspend_readahead_prepare
 * Set up for doing readahead on an image */
static int suspend_prepare_readahead(int index)
{
	unsigned long new_page = get_zeroed_page(GFP_ATOMIC | __GFP_NOWARN);

	if(!new_page)
		return -ENOMEM;

	suspend_readahead_pages[index] = virt_to_page(new_page);
	return 0;
}

/* suspend_readahead_cleanup
 * Clean up structures used for readahead */
static void suspend_cleanup_readahead(int page)
{
	__free_page(suspend_readahead_pages[page]);
	suspend_readahead_pages[page] = 0;
	return;
}

/*
 * suspend_end_bio
 *
 * Description:	Function called by block driver from interrupt context when I/O
 * 		is completed. This is the reason we use spinlocks in
 * 		manipulating the io_info lists. 		
 * 		Nearly the fs/buffer.c version, but we want to mark the page as 
 * 		done in our own structures too.
 */

static int suspend_end_bio(struct bio *bio, unsigned int num, int err)
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
	bio->bi_end_io = suspend_end_bio;
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

/* 
 * submit a batch. The submit function can wait on I/O, so we have
 * simple locking to avoid infinite recursion.
 */
static int submit_batched(void)
{
	static int running_already = 0;
	struct io_info *first;
	unsigned long flags;
	int num_submitted = 0;

	if (running_already)
		return 0;

	running_already = 1;
	spin_lock_irqsave(&ioinfo_submit_lock, flags);
	while(!list_empty(&ioinfo_submit_batch)) {
		first = list_entry(ioinfo_submit_batch.next, struct io_info,
				list);
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

	return num_submitted;
}

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

/*
 * get_io_info_struct
 *
 * Description:	Get an I/O struct.
 * Returns:	Pointer to the struct prepared for use.
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

/*
 * suspend_do_io
 *
 * Description:	Prepare and start a read or write operation.
 * 		Note that we use our own buffer for reading or writing.
 * 		This simplifies doing readahead and asynchronous writing.
 * 		We can begin a read without knowing the location into which
 * 		the data will eventually be placed, and the buffer passed
 * 		for a write can be reused immediately (essential for the
 * 		modules system).
 * 		Failure? What's that?
 * Returns:	The io_info struct created.
 */
static int suspend_do_io(int writing, struct block_device *bdev, long block0,
	struct page *page, int readahead_index, int syncio)
{
	struct io_info *io_info;
	unsigned long buffer_virt = 0;
	char *to, *from;

	io_info = get_io_info_struct();

	/* Done before submitting to avoid races. */
	if (syncio)
		waiting_on = io_info;

	/* Get our local buffer */
	suspend_message(SUSPEND_WRITER, SUSPEND_HIGH, 1,
			"Start_IO: [%p]", io_info);
	
	/* Copy settings to the io_info struct */
	io_info->writing = writing;
	io_info->dev = bdev;
	io_info->first_block = block0;
	io_info->dest_page = page;
	io_info->readahead_index = readahead_index;

	if (io_info->readahead_index == -1) {
		while (!(buffer_virt = get_zeroed_page(GFP_ATOMIC | __GFP_NOWARN)))
			do_bio_wait();

		suspend_message(SUSPEND_WRITER, SUSPEND_HIGH, 0,
				"[ALLOC BUFFER]->%d",
				real_nr_free_pages(all_zones_mask));
		io_info->bio_page = virt_to_page(buffer_virt);
	} else {
		unsigned long flags;
		int index = io_info->readahead_index / BITS_PER_LONG;
		int bit = io_info->readahead_index - index * BITS_PER_LONG;

		spin_lock_irqsave(&suspend_readahead_flags_lock, flags);
		clear_bit(bit, &suspend_readahead_flags[index]);
		spin_unlock_irqrestore(&suspend_readahead_flags_lock, flags);

		io_info->bio_page = page;
	}

	/* If writing, copy our data. The data is probably in
	 * lowmem, but we cannot be certain. If there is no
	 * compression/encryption, we might be passed the
	 * actual source page's address. */
	if (writing) {
		to = (char *) buffer_virt;
		from = kmap_atomic(page, KM_USER1);
		memcpy(to, from, PAGE_SIZE);
		kunmap_atomic(from, KM_USER1);
	}

	/* Submit the page */
	get_page(io_info->bio_page);
	
	suspend_message(SUSPEND_WRITER, SUSPEND_HIGH, 1,
		"-> (PRE BRW) %d\n", real_nr_free_pages(all_zones_mask));

	if (syncio)
	 	submit(io_info);
	else
		add_to_batch(io_info);
	
	atomic_inc(&outstanding_io);
	
	if (syncio)
		do { do_bio_wait(); } while (waiting_on);

	return 0;
}

/* We used to use bread here, but it doesn't correctly handle
 * blocksize != PAGE_SIZE. Now we create a submit_info to get the data we
 * want and use our normal routines (synchronously).
 */

static int suspend_bdev_page_io(int writing, struct block_device *bdev,
		long pos, struct page *page)
{
	return suspend_do_io(writing, bdev, pos, page, -1, 1);
}

static int suspend_bio_memory_needed(void)
{
	/* We want to have at least enough memory so as to have
	 * max_outstanding_io transactions on the fly at once. If we 
	 * can do more, fine. */
	return (max_outstanding_io * (PAGE_SIZE + sizeof(struct request) +
				sizeof(struct bio) + sizeof(struct io_info)));
}

static void suspend_set_devinfo(struct suspend_bdev_info *info)
{
	suspend_devinfo = info;
}

static void dump_block_chains(void)
{
	int i;

	for (i = 0; i < suspend_writer_posn.num_chains; i++) {
		struct extent *this;

		printk("Chain %d:", i);

		this = (suspend_writer_posn.chains + i)->first;

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
				suspend_writer_posn_save[i].chain_num,
				suspend_writer_posn_save[i].extent_num,
				suspend_writer_posn_save[i].offset);
}
static int forward_extra_blocks(void)
{
	int i;

	for (i = 1; i < suspend_devinfo[suspend_writer_posn.current_chain].
							blocks_per_page; i++)
		suspend_extent_state_next(&suspend_writer_posn);

	if (suspend_extent_state_eof(&suspend_writer_posn)) {
		printk("Extent state eof.\n");
		dump_block_chains();
		return -ENODATA;
	}

	return 0;
}

static int forward_one_page(void)
{
	int at_start = (suspend_writer_posn.current_chain == -1);

	/* Have to go forward one to ensure we're on the right chain,
	 * before we can know how many more blocks to skip.*/
	suspend_extent_state_next(&suspend_writer_posn);

	if (!at_start && forward_extra_blocks())
		return -ENODATA;

	if (extra_page_forward) {
		extra_page_forward = 0;
		return forward_one_page();
	}

	return 0;
}

/* Used in reading header, to jump to 2nd page after getting 1st page
 * direct from image header. */
static void set_extra_page_forward(void)
{
	extra_page_forward = 1;
}

static int suspend_bio_rw_page(int writing, struct page *page,
		int readahead_index, int sync)
{
	struct suspend_bdev_info *dev_info;

	if (test_action_state(SUSPEND_TEST_FILTER_SPEED))
		return 0;
		
	if (forward_one_page()) {
		printk("Failed to advance a page in the extent data.\n");
		return -ENODATA;
	}

	if (current_stream == 0 && writing &&
		suspend_writer_posn.current_chain == suspend_writer_posn_save[2].chain_num &&
		suspend_writer_posn.current_offset == suspend_writer_posn_save[2].offset) {
		dump_block_chains();
		BUG();
	}

	dev_info = &suspend_devinfo[suspend_writer_posn.current_chain];

	return suspend_do_io(writing, dev_info->bdev,
		suspend_writer_posn.current_offset <<
			dev_info->bmap_shift,
		page, readahead_index, sync);
}

static int suspend_rw_init(int writing, int stream_number)
{
	suspend_header_bytes_used = 0;

	suspend_extent_state_restore(&suspend_writer_posn,
			&suspend_writer_posn_save[stream_number]);

	suspend_writer_buffer_posn = writing ? 0 : PAGE_SIZE;

	current_stream = stream_number;

	readahead_index = readahead_submit_index = -1;

	pr_index = 0;

	return 0;
}

static void suspend_read_header_init(void)
{
	readahead_index = readahead_submit_index = -1;
}

static int suspend_rw_cleanup(int writing)
{
	if (writing && suspend_bio_rw_page(WRITE,
			virt_to_page(suspend_writer_buffer), -1, 0))
		return -EIO;

	if (writing && current_stream == 2)
		suspend_extent_state_save(&suspend_writer_posn,
				&suspend_writer_posn_save[1]);
	
	suspend_finish_all_io();
	
	if (!writing)
		while (readahead_index != readahead_submit_index) {
			suspend_cleanup_readahead(readahead_index);
			readahead_index++;
			if (readahead_index == max_outstanding_io)
				readahead_index = 0;
		}

	current_stream = 0;

	return 0;
}

static int suspend_bio_read_page_with_readahead(void)
{
	static int last_result;
	unsigned long *virt;

	if (readahead_index == -1) {
		last_result = 0;
		readahead_index = readahead_submit_index = 0;
	}

	/* Start a new readahead? */
	if (last_result) {
		/* We failed to submit a read, and have cleaned up
		 * all the readahead previously submitted */
		if (readahead_submit_index == readahead_index) {
			abort_suspend(SUSPEND_FAILED_IO, "Failed to submit"
				" a read and no readahead left.\n");
			return -EIO;
		}
		goto wait;
	}
	
	do {
		if (suspend_prepare_readahead(readahead_submit_index))
			break;

		last_result = suspend_bio_rw_page(READ,
			suspend_readahead_pages[readahead_submit_index], 
			readahead_submit_index, SUSPEND_ASYNC);
		if (last_result) {
			printk("Begin read chunk for page %d returned %d.\n",
				readahead_submit_index, last_result);
			suspend_cleanup_readahead(readahead_submit_index);
			break;
		}

		readahead_submit_index++;

		if (readahead_submit_index == max_outstanding_io)
			readahead_submit_index = 0;

	} while((!last_result) && (readahead_submit_index != readahead_index) &&
			(!suspend_readahead_ready(readahead_index)));

wait:
	suspend_wait_on_readahead(readahead_index);

	virt = kmap_atomic(suspend_readahead_pages[readahead_index], KM_USER1);
	memcpy(suspend_writer_buffer, virt, PAGE_SIZE);
	kunmap_atomic(virt, KM_USER1);

	suspend_cleanup_readahead(readahead_index);

	readahead_index++;
	if (readahead_index == max_outstanding_io)
		readahead_index = 0;

	return 0;
}

/*
 *
 */

static int suspend_rw_buffer(int writing, char *buffer, int buffer_size)
{
	int bytes_left = buffer_size;

	/* Read/write a chunk of the header */
	while (bytes_left) {
		char *source_start = buffer + buffer_size - bytes_left;
		char *dest_start = suspend_writer_buffer + suspend_writer_buffer_posn;
		int capacity = PAGE_SIZE - suspend_writer_buffer_posn;
		char *to = writing ? dest_start : source_start;
		char *from = writing ? source_start : dest_start;

		if (bytes_left <= capacity) {
			if (test_debug_state(SUSPEND_HEADER))
				printk("Copy %d bytes %d-%d from %p to %p.\n",
						bytes_left,
						suspend_header_bytes_used,
						suspend_header_bytes_used + bytes_left,
						from, to);
			memcpy(to, from, bytes_left);
			suspend_writer_buffer_posn += bytes_left;
			suspend_header_bytes_used += bytes_left;
			return 0;
		}

		/* Complete this page and start a new one */
		if (test_debug_state(SUSPEND_HEADER))
			printk("Copy %d bytes (%d-%d) from %p to %p.\n",
					capacity,
					suspend_header_bytes_used,
					suspend_header_bytes_used + capacity,
					from, to);
		memcpy(to, from, capacity);
		bytes_left -= capacity;
		suspend_header_bytes_used += capacity;

		if (!writing) {
			if (test_suspend_state(SUSPEND_TRY_RESUME_RD))
				sys_read(suspend_read_fd,
					suspend_writer_buffer, BLOCK_SIZE);
			else
				if (suspend_bio_read_page_with_readahead())
					return -EIO;
		} else if (suspend_bio_rw_page(WRITE,
					virt_to_page(suspend_writer_buffer),
					-1, SUSPEND_ASYNC))
				return -EIO;

		suspend_writer_buffer_posn = 0;
		suspend_cond_pause(0, NULL);
	}

	return 0;
}

/*
 * suspend_bio_read_page
 *
 * Read a (possibly compressed and/or encrypted) page from the image,
 * into buffer_page, returning it's index and the buffer size.
 *
 * If asynchronous I/O is requested, use readahead.
 */

static int suspend_bio_read_page(unsigned long *index, struct page *buffer_page,
		unsigned int *buf_size)
{
	int result;
	char *buffer_virt = kmap(buffer_page);

	pr_index++;

	while (!mutex_trylock(&suspend_bio_mutex))
		do_bio_wait();
	
	if ((result = suspend_rw_buffer(READ, (char *) index,
			sizeof(unsigned long)))) {
		abort_suspend(SUSPEND_FAILED_IO,
				"Read of index returned %d.\n", result);
		goto out;
	}

	if ((result = suspend_rw_buffer(READ, (char *) buf_size, sizeof(int)))) {
		abort_suspend(SUSPEND_FAILED_IO,
				"Read of buffer size is %d.\n", result);
		goto out;
	}

	result = suspend_rw_buffer(READ, buffer_virt, *buf_size);
	if (result)
		abort_suspend(SUSPEND_FAILED_IO,
				"Read of data returned %d.\n", result);

	PR_DEBUG("%d: Index %ld, %d bytes.\n", pr_index, *index, *buf_size);
out:
	mutex_unlock(&suspend_bio_mutex);
	kunmap(buffer_page);
	if (result)
		abort_suspend(SUSPEND_FAILED_IO,
			"Returning %d from suspend_bio_read_page.\n", result);
	return result;
}

/*
 * suspend_bio_write_page
 *
 * Write a (possibly compressed and/or encrypted) page to the image from
 * the buffer, together with it's index and buffer size.
 */

static int suspend_bio_write_page(unsigned long index, struct page *buffer_page,
		unsigned int buf_size)
{
	int result;
	char *buffer_virt = kmap(buffer_page);

	pr_index++;

	while (!mutex_trylock(&suspend_bio_mutex))
		do_bio_wait();
	
	if ((result = suspend_rw_buffer(WRITE, (char *) &index,
					sizeof(unsigned long))))
		goto out;

	if ((result = suspend_rw_buffer(WRITE, (char *) &buf_size, sizeof(int))))
		goto out;

	result = suspend_rw_buffer(WRITE, buffer_virt, buf_size);

	PR_DEBUG("%d: Index %ld, %d bytes.\n", pr_index, index, buf_size);
out:
	mutex_unlock(&suspend_bio_mutex);
	kunmap(buffer_page);
	return result;
}

/*
 * suspend_rw_header_chunk
 *
 * Read or write a portion of the header.
 */

static int suspend_rw_header_chunk(int writing,
		struct suspend_module_ops *owner,
		char *buffer, int buffer_size)
{
	if (owner) {
		owner->header_used += buffer_size;
		if (owner->header_used > owner->header_requested) {
			printk(KERN_EMERG "Suspend2 module %s is using more"
				"header space (%u) than it requested (%u).\n",
				owner->name,
				owner->header_used,
				owner->header_requested);
			return buffer_size;
		}
	}

	return suspend_rw_buffer(writing, buffer, buffer_size);
}

/*
 * write_header_chunk_finish
 *
 * Flush any buffered writes in the section of the image.
 */
static int write_header_chunk_finish(void)
{
	return suspend_bio_rw_page(WRITE, virt_to_page(suspend_writer_buffer),
		-1, 0) ? -EIO : 0;
}

static int suspend_bio_storage_needed(void)
{
	return 2 * sizeof(int);
}

static int suspend_bio_save_config_info(char *buf)
{
	int *ints = (int *) buf;
	ints[0] = max_outstanding_io;
	ints[1] = submit_batch_size;
	return 2 * sizeof(int);
}

static void suspend_bio_load_config_info(char *buf, int size)
{
	int *ints = (int *) buf;
	max_outstanding_io  = ints[0];
	submit_batch_size = ints[1];
}

static int suspend_bio_initialise(int starting_cycle)
{
	suspend_writer_buffer = (char *) get_zeroed_page(GFP_ATOMIC);

	return suspend_writer_buffer ? 0 : -ENOMEM;
}

static void suspend_bio_cleanup(int finishing_cycle)
{
	if (suspend_writer_buffer) {
		free_page((unsigned long) suspend_writer_buffer);
		suspend_writer_buffer = NULL;
	}
}

struct suspend_bio_ops suspend_bio_ops = {
	.bdev_page_io = suspend_bdev_page_io,
	.finish_all_io = suspend_finish_all_io,
	.forward_one_page = forward_one_page,
	.set_extra_page_forward = set_extra_page_forward,
	.set_devinfo = suspend_set_devinfo,
	.read_page = suspend_bio_read_page,
	.write_page = suspend_bio_write_page,
	.rw_init = suspend_rw_init,
	.rw_cleanup = suspend_rw_cleanup,
	.read_header_init = suspend_read_header_init,
	.rw_header_chunk = suspend_rw_header_chunk,
	.write_header_chunk_finish = write_header_chunk_finish,
};

static struct suspend_sysfs_data sysfs_params[] = {
	{ SUSPEND2_ATTR("max_outstanding_io", SYSFS_RW),
	  SYSFS_INT(&max_outstanding_io, 16, MAX_OUTSTANDING_IO, 0),
	},

	{ SUSPEND2_ATTR("submit_batch_size", SYSFS_RW),
	  SYSFS_INT(&submit_batch_size, 16, SUBMIT_BATCH_SIZE, 0),
	}
};

static struct suspend_module_ops suspend_blockwriter_ops = 
{
	.name					= "Block I/O",
	.type					= MISC_MODULE,
	.directory				= "block_io",
	.module					= THIS_MODULE,
	.memory_needed				= suspend_bio_memory_needed,
	.storage_needed				= suspend_bio_storage_needed,
	.save_config_info			= suspend_bio_save_config_info,
	.load_config_info			= suspend_bio_load_config_info,
	.initialise				= suspend_bio_initialise,
	.cleanup				= suspend_bio_cleanup,

	.sysfs_data		= sysfs_params,
	.num_sysfs_entries	= sizeof(sysfs_params) / sizeof(struct suspend_sysfs_data),
};

static __init int suspend_block_io_load(void)
{
	return suspend_register_module(&suspend_blockwriter_ops);
}

#ifdef CONFIG_SUSPEND2_FILE_EXPORTS
EXPORT_SYMBOL_GPL(suspend_read_fd);
#endif
#if defined(CONFIG_SUSPEND2_FILE_EXPORTS) || defined(CONFIG_SUSPEND2_SWAP_EXPORTS)
EXPORT_SYMBOL_GPL(suspend_writer_posn);
EXPORT_SYMBOL_GPL(suspend_writer_posn_save);
EXPORT_SYMBOL_GPL(suspend_writer_buffer);
EXPORT_SYMBOL_GPL(suspend_writer_buffer_posn);
EXPORT_SYMBOL_GPL(suspend_header_bytes_used);
EXPORT_SYMBOL_GPL(suspend_bio_ops);
#endif
#ifdef MODULE
static __exit void suspend_block_io_unload(void)
{
	suspend_unregister_module(&suspend_blockwriter_ops);
}

module_init(suspend_block_io_load);
module_exit(suspend_block_io_unload);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Nigel Cunningham");
MODULE_DESCRIPTION("Suspend2 block io functions");
#else
late_initcall(suspend_block_io_load);
#endif
