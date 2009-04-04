/*
 * kernel/power/tuxonice_io.c
 *
 * Copyright (C) 1998-2001 Gabor Kuti <seasons@fornax.hu>
 * Copyright (C) 1998,2001,2002 Pavel Machek <pavel@suse.cz>
 * Copyright (C) 2002-2003 Florent Chabaud <fchabaud@free.fr>
 * Copyright (C) 2002-2008 Nigel Cunningham (nigel at tuxonice net)
 *
 * This file is released under the GPLv2.
 *
 * It contains high level IO routines for hibernating.
 *
 */

#include <linux/suspend.h>
#include <linux/version.h>
#include <linux/utsname.h>
#include <linux/mount.h>
#include <linux/highmem.h>
#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/cpu.h>
#include <asm/tlbflush.h>

#include "tuxonice.h"
#include "tuxonice_modules.h"
#include "tuxonice_pageflags.h"
#include "tuxonice_io.h"
#include "tuxonice_ui.h"
#include "tuxonice_storage.h"
#include "tuxonice_prepare_image.h"
#include "tuxonice_extent.h"
#include "tuxonice_sysfs.h"
#include "tuxonice_builtin.h"
#include "tuxonice_checksum.h"
#include "tuxonice_alloc.h"
char alt_resume_param[256];

/* Variables shared between threads and updated under the mutex */
static int io_write, io_finish_at, io_base, io_barmax, io_pageset, io_result;
static int io_index, io_nextupdate, io_pc, io_pc_step;
static DEFINE_MUTEX(io_mutex);
static DEFINE_PER_CPU(struct page *, last_sought);
static DEFINE_PER_CPU(struct page *, last_high_page);
static DEFINE_PER_CPU(char *, checksum_locn);
static DEFINE_PER_CPU(struct pbe *, last_low_page);
static atomic_t io_count;
atomic_t toi_io_workers;
EXPORT_SYMBOL_GPL(toi_io_workers);

DECLARE_WAIT_QUEUE_HEAD(toi_io_queue_flusher);
EXPORT_SYMBOL_GPL(toi_io_queue_flusher);

int toi_bio_queue_flusher_should_finish;
EXPORT_SYMBOL_GPL(toi_bio_queue_flusher_should_finish);

/* Indicates that this thread should be used for checking throughput */
#define MONITOR ((void *) 1)

/**
 * toi_attempt_to_parse_resume_device - determine if we can hibernate
 *
 * Can we hibernate, using the current resume= parameter?
 **/
int toi_attempt_to_parse_resume_device(int quiet)
{
	struct list_head *Allocator;
	struct toi_module_ops *thisAllocator;
	int result, returning = 0;

	if (toi_activate_storage(0))
		return 0;

	toiActiveAllocator = NULL;
	clear_toi_state(TOI_RESUME_DEVICE_OK);
	clear_toi_state(TOI_CAN_RESUME);
	clear_result_state(TOI_ABORTED);

	if (!toiNumAllocators) {
		if (!quiet)
			printk(KERN_INFO "TuxOnIce: No storage allocators have "
				"been registered. Hibernating will be "
				"disabled.\n");
		goto cleanup;
	}

	if (!resume_file[0]) {
		if (!quiet)
			printk(KERN_INFO "TuxOnIce: Resume= parameter is empty."
				" Hibernating will be disabled.\n");
		goto cleanup;
	}

	list_for_each(Allocator, &toiAllocators) {
		thisAllocator = list_entry(Allocator, struct toi_module_ops,
								type_list);

		/*
		 * Not sure why you'd want to disable an allocator, but
		 * we should honour the flag if we're providing it
		 */
		if (!thisAllocator->enabled)
			continue;

		result = thisAllocator->parse_sig_location(
				resume_file, (toiNumAllocators == 1),
				quiet);

		switch (result) {
		case -EINVAL:
			/* For this allocator, but not a valid
			 * configuration. Error already printed. */
			goto cleanup;

		case 0:
			/* For this allocator and valid. */
			toiActiveAllocator = thisAllocator;

			set_toi_state(TOI_RESUME_DEVICE_OK);
			set_toi_state(TOI_CAN_RESUME);
			returning = 1;
			goto cleanup;
		}
	}
	if (!quiet)
		printk(KERN_INFO "TuxOnIce: No matching enabled allocator "
				"found. Resuming disabled.\n");
cleanup:
	toi_deactivate_storage(0);
	return returning;
}
EXPORT_SYMBOL_GPL(toi_attempt_to_parse_resume_device);

void attempt_to_parse_resume_device2(void)
{
	toi_prepare_usm();
	toi_attempt_to_parse_resume_device(0);
	toi_cleanup_usm();
}
EXPORT_SYMBOL_GPL(attempt_to_parse_resume_device2);

void save_restore_alt_param(int replace, int quiet)
{
	static char resume_param_save[255];
	static unsigned long toi_state_save;

	if (replace) {
		toi_state_save = toi_state;
		strcpy(resume_param_save, resume_file);
		strcpy(resume_file, alt_resume_param);
	} else {
		strcpy(resume_file, resume_param_save);
		toi_state = toi_state_save;
	}
	toi_attempt_to_parse_resume_device(quiet);
}

void attempt_to_parse_alt_resume_param(void)
{
	int ok = 0;

	/* Temporarily set resume_param to the poweroff value */
	if (!strlen(alt_resume_param))
		return;

	printk(KERN_INFO "=== Trying Poweroff Resume2 ===\n");
	save_restore_alt_param(SAVE, NOQUIET);
	if (test_toi_state(TOI_CAN_RESUME))
		ok = 1;

	printk(KERN_INFO "=== Done ===\n");
	save_restore_alt_param(RESTORE, QUIET);

	/* If not ok, clear the string */
	if (ok)
		return;

	printk(KERN_INFO "Can't resume from that location; clearing "
			"alt_resume_param.\n");
	alt_resume_param[0] = '\0';
}

