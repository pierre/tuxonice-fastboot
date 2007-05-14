/*
 * kernel/power/suspend.c
 */
/** \mainpage Suspend2.
 *
 * Suspend2 provides support for saving and restoring an image of
 * system memory to an arbitrary storage device, either on the local computer,
 * or across some network. The support is entirely OS based, so Suspend2 
 * works without requiring BIOS, APM or ACPI support. The vast majority of the
 * code is also architecture independant, so it should be very easy to port
 * the code to new architectures. Suspend includes support for SMP, 4G HighMem
 * and preemption. Initramfses and initrds are also supported.
 *
 * Suspend2 uses a modular design, in which the method of storing the image is
 * completely abstracted from the core code, as are transformations on the data
 * such as compression and/or encryption (multiple 'modules' can be used to
 * provide arbitrary combinations of functionality). The user interface is also
 * modular, so that arbitrarily simple or complex interfaces can be used to
 * provide anything from debugging information through to eye candy.
 * 
 * \section Copyright
 *
 * Suspend2 is released under the GPLv2.
 *
 * Copyright (C) 1998-2001 Gabor Kuti <seasons@fornax.hu><BR>
 * Copyright (C) 1998,2001,2002 Pavel Machek <pavel@suse.cz><BR>
 * Copyright (C) 2002-2003 Florent Chabaud <fchabaud@free.fr><BR>
 * Copyright (C) 2002-2007 Nigel Cunningham (nigel at suspend2 net)<BR>
 *
 * \section Credits
 * 
 * Nigel would like to thank the following people for their work:
 * 
 * Bernard Blackham <bernard@blackham.com.au><BR>
 * Web page & Wiki administration, some coding. A person without whom
 * Suspend would not be where it is.
 *
 * Michael Frank <mhf@linuxmail.org><BR>
 * Extensive testing and help with improving stability. I was constantly
 * amazed by the quality and quantity of Michael's help.
 *
 * Pavel Machek <pavel@ucw.cz><BR>
 * Modifications, defectiveness pointing, being with Gabor at the very beginning,
 * suspend to swap space, stop all tasks. Port to 2.4.18-ac and 2.5.17. Even
 * though Pavel and I disagree on the direction suspend to disk should take, I
 * appreciate the valuable work he did in helping Gabor get the concept working.
 *
 * ..and of course the myriads of Suspend2 users who have helped diagnose
 * and fix bugs, made suggestions on how to improve the code, proofread
 * documentation, and donated time and money.
 *
 * Thanks also to corporate sponsors:
 *
 * <B>Redhat.</B>Sometime employer from May 2006 (my fault, not Redhat's!).
 *
 * <B>Cyclades.com.</B> Nigel's employers from Dec 2004 until May 2006, who
 * allowed him to work on Suspend and PM related issues on company time.
 * 
 * <B>LinuxFund.org.</B> Sponsored Nigel's work on Suspend for four months Oct 2003
 * to Jan 2004.
 *
 * <B>LAC Linux.</B> Donated P4 hardware that enabled development and ongoing
 * maintenance of SMP and Highmem support.
 *
 * <B>OSDL.</B> Provided access to various hardware configurations, make occasional
 * small donations to the project.
 */

#include <linux/suspend.h>
#include <linux/writeback.h>
#include <linux/freezer.h>
#include <linux/utsrelease.h>
#include <linux/cpu.h>

#include "modules.h"
#include "sysfs.h"
#include "prepare_image.h"
#include "io.h"
#include "ui.h"
#include "power_off.h"
#include "storage.h"
#include "checksum.h"
#include "cluster.h"
#include "suspend_builtin.h"

/*! Pageset metadata. */
struct pagedir pagedir2 = {2}; 

static int had_pmsem = 0;
static mm_segment_t oldfs;
static atomic_t actions_running;
static int block_dump_save;

int do_suspend2_step(int step);

/*
 * Basic clean-up routine.
 */
void suspend_finish_anything(int suspend_or_resume)
{
	if (!atomic_dec_and_test(&actions_running))
		return;

	suspend_cleanup_modules(suspend_or_resume);
	suspend_put_modules();
	clear_suspend_state(SUSPEND_RUNNING);
	set_fs(oldfs);
	if (suspend_or_resume) {
		block_dump = block_dump_save;
		set_cpus_allowed(current, CPU_MASK_ALL);
	}
}

