/*
 * kernel/power/tuxonice_io.c
 *
 * Copyright (C) 1998-2001 Gabor Kuti <seasons@fornax.hu>
 * Copyright (C) 1998,2001,2002 Pavel Machek <pavel@suse.cz>
 * Copyright (C) 2002-2003 Florent Chabaud <fchabaud@free.fr>
 * Copyright (C) 2002-2007 Nigel Cunningham (nigel at suspend2 net)
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

char alt_resume_param[256];

/* Variables shared between threads and updated under the mutex */
static int io_write, io_finish_at, io_base, io_barmax, io_pageset, io_result;
static int io_index, io_nextupdate, io_pc, io_pc_step;
static unsigned long pfn, other_pfn;
static DEFINE_MUTEX(io_mutex);
static DEFINE_PER_CPU(struct page *, last_sought);
static DEFINE_PER_CPU(struct page *, last_high_page);
static DEFINE_PER_CPU(struct pbe *, last_low_page);
static atomic_t worker_thread_count;
static atomic_t io_count;

/* toi_attempt_to_parse_resume_device
 *
 * Can we hibernate, using the current resume= parameter?
 */
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
			printk("TuxOnIce: No storage allocators have been "
				"registered. Hibernating will be disabled.\n");
		goto cleanup;
	}
	
	if (!resume_file[0]) {
		if (!quiet)
			printk("TuxOnIce: Resume= parameter is empty."
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
		printk("TuxOnIce: No matching enabled allocator found. "
				"Resuming disabled.\n");
cleanup:
	toi_deactivate_storage(0);
	return returning;
}

void attempt_to_parse_resume_device2(void)
{
	toi_prepare_usm();
	toi_attempt_to_parse_resume_device(0);
	toi_cleanup_usm();
}

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

	printk("=== Trying Poweroff Resume2 ===\n");
	save_restore_alt_param(SAVE, NOQUIET);
	if (test_toi_state(TOI_CAN_RESUME))
		ok = 1;
	
	printk("=== Done ===\n");
	save_restore_alt_param(RESTORE, QUIET);
	
	/* If not ok, clear the string */
	if (ok)
		return;

	printk("Can't resume from that location; clearing alt_resume_param.\n");
	alt_resume_param[0] = '\0';
}

/* noresume_reset_modules
 *
 * Description:	When we read the start of an image, modules (and especially the
 * 		active allocator) might need to reset data structures if we
 * 		decide to remove the image rather than resuming from it.
 */

static void noresume_reset_modules(void)
{
	struct toi_module_ops *this_filter;
	
	list_for_each_entry(this_filter, &toi_filters, type_list)
		if (this_filter->noresume_reset)
			this_filter->noresume_reset();

	if (toiActiveAllocator && toiActiveAllocator->noresume_reset)
		toiActiveAllocator->noresume_reset();
}

/* fill_toi_header()
 * 
 * Description:	Fill the hibernate header structure.
 * Arguments:	struct toi_header: Header data structure to be filled.
 */

static void fill_toi_header(struct toi_header *sh)
{
	int i;
	
	memset((char *)sh, 0, sizeof(*sh));

	sh->version_code = LINUX_VERSION_CODE;
	sh->num_physpages = num_physpages;
	memcpy(&sh->uts, init_utsname(), sizeof(struct new_utsname));
	sh->page_size = PAGE_SIZE;
	sh->pagedir = pagedir1;
	sh->pageset_2_size = pagedir2.size;
	sh->param0 = toi_result;
	sh->param1 = toi_action;
	sh->param2 = toi_debug_state;
	sh->param3 = console_loglevel;
	sh->root_fs = current->fs->rootmnt->mnt_sb->s_dev;
	for (i = 0; i < 4; i++)
		sh->io_time[i/2][i%2] = toi_io_time[i/2][i%2];
}

/*
 * rw_init_modules
 *
 * Iterate over modules, preparing the ones that will be used to read or write
 * data.
 */