/**
 * noresume_reset_modules - reset data structures in case of non resuming
 *
 * When we read the start of an image, modules (and especially the
 * active allocator) might need to reset data structures if we
 * decide to remove the image rather than resuming from it.
 **/
static void noresume_reset_modules(void)
{
	struct toi_module_ops *this_filter;

	list_for_each_entry(this_filter, &toi_filters, type_list)
		if (this_filter->noresume_reset)
			this_filter->noresume_reset();

	if (toiActiveAllocator && toiActiveAllocator->noresume_reset)
		toiActiveAllocator->noresume_reset();
}

/**
 * fill_toi_header - fill the hibernate header structure
 * @struct toi_header: Header data structure to be filled.
 **/
static int fill_toi_header(struct toi_header *sh)
{
	int i, error;

	error = init_header((struct swsusp_info *) sh);
	if (error)
		return error;

	sh->pagedir = pagedir1;
	sh->pageset_2_size = pagedir2.size;
	sh->param0 = toi_result;
	sh->param1 = toi_bkd.toi_action;
	sh->param2 = toi_bkd.toi_debug_state;
	sh->param3 = toi_bkd.toi_default_console_level;
	sh->root_fs = current->fs->root.mnt->mnt_sb->s_dev;
	for (i = 0; i < 4; i++)
		sh->io_time[i/2][i%2] = toi_bkd.toi_io_time[i/2][i%2];
	sh->bkd = boot_kernel_data_buffer;
	return 0;
}

/**
 * rw_init_modules - initialize modules
 * @rw:		Whether we are reading of writing an image.
 * @which:	Section of the image being processed.
 *
 * Iterate over modules, preparing the ones that will be used to read or write
 * data.
 **/
static int rw_init_modules(int rw, int which)
{
	struct toi_module_ops *this_module;
	/* Initialise page transformers */
	list_for_each_entry(this_module, &toi_filters, type_list) {
		if (!this_module->enabled)
			continue;
		if (this_module->rw_init && this_module->rw_init(rw, which)) {
			abort_hibernate(TOI_FAILED_MODULE_INIT,
				"Failed to initialize the %s filter.",
				this_module->name);
			return 1;
		}
	}

	/* Initialise allocator */
	if (toiActiveAllocator->rw_init(rw, which)) {
		abort_hibernate(TOI_FAILED_MODULE_INIT,
				"Failed to initialise the allocator.");
		return 1;
	}

	/* Initialise other modules */
	list_for_each_entry(this_module, &toi_modules, module_list) {
		if (!this_module->enabled ||
		    this_module->type == FILTER_MODULE ||
		    this_module->type == WRITER_MODULE)
			continue;
		if (this_module->rw_init && this_module->rw_init(rw, which)) {
			set_abort_result(TOI_FAILED_MODULE_INIT);
			printk(KERN_INFO "Setting aborted flag due to module "
					"init failure.\n");
			return 1;
		}
	}

	return 0;
}

/**
 * rw_cleanup_modules - cleanup modules
 * @rw:	Whether we are reading of writing an image.
 *
 * Cleanup components after reading or writing a set of pages.
 * Only the allocator may fail.
 **/
static int rw_cleanup_modules(int rw)
{
	struct toi_module_ops *this_module;
	int result = 0;

	/* Cleanup other modules */
	list_for_each_entry(this_module, &toi_modules, module_list) {
		if (!this_module->enabled ||
		    this_module->type == FILTER_MODULE ||
		    this_module->type == WRITER_MODULE)
			continue;
		if (this_module->rw_cleanup)
			result |= this_module->rw_cleanup(rw);
	}

	/* Flush data and cleanup */
	list_for_each_entry(this_module, &toi_filters, type_list) {
		if (!this_module->enabled)
			continue;
		if (this_module->rw_cleanup)
			result |= this_module->rw_cleanup(rw);
	}

	result |= toiActiveAllocator->rw_cleanup(rw);

	return result;
}

static struct page *copy_page_from_orig_page(struct page *orig_page)
{
	int is_high = PageHighMem(orig_page), index, min, max;
	struct page *high_page = NULL,
		    **my_last_high_page = &__get_cpu_var(last_high_page),
		    **my_last_sought = &__get_cpu_var(last_sought);
	struct pbe *this, **my_last_low_page = &__get_cpu_var(last_low_page);
	void *compare;

	if (is_high) {
		if (*my_last_sought && *my_last_high_page &&
				*my_last_sought < orig_page)
			high_page = *my_last_high_page;
		else
			high_page = (struct page *) restore_highmem_pblist;
		this = (struct pbe *) kmap(high_page);
		compare = orig_page;
	} else {
		if (*my_last_sought && *my_last_low_page &&
				*my_last_sought < orig_page)
			this = *my_last_low_page;
		else
			this = restore_pblist;
		compare = page_address(orig_page);
	}

	*my_last_sought = orig_page;

	/* Locate page containing pbe */
	while (this[PBES_PER_PAGE - 1].next &&
			this[PBES_PER_PAGE - 1].orig_address < compare) {
		if (is_high) {
			struct page *next_high_page = (struct page *)
				this[PBES_PER_PAGE - 1].next;
			kunmap(high_page);
			this = kmap(next_high_page);
			high_page = next_high_page;
		} else
			this = this[PBES_PER_PAGE - 1].next;
	}

	/* Do a binary search within the page */
	min = 0;
	max = PBES_PER_PAGE;
	index = PBES_PER_PAGE / 2;
	while (max - min) {
		if (!this[index].orig_address ||
		    this[index].orig_address > compare)
			max = index;
		else if (this[index].orig_address == compare) {
			if (is_high) {
				struct page *page = this[index].address;
				*my_last_high_page = high_page;
				kunmap(high_page);
				return page;
			}
			*my_last_low_page = this;
			return virt_to_page(this[index].address);
		} else
			min = index;
		index = ((max + min) / 2);
	};