/*
 * Basic set-up routine.
 */
int suspend_start_anything(int suspend_or_resume)
{
	if (atomic_add_return(1, &actions_running) != 1) {
		if (suspend_or_resume) {
			printk("Can't start a cycle when actions are "
					"already running.\n");
			atomic_dec(&actions_running);
			return -EBUSY;
		} else
			return 0;
	}

	oldfs = get_fs();
	set_fs(KERNEL_DS);

	if (!suspendActiveAllocator) {
		/* Be quiet if we're not trying to suspend or resume */ 
		if (suspend_or_resume)
			printk("No storage allocator is currently active. "
					"Rechecking whether we can use one.\n");
		suspend_attempt_to_parse_resume_device(!suspend_or_resume);
	}

	set_suspend_state(SUSPEND_RUNNING);

	if (suspend_get_modules()) {
		printk("Suspend2: Get modules failed!\n");
		goto out_err;
	}

	if (suspend_initialise_modules(suspend_or_resume)) {
		printk("Suspend2: Initialise modules failed!\n");
		goto out_err;
	}

	if (suspend_or_resume) {
		block_dump_save = block_dump;
		block_dump = 0;
		set_cpus_allowed(current, CPU_MASK_CPU0);
	}

	return 0;

out_err:
	if (suspend_or_resume)
		block_dump_save = block_dump;
	suspend_finish_anything(suspend_or_resume);
	return -EBUSY;
}

/*
 * Nosave page tracking.
 *
 * Here rather than in prepare_image because we want to do it once only at the
 * start of a cycle.
 */
extern struct list_head nosave_regions;

struct nosave_region {
	struct list_head list;
	unsigned long start_pfn;
	unsigned long end_pfn;
};

static void mark_nosave_pages(void)
{
	struct nosave_region *region;

	list_for_each_entry(region, &nosave_regions, list) {
		unsigned long pfn;

		for (pfn = region->start_pfn; pfn < region->end_pfn; pfn++)
			SetPageNosave(pfn_to_page(pfn));
	}
}

/*
 * Allocate & free bitmaps.
 */
static int allocate_bitmaps(void)
{
	if (allocate_dyn_pageflags(&pageset1_map) ||
	    allocate_dyn_pageflags(&pageset1_copy_map) ||
	    allocate_dyn_pageflags(&pageset2_map) ||
	    allocate_dyn_pageflags(&io_map) ||
	    allocate_dyn_pageflags(&nosave_map) ||
	    allocate_dyn_pageflags(&free_map) ||
	    allocate_dyn_pageflags(&page_resave_map))
		return 1;

	return 0;
}

static void free_bitmaps(void)
{
	free_dyn_pageflags(&pageset1_map);
	free_dyn_pageflags(&pageset1_copy_map);
	free_dyn_pageflags(&pageset2_map);
	free_dyn_pageflags(&io_map);
	free_dyn_pageflags(&nosave_map);
	free_dyn_pageflags(&free_map);
	free_dyn_pageflags(&page_resave_map);
}

static int io_MB_per_second(int read_write)
{
	return (suspend_io_time[read_write][1]) ?
		MB((unsigned long) suspend_io_time[read_write][0]) * HZ /
		suspend_io_time[read_write][1] : 0;
}

/* get_debug_info
 * Functionality:	Store debug info in a buffer.
 */
#define SNPRINTF(a...) 	len += snprintf_used(((char *)buffer) + len, \
		count - len - 1, ## a)
