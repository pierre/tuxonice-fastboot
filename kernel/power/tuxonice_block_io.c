/*
 * kernel/power/tuxonice_block_io.c
 *
 * Copyright (C) 2004-2008 Nigel Cunningham (nigel at tuxonice net)
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
#include "tuxonice_alloc.h"
#include "tuxonice_io.h"

#define MEMORY_ONLY 1
#define THROTTLE_WAIT 2

/* #define MEASURE_MUTEX_CONTENTION */
#ifndef MEASURE_MUTEX_CONTENTION
#define my_mutex_lock(index, the_lock) mutex_lock(the_lock)
#define my_mutex_unlock(index, the_lock) mutex_unlock(the_lock)
#else
unsigned long mutex_times[2][2][NR_CPUS];
#define my_mutex_lock(index, the_lock) do { \
	int have_mutex; \
	have_mutex = mutex_trylock(the_lock); \
	if (!have_mutex) { \
		mutex_lock(the_lock); \
		mutex_times[index][0][smp_processor_id()]++; \
	} else { \
		mutex_times[index][1][smp_processor_id()]++; \
	}

#define my_mutex_unlock(index, the_lock) \
	mutex_unlock(the_lock); \
} while (0)
#endif

static int target_outstanding_io = 1024;
static int max_outstanding_writes, max_outstanding_reads;

static struct page *bio_queue_head, *bio_queue_tail;
static atomic_t toi_bio_queue_size;
static DEFINE_SPINLOCK(bio_queue_lock);

static int free_mem_throttle, throughput_throttle;
static int more_readahead = 1;
static struct page *readahead_list_head, *readahead_list_tail;
static DECLARE_WAIT_QUEUE_HEAD(readahead_list_wait);

static struct page *waiting_on;

static atomic_t toi_io_in_progress, toi_io_done;
static DECLARE_WAIT_QUEUE_HEAD(num_in_progress_wait);

static int extra_page_forward;

static int current_stream;
/* 0 = Header, 1 = Pageset1, 2 = Pageset2, 3 = End of PS1 */
struct hibernate_extent_iterate_saved_state toi_writer_posn_save[4];
EXPORT_SYMBOL_GPL(toi_writer_posn_save);

/* Pointer to current entry being loaded/saved. */
struct toi_extent_iterate_state toi_writer_posn;
EXPORT_SYMBOL_GPL(toi_writer_posn);

/* Not static, so that the allocators can setup and complete
 * writing the header */
char *toi_writer_buffer;
EXPORT_SYMBOL_GPL(toi_writer_buffer);

int toi_writer_buffer_posn;
EXPORT_SYMBOL_GPL(toi_writer_buffer_posn);

static struct toi_bdev_info *toi_devinfo;

static DEFINE_MUTEX(toi_bio_mutex);
static DEFINE_MUTEX(toi_bio_readahead_mutex);

static struct task_struct *toi_queue_flusher;
static int toi_bio_queue_flush_pages(int dedicated_thread);

#define TOTAL_OUTSTANDING_IO (atomic_read(&toi_io_in_progress) + \
	       atomic_read(&toi_bio_queue_size))

/**
 * set_free_mem_throttle - set the point where we pause to avoid oom.
 *
 * Initially, this value is zero, but when we first fail to allocate memory,
 * we set it (plus a buffer) and thereafter throttle i/o once that limit is
 * reached.
 **/
static void set_free_mem_throttle(void)
{
	int new_throttle = nr_unallocated_buffer_pages() + 256;

	if (new_throttle > free_mem_throttle)
		free_mem_throttle = new_throttle;
}

#define NUM_REASONS 7
static atomic_t reasons[NUM_REASONS];
static char *reason_name[NUM_REASONS] = {
	"readahead not ready",
	"bio allocation",
	"synchronous I/O",
	"toi_bio_get_new_page",
	"memory low",
	"readahead buffer allocation",
	"throughput_throttle",
};

/**
 * do_bio_wait - wait for some TuxOnIce I/O to complete
 * @reason: The array index of the reason we're waiting.
 *
 * Wait for a particular page of I/O if we're after a particular page.
 * If we're not after a particular page, wait instead for all in flight
 * I/O to be completed or for us to have enough free memory to be able
 * to submit more I/O.
 *
 * If we wait, we also update our statistics regarding why we waited.
 **/
static void do_bio_wait(int reason)
{
	struct page *was_waiting_on = waiting_on;

	/* On SMP, waiting_on can be reset, so we make a copy */
	if (was_waiting_on) {
		if (PageLocked(was_waiting_on)) {
			wait_on_page_bit(was_waiting_on, PG_locked);
			atomic_inc(&reasons[reason]);
		}
	} else {
		atomic_inc(&reasons[reason]);

		wait_event(num_in_progress_wait,
			!atomic_read(&toi_io_in_progress) ||
			nr_unallocated_buffer_pages() > free_mem_throttle);
	}
}

/**
 * throttle_if_needed - wait for I/O completion if throttle points are reached
 * @flags: What to check and how to act.
 *
 * Check whether we need to wait for some I/O to complete. We always check
 * whether we have enough memory available, but may also (depending upon
 * @reason) check if the throughput throttle limit has been reached.
 **/