	if (is_high)
		kunmap(high_page);

	abort_hibernate(TOI_FAILED_IO, "Failed to get destination page for"
		" orig page %p. This[min].orig_address=%p.\n", orig_page,
		this[index].orig_address);
	return NULL;
}

/**
 * worker_rw_loop - main loop to read/write pages
 *
 * The main I/O loop for reading or writing pages. The io_map bitmap is used to
 * track the pages to read/write.
 * If we are reading, the pages are loaded to their final (mapped) pfn.
 **/
static int worker_rw_loop(void *data)
{
	unsigned long data_pfn, write_pfn, next_jiffies = jiffies + HZ / 2,
		      jif_index = 1;
	int result, my_io_index = 0, last_worker;
	struct toi_module_ops *first_filter = toi_get_next_filter(NULL);
	struct page *buffer = toi_alloc_page(28, TOI_ATOMIC_GFP);

	current->flags |= PF_NOFREEZE;

	atomic_inc(&toi_io_workers);
	mutex_lock(&io_mutex);

	do {
		unsigned int buf_size;
		int was_present = 1;

		if (data && jiffies > next_jiffies) {
			next_jiffies += HZ / 2;
			if (toiActiveAllocator->update_throughput_throttle)
				toiActiveAllocator->update_throughput_throttle(
						jif_index);
			jif_index++;
		}

		/*
		 * What page to use? If reading, don't know yet which page's
		 * data will be read, so always use the buffer. If writing,
		 * use the copy (Pageset1) or original page (Pageset2), but
		 * always write the pfn of the original page.
		 */
		if (io_write) {
			struct page *page;
			char **my_checksum_locn = &__get_cpu_var(checksum_locn);

			data_pfn = memory_bm_next_pfn(io_map);

			/* Another thread could have beaten us to it. */
			if (data_pfn == BM_END_OF_MAP) {
				if (atomic_read(&io_count)) {
					printk(KERN_INFO "Ran out of pfns but "
						"io_count is still %d.\n",
						atomic_read(&io_count));
					BUG();
				}
				break;
			}

			my_io_index = io_finish_at -
				atomic_sub_return(1, &io_count);

			memory_bm_clear_bit(io_map, data_pfn);
			page = pfn_to_page(data_pfn);

			was_present = kernel_page_present(page);
			if (!was_present)
				kernel_map_pages(page, 1, 1);

			if (io_pageset == 1)
				write_pfn = memory_bm_next_pfn(pageset1_map);
			else {
				write_pfn = data_pfn;
				*my_checksum_locn =
					tuxonice_get_next_checksum();
			}

			mutex_unlock(&io_mutex);

			if (io_pageset == 2 &&
			    tuxonice_calc_checksum(page, *my_checksum_locn))
					return 1;

			result = first_filter->write_page(write_pfn, page,
					PAGE_SIZE);

			if (!was_present)
				kernel_map_pages(page, 1, 0);
		} else { /* Reading */
			my_io_index = io_finish_at -
				atomic_sub_return(1, &io_count);
			mutex_unlock(&io_mutex);

			/*
			 * Are we aborting? If so, don't submit any more I/O as
			 * resetting the resume_attempted flag (from ui.c) will
			 * clear the bdev flags, making this thread oops.
			 */
			if (unlikely(test_toi_state(TOI_STOP_RESUME))) {
				atomic_dec(&toi_io_workers);
				if (!atomic_read(&toi_io_workers))
					set_toi_state(TOI_IO_STOPPED);
				while (1)
					schedule();
			}

			/* See toi_bio_read_page in tuxonice_block_io.c:
			 * read the next page in the image.
			 */
			result = first_filter->read_page(&write_pfn, buffer,
					&buf_size);
			if (buf_size != PAGE_SIZE) {
				abort_hibernate(TOI_FAILED_IO,
					"I/O pipeline returned %d bytes instead"
					" of %ud.\n", buf_size, PAGE_SIZE);
				mutex_lock(&io_mutex);
				break;
			}
		}

		if (result) {
			io_result = result;
			if (io_write) {
				printk(KERN_INFO "Write chunk returned %d.\n",
						result);
				abort_hibernate(TOI_FAILED_IO,
					"Failed to write a chunk of the "
					"image.");
				mutex_lock(&io_mutex);
				break;
			}
			panic("Read chunk returned (%d)", result);
		}

		/*
		 * Discard reads of resaved pages while reading ps2
		 * and unwanted pages while rereading ps2 when aborting.
		 */
		if (!io_write && !PageResave(pfn_to_page(write_pfn))) {
			struct page *final_page = pfn_to_page(write_pfn),
				    *copy_page = final_page;
			char *virt, *buffer_virt;

			if (io_pageset == 1 && !load_direct(final_page)) {
				copy_page =
					copy_page_from_orig_page(final_page);
				BUG_ON(!copy_page);
			}

			if (memory_bm_test_bit(io_map, write_pfn)) {
				virt = kmap(copy_page);
				buffer_virt = kmap(buffer);
				was_present = kernel_page_present(copy_page);
				if (!was_present)
					kernel_map_pages(copy_page, 1, 1);
				memcpy(virt, buffer_virt, PAGE_SIZE);
				if (!was_present)
					kernel_map_pages(copy_page, 1, 0);
				kunmap(copy_page);
				kunmap(buffer);
				memory_bm_clear_bit(io_map, write_pfn);
			} else {
				mutex_lock(&io_mutex);
				atomic_inc(&io_count);
				mutex_unlock(&io_mutex);
			}
		}

		if (my_io_index + io_base == io_nextupdate)
			io_nextupdate = toi_update_status(my_io_index +
				io_base, io_barmax, " %d/%d MB ",
				MB(io_base+my_io_index+1), MB(io_barmax));

		if (my_io_index == io_pc) {
			printk("%s%d%%...", io_pc_step == 1 ? KERN_ERR : "",
					20 * io_pc_step);
			io_pc_step++;
			io_pc = io_finish_at * io_pc_step / 5;
		}

		toi_cond_pause(0, NULL);

		/*
		 * Subtle: If there's less I/O still to be done than threads
		 * running, quit. This stops us doing I/O beyond the end of
		 * the image when reading.
		 *
		 * Possible race condition. Two threads could do the test at
		 * the same time; one should exit and one should continue.
		 * Therefore we take the mutex before comparing and exiting.
		 */

		mutex_lock(&io_mutex);

	} while (atomic_read(&io_count) >= atomic_read(&toi_io_workers) &&
		!(io_write && test_result_state(TOI_ABORTED)));

	last_worker = atomic_dec_and_test(&toi_io_workers);
	mutex_unlock(&io_mutex);

	if (last_worker) {
		toi_bio_queue_flusher_should_finish = 1;
		wake_up(&toi_io_queue_flusher);
		toiActiveAllocator->finish_all_io();
	}

	toi__free_page(28, buffer);

	return 0;
}