static int get_suspend_debug_info(const char *buffer, int count)
{
	int len = 0;

	SNPRINTF("Suspend2 debugging info:\n");
	SNPRINTF("- Suspend core   : %s\n", SUSPEND_CORE_VERSION);
	SNPRINTF("- Kernel Version : %s\n", UTS_RELEASE);
	SNPRINTF("- Compiler vers. : %d.%d\n", __GNUC__, __GNUC_MINOR__);
	SNPRINTF("- Attempt number : %d\n", nr_suspends);
	SNPRINTF("- Parameters     : %ld %ld %ld %d %d %ld\n",
			suspend_result,
			suspend_action,
			suspend_debug_state,
			suspend_default_console_level,
			image_size_limit,
			suspend_powerdown_method);
	SNPRINTF("- Overall expected compression percentage: %d.\n",
			100 - suspend_expected_compression_ratio());
	len+= suspend_print_module_debug_info(((char *) buffer) + len, 
			count - len - 1);
	if (suspend_io_time[0][1]) {
		if ((io_MB_per_second(0) < 5) || (io_MB_per_second(1) < 5)) {
			SNPRINTF("- I/O speed: Write %d KB/s",
			  (KB((unsigned long) suspend_io_time[0][0]) * HZ /
			  suspend_io_time[0][1]));
			if (suspend_io_time[1][1])
				SNPRINTF(", Read %d KB/s",
				  (KB((unsigned long) suspend_io_time[1][0]) * HZ /
				  suspend_io_time[1][1]));
		} else {
			SNPRINTF("- I/O speed: Write %d MB/s",
			 (MB((unsigned long) suspend_io_time[0][0]) * HZ /
			  suspend_io_time[0][1]));
			if (suspend_io_time[1][1])
				SNPRINTF(", Read %d MB/s",
				 (MB((unsigned long) suspend_io_time[1][0]) * HZ /
				  suspend_io_time[1][1]));
		}
		SNPRINTF(".\n");
	}
	else
		SNPRINTF("- No I/O speed stats available.\n");
	SNPRINTF("- Extra pages    : %d used/%d.\n",
			extra_pd1_pages_used, extra_pd1_pages_allowance);

	return len;
}

/*
 * do_cleanup
 */

static void do_cleanup(void)
{
	int i = 0;
	char *buffer;;

	suspend_prepare_status(DONT_CLEAR_BAR, "Cleaning up...");
	relink_lru_lists();

	free_checksum_pages();

	buffer = (char *) get_zeroed_page(GFP_ATOMIC);

	if (buffer)
		i = get_suspend_debug_info(buffer, PAGE_SIZE);

	suspend_free_extra_pagedir_memory();
	
	pagedir1.size = pagedir2.size = 0;
	set_highmem_size(pagedir1, 0);
	set_highmem_size(pagedir2, 0);

	restore_avenrun();

	thaw_processes();

#ifdef CONFIG_SUSPEND2_KEEP_IMAGE
	if (test_action_state(SUSPEND_KEEP_IMAGE) &&
	    !test_result_state(SUSPEND_ABORTED)) {
		suspend_message(SUSPEND_ANY_SECTION, SUSPEND_LOW, 1,
			"Suspend2: Not invalidating the image due "
			"to Keep Image being enabled.\n");
		set_result_state(SUSPEND_KEPT_IMAGE);
	} else
#endif
		if (suspendActiveAllocator)
			suspendActiveAllocator->invalidate_image();

	free_bitmaps();

	if (buffer && i) {
		/* Printk can only handle 1023 bytes, including
		 * its level mangling. */
		for (i = 0; i < 3; i++)
			printk("%s", buffer + (1023 * i));
		free_page((unsigned long) buffer);
	}

	if (!test_action_state(SUSPEND_LATE_CPU_HOTPLUG))
		enable_nonboot_cpus();
	suspend_cleanup_console();

	suspend_deactivate_storage(0);

	clear_suspend_state(SUSPEND_IGNORE_LOGLEVEL);
	clear_suspend_state(SUSPEND_TRYING_TO_RESUME);
	clear_suspend_state(SUSPEND_NOW_RESUMING);

	if (!had_pmsem)
		mutex_unlock(&pm_mutex);
}

static int check_still_keeping_image(void)
{
	if (test_action_state(SUSPEND_KEEP_IMAGE)) {
		printk("Image already stored: powering down immediately.");
		do_suspend2_step(STEP_SUSPEND_POWERDOWN);
		return 1;	/* Just in case we're using S3 */
	}

	printk("Invalidating previous image.\n");
	suspendActiveAllocator->invalidate_image();

	return 0;
}

static int suspend_init(void)
{
	suspend_result = 0;

	printk(KERN_INFO "Suspend2: Initiating a software suspend cycle.\n");

	nr_suspends++;
	
	save_avenrun();

	suspend_io_time[0][0] = suspend_io_time[0][1] = 
		suspend_io_time[1][0] =	suspend_io_time[1][1] = 0;

	if (!test_suspend_state(SUSPEND_CAN_SUSPEND) ||
	    allocate_bitmaps())
		return 0;
	
	mark_nosave_pages();

	suspend_prepare_console();
	if (!test_action_state(SUSPEND_LATE_CPU_HOTPLUG))
		disable_nonboot_cpus();

	return 1;
}