static int throttle_if_needed(int flags)
{
	int free_pages = nr_unallocated_buffer_pages();

	/* Getting low on memory and I/O is in progress? */
	while (unlikely(free_pages < free_mem_throttle) &&
			atomic_read(&toi_io_in_progress)) {
		if (!(flags & THROTTLE_WAIT))
			return -ENOMEM;
		do_bio_wait(4);
		free_pages = nr_unallocated_buffer_pages();
	}

	while (!(flags & MEMORY_ONLY) && throughput_throttle &&
		TOTAL_OUTSTANDING_IO >= throughput_throttle) {
		int result = toi_bio_queue_flush_pages(0);
		if (result)
			return result;
		atomic_inc(&reasons[6]);
		wait_event(num_in_progress_wait,
			!atomic_read(&toi_io_in_progress) ||
			TOTAL_OUTSTANDING_IO < throughput_throttle);
	}

	return 0;
}

/**
 * update_throughput_throttle - update the raw throughput throttle
 * @jif_index: The number of times this function has been called.
 *
 * This function is called twice per second by the core, and used to limit the
 * amount of I/O we submit at once, spreading out our waiting through the
 * whole job and letting userui get an opportunity to do its work.
 *
 * We don't start limiting I/O until 1/2s has gone so that we get a
 * decent sample for our initial limit, and keep updating it because
 * throughput may vary (on rotating media, eg) with our block number.
 *
 * We throttle to 1/10s worth of I/O.
 **/
static void update_throughput_throttle(int jif_index)
{
	int done = atomic_read(&toi_io_done);
	throughput_throttle = done / jif_index / 5;
}

/**
 * toi_finish_all_io - wait for all outstanding i/o to complete
 *
 * Flush any queued but unsubmitted I/O and wait for it all to complete.
 **/
static int toi_finish_all_io(void)
{
	int result = toi_bio_queue_flush_pages(0);
	wait_event(num_in_progress_wait, !TOTAL_OUTSTANDING_IO);
	return result;
}

/**
 * toi_end_bio - bio completion function.
 * @bio: bio that has completed.
 * @err: Error value. Yes, like end_swap_bio_read, we ignore it.
 *
 * Function called by the block driver from interrupt context when I/O is
 * completed. If we were writing the page, we want to free it and will have
 * set bio->bi_private to the parameter we should use in telling the page
 * allocation accounting code what the page was allocated for. If we're
 * reading the page, it will be in the singly linked list made from
 * page->private pointers.
 **/
static void toi_end_bio(struct bio *bio, int err)
{
	struct page *page = bio->bi_io_vec[0].bv_page;

	BUG_ON(!test_bit(BIO_UPTODATE, &bio->bi_flags));

	unlock_page(page);
	bio_put(bio);

	if (waiting_on == page)
		waiting_on = NULL;

	put_page(page);

	if (bio->bi_private)
		toi__free_page((int) ((unsigned long) bio->bi_private) , page);

	bio_put(bio);

	atomic_dec(&toi_io_in_progress);
	atomic_inc(&toi_io_done);

	wake_up(&num_in_progress_wait);
}

/**
 * submit - submit BIO request
 * @writing: READ or WRITE.
 * @dev: The block device we're using.
 * @first_block: The first sector we're using.
 * @page: The page being used for I/O.
 * @free_group: If writing, the group that was used in allocating the page
 * 	and which will be used in freeing the page from the completion
 * 	routine.
 *
 * Based on Patrick Mochell's pmdisk code from long ago: "Straight from the
 * textbook - allocate and initialize the bio. If we're writing, make sure
 * the page is marked as dirty. Then submit it and carry on."
 *
 * If we're just testing the speed of our own code, we fake having done all
 * the hard work and all toi_end_bio immediately.
 **/
static int submit(int writing, struct block_device *dev, sector_t first_block,
		struct page *page, int free_group)
{
	struct bio *bio = NULL;
	int cur_outstanding_io, result;

	/*
	 * Shouldn't throttle if reading - can deadlock in the single
	 * threaded case as pages are only freed when we use the
	 * readahead.
	 */
	if (writing) {
		result = throttle_if_needed(MEMORY_ONLY | THROTTLE_WAIT);
		if (result)
			return result;
	}

	while (!bio) {
		bio = bio_alloc(TOI_ATOMIC_GFP, 1);
		if (!bio) {
			set_free_mem_throttle();
			do_bio_wait(1);
		}
	}

	bio->bi_bdev = dev;
	bio->bi_sector = first_block;
	bio->bi_private = (void *) ((unsigned long) free_group);
	bio->bi_end_io = toi_end_bio;

	if (bio_add_page(bio, page, PAGE_SIZE, 0) < PAGE_SIZE) {
		printk(KERN_DEBUG "ERROR: adding page to bio at %lld\n",
				(unsigned long long) first_block);
		bio_put(bio);
		return -EFAULT;
	}

	bio_get(bio);