static int start_other_threads(void)
{
	int cpu, num_started = 0;
	struct task_struct *p;

	for_each_online_cpu(cpu) {
		if (cpu == smp_processor_id())
			continue;

		p = kthread_create(worker_rw_loop, num_started ? NULL : MONITOR,
				"ktoi_io/%d", cpu);
		if (IS_ERR(p)) {
			printk(KERN_ERR "ktoi_io for %i failed\n", cpu);
			continue;
		}
		kthread_bind(p, cpu);
		p->flags |= PF_MEMALLOC;
		wake_up_process(p);
		num_started++;
	}

	return num_started;
}

/**
 * do_rw_loop - main highlevel function for reading or writing pages
 *
 * Create the io_map bitmap and call worker_rw_loop to perform I/O operations.
 **/
static int do_rw_loop(int write, int finish_at, struct memory_bitmap *pageflags,
		int base, int barmax, int pageset)
{
	int index = 0, cpu, num_other_threads = 0;
	unsigned long pfn;

	if (!finish_at)
		return 0;

	io_write = write;
	io_finish_at = finish_at;
	io_base = base;
	io_barmax = barmax;
	io_pageset = pageset;
	io_index = 0;
	io_pc = io_finish_at / 5;
	io_pc_step = 1;
	io_result = 0;
	io_nextupdate = base + 1;
	toi_bio_queue_flusher_should_finish = 0;

	for_each_online_cpu(cpu) {
		per_cpu(last_sought, cpu) = NULL;
		per_cpu(last_low_page, cpu) = NULL;
		per_cpu(last_high_page, cpu) = NULL;
	}

	/* Ensure all bits clear */
	memory_bm_clear(io_map);

	/* Set the bits for the pages to write */
	memory_bm_position_reset(pageflags);

	pfn = memory_bm_next_pfn(pageflags);

	while (pfn != BM_END_OF_MAP && index < finish_at) {
		memory_bm_set_bit(io_map, pfn);
		pfn = memory_bm_next_pfn(pageflags);
		index++;
	}

	BUG_ON(index < finish_at);

	atomic_set(&io_count, finish_at);

	memory_bm_position_reset(pageset1_map);

	clear_toi_state(TOI_IO_STOPPED);
	memory_bm_position_reset(io_map);

	if (!test_action_state(TOI_NO_MULTITHREADED_IO))
		num_other_threads = start_other_threads();

	if (!num_other_threads || !toiActiveAllocator->io_flusher ||
		test_action_state(TOI_NO_FLUSHER_THREAD))
		worker_rw_loop(num_other_threads ? NULL : MONITOR);
	else
		toiActiveAllocator->io_flusher(write);

	while (atomic_read(&toi_io_workers))
		schedule();

	set_toi_state(TOI_IO_STOPPED);
	if (unlikely(test_toi_state(TOI_STOP_RESUME))) {
		while (1)
			schedule();
	}

	if (!io_result && !test_result_state(TOI_ABORTED)) {
		unsigned long next;

		printk("done.\n");

		toi_update_status(io_base + io_finish_at, io_barmax,
				" %d/%d MB ",
				MB(io_base + io_finish_at), MB(io_barmax));

		memory_bm_position_reset(io_map);
		next = memory_bm_next_pfn(io_map);
		if  (next != BM_END_OF_MAP) {
			printk(KERN_INFO "Finished I/O loop but still work to "
					"do?\nFinish at = %d. io_count = %d.\n",
					finish_at, atomic_read(&io_count));
			printk(KERN_INFO "I/O bitmap still records work to do."
					"%ld.\n", next);
			BUG();
		}
	}

	return io_result;
}

/**
 * write_pageset - write a pageset to disk.
 * @pagedir:	Which pagedir to write.
 *
 * Returns:
 *	Zero on success or -1 on failure.
 **/