static int can_suspend(void)
{
	if (!had_pmsem && !mutex_trylock(&pm_mutex)) {
		printk("Suspend2: Failed to obtain pm_mutex.\n");
		set_result_state(SUSPEND_ABORTED);
		set_result_state(SUSPEND_PM_SEM);
		return 0;
	}

	if (!test_suspend_state(SUSPEND_CAN_SUSPEND))
		suspend_attempt_to_parse_resume_device(0);

	if (!test_suspend_state(SUSPEND_CAN_SUSPEND)) {
		printk("Suspend2: Software suspend is disabled.\n"
			"This may be because you haven't put something along "
			"the lines of\n\nresume2=swap:/dev/hda1\n\n"
			"in lilo.conf or equivalent. (Where /dev/hda1 is your "
			"swap partition).\n");
		set_result_state(SUSPEND_ABORTED);
		if (!had_pmsem)
			mutex_unlock(&pm_mutex);
		return 0;
	}
	
	return 1;
}

static int do_power_down(void)
{
	/* If switching images fails, do normal powerdown */
	if (poweroff_resume2[0])
		do_suspend2_step(STEP_RESUME_ALT_IMAGE);

	suspend_cond_pause(1, "About to power down or reboot.");
	suspend_power_down();

	/* If we return, it's because we suspended to ram */
	if (read_pageset2(1))
		panic("Attempt to reload pagedir 2 failed. Try rebooting.");

	barrier();
	mb();
	do_cleanup();
	return 0;
}

/*
 * __save_image
 * Functionality    : High level routine which performs the steps necessary
 *                    to save the image after preparatory steps have been taken.
 * Key Assumptions  : Processes frozen, sufficient memory available, drivers
 *                    suspended.
 */
static int __save_image(void)
{
	int temp_result;

	suspend_prepare_status(DONT_CLEAR_BAR, "Starting to save the image..");

	suspend_message(SUSPEND_ANY_SECTION, SUSPEND_LOW, 1,
		" - Final values: %d and %d.\n",
		pagedir1.size, pagedir2.size);

	suspend_cond_pause(1, "About to write pagedir2.");

	calculate_check_checksums(0);

	temp_result = write_pageset(&pagedir2);
	
	if (temp_result == -1 || test_result_state(SUSPEND_ABORTED))
		return 1;

	suspend_cond_pause(1, "About to copy pageset 1.");

	if (test_result_state(SUSPEND_ABORTED))
		return 1;

	suspend_deactivate_storage(1);

	suspend_prepare_status(DONT_CLEAR_BAR, "Doing atomic copy.");
	
	suspend2_in_suspend = 1;
	
	if (device_suspend(PMSG_FREEZE)) {
		set_result_state(SUSPEND_DEVICE_REFUSED);
		set_result_state(SUSPEND_ABORTED);
		return 1;
	}
	
	if (test_action_state(SUSPEND_LATE_CPU_HOTPLUG))
		disable_nonboot_cpus();

	temp_result = suspend2_suspend();

	if (pm_ops && pm_ops->finish && suspend_powerdown_method > 3)
		pm_ops->finish(suspend_powerdown_method);

	if (test_action_state(SUSPEND_LATE_CPU_HOTPLUG))
		enable_nonboot_cpus();

	device_resume();

	if (temp_result)
		return 1;
	
	/* Resume time? */
	if (!suspend2_in_suspend) {
		copyback_post();
		return 0;
	}

	/* Nope. Suspending. So, see if we can save the image... */

	if (suspend_activate_storage(1))
		panic("Failed to reactivate our storage.");
	
	suspend_update_status(pagedir2.size,
			pagedir1.size + pagedir2.size,
			NULL);
	
	if (test_result_state(SUSPEND_ABORTED))
		goto abort_reloading_pagedir_two;

	suspend_cond_pause(1, "About to write pageset1.");

	suspend_message(SUSPEND_ANY_SECTION, SUSPEND_LOW, 1,
			"-- Writing pageset1\n");

	temp_result = write_pageset(&pagedir1);

	/* We didn't overwrite any memory, so no reread needs to be done. */
	if (test_action_state(SUSPEND_TEST_FILTER_SPEED))
		return 1;

	if (temp_result == 1 || test_result_state(SUSPEND_ABORTED))
		goto abort_reloading_pagedir_two;

	suspend_cond_pause(1, "About to write header.");

	if (test_result_state(SUSPEND_ABORTED))
		goto abort_reloading_pagedir_two;

	temp_result = write_image_header();

	if (test_action_state(SUSPEND_TEST_BIO))
		return 1;

	if (!temp_result && !test_result_state(SUSPEND_ABORTED))
		return 0;

abort_reloading_pagedir_two:
	temp_result = read_pageset2(1);

	/* If that failed, we're sunk. Panic! */
	if (temp_result)
		panic("Attempt to reload pagedir 2 while aborting "
				"a suspend failed.");

	return 1;
}