static int rw_init_modules(int rw, int which)
{
	struct toi_module_ops *this_module;
	/* Initialise page transformers */
	list_for_each_entry(this_module, &toi_filters, type_list) {
		if (!this_module->enabled)
			continue;
		if (this_module->rw_init && this_module->rw_init(rw, which)) {
			abort_hibernate(TOI_FAILED_MODULE_INIT,
				"Failed to initialise the %s filter.",
				this_module->name);
			return 1;
		}
	}

	/* Initialise allocator */
	if (toiActiveAllocator->rw_init(rw, which)) {
		abort_hibernate(TOI_FAILED_MODULE_INIT,
				"Failed to initialise the allocator."); 
		if (!rw)
			toiActiveAllocator->remove_image();
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
			printk("Setting aborted flag due to module init failure.\n");
			return 1;
		}
	}

	return 0;
}

/*
 * rw_cleanup_modules
 *
 * Cleanup components after reading or writing a set of pages.
 * Only the allocator may fail.
 */
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
		if (*my_last_sought && *my_last_high_page && *my_last_sought < orig_page)
			high_page = *my_last_high_page;
		else
			high_page = (struct page *) restore_highmem_pblist;
		this = (struct pbe *) kmap(high_page);
		compare = orig_page;
	} else {
		if (*my_last_sought && *my_last_low_page && *my_last_sought < orig_page)
			this = *my_last_low_page;
		else
			this = restore_pblist;
		compare = page_address(orig_page);
	}

	*my_last_sought = orig_page;

	/* Locate page containing pbe */
	while ( this[PBES_PER_PAGE - 1].next &&
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

/*
 * do_rw_loop
 *
 * The main I/O loop for reading or writing pages.
 */
static int worker_rw_loop(void *data)
{
	unsigned long orig_pfn, write_pfn;
	int result, my_io_index = 0;
	struct toi_module_ops *first_filter = toi_get_next_filter(NULL);
	struct page *buffer = alloc_page(TOI_ATOMIC_GFP);

	atomic_inc(&worker_thread_count);

	mutex_lock(&io_mutex);

	do {
		int buf_size;

		/*
		 * What page to use? If reading, don't know yet which page's
		 * data will be read, so always use the buffer. If writing,
		 * use the copy (Pageset1) or original page (Pageset2), but
		 * always write the pfn of the original page.
		 */
		if (io_write) {
			struct page *page;

			pfn = get_next_bit_on(io_map, pfn);

			/* Another thread could have beaten us to it. */
			if (pfn == max_pfn + 1) {
				if (atomic_read(&io_count)) {
					printk("Ran out of pfns but io_count is still %d.\n", atomic_read(&io_count));
					BUG();
				}
				break;
			}

			atomic_dec(&io_count);

			orig_pfn = pfn;
			write_pfn = pfn;

			/* 
			 * Other_pfn is updated by all threads, so we're not
			 * writing the same page multiple times.
			 */
			clear_dynpageflag(&io_map, pfn_to_page(pfn));
			if (io_pageset == 1) {
				other_pfn = get_next_bit_on(pageset1_map, other_pfn);
				write_pfn = other_pfn;
			}
			page = pfn_to_page(pfn);

			my_io_index = io_finish_at - atomic_read(&io_count);

			mutex_unlock(&io_mutex);

			result = first_filter->write_page(write_pfn, page,
					PAGE_SIZE);
		} else {
			atomic_dec(&io_count);
			mutex_unlock(&io_mutex);

			/* 
			 * Are we aborting? If so, don't submit any more I/O as
			 * resetting the resume_attempted flag (from ui.c) will
			 * clear the bdev flags, making this thread oops.
			 */
			if (unlikely(test_toi_state(TOI_STOP_RESUME))) {
				atomic_dec(&worker_thread_count);
				if (!atomic_read(&worker_thread_count))
					set_toi_state(TOI_IO_STOPPED);
				while (1)
					schedule();
			}

			result = first_filter->read_page(&write_pfn, buffer,
					&buf_size);
			if (buf_size != PAGE_SIZE) {
				abort_hibernate(TOI_FAILED_IO,
					"I/O pipeline returned %d bytes instead "
					"of %d.\n", buf_size, PAGE_SIZE);
				mutex_lock(&io_mutex);
				break;
			}
		}

		if (result) {
			io_result = result;
			if (io_write) {
				printk("Write chunk returned %d.\n", result);
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
				copy_page = copy_page_from_orig_page(final_page);
				BUG_ON(!copy_page);
			}

			if (test_dynpageflag(&io_map, final_page)) {
				virt = kmap(copy_page);
				buffer_virt = kmap(buffer);
				memcpy(virt, buffer_virt, PAGE_SIZE);
				kunmap(copy_page);
				kunmap(buffer);
				clear_dynpageflag(&io_map, final_page);
				mutex_lock(&io_mutex);
				my_io_index = io_finish_at - atomic_read(&io_count);
				mutex_unlock(&io_mutex);
			} else {
				mutex_lock(&io_mutex);
				atomic_inc(&io_count);
				mutex_unlock(&io_mutex);
			}
		}

		/* Strictly speaking, this is racy - another thread could
		 * output the next the next percentage before we've done
		 * ours. 1/5th of the pageset would have to be done first,
		 * though, so I'm not worried. In addition, the only impact
		 * would be messed up output, not image corruption. Doing
		 * this under the mutex seems an unnecessary slowdown.
		 */
		if ((my_io_index + io_base) >= io_nextupdate)
			io_nextupdate = toi_update_status(my_io_index +
				io_base, io_barmax, " %d/%d MB ",
				MB(io_base+my_io_index+1), MB(io_barmax));

		if ((my_io_index + 1) == io_pc) {
			printk("%d%%...", 20 * io_pc_step);
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

	} while(atomic_read(&io_count) >= atomic_read(&worker_thread_count) &&
		!(io_write && test_result_state(TOI_ABORTED)));

	atomic_dec(&worker_thread_count);
	mutex_unlock(&io_mutex);

	__free_pages(buffer, 0);

	return 0;
}

void start_other_threads(void)
{
	int cpu;
	struct task_struct *p;

	for_each_online_cpu(cpu) {
		if (cpu == smp_processor_id())
			continue;

		p = kthread_create(worker_rw_loop, NULL, "ks2io/%d", cpu);
		if (IS_ERR(p)) {
			printk("ks2io for %i failed\n", cpu);
			continue;
		}
		kthread_bind(p, cpu);
		wake_up_process(p);
	}
}

/*
 * do_rw_loop
 *
 * The main I/O loop for reading or writing pages.
 */
static int do_rw_loop(int write, int finish_at, dyn_pageflags_t *pageflags,
		int base, int barmax, int pageset)
{
	int index = 0, cpu;

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
	io_nextupdate = 0;

	for_each_online_cpu(cpu) {
		per_cpu(last_sought, cpu) = NULL;
		per_cpu(last_low_page, cpu) = NULL;
		per_cpu(last_high_page, cpu) = NULL;
	}

	/* Ensure all bits clear */
	pfn = get_next_bit_on(io_map, max_pfn + 1);

	while (pfn < max_pfn + 1) {
		clear_dynpageflag(&io_map, pfn_to_page(pfn));
		pfn = get_next_bit_on(io_map, pfn);
	}

	/* Set the bits for the pages to write */
	pfn = get_next_bit_on(*pageflags, max_pfn + 1);

	while (pfn < max_pfn + 1 && index < finish_at) {
		set_dynpageflag(&io_map, pfn_to_page(pfn));
		pfn = get_next_bit_on(*pageflags, pfn);
		index++;
	}

	BUG_ON(index < finish_at);

	atomic_set(&io_count, finish_at);

	pfn = max_pfn + 1;
	other_pfn = pfn;

	clear_toi_state(TOI_IO_STOPPED);

	if (!test_action_state(TOI_NO_MULTITHREADED_IO))
		start_other_threads();
	worker_rw_loop(NULL);

	while (atomic_read(&worker_thread_count))
		schedule();

	set_toi_state(TOI_IO_STOPPED);
	if (unlikely(test_toi_state(TOI_STOP_RESUME))) {
		while (1)
			schedule();
	}

	if (!io_result) {
		printk("done.\n");

		toi_update_status(io_base + io_finish_at, io_barmax, " %d/%d MB ",
				MB(io_base + io_finish_at), MB(io_barmax));
	}

	if (io_write && test_result_state(TOI_ABORTED))
		io_result = 1;
	else /* All I/O done? */
		BUG_ON(get_next_bit_on(io_map, max_pfn + 1) != max_pfn + 1);

	return io_result;
}

/* write_pageset()
 *
 * Description:	Write a pageset to disk.
 * Arguments:	pagedir:	Which pagedir to write..
 * Returns:	Zero on success or -1 on failure.
 */

int write_pageset(struct pagedir *pagedir)
{
	int finish_at, base = 0, start_time, end_time;
	int barmax = pagedir1.size + pagedir2.size;
	long error = 0;
	dyn_pageflags_t *pageflags;

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
			pageflags = &pageset1_map;
		else
			pageflags = &pageset1_copy_map;
	} else {
		toi_prepare_status(CLEAR_BAR, "Writing caches...");
		pageflags = &pageset2_map;
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
		toi_io_time[0][0] += finish_at,
		toi_io_time[0][1] += (end_time - start_time);
	}

	return error;
}

/* read_pageset()
 *
 * Description:	Read a pageset from disk.
 * Arguments:	whichtowrite:	Controls what debugging output is printed.
 * 		overwrittenpagesonly: Whether to read the whole pageset or
 * 		only part.
 * Returns:	Zero on success or -1 on failure.
 */

static int read_pageset(struct pagedir *pagedir, int overwrittenpagesonly)
{
	int result = 0, base = 0, start_time, end_time;
	int finish_at = pagedir->size;
	int barmax = pagedir1.size + pagedir2.size;
	dyn_pageflags_t *pageflags;

	if (pagedir->id == 1) {
		toi_prepare_status(CLEAR_BAR,
				"Reading kernel & process data...");
		pageflags = &pageset1_map;
	} else {
		toi_prepare_status(DONT_CLEAR_BAR, "Reading caches...");
		if (overwrittenpagesonly)
			barmax = finish_at = min(pagedir1.size, 
						 pagedir2.size);
		else {
			base = pagedir1.size;
		}
		pageflags = &pageset2_map;
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
	end_time=jiffies;

	if ((end_time - start_time) && (!test_result_state(TOI_ABORTED))) {
		toi_io_time[1][0] += finish_at,
		toi_io_time[1][1] += (end_time - start_time);
	}

	return result;
}

/* write_module_configs()
 *
 * Description:	Store the configuration for each module in the image header.
 * Returns:	Int: Zero on success, Error value otherwise.
 */
static int write_module_configs(void)
{
	struct toi_module_ops *this_module;
	char *buffer = (char *) get_zeroed_page(TOI_ATOMIC_GFP);
	int len, index = 1;
	struct toi_module_header toi_module_header;

	if (!buffer) {
		printk("Failed to allocate a buffer for saving "
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
	toiActiveAllocator->rw_header_chunk(WRITE, 
			NULL,
			(char *) &toi_module_header,
			sizeof(toi_module_header));

	free_page((unsigned long) buffer);
	return 0;
}

/* read_module_configs()
 *
 * Description:	Reload module configurations from the image header.
 * Returns:	Int. Zero on success, error value otherwise.
 */

static int read_module_configs(void)
{
	struct toi_module_ops *this_module;
	char *buffer = (char *) get_zeroed_page(TOI_ATOMIC_GFP);
	int len, result = 0;
	struct toi_module_header toi_module_header;

	if (!buffer) {
		printk("Failed to allocate a buffer for reloading module "
				"configuration info.\n");
		return -ENOMEM;
	}
		
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
		printk("Failed to read the next module header.\n");
		free_page((unsigned long) buffer);
		return -EINVAL;
	}

	/* For each module (in registration order) */
	while (toi_module_header.name[0]) {

		/* Find the module */
		this_module = toi_find_module_given_name(toi_module_header.name);

		if (!this_module) {
			/* 
			 * Is it used? Only need to worry about filters. The active
			 * allocator must be loaded!
			 */
			if (toi_module_header.enabled) {
				toi_early_boot_message(1, TOI_CONTINUE_REQ,
					"It looks like we need module %s for "
					"reading the image but it hasn't been "
					"registered.\n",
					toi_module_header.name);
				if (!(test_toi_state(TOI_CONTINUE_REQ))) {
					toiActiveAllocator->remove_image();
					free_page((unsigned long) buffer);
					return -EINVAL;
				}
			} else
				printk("Module %s configuration data found, but"
					" the module hasn't registered. Looks "
					"like it was disabled, so we're "
					"ignoring it's data.",
					toi_module_header.name);
		}
		
		/* Get the length of the data (if any) */
		result = toiActiveAllocator->rw_header_chunk(READ, NULL,
				(char *) &len, sizeof(int));
		if (result) {
			printk("Failed to read the length of the module %s's"
					" configuration data.\n",
					toi_module_header.name);
			free_page((unsigned long) buffer);
			return -EINVAL;
		}

		/* Read any data and pass to the module (if we found one) */
		if (len) {
			toiActiveAllocator->rw_header_chunk(READ, NULL,
					buffer, len);
			if (this_module) {
				if (!this_module->save_config_info) {
					printk("Huh? Module %s appears to have "
						"a save_config_info, but not a "
						"load_config_info function!\n",
						this_module->name);
				} else
					this_module->load_config_info(buffer, len);
			}
		}

		if (this_module) {
			/* Now move this module to the tail of its lists. This
			 * will put it in order. Any new modules will end up at
			 * the top of the lists. They should have been set to
			 * disabled when loaded (people will normally not edit
			 * an initrd to load a new module and then hibernate
			 * without using it!).
			 */

			toi_move_module_tail(this_module);

			/* 
			 * We apply the disabled state; modules don't need to
			 * save whether they were disabled and if they do, we
			 * override them anyway.
			 */
			this_module->enabled = toi_module_header.enabled;
		}

		/* Get the next module header */
		result = toiActiveAllocator->rw_header_chunk(READ, NULL,
				(char *) &toi_module_header,
				sizeof(toi_module_header));

		if (result) {
			printk("Failed to read the next module header.\n");
			free_page((unsigned long) buffer);
			return -EINVAL;
		}

	}

	free_page((unsigned long) buffer);
	return 0;
}

/* write_image_header()
 *
 * Description:	Write the image header after write the image proper.
 * Returns:	Int. Zero on success or -1 on failure.
 */

int write_image_header(void)
{
	int ret;
	int total = pagedir1.size + pagedir2.size+2;
	char *header_buffer = NULL;

	/* Now prepare to write the header */
	if ((ret = toiActiveAllocator->write_header_init())) {
		abort_hibernate(TOI_FAILED_MODULE_INIT,
				"Active allocator's write_header_init"
				" function failed.");
		goto write_image_header_abort;
	}

	/* Get a buffer */
	header_buffer = (char *) get_zeroed_page(TOI_ATOMIC_GFP);
	if (!header_buffer) {
		abort_hibernate(TOI_OUT_OF_MEMORY,
			"Out of memory when trying to get page for header!");
		goto write_image_header_abort;
	}

	/* Write hibernate header */
	fill_toi_header((struct toi_header *) header_buffer);
	toiActiveAllocator->rw_header_chunk(WRITE, NULL,
			header_buffer, sizeof(struct toi_header));

	free_page((unsigned long) header_buffer);

	/* Write module configurations */
	if ((ret = write_module_configs())) {
		abort_hibernate(TOI_FAILED_IO,
				"Failed to write module configs.");
		goto write_image_header_abort;
	}

	save_dyn_pageflags(pageset1_map);

	/* Flush data and let allocator cleanup */
	if (toiActiveAllocator->write_header_cleanup()) {
		abort_hibernate(TOI_FAILED_IO,
				"Failed to cleanup writing header.");
		goto write_image_header_abort_no_cleanup;
	}

	if (test_result_state(TOI_ABORTED))
		goto write_image_header_abort_no_cleanup;

	toi_message(TOI_IO, TOI_VERBOSE, 1, "|\n");
	toi_update_status(total, total, NULL);

	return 0;

write_image_header_abort:
	toiActiveAllocator->write_header_cleanup();
write_image_header_abort_no_cleanup:
	return -1;
}

/* sanity_check()
 *
 * Description:	Perform a few checks, seeking to ensure that the kernel being
 * 		booted matches the one hibernated. They need to match so we can
 * 		be _sure_ things will work. It is not absolutely impossible for
 * 		resuming from a different kernel to work, just not assured.
 * Arguments:	Struct toi_header. The header which was saved at hibernate
 * 		time.
 */
static char *sanity_check(struct toi_header *sh)
{
	if (sh->version_code != LINUX_VERSION_CODE)
		return "Incorrect kernel version.";
	
	if (sh->num_physpages != num_physpages)
		return "Incorrect memory size.";

	if (strncmp(sh->uts.sysname, init_utsname()->sysname, 65))
		return "Incorrect system type.";

	if (strncmp(sh->uts.release, init_utsname()->release, 65))
		return "Incorrect release.";

	if (strncmp(sh->uts.version, init_utsname()->version, 65))
		return "Right kernel version but wrong build number.";

	if (strncmp(sh->uts.machine, init_utsname()->machine, 65))
		return "Incorrect machine type.";

	if (sh->page_size != PAGE_SIZE)
		return "Incorrect PAGE_SIZE.";

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

	return 0;
}

/* __read_pageset1
 *
 * Description:	Test for the existence of an image and attempt to load it.
 * Returns:	Int. Zero if image found and pageset1 successfully loaded.
 * 		Error if no image found or loaded.
 */
static int __read_pageset1(void)
{			
	int i, result = 0;
	char *header_buffer = (char *) get_zeroed_page(TOI_ATOMIC_GFP),
	     *sanity_error = NULL;
	struct toi_header *toi_header;

	if (!header_buffer) {
		printk("Unable to allocate a page for reading the signature.\n");
		return -ENOMEM;
	}
	
	/* Check for an image */
	if (!(result = toiActiveAllocator->image_exists())) {
		result = -ENODATA;
		noresume_reset_modules();
		printk("TuxOnIce: No image found.\n");
		goto out;
	}

	/* Check for noresume command line option */
	if (test_toi_state(TOI_NORESUME_SPECIFIED)) {
		printk("TuxOnIce: Noresume on command line. Removed image.\n");
		goto out_remove_image;
	}

	/* Check whether we've resumed before */
	if (test_toi_state(TOI_RESUMED_BEFORE)) {
		toi_early_boot_message(1, 0, NULL);
		if (!(test_toi_state(TOI_CONTINUE_REQ))) {
			printk("TuxOnIce: Tried to resume before: "
					"Invalidated image.\n");
			goto out_remove_image;
		}
	}

	clear_toi_state(TOI_CONTINUE_REQ);

	/* 
	 * Prepare the active allocator for reading the image header. The
	 * activate allocator might read its own configuration.
	 * 
	 * NB: This call may never return because there might be a signature
	 * for a different image such that we warn the user and they choose
	 * to reboot. (If the device ids look erroneous (2.4 vs 2.6) or the
	 * location of the image might be unavailable if it was stored on a
	 * network connection.
	 */

	if ((result = toiActiveAllocator->read_header_init())) {
		printk("TuxOnIce: Failed to initialise, reading the image "
				"header.\n");
		goto out_remove_image;
	}
	
	/* Read hibernate header */
	if ((result = toiActiveAllocator->rw_header_chunk(READ, NULL,
			header_buffer, sizeof(struct toi_header))) < 0) {
		printk("TuxOnIce: Failed to read the image signature.\n");
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
		printk("TuxOnIce: Sanity check failed.\n");
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
	toi_action = toi_header->param1;
	toi_debug_state = toi_header->param2;
	toi_default_console_level = toi_header->param3;
	clear_toi_state(TOI_IGNORE_LOGLEVEL);
	pagedir2.size = toi_header->pageset_2_size;
	for (i = 0; i < 4; i++)
		toi_io_time[i/2][i%2] =
			toi_header->io_time[i/2][i%2];

	/* Read module configurations */
	if ((result = read_module_configs())) {
		pagedir1.size = pagedir2.size = 0;
		printk("TuxOnIce: Failed to read TuxOnIce module "
				"configurations.\n");
		clear_action_state(TOI_KEEP_IMAGE);
		goto out_remove_image;
	}

	toi_prepare_console();

	set_toi_state(TOI_NOW_RESUMING);

	if (pre_resume_freeze())
		goto out_reset_console;

	toi_cond_pause(1, "About to read original pageset1 locations.");

	/*
	 * Read original pageset1 locations. These are the addresses we can't
	 * use for the data to be restored.
	 */

	if (allocate_dyn_pageflags(&pageset1_map) ||
	    allocate_dyn_pageflags(&pageset1_copy_map) ||
	    allocate_dyn_pageflags(&io_map))
		goto out_reset_console;

	if (load_dyn_pageflags(pageset1_map))
		goto out_reset_console;

	/* Clean up after reading the header */
	if ((result = toiActiveAllocator->read_header_cleanup())) {
		printk("TuxOnIce: Failed to cleanup after reading the image "
				"header.\n");
		goto out_reset_console;
	}

	toi_cond_pause(1, "About to read pagedir.");

	/* 
	 * Get the addresses of pages into which we will load the kernel to
	 * be copied back
	 */
	if (toi_get_pageset1_load_addresses()) {
		printk("TuxOnIce: Failed to get load addresses for pageset1.\n");
		goto out_reset_console;
	}

	/* Read the original kernel back */
	toi_cond_pause(1, "About to read pageset 1.");

	if (read_pageset(&pagedir1, 0)) {
		toi_prepare_status(CLEAR_BAR, "Failed to read pageset 1.");
		result = -EIO;
		printk("TuxOnIce: Failed to get load pageset1.\n");
		goto out_reset_console;
	}

	toi_cond_pause(1, "About to restore original kernel.");
	result = 0;

	if (!test_action_state(TOI_KEEP_IMAGE) &&
	    toiActiveAllocator->mark_resume_attempted)
		toiActiveAllocator->mark_resume_attempted(1);

out:
	free_page((unsigned long) header_buffer);
	return result;

out_reset_console:
	toi_cleanup_console();

out_remove_image:
	free_dyn_pageflags(&pageset1_map);
	free_dyn_pageflags(&pageset1_copy_map);
	free_dyn_pageflags(&io_map);
	result = -EINVAL;
	if (!test_action_state(TOI_KEEP_IMAGE))
		toiActiveAllocator->remove_image();
	toiActiveAllocator->read_header_cleanup();
	noresume_reset_modules();
	goto out;
}

/* read_pageset1()
 *
 * Description:	Attempt to read the header and pageset1 of a hibernate image.
 * 		Handle the outcome, complaining where appropriate.
 */

int read_pageset1(void)
{
	int error;

	error = __read_pageset1();

	switch (error) {
		case 0:
		case -ENODATA:
		case -EINVAL:	/* non fatal error */
			break;
		default:
			if (test_result_state(TOI_ABORTED))
				break;

			abort_hibernate(TOI_IMAGE_ERROR,
					"TuxOnIce: Error %d resuming\n",
					error);
	}
	return error;
}

/*
 * get_have_image_data()
 */
static char *get_have_image_data(void)
{
	char *output_buffer = (char *) get_zeroed_page(TOI_ATOMIC_GFP);
	struct toi_header *toi_header;

	if (!output_buffer) {
		printk("Output buffer null.\n");
		return NULL;
	}

	/* Check for an image */
	if (!toiActiveAllocator->image_exists() ||
	    toiActiveAllocator->read_header_init() ||
	    toiActiveAllocator->rw_header_chunk(READ, NULL,
			output_buffer, sizeof(struct toi_header))) {
		sprintf(output_buffer, "0\n");
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

/* read_pageset2()
 *
 * Description:	Read in part or all of pageset2 of an image, depending upon
 * 		whether we are hibernating and have only overwritten a portion
 * 		with pageset1 pages, or are resuming and need to read them 
 * 		all.
 * Arguments:	Int. Boolean. Read only pages which would have been
 * 		overwritten by pageset1?
 * Returns:	Int. Zero if no error, otherwise the error value.
 */
int read_pageset2(int overwrittenpagesonly)
{
	int result = 0;

	if (!pagedir2.size)
		return 0;

	result = read_pageset(&pagedir2, overwrittenpagesonly);

	toi_update_status(100, 100, NULL);
	toi_cond_pause(1, "Pagedir 2 read.");

	return result;
}

/* image_exists_read
 * 
 * Return 0 or 1, depending on whether an image is found.
 * Incoming buffer is PAGE_SIZE and result is guaranteed
 * to be far less than that, so we don't worry about
 * overflow.
 */
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
			free_page((unsigned long) result);
		}
	}

	toi_deactivate_storage(0);

	return len;
}

/* image_exists_write
 * 
 * Invalidate an image if one exists.
 */
int image_exists_write(const char *buffer, int count)
{
	if (toi_activate_storage(0))
		return count;

	if (toiActiveAllocator && toiActiveAllocator->image_exists())
		toiActiveAllocator->remove_image();

	toi_deactivate_storage(0);

	clear_result_state(TOI_KEPT_IMAGE);

	return count;
}

#ifdef CONFIG_TOI_EXPORTS
EXPORT_SYMBOL_GPL(toi_attempt_to_parse_resume_device);
EXPORT_SYMBOL_GPL(attempt_to_parse_resume_device2);
#endif