int write_pageset(struct pagedir *pagedir)
{
	int finish_at, base = 0, start_time, end_time;
	int barmax = pagedir1.size + pagedir2.size;
	long error = 0;
	struct memory_bitmap *pageflags;

	/*
	 * Even if there is nothing to read or write, the allocator
	 * may need the init/cleanup for it's housekeeping.  (eg:
	 * Pageset1 may start where pageset2 ends when writing).
	 */
	finish_at = pagedir->size;

	if (pagedir->id == 1) {
		toi_prepare_status(DONT_CLEAR_BAR,
				"Writing kernel & process data...");
		base = pagedir2.size;
		if (test_action_state(TOI_TEST_FILTER_SPEED) ||
		    test_action_state(TOI_TEST_BIO))
			pageflags = pageset1_map;
		else
			pageflags = pageset1_copy_map;
	} else {
		toi_prepare_status(DONT_CLEAR_BAR, "Writing caches...");
		pageflags = pageset2_map;
	}

	start_time = jiffies;

	if (rw_init_modules(1, pagedir->id)) {
		abort_hibernate(TOI_FAILED_MODULE_INIT,
				"Failed to initialise modules for writing.");
		error = 1;
	}

	if (!error)
		error = do_rw_loop(1, finish_at, pageflags, base, barmax,
				pagedir->id);

	if (rw_cleanup_modules(WRITE) && !error) {
		abort_hibernate(TOI_FAILED_MODULE_CLEANUP,
				"Failed to cleanup after writing.");
		error = 1;
	}

	end_time = jiffies;

	if ((end_time - start_time) && (!test_result_state(TOI_ABORTED))) {
		toi_bkd.toi_io_time[0][0] += finish_at,
		toi_bkd.toi_io_time[0][1] += (end_time - start_time);
	}

	return error;
}

/**
 * read_pageset - highlevel function to read a pageset from disk
 * @pagedir:			pageset to read
 * @overwrittenpagesonly:	Whether to read the whole pageset or
 *				only part of it.
 *
 * Returns:
 *	Zero on success or -1 on failure.
 **/
static int read_pageset(struct pagedir *pagedir, int overwrittenpagesonly)
{
	int result = 0, base = 0, start_time, end_time;
	int finish_at = pagedir->size;
	int barmax = pagedir1.size + pagedir2.size;
	struct memory_bitmap *pageflags;

	if (pagedir->id == 1) {
		toi_prepare_status(DONT_CLEAR_BAR,
				"Reading kernel & process data...");
		pageflags = pageset1_map;
	} else {
		toi_prepare_status(DONT_CLEAR_BAR, "Reading caches...");
		if (overwrittenpagesonly) {
			barmax = min(pagedir1.size, pagedir2.size);
			finish_at = min(pagedir1.size, pagedir2.size);
		} else
			base = pagedir1.size;
		pageflags = pageset2_map;
	}

	start_time = jiffies;

	if (rw_init_modules(0, pagedir->id)) {
		toiActiveAllocator->remove_image();
		result = 1;
	} else
		result = do_rw_loop(0, finish_at, pageflags, base, barmax,
				pagedir->id);

	if (rw_cleanup_modules(READ) && !result) {
		abort_hibernate(TOI_FAILED_MODULE_CLEANUP,
				"Failed to cleanup after reading.");
		result = 1;
	}

	/* Statistics */
	end_time = jiffies;

	if ((end_time - start_time) && (!test_result_state(TOI_ABORTED))) {
		toi_bkd.toi_io_time[1][0] += finish_at,
		toi_bkd.toi_io_time[1][1] += (end_time - start_time);
	}

	return result;
}

/**
 * write_module_configs - store the modules configuration
 *
 * The configuration for each module is stored in the image header.
 * Returns: Int
 *	Zero on success, Error value otherwise.
 **/
static int write_module_configs(void)
{
	struct toi_module_ops *this_module;
	char *buffer = (char *) toi_get_zeroed_page(22, TOI_ATOMIC_GFP);
	int len, index = 1;
	struct toi_module_header toi_module_header;

	if (!buffer) {
		printk(KERN_INFO "Failed to allocate a buffer for saving "
				"module configuration info.\n");
		return -ENOMEM;
	}

	/*
	 * We have to know which data goes with which module, so we at
	 * least write a length of zero for a module. Note that we are
	 * also assuming every module's config data takes <= PAGE_SIZE.
	 */

	/* For each module (in registration order) */
	list_for_each_entry(this_module, &toi_modules, module_list) {
		if (!this_module->enabled || !this_module->storage_needed ||
		    (this_module->type == WRITER_MODULE &&
		     toiActiveAllocator != this_module))
			continue;

		/* Get the data from the module */
		len = 0;
		if (this_module->save_config_info)
			len = this_module->save_config_info(buffer);

		/* Save the details of the module */
		toi_module_header.enabled = this_module->enabled;
		toi_module_header.type = this_module->type;
		toi_module_header.index = index++;
		strncpy(toi_module_header.name, this_module->name,
					sizeof(toi_module_header.name));
		toiActiveAllocator->rw_header_chunk(WRITE,
				this_module,
				(char *) &toi_module_header,
				sizeof(toi_module_header));

		/* Save the size of the data and any data returned */
		toiActiveAllocator->rw_header_chunk(WRITE,
				this_module,
				(char *) &len, sizeof(int));
		if (len)
			toiActiveAllocator->rw_header_chunk(
				WRITE, this_module, buffer, len);
	}

	/* Write a blank header to terminate the list */
	toi_module_header.name[0] = '\0';
	toiActiveAllocator->rw_header_chunk(WRITE, NULL,
			(char *) &toi_module_header, sizeof(toi_module_header));

	toi_free_page(22, (unsigned long) buffer);
	return 0;
}

/**
 * read_one_module_config - read and configure one module
 *
 * Read the configuration for one module, and configure the module
 * to match if it is loaded.
 *
 * Returns: Int
 *	Zero on success, Error value otherwise.
 **/