/* 
 * do_save_image
 *
 * Save the prepared image.
 */

static int do_save_image(void)
{
	int result = __save_image();
	if (!suspend2_in_suspend || result)
		do_cleanup();
	return result;
}


/* do_prepare_image
 *
 * Seek to initialise and prepare an image to be saved. On failure,
 * cleanup.
 */

static int do_prepare_image(void)
{
	if (suspend_activate_storage(0))
		return 1;

	/*
	 * If kept image and still keeping image and suspending to RAM, we will 
	 * return 1 after suspending and resuming (provided the power doesn't
	 * run out.
	 */

	if (!can_suspend() ||
	    (test_result_state(SUSPEND_KEPT_IMAGE) &&
	     check_still_keeping_image()))
		goto cleanup;

	if (suspend_init() && !suspend_prepare_image() &&
			!test_result_state(SUSPEND_ABORTED))
		return 0;

cleanup:
	do_cleanup();
	return 1;
}

static int do_check_can_resume(void)
{
	char *buf = (char *) get_zeroed_page(GFP_KERNEL);
	int result = 0;

	if (!buf)
		return 0;

	/* Only interested in first byte, so throw away return code. */
	image_exists_read(buf, PAGE_SIZE);

	if (buf[0] == '1')
		result = 1;

	free_page((unsigned long) buf);
	return result;
}

/*
 * We check if we have an image and if so we try to resume.
 */
static int do_load_atomic_copy(void)
{
	int read_image_result = 0;

	if (sizeof(swp_entry_t) != sizeof(long)) {
		printk(KERN_WARNING "Suspend2: "
			"The size of swp_entry_t != size of long. "
			"Please report this!\n");
		return 1;
	}
	
	if (!resume2_file[0])
		printk(KERN_WARNING "Suspend2: "
			"You need to use a resume2= command line parameter to "
			"tell Suspend2 where to look for an image.\n");

	suspend_activate_storage(0);

	if (!(test_suspend_state(SUSPEND_RESUME_DEVICE_OK)) &&
		!suspend_attempt_to_parse_resume_device(0)) {
		/* 
		 * Without a usable storage device we can do nothing - 
		 * even if noresume is given
		 */

		if (!suspendNumAllocators)
			printk(KERN_ALERT "Suspend2: "
			  "No storage allocators have been registered.\n");
		else
			printk(KERN_ALERT "Suspend2: "
				"Missing or invalid storage location "
				"(resume2= parameter). Please correct and "
				"rerun lilo (or equivalent) before "
				"suspending.\n");
		suspend_deactivate_storage(0);
		return 1;
	}

	read_image_result = read_pageset1(); /* non fatal error ignored */

	if (test_suspend_state(SUSPEND_NORESUME_SPECIFIED)) {
		printk(KERN_WARNING "Suspend2: Resuming disabled as requested.\n");
		clear_suspend_state(SUSPEND_NORESUME_SPECIFIED);
	}

	suspend_deactivate_storage(0);
	
	if (read_image_result)
		return 1;

	return 0;
}