	cur_outstanding_io = atomic_add_return(1, &toi_io_in_progress);
	if (writing) {
		if (cur_outstanding_io > max_outstanding_writes)
			max_outstanding_writes = cur_outstanding_io;
	} else {
		if (cur_outstanding_io > max_outstanding_reads)
			max_outstanding_reads = cur_outstanding_io;
	}


	if (unlikely(test_action_state(TOI_TEST_FILTER_SPEED))) {
		/* Fake having done the hard work */
		set_bit(BIO_UPTODATE, &bio->bi_flags);
		toi_end_bio(bio, 0);
	} else
		submit_bio(writing | (1 << BIO_RW_SYNCIO) |
				(1 << BIO_RW_UNPLUG), bio);

	return 0;
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
 **/
static int toi_do_io(int writing, struct block_device *bdev, long block0,
	struct page *page, int is_readahead, int syncio, int free_group)
{
	page->private = 0;

	/* Do here so we don't race against toi_bio_get_next_page_read */
	lock_page(page);

	if (is_readahead) {
		if (readahead_list_head)
			readahead_list_tail->private = (unsigned long) page;
		else
			readahead_list_head = page;

		readahead_list_tail = page;
		wake_up(&readahead_list_wait);
	}

	/* Done before submitting to avoid races. */
	if (syncio)
		waiting_on = page;

	/* Submit the page */
	get_page(page);

	if (submit(writing, bdev, block0, page, free_group))
		return -EFAULT;

	if (syncio)
		do_bio_wait(2);