static int read_one_module_config(struct toi_module_header *header)
{
	struct toi_module_ops *this_module;
	int result, len;
	char *buffer;

	/* Find the module */
	this_module = toi_find_module_given_name(header->name);

	if (!this_module) {
		if (header->enabled) {
			toi_early_boot_message(1, TOI_CONTINUE_REQ,
				"It looks like we need module %s for reading "
				"the image but it hasn't been registered.\n",
				header->name);
			if (!(test_toi_state(TOI_CONTINUE_REQ)))
				return -EINVAL;
		} else
			printk(KERN_INFO "Module %s configuration data found, "
				"but the module hasn't registered. Looks like "
				"it was disabled, so we're ignoring its data.",
				header->name);
	}

	/* Get the length of the data (if any) */
	result = toiActiveAllocator->rw_header_chunk(READ, NULL, (char *) &len,
			sizeof(int));
	if (result) {
		printk(KERN_ERR "Failed to read the length of the module %s's"
				" configuration data.\n",
				header->name);
		return -EINVAL;
	}

	/* Read any data and pass to the module (if we found one) */
	if (!len)
		return 0;

	buffer = (char *) toi_get_zeroed_page(23, TOI_ATOMIC_GFP);

	if (!buffer) {
		printk(KERN_ERR "Failed to allocate a buffer for reloading "
				"module configuration info.\n");
		return -ENOMEM;
	}

	toiActiveAllocator->rw_header_chunk(READ, NULL, buffer, len);

	if (!this_module)
		goto out;

	if (!this_module->save_config_info)
		printk(KERN_ERR "Huh? Module %s appears to have a "
				"save_config_info, but not a load_config_info "
				"function!\n", this_module->name);
	else
		this_module->load_config_info(buffer, len);

	/*
	 * Now move this module to the tail of its lists. This will put it in
	 * order. Any new modules will end up at the top of the lists. They
	 * should have been set to disabled when loaded (people will
	 * normally not edit an initrd to load a new module and then hibernate
	 * without using it!).
	 */

	toi_move_module_tail(this_module);

	this_module->enabled = header->enabled;

out:
	toi_free_page(23, (unsigned long) buffer);
	return 0;
}

/**
 * read_module_configs - reload module configurations from the image header.
 *
 * Returns: Int
 *	Zero on success or an error code.
 **/
static int read_module_configs(void)
{
	int result = 0;
	struct toi_module_header toi_module_header;
	struct toi_module_ops *this_module;

	/* All modules are initially disabled. That way, if we have a module
	 * loaded now that wasn't loaded when we hibernated, it won't be used
	 * in trying to read the data.
	 */
	list_for_each_entry(this_module, &toi_modules, module_list)
		this_module->enabled = 0;

	/* Get the first module header */
	result = toiActiveAllocator->rw_header_chunk(READ, NULL,
			(char *) &toi_module_header,
			sizeof(toi_module_header));
	if (result) {
		printk(KERN_ERR "Failed to read the next module header.\n");
		return -EINVAL;
	}

	/* For each module (in registration order) */
	while (toi_module_header.name[0]) {
		result = read_one_module_config(&toi_module_header);

		if (result)
			return -EINVAL;

		/* Get the next module header */
		result = toiActiveAllocator->rw_header_chunk(READ, NULL,
				(char *) &toi_module_header,
				sizeof(toi_module_header));

		if (result) {
			printk(KERN_ERR "Failed to read the next module "
					"header.\n");
			return -EINVAL;
		}
	}

	return 0;
}

/**
 * write_image_header - write the image header after write the image proper
 *
 * Returns: Int
 *	Zero on success, error value otherwise.
 **/
int write_image_header(void)
{
	int ret;
	int total = pagedir1.size + pagedir2.size+2;
	char *header_buffer = NULL;

	/* Now prepare to write the header */
	ret = toiActiveAllocator->write_header_init();
	if (ret) {
		abort_hibernate(TOI_FAILED_MODULE_INIT,
				"Active allocator's write_header_init"
				" function failed.");
		goto write_image_header_abort;
	}

	/* Get a buffer */
	header_buffer = (char *) toi_get_zeroed_page(24, TOI_ATOMIC_GFP);
	if (!header_buffer) {
		abort_hibernate(TOI_OUT_OF_MEMORY,
			"Out of memory when trying to get page for header!");
		goto write_image_header_abort;
	}

	/* Write hibernate header */
	if (fill_toi_header((struct toi_header *) header_buffer)) {
		abort_hibernate(TOI_OUT_OF_MEMORY,
			"Failure to fill header information!");
		goto write_image_header_abort;
	}
	toiActiveAllocator->rw_header_chunk(WRITE, NULL,
			header_buffer, sizeof(struct toi_header));

	toi_free_page(24, (unsigned long) header_buffer);

	/* Write module configurations */
	ret = write_module_configs();
	if (ret) {
		abort_hibernate(TOI_FAILED_IO,
				"Failed to write module configs.");
		goto write_image_header_abort;
	}

	memory_bm_write(pageset1_map, toiActiveAllocator->rw_header_chunk);

	/* Flush data and let allocator cleanup */
	if (toiActiveAllocator->write_header_cleanup()) {
		abort_hibernate(TOI_FAILED_IO,
				"Failed to cleanup writing header.");
		goto write_image_header_abort_no_cleanup;
	}

	if (test_result_state(TOI_ABORTED))
		goto write_image_header_abort_no_cleanup;

	toi_update_status(total, total, NULL);

	return 0;

write_image_header_abort:
	toiActiveAllocator->write_header_cleanup();
write_image_header_abort_no_cleanup:
	return -1;
}

/**
 * sanity_check - check the header
 * @sh:	the header which was saved at hibernate time.
 *
 * Perform a few checks, seeking to ensure that the kernel being
 * booted matches the one hibernated. They need to match so we can
 * be _sure_ things will work. It is not absolutely impossible for
 * resuming from a different kernel to work, just not assured.
 **/
static char *sanity_check(struct toi_header *sh)
{
	char *reason = check_image_kernel((struct swsusp_info *) sh);

	if (reason)
		return reason;

	if (!test_action_state(TOI_IGNORE_ROOTFS)) {
		const struct super_block *sb;
		list_for_each_entry(sb, &super_blocks, s_list) {
			if ((!(sb->s_flags & MS_RDONLY)) &&
			    (sb->s_type->fs_flags & FS_REQUIRES_DEV))
				return "Device backed fs has been mounted "
					"rw prior to resume or initrd/ramfs "
					"is mounted rw.";
		}
	}

	return NULL;
}