void prepare_restore_load_alt_image(int prepare)
{
	static dyn_pageflags_t pageset1_map_save, pageset1_copy_map_save;

	if (prepare) {
		pageset1_map_save = pageset1_map;
		pageset1_map = NULL;
		pageset1_copy_map_save = pageset1_copy_map;
		pageset1_copy_map = NULL;
		set_suspend_state(SUSPEND_LOADING_ALT_IMAGE);
		suspend_reset_alt_image_pageset2_pfn();
	} else {
		if (pageset1_map)
			free_dyn_pageflags(&pageset1_map);
		pageset1_map = pageset1_map_save;
		if (pageset1_copy_map)
			free_dyn_pageflags(&pageset1_copy_map);
		pageset1_copy_map = pageset1_copy_map_save;
		clear_suspend_state(SUSPEND_NOW_RESUMING);
		clear_suspend_state(SUSPEND_LOADING_ALT_IMAGE);
	}
}

int pre_resume_freeze(void)
{
	suspend_prepare_status(DONT_CLEAR_BAR,	"Prepare console");

	if (test_action_state(SUSPEND_PM_PREPARE_CONSOLE))
		pm_prepare_console();

	if (!test_action_state(SUSPEND_LATE_CPU_HOTPLUG)) {
		suspend_prepare_status(DONT_CLEAR_BAR,	"Disable nonboot cpus.");
		disable_nonboot_cpus();
	}

	suspend_prepare_status(DONT_CLEAR_BAR,	"Freeze processes.");

	if (freeze_processes()) {
		printk("Some processes failed to suspend\n");
		return 1;
	}

	return 0;
}

void post_resume_thaw(void)
{
	thaw_processes();
	if (!test_action_state(SUSPEND_LATE_CPU_HOTPLUG))
		enable_nonboot_cpus();

	if (test_action_state(SUSPEND_PM_PREPARE_CONSOLE))
		pm_restore_console();
}

int do_suspend2_step(int step)
{
	int result;

	switch (step) {
		case STEP_SUSPEND_PREPARE_IMAGE:
			return do_prepare_image();
		case STEP_SUSPEND_SAVE_IMAGE:
			return do_save_image();
		case STEP_SUSPEND_POWERDOWN:
			return do_power_down();
		case STEP_RESUME_CAN_RESUME:
			return do_check_can_resume();
		case STEP_RESUME_LOAD_PS1:
			pre_resume_freeze();
			return do_load_atomic_copy();
		case STEP_RESUME_DO_RESTORE:
			/* 
			 * If we succeed, this doesn't return.
			 * Instead, we return from do_save_image() in the
			 * suspended kernel.
			 */
			result = suspend_atomic_restore();
			if (result)
				post_resume_thaw();
			return result;
		case STEP_RESUME_ALT_IMAGE:
			printk("Trying to resume alternate image.\n");
			suspend2_in_suspend = 0;
			replace_restore_resume2(1, 0);
			prepare_restore_load_alt_image(1);
			if (!do_check_can_resume()) {
				printk("Nothing to resume from.\n");
				goto out;
			}
			if (!do_load_atomic_copy()) {
				printk("Failed to load image.\n");
				suspend_atomic_restore();
			}
out:
			prepare_restore_load_alt_image(0);
			replace_restore_resume2(0, 0);
			break;
	}

	return 0;
}

/* -- Functions for kickstarting a suspend or resume --- */

/*
 * Check if we have an image and if so try to resume.
 */
void __suspend_try_resume(void)
{
	set_suspend_state(SUSPEND_TRYING_TO_RESUME);
	clear_suspend_state(SUSPEND_RESUME_NOT_DONE);

	if (do_suspend2_step(STEP_RESUME_CAN_RESUME) &&
	    !do_suspend2_step(STEP_RESUME_LOAD_PS1) &&
	    do_suspend2_step(STEP_RESUME_DO_RESTORE))
		do_cleanup();

	clear_suspend_state(SUSPEND_IGNORE_LOGLEVEL);
	clear_suspend_state(SUSPEND_TRYING_TO_RESUME);
	clear_suspend_state(SUSPEND_NOW_RESUMING);
}

/* Wrapper for when called from init/do_mounts.c */
void __suspend2_try_resume(void)
{
	clear_suspend_state(SUSPEND_RESUME_NOT_DONE);

	if (suspend_start_anything(SYSFS_RESUMING))
		return;

	mutex_lock(&pm_mutex);
	__suspend_try_resume();

	/* 
	 * For initramfs, we have to clear the boot time
	 * flag after trying to resume
	 */
	clear_suspend_state(SUSPEND_BOOT_TIME);

	mutex_unlock(&pm_mutex);

	suspend_finish_anything(SYSFS_RESUMING);
}