	return 0;
}

/**
 * toi_bdev_page_io - simpler interface to do directly i/o on a single page
 * @writing: Whether reading or writing.
 * @bdev: Block device on which we're operating.
 * @pos: Sector at which page to read or write starts.
 * @page: Page to be read/written.
 *
 * A simple interface to submit a page of I/O and wait for its completion.
 * The caller must free the page used.
 **/
static int toi_bdev_page_io(int writing, struct block_device *bdev,
		long pos, struct page *page)
{
	return toi_do_io(writing, bdev, pos, page, 0, 1, 0);
}

/**
 * toi_bio_memory_needed - report the amount of memory needed for block i/o
 *
 * We want to have at least enough memory so as to have target_outstanding_io
 * or more transactions on the fly at once. If we can do more, fine.
 **/
static int toi_bio_memory_needed(void)
{
	return target_outstanding_io * (PAGE_SIZE + sizeof(struct request) +
				sizeof(struct bio));
}

/**
 * toi_bio_print_debug_stats - put out debugging info in the buffer provided
 * @buffer: A buffer of size @size into which text should be placed.
 * @size: The size of @buffer.
 *
 * Fill a buffer with debugging info. This is used for both our debug_info sysfs
 * entry and for recording the same info in dmesg.
 **/
static int toi_bio_print_debug_stats(char *buffer, int size)
{
	int len = scnprintf(buffer, size, "- Max outstanding reads %d. Max "
			"writes %d.\n", max_outstanding_reads,
			max_outstanding_writes);

	len += scnprintf(buffer + len, size - len,
		"  Memory_needed: %d x (%lu + %u + %u) = %d bytes.\n",
		target_outstanding_io,
		PAGE_SIZE, (unsigned int) sizeof(struct request),
		(unsigned int) sizeof(struct bio), toi_bio_memory_needed());

#ifdef MEASURE_MUTEX_CONTENTION
	{
	int i;

	len += scnprintf(buffer + len, size - len,
		"  Mutex contention while reading:\n  Contended      Free\n");

	for_each_online_cpu(i)
		len += scnprintf(buffer + len, size - len,
		"  %9lu %9lu\n",
		mutex_times[0][0][i], mutex_times[0][1][i]);

	len += scnprintf(buffer + len, size - len,
		"  Mutex contention while writing:\n  Contended      Free\n");

	for_each_online_cpu(i)
		len += scnprintf(buffer + len, size - len,
		"  %9lu %9lu\n",
		mutex_times[1][0][i], mutex_times[1][1][i]);

	}
#endif

	return len + scnprintf(buffer + len, size - len,
		"  Free mem throttle point reached %d.\n", free_mem_throttle);
}

/**
 * toi_set_devinfo - set the bdev info used for i/o
 * @info: Pointer to an array of struct toi_bdev_info - the list of
 * bdevs and blocks on them in which the image is stored.
 *
 * Set the list of bdevs and blocks in which the image will be stored.
 * Think of them (all together) as one long tape on which the data will be
 * stored.
 **/
static void toi_set_devinfo(struct toi_bdev_info *info)
{
	toi_devinfo = info;
}

/**
 * dump_block_chains - print the contents of the bdev info array.
 **/
static void dump_block_chains(void)
{
	int i;

	for (i = 0; i < toi_writer_posn.num_chains; i++) {
		struct hibernate_extent *this;

		this = (toi_writer_posn.chains + i)->first;

		if (!this)
			continue;

		printk(KERN_DEBUG "Chain %d:", i);

		while (this) {
			printk(" [%lu-%lu]%s", this->start,
					this->end, this->next ? "," : "");
			this = this->next;
		}

		printk("\n");
	}

	for (i = 0; i < 4; i++)
		printk(KERN_DEBUG "Posn %d: Chain %d, extent %d, offset %lu.\n",
				i, toi_writer_posn_save[i].chain_num,
				toi_writer_posn_save[i].extent_num,
				toi_writer_posn_save[i].offset);
}

static int total_header_bytes;
static int unowned;

static int debug_broken_header(void)
{
	printk(KERN_DEBUG "Image header too big for size allocated!\n");
	print_toi_header_storage_for_modules();
	printk(KERN_DEBUG "Page flags : %d.\n", toi_pageflags_space_needed());
	printk(KERN_DEBUG "toi_header : %ld.\n", sizeof(struct toi_header));
	printk(KERN_DEBUG "Total unowned : %d.\n", unowned);
	printk(KERN_DEBUG "Total used : %d (%ld pages).\n", total_header_bytes,
			DIV_ROUND_UP(total_header_bytes, PAGE_SIZE));
	printk(KERN_DEBUG "Space needed now : %ld.\n",
			get_header_storage_needed());
	dump_block_chains();
	abort_hibernate(TOI_HEADER_TOO_BIG, "Header reservation too small.");
	return -EIO;
}

/**
 * go_next_page - skip blocks to the start of the next page
 * @writing: Whether we're reading or writing the image.
 *
 * Go forward one page, or two if extra_page_forward is set. It only gets
 * set at the start of reading the image header, to skip the first page
 * of the header, which is read without using the extent chains.
 **/
static int go_next_page(int writing, int section_barrier)
{
	int i, max = (toi_writer_posn.current_chain == -1) ? 1 :
	  toi_devinfo[toi_writer_posn.current_chain].blocks_per_page,
		compare_to = 0;

	/* Have we already used the last page of the stream? */
	switch (current_stream) {
	case 0:
		compare_to = 2;
		break;
	case 1:
		compare_to = 3;
		break;
	case 2:
		compare_to = 1;
		break;
	}

	if (section_barrier && toi_writer_posn.current_chain ==
			toi_writer_posn_save[compare_to].chain_num &&
	    toi_writer_posn.current_offset ==
			toi_writer_posn_save[compare_to].offset) {
		if (writing) {
		       if (!current_stream)
			       return debug_broken_header();
		} else {
			more_readahead = 0;
			return -ENODATA;
		}
	}

	/* Nope. Go foward a page - or maybe two */
	for (i = 0; i < max; i++)
		toi_extent_state_next(&toi_writer_posn);

	if (toi_extent_state_eof(&toi_writer_posn)) {
		/* Don't complain if readahead falls off the end */
		if (writing && section_barrier) {
			printk(KERN_DEBUG "Extent state eof. "
				"Expected compression ratio too optimistic?\n");
			dump_block_chains();
		}
		return -ENODATA;
	}

	if (extra_page_forward) {
		extra_page_forward = 0;
		return go_next_page(writing, section_barrier);
	}

	return 0;
}

/**
 * set_extra_page_forward - make us skip an extra page on next go_next_page
 *
 * Used in reading header, to jump to 2nd page after getting 1st page
 * direct from image header.
 **/
static void set_extra_page_forward(void)
{
	extra_page_forward = 1;
}

/**
 * toi_bio_rw_page - do i/o on the next disk page in the image
 * @writing: Whether reading or writing.
 * @page: Page to do i/o on.
 * @is_readahead: Whether we're doing readahead
 * @free_group: The group used in allocating the page
 *
 * Submit a page for reading or writing, possibly readahead.
 * Pass the group used in allocating the page as well, as it should
 * be freed on completion of the bio if we're writing the page.
 **/
static int toi_bio_rw_page(int writing, struct page *page,
		int is_readahead, int free_group)
{
	struct toi_bdev_info *dev_info;
	int result = go_next_page(writing, 1);

	if (result)
		return result;

	dev_info = &toi_devinfo[toi_writer_posn.current_chain];

	return toi_do_io(writing, dev_info->bdev,
		toi_writer_posn.current_offset <<
			dev_info->bmap_shift,
		page, is_readahead, 0, free_group);
}

/**
 * toi_rw_init - prepare to read or write a stream in the image
 * @writing: Whether reading or writing.
 * @stream number: Section of the image being processed.
 *
 * Prepare to read or write a section ('stream') in the image.
 **/
static int toi_rw_init(int writing, int stream_number)
{
	if (stream_number)
		toi_extent_state_restore(&toi_writer_posn,
				&toi_writer_posn_save[stream_number]);
	else
		toi_extent_state_goto_start(&toi_writer_posn);

	atomic_set(&toi_io_done, 0);
	toi_writer_buffer = (char *) toi_get_zeroed_page(11, TOI_ATOMIC_GFP);
	toi_writer_buffer_posn = writing ? 0 : PAGE_SIZE;

	current_stream = stream_number;

	more_readahead = 1;

	return toi_writer_buffer ? 0 : -ENOMEM;
}

/**
 * toi_read_header_init - prepare to read the image header
 *
 * Reset readahead indices prior to starting to read a section of the image.
 **/
static void toi_read_header_init(void)
{
	toi_writer_buffer = (char *) toi_get_zeroed_page(11, TOI_ATOMIC_GFP);
	more_readahead = 1;
}

/**
 * toi_bio_queue_write - queue a page for writing
 * @full_buffer: Pointer to a page to be queued
 *
 * Add a page to the queue to be submitted. If we're the queue flusher,
 * we'll do this once we've dropped toi_bio_mutex, so other threads can
 * continue to submit I/O while we're on the slow path doing the actual
 * submission.
 **/
static void toi_bio_queue_write(char **full_buffer)
{
	struct page *page = virt_to_page(*full_buffer);
	unsigned long flags;

	page->private = 0;

	spin_lock_irqsave(&bio_queue_lock, flags);
	if (!bio_queue_head)
		bio_queue_head = page;
	else
		bio_queue_tail->private = (unsigned long) page;

	bio_queue_tail = page;
	atomic_inc(&toi_bio_queue_size);

	spin_unlock_irqrestore(&bio_queue_lock, flags);
	wake_up(&toi_io_queue_flusher);

	*full_buffer = NULL;
}

/**
 * toi_rw_cleanup - Cleanup after i/o.
 * @writing: Whether we were reading or writing.
 *
 * Flush all I/O and clean everything up after reading or writing a
 * section of the image.
 **/
static int toi_rw_cleanup(int writing)
{
	int i, result;

	if (writing) {
		int result;

		if (toi_writer_buffer_posn && !test_result_state(TOI_ABORTED))
			toi_bio_queue_write(&toi_writer_buffer);

		result = toi_bio_queue_flush_pages(0);

		if (result)
			return result;

		if (current_stream == 2)
			toi_extent_state_save(&toi_writer_posn,
					&toi_writer_posn_save[1]);
		else if (current_stream == 1)
			toi_extent_state_save(&toi_writer_posn,
					&toi_writer_posn_save[3]);
	}

	result = toi_finish_all_io();

	while (readahead_list_head) {
		void *next = (void *) readahead_list_head->private;
		toi__free_page(12, readahead_list_head);
		readahead_list_head = next;
	}

	readahead_list_tail = NULL;

	if (!current_stream)
		return result;

	for (i = 0; i < NUM_REASONS; i++) {
		if (!atomic_read(&reasons[i]))
			continue;
		printk(KERN_DEBUG "Waited for i/o due to %s %d times.\n",
				reason_name[i], atomic_read(&reasons[i]));
		atomic_set(&reasons[i], 0);
	}

	current_stream = 0;
	return result;
}

/**
 * toi_start_one_readahead - start one page of readahead
 * @dedicated_thread: Is this a thread dedicated to doing readahead?
 *
 * Start one new page of readahead. If this is being called by a thread
 * whose only just is to submit readahead, don't quit because we failed
 * to allocate a page.
 **/
static int toi_start_one_readahead(int dedicated_thread)
{
	char *buffer = NULL;
	int oom = 0, result;

	result = throttle_if_needed(dedicated_thread ? THROTTLE_WAIT : 0);
	if (result)
		return result;

	mutex_lock(&toi_bio_readahead_mutex);

	while (!buffer) {
		buffer = (char *) toi_get_zeroed_page(12,
				TOI_ATOMIC_GFP);
		if (!buffer) {
			if (oom && !dedicated_thread) {
				mutex_unlock(&toi_bio_readahead_mutex);
				return -ENOMEM;
			}

			oom = 1;
			set_free_mem_throttle();
			do_bio_wait(5);
		}
	}

	result = toi_bio_rw_page(READ, virt_to_page(buffer), 1, 0);
	mutex_unlock(&toi_bio_readahead_mutex);
	return result;
}

/**
 * toi_start_new_readahead - start new readahead
 * @dedicated_thread: Are we dedicated to this task?
 *
 * Start readahead of image pages.
 *
 * We can be called as a thread dedicated to this task (may be helpful on
 * systems with lots of CPUs), in which case we don't exit until there's no
 * more readahead.
 *
 * If this is not called by a dedicated thread, we top up our queue until
 * there's no more readahead to submit, we've submitted the number given
 * in target_outstanding_io or the number in progress exceeds the target
 * outstanding I/O value.
 *
 * No mutex needed because this is only ever called by the first cpu.
 **/
static int toi_start_new_readahead(int dedicated_thread)
{
	int last_result, num_submitted = 0;

	/* Start a new readahead? */
	if (!more_readahead)
		return 0;

	do {
		last_result = toi_start_one_readahead(dedicated_thread);

		if (last_result) {
			if (last_result == -ENOMEM || last_result == -ENODATA)
				return 0;

			printk(KERN_DEBUG
				"Begin read chunk returned %d.\n",
				last_result);
		} else
			num_submitted++;

	} while (more_readahead && !last_result &&
		 (dedicated_thread ||
		  (num_submitted < target_outstanding_io &&
		   atomic_read(&toi_io_in_progress) < target_outstanding_io)));

	return last_result;
}

/**
 * bio_io_flusher - start the dedicated I/O flushing routine
 * @writing: Whether we're writing the image.
 **/
static int bio_io_flusher(int writing)
{

	if (writing)
		return toi_bio_queue_flush_pages(1);
	else
		return toi_start_new_readahead(1);
}

/**
 * toi_bio_get_next_page_read - read a disk page, perhaps with readahead
 * @no_readahead: Whether we can use readahead
 *
 * Read a page from disk, submitting readahead and cleaning up finished i/o
 * while we wait for the page we're after.
 **/
static int toi_bio_get_next_page_read(int no_readahead)
{
	unsigned long *virt;
	struct page *next;

	/*
	 * When reading the second page of the header, we have to
	 * delay submitting the read until after we've gotten the
	 * extents out of the first page.
	 */
	if (unlikely(no_readahead && toi_start_one_readahead(0))) {
		printk(KERN_DEBUG "No readahead and toi_start_one_readahead "
				"returned non-zero.\n");
		return -EIO;
	}

	if (unlikely(!readahead_list_head)) {
		BUG_ON(!more_readahead);
		if (unlikely(toi_start_one_readahead(0))) {
			printk(KERN_DEBUG "No readahead and "
			 "toi_start_one_readahead returned non-zero.\n");
			return -EIO;
		}
	}

	if (PageLocked(readahead_list_head)) {
		waiting_on = readahead_list_head;
		do_bio_wait(0);
	}

	virt = page_address(readahead_list_head);
	memcpy(toi_writer_buffer, virt, PAGE_SIZE);

	next = (struct page *) readahead_list_head->private;
	toi__free_page(12, readahead_list_head);
	readahead_list_head = next;
	return 0;
}

/**
 * toi_bio_queue_flush_pages - flush the queue of pages queued for writing
 * @dedicated_thread: Whether we're a dedicated thread
 *
 * Flush the queue of pages ready to be written to disk.
 *
 * If we're a dedicated thread, stay in here until told to leave,
 * sleeping in wait_event.
 *
 * The first thread is normally the only one to come in here. Another
 * thread can enter this routine too, though, via throttle_if_needed.
 * Since that's the case, we must be careful to only have one thread
 * doing this work at a time. Otherwise we have a race and could save
 * pages out of order.
 *
 * If an error occurs, free all remaining pages without submitting them
 * for I/O.
 **/

int toi_bio_queue_flush_pages(int dedicated_thread)
{
	unsigned long flags;
	int result = 0;
	static int busy;

	if (busy)
		return 0;

	busy = 1;

top:
	spin_lock_irqsave(&bio_queue_lock, flags);
	while (bio_queue_head) {
		struct page *page = bio_queue_head;
		bio_queue_head = (struct page *) page->private;
		if (bio_queue_tail == page)
			bio_queue_tail = NULL;
		atomic_dec(&toi_bio_queue_size);
		spin_unlock_irqrestore(&bio_queue_lock, flags);
		if (!result)
			result = toi_bio_rw_page(WRITE, page, 0, 11);
		if (result)
			toi__free_page(11 , page);
		spin_lock_irqsave(&bio_queue_lock, flags);
	}
	spin_unlock_irqrestore(&bio_queue_lock, flags);

	if (dedicated_thread) {
		wait_event(toi_io_queue_flusher, bio_queue_head ||
				toi_bio_queue_flusher_should_finish);
		if (likely(!toi_bio_queue_flusher_should_finish))
			goto top;
		toi_bio_queue_flusher_should_finish = 0;
	}

	busy = 0;
	return result;
}

/**
 * toi_bio_get_new_page - get a new page for I/O
 * @full_buffer: Pointer to a page to allocate.
 **/
static int toi_bio_get_new_page(char **full_buffer)
{
	int result = throttle_if_needed(THROTTLE_WAIT);
	if (result)
		return result;

	while (!*full_buffer) {
		*full_buffer = (char *) toi_get_zeroed_page(11, TOI_ATOMIC_GFP);
		if (!*full_buffer) {
			set_free_mem_throttle();
			do_bio_wait(3);
		}
	}

	return 0;
}

/**
 * toi_rw_buffer - combine smaller buffers into PAGE_SIZE I/O
 * @writing:		Bool - whether writing (or reading).
 * @buffer:		The start of the buffer to write or fill.
 * @buffer_size:	The size of the buffer to write or fill.
 * @no_readahead:	Don't try to start readhead (when getting extents).
 **/
static int toi_rw_buffer(int writing, char *buffer, int buffer_size,
		int no_readahead)
{
	int bytes_left = buffer_size, result = 0;

	while (bytes_left) {
		char *source_start = buffer + buffer_size - bytes_left;
		char *dest_start = toi_writer_buffer + toi_writer_buffer_posn;
		int capacity = PAGE_SIZE - toi_writer_buffer_posn;
		char *to = writing ? dest_start : source_start;
		char *from = writing ? source_start : dest_start;

		if (bytes_left <= capacity) {
			memcpy(to, from, bytes_left);
			toi_writer_buffer_posn += bytes_left;
			return 0;
		}

		/* Complete this page and start a new one */
		memcpy(to, from, capacity);
		bytes_left -= capacity;

		if (!writing) {
			/*
			 * Perform actual I/O:
			 * read readahead_list_head into toi_writer_buffer
			 */
			int result = toi_bio_get_next_page_read(no_readahead);
			if (result)
				return result;
		} else {
			toi_bio_queue_write(&toi_writer_buffer);
			result = toi_bio_get_new_page(&toi_writer_buffer);
			if (result)
				return result;
		}

		toi_writer_buffer_posn = 0;
		toi_cond_pause(0, NULL);
	}

	return 0;
}

/**
 * toi_bio_read_page - read a page of the image
 * @pfn:		The pfn where the data belongs.
 * @buffer_page:	The page containing the (possibly compressed) data.
 * @buf_size:		The number of bytes on @buffer_page used (PAGE_SIZE).
 *
 * Read a (possibly compressed) page from the image, into buffer_page,
 * returning its pfn and the buffer size.
 **/
static int toi_bio_read_page(unsigned long *pfn, struct page *buffer_page,
		unsigned int *buf_size)
{
	int result = 0;
	char *buffer_virt = kmap(buffer_page);

	/*
	 * Only call start_new_readahead if we don't have a dedicated thread
	 * and we're the queue flusher.
	 */
	if (current == toi_queue_flusher) {
		int result2 = toi_start_new_readahead(0);
		if (result2) {
			printk(KERN_DEBUG "Queue flusher and "
			 "toi_start_one_readahead returned non-zero.\n");
			result = -EIO;
			goto out;
		}
	}

	my_mutex_lock(0, &toi_bio_mutex);

	/*
	 * Structure in the image:
	 *	[destination pfn|page size|page data]
	 * buf_size is PAGE_SIZE
	 */
	if (toi_rw_buffer(READ, (char *) pfn, sizeof(unsigned long), 0) ||
	    toi_rw_buffer(READ, (char *) buf_size, sizeof(int), 0) ||
	    toi_rw_buffer(READ, buffer_virt, *buf_size, 0)) {
		abort_hibernate(TOI_FAILED_IO, "Read of data failed.");
		result = 1;
	}

	my_mutex_unlock(0, &toi_bio_mutex);
out:
	kunmap(buffer_page);
	return result;
}

/**
 * toi_bio_write_page - write a page of the image
 * @pfn:		The pfn where the data belongs.
 * @buffer_page:	The page containing the (possibly compressed) data.
 * @buf_size:	The number of bytes on @buffer_page used.
 *
 * Write a (possibly compressed) page to the image from the buffer, together
 * with it's index and buffer size.
 **/
static int toi_bio_write_page(unsigned long pfn, struct page *buffer_page,
		unsigned int buf_size)
{
	char *buffer_virt;
	int result = 0, result2 = 0;

	if (unlikely(test_action_state(TOI_TEST_FILTER_SPEED)))
		return 0;

	my_mutex_lock(1, &toi_bio_mutex);

	if (test_result_state(TOI_ABORTED)) {
		my_mutex_unlock(1, &toi_bio_mutex);
		return -EIO;
	}

	buffer_virt = kmap(buffer_page);

	/*
	 * Structure in the image:
	 *	[destination pfn|page size|page data]
	 * buf_size is PAGE_SIZE
	 */
	if (toi_rw_buffer(WRITE, (char *) &pfn, sizeof(unsigned long), 0) ||
	    toi_rw_buffer(WRITE, (char *) &buf_size, sizeof(int), 0) ||
	    toi_rw_buffer(WRITE, buffer_virt, buf_size, 0)) {
		printk(KERN_DEBUG "toi_rw_buffer returned non-zero to "
				"toi_bio_write_page.\n");
		result = -EIO;
	}

	kunmap(buffer_page);
	my_mutex_unlock(1, &toi_bio_mutex);

	if (current == toi_queue_flusher)
		result2 = toi_bio_queue_flush_pages(0);

	return result ? result : result2;
}

/**
 * _toi_rw_header_chunk - read or write a portion of the image header
 * @writing:		Whether reading or writing.
 * @owner:		The module for which we're writing.
 *			Used for confirming that modules
 *			don't use more header space than they asked for.
 * @buffer:		Address of the data to write.
 * @buffer_size:	Size of the data buffer.
 * @no_readahead:	Don't try to start readhead (when getting extents).
 *
 * Perform PAGE_SIZE I/O. Start readahead if needed.
 **/
static int _toi_rw_header_chunk(int writing, struct toi_module_ops *owner,
		char *buffer, int buffer_size, int no_readahead)
{
	int result = 0;

	if (owner) {
		owner->header_used += buffer_size;
		toi_message(TOI_HEADER, TOI_LOW, 1,
			"Header: %s : %d bytes (%d/%d).\n",
			owner->name,
			buffer_size, owner->header_used,
			owner->header_requested);
		if (owner->header_used > owner->header_requested) {
			printk(KERN_EMERG "TuxOnIce module %s is using more "
				"header space (%u) than it requested (%u).\n",
				owner->name,
				owner->header_used,
				owner->header_requested);
			return buffer_size;
		}
	} else {
		unowned += buffer_size;
		toi_message(TOI_HEADER, TOI_LOW, 1,
			"Header: (No owner): %d bytes (%d total so far)\n",
			buffer_size, unowned);
	}

	if (!writing && !no_readahead)
		result = toi_start_new_readahead(0);

	if (!result)
		result = toi_rw_buffer(writing, buffer, buffer_size,
				no_readahead);

	total_header_bytes += buffer_size;
	return result;
}

static int toi_rw_header_chunk(int writing, struct toi_module_ops *owner,
		char *buffer, int size)
{
	return _toi_rw_header_chunk(writing, owner, buffer, size, 0);
}

static int toi_rw_header_chunk_noreadahead(int writing,
		struct toi_module_ops *owner, char *buffer, int size)
{
	return _toi_rw_header_chunk(writing, owner, buffer, size, 1);
}

/**
 * write_header_chunk_finish - flush any buffered header data
 **/
static int write_header_chunk_finish(void)
{
	int result = 0;

	if (toi_writer_buffer_posn)
		toi_bio_queue_write(&toi_writer_buffer);

	result = toi_finish_all_io();

	unowned = 0;
	total_header_bytes = 0;
	return result;
}

/**
 * toi_bio_storage_needed - get the amount of storage needed for my fns
 **/
static int toi_bio_storage_needed(void)
{
	return sizeof(int);
}

/**
 * toi_bio_save_config_info - save block I/O config to image header
 * @buf:	PAGE_SIZE'd buffer into which data should be saved.
 **/
static int toi_bio_save_config_info(char *buf)
{
	int *ints = (int *) buf;
	ints[0] = target_outstanding_io;
	return sizeof(int);
}

/**
 * toi_bio_load_config_info - restore block I/O config
 * @buf:	Data to be reloaded.
 * @size:	Size of the buffer saved.
 **/
static void toi_bio_load_config_info(char *buf, int size)
{
	int *ints = (int *) buf;
	target_outstanding_io  = ints[0];
}

/**
 * toi_bio_initialise - initialise bio code at start of some action
 * @starting_cycle:	Whether starting a hibernation cycle, or just reading or
 *			writing a sysfs value.
 **/
static int toi_bio_initialise(int starting_cycle)
{
	if (starting_cycle) {
		max_outstanding_writes = 0;
		max_outstanding_reads = 0;
		toi_queue_flusher = current;
#ifdef MEASURE_MUTEX_CONTENTION
		{
		int i, j, k;

		for (i = 0; i < 2; i++)
			for (j = 0; j < 2; j++)
				for_each_online_cpu(k)
					mutex_times[i][j][k] = 0;
		}
#endif
	}

	return 0;
}

/**
 * toi_bio_cleanup - cleanup after some action
 * @finishing_cycle:	Whether completing a cycle.
 **/
static void toi_bio_cleanup(int finishing_cycle)
{
	if (toi_writer_buffer) {
		toi_free_page(11, (unsigned long) toi_writer_buffer);
		toi_writer_buffer = NULL;
	}
}

struct toi_bio_ops toi_bio_ops = {
	.bdev_page_io = toi_bdev_page_io,
	.finish_all_io = toi_finish_all_io,
	.update_throughput_throttle = update_throughput_throttle,
	.forward_one_page = go_next_page,
	.set_extra_page_forward = set_extra_page_forward,
	.set_devinfo = toi_set_devinfo,
	.read_page = toi_bio_read_page,
	.write_page = toi_bio_write_page,
	.rw_init = toi_rw_init,
	.rw_cleanup = toi_rw_cleanup,
	.read_header_init = toi_read_header_init,
	.rw_header_chunk = toi_rw_header_chunk,
	.rw_header_chunk_noreadahead = toi_rw_header_chunk_noreadahead,
	.write_header_chunk_finish = write_header_chunk_finish,
	.io_flusher = bio_io_flusher,
};
EXPORT_SYMBOL_GPL(toi_bio_ops);

static struct toi_sysfs_data sysfs_params[] = {
	SYSFS_INT("target_outstanding_io", SYSFS_RW, &target_outstanding_io,
			0, 16384, 0, NULL),
};

static struct toi_module_ops toi_blockwriter_ops = {
	.name					= "lowlevel i/o",
	.type					= MISC_HIDDEN_MODULE,
	.directory				= "block_io",
	.module					= THIS_MODULE,
	.print_debug_info			= toi_bio_print_debug_stats,
	.memory_needed				= toi_bio_memory_needed,
	.storage_needed				= toi_bio_storage_needed,
	.save_config_info			= toi_bio_save_config_info,
	.load_config_info			= toi_bio_load_config_info,
	.initialise				= toi_bio_initialise,
	.cleanup				= toi_bio_cleanup,

	.sysfs_data		= sysfs_params,
	.num_sysfs_entries	= sizeof(sysfs_params) /
		sizeof(struct toi_sysfs_data),
};

/**
 * toi_block_io_load - load time routine for block I/O module
 *
 * Register block i/o ops and sysfs entries.
 **/
static __init int toi_block_io_load(void)
{
	return toi_register_module(&toi_blockwriter_ops);
}

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