static DECLARE_WAIT_QUEUE_HEAD(freeze_wait);

#define FREEZE_IN_PROGRESS (~0)

static int freeze_result;

static void do_freeze(struct work_struct *dummy)
{
	freeze_result = freeze_processes();
	wake_up(&freeze_wait);
}

static DECLARE_WORK(freeze_work, do_freeze);

/**
 * __read_pageset1 - test for the existence of an image and attempt to load it
 *
 * Returns:	Int
 *	Zero if image found and pageset1 successfully loaded.
 *	Error if no image found or loaded.
 **/
static int __read_pageset1(void)
{
	int i, result = 0;
	char *header_buffer = (char *) toi_get_zeroed_page(25, TOI_ATOMIC_GFP),
	     *sanity_error = NULL;
	struct toi_header *toi_header;

	if (!header_buffer) {
		printk(KERN_INFO "Unable to allocate a page for reading the "
				"signature.\n");
		return -ENOMEM;
	}

	/* Check for an image */
	result = toiActiveAllocator->image_exists(1);
	if (!result) {
		result = -ENODATA;
		noresume_reset_modules();
		printk(KERN_INFO "TuxOnIce: No image found.\n");
		goto out;
	}

	/*
	 * Prepare the active allocator for reading the image header. The
	 * activate allocator might read its own configuration.
	 *
	 * NB: This call may never return because there might be a signature
	 * for a different image such that we warn the user and they choose
	 * to reboot. (If the device ids look erroneous (2.4 vs 2.6) or the
	 * location of the image might be unavailable if it was stored on a
	 * network connection).
	 */

	result = toiActiveAllocator->read_header_init();
	if (result) {
		printk(KERN_INFO "TuxOnIce: Failed to initialise, reading the "
				"image header.\n");
		goto out_remove_image;
	}

	/* Check for noresume command line option */
	if (test_toi_state(TOI_NORESUME_SPECIFIED)) {
		printk(KERN_INFO "TuxOnIce: Noresume on command line. Removed "
				"image.\n");
		goto out_remove_image;
	}

	/* Check whether we've resumed before */
	if (test_toi_state(TOI_RESUMED_BEFORE)) {
		toi_early_boot_message(1, 0, NULL);
		if (!(test_toi_state(TOI_CONTINUE_REQ))) {
			printk(KERN_INFO "TuxOnIce: Tried to resume before: "
					"Invalidated image.\n");
			goto out_remove_image;
		}
	}

	clear_toi_state(TOI_CONTINUE_REQ);

	/* Read hibernate header */
	result = toiActiveAllocator->rw_header_chunk(READ, NULL,
			header_buffer, sizeof(struct toi_header));
	if (result < 0) {
		printk(KERN_ERR "TuxOnIce: Failed to read the image "
				"signature.\n");
		goto out_remove_image;
	}

	toi_header = (struct toi_header *) header_buffer;

	/*
	 * NB: This call may also result in a reboot rather than returning.
	 */

	sanity_error = sanity_check(toi_header);
	if (sanity_error) {
		toi_early_boot_message(1, TOI_CONTINUE_REQ,
				sanity_error);
		printk(KERN_INFO "TuxOnIce: Sanity check failed.\n");
		goto out_remove_image;
	}

	/*
	 * We have an image and it looks like it will load okay.
	 *
	 * Get metadata from header. Don't override commandline parameters.
	 *
	 * We don't need to save the image size limit because it's not used
	 * during resume and will be restored with the image anyway.
	 */

	memcpy((char *) &pagedir1,
		(char *) &toi_header->pagedir, sizeof(pagedir1));
	toi_result = toi_header->param0;
	toi_bkd.toi_action = toi_header->param1;
	toi_bkd.toi_debug_state = toi_header->param2;
	toi_bkd.toi_default_console_level = toi_header->param3;
	clear_toi_state(TOI_IGNORE_LOGLEVEL);
	pagedir2.size = toi_header->pageset_2_size;
	for (i = 0; i < 4; i++)
		toi_bkd.toi_io_time[i/2][i%2] =
			toi_header->io_time[i/2][i%2];

	set_toi_state(TOI_BOOT_KERNEL);
	boot_kernel_data_buffer = toi_header->bkd;

	/* Read module configurations */
	result = read_module_configs();
	if (result) {
		pagedir1.size = 0;
		pagedir2.size = 0;
		printk(KERN_INFO "TuxOnIce: Failed to read TuxOnIce module "
				"configurations.\n");
		clear_action_state(TOI_KEEP_IMAGE);
		goto out_remove_image;
	}

	toi_prepare_console();

	set_toi_state(TOI_NOW_RESUMING);

	if (!test_action_state(TOI_LATE_CPU_HOTPLUG)) {
		toi_prepare_status(DONT_CLEAR_BAR, "Disable nonboot cpus.");
		if (disable_nonboot_cpus()) {
			set_abort_result(TOI_CPU_HOTPLUG_FAILED);
			goto out_reset_console;
		}
	}

	if (usermodehelper_disable())
		goto out_enable_nonboot_cpus;

	current->flags |= PF_NOFREEZE;
	freeze_result = FREEZE_IN_PROGRESS;

	schedule_work_on(first_cpu(cpu_online_map), &freeze_work);

	toi_cond_pause(1, "About to read original pageset1 locations.");

	/*
	 * See _toi_rw_header_chunk in tuxonice_block_io.c:
	 * Initialize pageset1_map by reading the map from the image.
	 */
	if (memory_bm_read(pageset1_map, toiActiveAllocator->rw_header_chunk))
		goto out_thaw;

	/*
	 * See toi_rw_cleanup in tuxonice_block_io.c:
	 * Clean up after reading the header.
	 */
	result = toiActiveAllocator->read_header_cleanup();
	if (result) {
		printk(KERN_ERR "TuxOnIce: Failed to cleanup after reading the "
				"image header.\n");
		goto out_thaw;
	}

	toi_cond_pause(1, "About to read pagedir.");

	/*
	 * Get the addresses of pages into which we will load the kernel to
	 * be copied back and check if they conflict with the ones we are using.
	 */
	if (toi_get_pageset1_load_addresses()) {
		printk(KERN_INFO "TuxOnIce: Failed to get load addresses for "
				"pageset1.\n");
		goto out_thaw;
	}

	/* Read the original kernel back */
	toi_cond_pause(1, "About to read pageset 1.");

	/* Given the pagemap, read back the data from disk */
	if (read_pageset(&pagedir1, 0)) {
		toi_prepare_status(DONT_CLEAR_BAR, "Failed to read pageset 1.");
		result = -EIO;
		goto out_thaw;
	}

	toi_cond_pause(1, "About to restore original kernel.");
	result = 0;

	if (!test_action_state(TOI_KEEP_IMAGE) &&
	    toiActiveAllocator->mark_resume_attempted)
		toiActiveAllocator->mark_resume_attempted(1);

	wait_event(freeze_wait, freeze_result != FREEZE_IN_PROGRESS);
out:
	current->flags &= ~PF_NOFREEZE;
	toi_free_page(25, (unsigned long) header_buffer);
	return result;

out_thaw:
	wait_event(freeze_wait, freeze_result != FREEZE_IN_PROGRESS);
	thaw_processes();
	usermodehelper_enable();
out_enable_nonboot_cpus:
	enable_nonboot_cpus();
out_reset_console:
	toi_cleanup_console();
out_remove_image:
	result = -EINVAL;
	if (!test_action_state(TOI_KEEP_IMAGE))
		toiActiveAllocator->remove_image();
	toiActiveAllocator->read_header_cleanup();
	noresume_reset_modules();
	goto out;
}