/*
 * suspend2_try_suspend
 * Functionality   : 
 * Called From     : drivers/acpi/sleep/main.c
 *                   kernel/reboot.c
 */
int __suspend2_try_suspend(int have_pmsem)
{
	int result = 0, sys_power_disk = 0;

	if (!atomic_read(&actions_running)) {
		/* Came in via /sys/power/disk */
		if (suspend_start_anything(SYSFS_SUSPENDING))
			return -EBUSY;
		sys_power_disk = 1;
	}

	had_pmsem = have_pmsem;

	if (strlen(poweroff_resume2)) {
		attempt_to_parse_po_resume_device2();

		if (!strlen(poweroff_resume2)) {
			printk("Poweroff resume2 now invalid. Aborting.\n");
			goto out;
		}
	}

	if ((result = do_suspend2_step(STEP_SUSPEND_PREPARE_IMAGE)))
		goto out;

	if (test_action_state(SUSPEND_FREEZER_TEST)) {
		do_cleanup();
		goto out;
	}

	if ((result = do_suspend2_step(STEP_SUSPEND_SAVE_IMAGE)))
		goto out;

	/* This code runs at resume time too! */
	if (suspend2_in_suspend)
		result = do_suspend2_step(STEP_SUSPEND_POWERDOWN);
out:
	if (sys_power_disk)
		suspend_finish_anything(SYSFS_SUSPENDING);
	return result;
}

/*
 * This array contains entries that are automatically registered at
 * boot. Modules and the console code register their own entries separately.
 */
static struct suspend_sysfs_data sysfs_params[] = {
	{ SUSPEND2_ATTR("extra_pages_allowance", SYSFS_RW),
	  SYSFS_INT(&extra_pd1_pages_allowance, 0, INT_MAX, 0)
	},

	{ SUSPEND2_ATTR("image_exists", SYSFS_RW),
	  SYSFS_CUSTOM(image_exists_read, image_exists_write,
			  SYSFS_NEEDS_SM_FOR_BOTH)
	},

	{ SUSPEND2_ATTR("resume2", SYSFS_RW),
	  SYSFS_STRING(resume2_file, 255, SYSFS_NEEDS_SM_FOR_WRITE),
	  .write_side_effect = attempt_to_parse_resume_device2,
	},

	{ SUSPEND2_ATTR("poweroff_resume2", SYSFS_RW),
	  SYSFS_STRING(poweroff_resume2, 255, SYSFS_NEEDS_SM_FOR_WRITE),
	  .write_side_effect = attempt_to_parse_po_resume_device2,
	},
	{ SUSPEND2_ATTR("debug_info", SYSFS_READONLY),
	  SYSFS_CUSTOM(get_suspend_debug_info, NULL, 0)
	},
	
	{ SUSPEND2_ATTR("ignore_rootfs", SYSFS_RW),
	  SYSFS_BIT(&suspend_action, SUSPEND_IGNORE_ROOTFS, 0)
	},
	
	{ SUSPEND2_ATTR("image_size_limit", SYSFS_RW),
	  SYSFS_INT(&image_size_limit, -2, INT_MAX, 0)
	},

	{ SUSPEND2_ATTR("last_result", SYSFS_RW),
	  SYSFS_UL(&suspend_result, 0, 0, 0)
	},
	
	{ SUSPEND2_ATTR("no_multithreaded_io", SYSFS_RW),
	  SYSFS_BIT(&suspend_action, SUSPEND_NO_MULTITHREADED_IO, 0)
	},

	{ SUSPEND2_ATTR("full_pageset2", SYSFS_RW),
	  SYSFS_BIT(&suspend_action, SUSPEND_PAGESET2_FULL, 0)
	},

	{ SUSPEND2_ATTR("reboot", SYSFS_RW),
	  SYSFS_BIT(&suspend_action, SUSPEND_REBOOT, 0)
	},

#ifdef CONFIG_SOFTWARE_SUSPEND
	{ SUSPEND2_ATTR("replace_swsusp", SYSFS_RW),
	  SYSFS_BIT(&suspend_action, SUSPEND_REPLACE_SWSUSP, 0)
	},
#endif