/**
 * read_pageset1 - highlevel function to read the saved pages
 *
 * Attempt to read the header and pageset1 of a hibernate image.
 * Handle the outcome, complaining where appropriate.
 **/
int read_pageset1(void)
{
	int error;

	error = __read_pageset1();

	if (error && error != -ENODATA && error != -EINVAL &&
					!test_result_state(TOI_ABORTED))
		abort_hibernate(TOI_IMAGE_ERROR,
			"TuxOnIce: Error %d resuming\n", error);

	return error;
}

/**
 * get_have_image_data - check the image header
 **/
static char *get_have_image_data(void)
{
	char *output_buffer = (char *) toi_get_zeroed_page(26, TOI_ATOMIC_GFP);
	struct toi_header *toi_header;

	if (!output_buffer) {
		printk(KERN_INFO "Output buffer null.\n");
		return NULL;
	}

	/* Check for an image */
	if (!toiActiveAllocator->image_exists(1) ||
	    toiActiveAllocator->read_header_init() ||
	    toiActiveAllocator->rw_header_chunk(READ, NULL,
			output_buffer, sizeof(struct toi_header))) {
		sprintf(output_buffer, "0\n");
		/*
		 * From an initrd/ramfs, catting have_image and
		 * getting a result of 0 is sufficient.
		 */
		clear_toi_state(TOI_BOOT_TIME);
		goto out;
	}

	toi_header = (struct toi_header *) output_buffer;

	sprintf(output_buffer, "1\n%s\n%s\n",
			toi_header->uts.machine,
			toi_header->uts.version);

	/* Check whether we've resumed before */
	if (test_toi_state(TOI_RESUMED_BEFORE))
		strcat(output_buffer, "Resumed before.\n");

out:
	noresume_reset_modules();
	return output_buffer;
}

/**
 * read_pageset2 - read second part of the image
 * @overwrittenpagesonly:	Read only pages which would have been
 *				verwritten by pageset1?
 *
 * Read in part or all of pageset2 of an image, depending upon
 * whether we are hibernating and have only overwritten a portion
 * with pageset1 pages, or are resuming and need to read them
 * all.
 *
 * Returns: Int
 *	Zero if no error, otherwise the error value.
 **/
int read_pageset2(int overwrittenpagesonly)
{
	int result = 0;

	if (!pagedir2.size)
		return 0;

	result = read_pageset(&pagedir2, overwrittenpagesonly);

	toi_cond_pause(1, "Pagedir 2 read.");

	return result;
}

/**
 * image_exists_read - has an image been found?
 * @page:	Output buffer
 *
 * Store 0 or 1 in page, depending on whether an image is found.
 * Incoming buffer is PAGE_SIZE and result is guaranteed
 * to be far less than that, so we don't worry about
 * overflow.
 **/
int image_exists_read(const char *page, int count)
{
	int len = 0;
	char *result;

	if (toi_activate_storage(0))
		return count;

	if (!test_toi_state(TOI_RESUME_DEVICE_OK))
		toi_attempt_to_parse_resume_device(0);

	if (!toiActiveAllocator) {
		len = sprintf((char *) page, "-1\n");
	} else {
		result = get_have_image_data();
		if (result) {
			len = sprintf((char *) page, "%s",  result);
			toi_free_page(26, (unsigned long) result);
		}
	}

	toi_deactivate_storage(0);

	return len;
}

/**
 * image_exists_write - invalidate an image if one exists
 **/
int image_exists_write(const char *buffer, int count)
{
	if (toi_activate_storage(0))
		return count;

	if (toiActiveAllocator && toiActiveAllocator->image_exists(1))
		toiActiveAllocator->remove_image();

	toi_deactivate_storage(0);

	clear_result_state(TOI_KEPT_IMAGE);

	return count;
}