	{ SUSPEND2_ATTR("resume_commandline", SYSFS_RW),
	  SYSFS_STRING(suspend2_nosave_commandline, COMMAND_LINE_SIZE, 0)
	},

	{ SUSPEND2_ATTR("version", SYSFS_READONLY),
	  SYSFS_STRING(SUSPEND_CORE_VERSION, 0, 0)
	},

	{ SUSPEND2_ATTR("no_load_direct", SYSFS_RW),
	  SYSFS_BIT(&suspend_action, SUSPEND_NO_DIRECT_LOAD, 0)
	},

	{ SUSPEND2_ATTR("freezer_test", SYSFS_RW),
	  SYSFS_BIT(&suspend_action, SUSPEND_FREEZER_TEST, 0)
	},

	{ SUSPEND2_ATTR("test_bio", SYSFS_RW),
	  SYSFS_BIT(&suspend_action, SUSPEND_TEST_BIO, 0)
	},

	{ SUSPEND2_ATTR("test_filter_speed", SYSFS_RW),
	  SYSFS_BIT(&suspend_action, SUSPEND_TEST_FILTER_SPEED, 0)
	},

	{ SUSPEND2_ATTR("slow", SYSFS_RW),
	  SYSFS_BIT(&suspend_action, SUSPEND_SLOW, 0)
	},

	{ SUSPEND2_ATTR("no_pageset2", SYSFS_RW),
	  SYSFS_BIT(&suspend_action, SUSPEND_NO_PAGESET2, 0)
	},
	  
	{ SUSPEND2_ATTR("late_cpu_hotplug", SYSFS_RW),
	  SYSFS_BIT(&suspend_action, SUSPEND_LATE_CPU_HOTPLUG, 0)
	},
	  
#if defined(CONFIG_ACPI)
	{ SUSPEND2_ATTR("powerdown_method", SYSFS_RW),
	  SYSFS_UL(&suspend_powerdown_method, 0, 5, 0)
	},
#endif

#ifdef CONFIG_SUSPEND2_KEEP_IMAGE
	{ SUSPEND2_ATTR("keep_image", SYSFS_RW),
	  SYSFS_BIT(&suspend_action, SUSPEND_KEEP_IMAGE, 0)
	},
#endif
};

struct suspend2_core_fns my_fns = {
	.get_nonconflicting_page = __suspend_get_nonconflicting_page,
	.post_context_save = __suspend_post_context_save,
	.try_suspend = __suspend2_try_suspend,
	.try_resume = __suspend2_try_resume,
};
 
static __init int core_load(void)
{
	int i,
	    numfiles = sizeof(sysfs_params) / sizeof(struct suspend_sysfs_data);

	printk("Suspend v" SUSPEND_CORE_VERSION "\n");

	if (s2_sysfs_init())
		return 1;

	for (i=0; i< numfiles; i++)
		suspend_register_sysfs_file(&suspend2_subsys.kobj,
				&sysfs_params[i]);

	s2_core_fns = &my_fns;

	if (s2_checksum_init())
		return 1;
	if (s2_cluster_init())
		return 1;
	if (s2_usm_init())
		return 1;
	if (s2_ui_init())
		return 1;

#ifdef CONFIG_SOFTWARE_SUSPEND
	/* Overriding resume2= with resume=? */
	if (test_action_state(SUSPEND_REPLACE_SWSUSP) && resume_file[0])
		strncpy(resume2_file, resume_file, 256);
#endif

	return 0;
}

#ifdef MODULE
static __exit void core_unload(void)
{
	int i,
	    numfiles = sizeof(sysfs_params) / sizeof(struct suspend_sysfs_data);

	s2_ui_exit();
	s2_checksum_exit();
	s2_cluster_exit();
	s2_usm_exit();

	for (i=0; i< numfiles; i++)
		suspend_unregister_sysfs_file(&suspend2_subsys.kobj,
				&sysfs_params[i]);

	s2_core_fns = NULL;

	s2_sysfs_exit();
}
MODULE_LICENSE("GPL");
module_init(core_load);
module_exit(core_unload);
#else
late_initcall(core_load);
#endif

#ifdef CONFIG_SUSPEND2_EXPORTS
EXPORT_SYMBOL_GPL(pagedir2);
#endif
