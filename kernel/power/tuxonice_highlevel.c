/*
 * kernel/power/tuxonice_highlevel.c
 */
/** \mainpage TuxOnIce.
 *
 * TuxOnIce provides support for saving and restoring an image of
 * system memory to an arbitrary storage device, either on the local computer,
 * or across some network. The support is entirely OS based, so TuxOnIce
 * works without requiring BIOS, APM or ACPI support. The vast majority of the
 * code is also architecture independant, so it should be very easy to port
 * the code to new architectures. TuxOnIce includes support for SMP, 4G HighMem
 * and preemption. Initramfses and initrds are also supported.
 *
 * TuxOnIce uses a modular design, in which the method of storing the image is
 * completely abstracted from the core code, as are transformations on the data
 * such as compression and/or encryption (multiple 'modules' can be used to
 * provide arbitrary combinations of functionality). The user interface is also
 * modular, so that arbitrarily simple or complex interfaces can be used to
 * provide anything from debugging information through to eye candy.
 *
 * \section Copyright
 *
 * TuxOnIce is released under the GPLv2.
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
 * TuxOnIce would not be where it is.
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
 * ..and of course the myriads of TuxOnIce users who have helped diagnose
 * and fix bugs, made suggestions on how to improve the code, proofread
 * documentation, and donated time and money.
 *
 * Thanks also to corporate sponsors:
 *
 * <B>Redhat.</B>Sometime employer from May 2006 (my fault, not Redhat's!).
 *
 * <B>Cyclades.com.</B> Nigel's employers from Dec 2004 until May 2006, who
 * allowed him to work on TuxOnIce and PM related issues on company time.
 *
 * <B>LinuxFund.org.</B> Sponsored Nigel's work on TuxOnIce for four months Oct 2003
 * to Jan 2004.
 *
 * <B>LAC Linux.</B> Donated P4 hardware that enabled development and ongoing
 * maintenance of SMP and Highmem support.
 *
 * <B>OSDL.</B> Provided access to various hardware configurations, make occasional
 * small donations to the project.
 */

#include <linux/suspend.h>
#include <linux/module.h>
#include <linux/freezer.h>
#include <linux/utsrelease.h>
#include <linux/cpu.h>
#include <linux/console.h>
#include <asm/uaccess.h>

#include "tuxonice_modules.h"
#include "tuxonice_sysfs.h"
#include "tuxonice_prepare_image.h"
#include "tuxonice_io.h"
#include "tuxonice_ui.h"
#include "tuxonice_power_off.h"
#include "tuxonice_storage.h"
#include "tuxonice_checksum.h"
#include "tuxonice_cluster.h"
#include "tuxonice_builtin.h"
#include "tuxonice_atomic_copy.h"

/*! Pageset metadata. */
struct pagedir pagedir2 = {2};

static int get_pmsem = 0, got_pmsem;
static mm_segment_t oldfs;
static atomic_t actions_running;
static int block_dump_save;
extern int block_dump;

int do_toi_step(int step);

/**
 * toi_finish_anything - Cleanup after doing anything.
 *
 * @toi_or_resume: Whether finishing a cycle or attempt at resuming.
 *
 * This is our basic clean-up routine, matching start_anything below. We
 * call cleanup routines, drop module references and restore process fs and
 * cpus allowed masks, together with the global block_dump variable's value.
 */
void toi_finish_anything(int toi_or_resume)
{
	if (!atomic_dec_and_test(&actions_running))
		return;

	toi_cleanup_modules(toi_or_resume);
	toi_put_modules();
	set_fs(oldfs);
	if (toi_or_resume) {
		block_dump = block_dump_save;
		set_cpus_allowed(current, CPU_MASK_ALL);
	}
}

/**
 * toi_start_anything - Basic initialisation for TuxOnIce.
 *
 * @toi_or_resume: Whether starting a cycle or attempt at resuming.
 *
 * Our basic initialisation routine. Take references on modules, use the
 * kernel segment, recheck resume= if no active allocator is set, initialise
 * modules, save and reset block_dump and ensure we're running on CPU0.
 */
int toi_start_anything(int toi_or_resume)
{
	if (atomic_add_return(1, &actions_running) != 1) {
		if (toi_or_resume) {
			printk("Can't start a cycle when actions are "
					"already running.\n");
			atomic_dec(&actions_running);
			return -EBUSY;
		} else
			return 0;
	}

	oldfs = get_fs();
	set_fs(KERNEL_DS);

	/* Be quiet if we're not trying to hibernate or resume */
	if (!toiActiveAllocator)
		toi_attempt_to_parse_resume_device(!toi_or_resume);

	if (toi_get_modules()) {
		printk("TuxOnIce: Get modules failed!\n");
		goto out_err;
	}

	if (toi_initialise_modules(toi_or_resume)) {
		printk("TuxOnIce: Initialise modules failed!\n");
		goto out_err;
	}

	if (toi_or_resume) {
		block_dump_save = block_dump;
		block_dump = 0;
		set_cpus_allowed(current, CPU_MASK_CPU0);
	}

	return 0;

out_err:
	if (toi_or_resume)
		block_dump_save = block_dump;
	toi_finish_anything(toi_or_resume);
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

/**
 * mark_nosave_pages - Set up our Nosave bitmap.
 *
 * Build a bitmap of Nosave pages from the list. The bitmap allows faster
 * use when preparing the image.
 */
static void mark_nosave_pages(void)
{
	struct nosave_region *region;

	list_for_each_entry(region, &nosave_regions, list) {
		unsigned long pfn;

		for (pfn = region->start_pfn; pfn < region->end_pfn; pfn++)
			SetPageNosave(pfn_to_page(pfn));
	}
}

/**
 * allocate_bitmaps: Allocate bitmaps used to record page states.
 *
 * Allocate the bitmaps we use to record the various TuxOnIce related
 * page states.
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

/**
 * free_bitmaps: Free the bitmaps used to record page states.
 *
 * Free the bitmaps allocated above. It is not an error to call
 * free_dyn_pageflags on a bitmap that isn't currentyl allocated.
 */
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

/**
 * io_MB_per_second: Return the number of MB/s read or written.
 *
 * @write: Whether to return the speed at which we wrote.
 *
 * Calculate the number of megabytes per second that were read or written.
 */
static int io_MB_per_second(int write)
{
	return (toi_io_time[write][1]) ?
		MB((unsigned long) toi_io_time[write][0]) * HZ /
		toi_io_time[write][1] : 0;
}

/**
 * get_debug_info: Fill a buffer with debugging information.
 *
 * @buffer: The buffer to be filled.
 * @count: The size of the buffer, in bytes.
 *
 * Fill a (usually PAGE_SIZEd) buffer with the debugging info that we will
 * either printk or return via sysfs.
 */
#define SNPRINTF(a...) 	len += snprintf_used(((char *)buffer) + len, \
		count - len - 1, ## a)
static int get_toi_debug_info(const char *buffer, int count)
{
	int len = 0;

	SNPRINTF("TuxOnIce debugging info:\n");
	SNPRINTF("- TuxOnIce core  : " TOI_CORE_VERSION "\n");
	SNPRINTF("- Kernel Version : " UTS_RELEASE "\n");
	SNPRINTF("- Compiler vers. : %d.%d\n", __GNUC__, __GNUC_MINOR__);
	SNPRINTF("- Attempt number : %d\n", nr_hibernates);
	SNPRINTF("- Parameters     : %ld %ld %ld %d %d %ld\n",
			toi_result,
			toi_action,
			toi_debug_state,
			toi_default_console_level,
			image_size_limit,
			toi_poweroff_method);
	SNPRINTF("- Overall expected compression percentage: %d.\n",
			100 - toi_expected_compression_ratio());
	len+= toi_print_module_debug_info(((char *) buffer) + len,
			count - len - 1);
	if (toi_io_time[0][1]) {
		if ((io_MB_per_second(0) < 5) || (io_MB_per_second(1) < 5)) {
			SNPRINTF("- I/O speed: Write %d KB/s",
			  (KB((unsigned long) toi_io_time[0][0]) * HZ /
			  toi_io_time[0][1]));
			if (toi_io_time[1][1])
				SNPRINTF(", Read %d KB/s",
				  (KB((unsigned long) toi_io_time[1][0]) * HZ /
				  toi_io_time[1][1]));
		} else {
			SNPRINTF("- I/O speed: Write %d MB/s",
			 (MB((unsigned long) toi_io_time[0][0]) * HZ /
			  toi_io_time[0][1]));
			if (toi_io_time[1][1])
				SNPRINTF(", Read %d MB/s",
				 (MB((unsigned long) toi_io_time[1][0]) * HZ /
				  toi_io_time[1][1]));
		}
		SNPRINTF(".\n");
	}
	else
		SNPRINTF("- No I/O speed stats available.\n");
	SNPRINTF("- Extra pages    : %d used/%d.\n",
			extra_pd1_pages_used, extra_pd1_pages_allowance);

	return len;
}

/**
 * do_cleanup: Cleanup after attempting to hibernate or resume.
 *
 * @get_debug_info: Whether to allocate and return debugging info.
 *
 * Cleanup after attempting to hibernate or resume, possibly getting
 * debugging info as we do so.
 */
static void do_cleanup(int get_debug_info)
{
	int i = 0;
	char *buffer = NULL;

	if (get_debug_info)
		toi_prepare_status(DONT_CLEAR_BAR, "Cleaning up...");
	relink_lru_lists();

	free_checksum_pages();

	if (get_debug_info)
		buffer = (char *) get_zeroed_page(TOI_ATOMIC_GFP);

	if (buffer)
		i = get_toi_debug_info(buffer, PAGE_SIZE);

	toi_free_extra_pagedir_memory();

	pagedir1.size = pagedir2.size = 0;
	set_highmem_size(pagedir1, 0);
	set_highmem_size(pagedir2, 0);

	restore_avenrun();

	thaw_processes();

#ifdef CONFIG_TOI_KEEP_IMAGE
	if (test_action_state(TOI_KEEP_IMAGE) &&
	    !test_result_state(TOI_ABORTED)) {
		toi_message(TOI_ANY_SECTION, TOI_LOW, 1,
			"TuxOnIce: Not invalidating the image due "
			"to Keep Image being enabled.\n");
		set_result_state(TOI_KEPT_IMAGE);
	} else
#endif
		if (toiActiveAllocator)
			toiActiveAllocator->remove_image();

	free_bitmaps();

	if (buffer && i) {
		/* Printk can only handle 1023 bytes, including
		 * its level mangling. */
		for (i = 0; i < 3; i++)
			printk("%s", buffer + (1023 * i));
		free_page((unsigned long) buffer);
	}

	if (!test_action_state(TOI_LATE_CPU_HOTPLUG))
		enable_nonboot_cpus();
	toi_cleanup_console();

	free_attention_list();

	toi_deactivate_storage(0);

	clear_toi_state(TOI_IGNORE_LOGLEVEL);
	clear_toi_state(TOI_TRYING_TO_RESUME);
	clear_toi_state(TOI_NOW_RESUMING);

	if (got_pmsem) {
		mutex_unlock(&pm_mutex);
		got_pmsem = 0;
	}
}

/**
 * check_still_keeping_image: We kept an image; check whether to reuse it.
 *
 * We enter this routine when we have kept an image. If the user has said they
 * want to still keep it, all we need to do is powerdown. If powering down
 * means hibernating to ram and the power doesn't run out, we'll return 1.
 * If we do power off properly or the battery runs out, we'll resume via the
 * normal paths.
 *
 * If the user has said they want to remove the previously kept image, we
 * remove it, and return 0. We'll then store a new image.
 */
static int check_still_keeping_image(void)
{
	if (test_action_state(TOI_KEEP_IMAGE)) {
		printk("Image already stored: powering down immediately.");
		do_toi_step(STEP_HIBERNATE_POWERDOWN);
		return 1;	/* Just in case we're using S3 */
	}

	printk("Invalidating previous image.\n");
	toiActiveAllocator->remove_image();

	return 0;
}

/**
 * toi_init: Prepare to hibernate to disk.
 *
 * Initialise variables & data structures, in preparation for
 * hibernating to disk.
 */
static int toi_init(void)
{
	toi_result = 0;

	toi_print_modules();
	printk(KERN_INFO "Initiating a hibernation cycle.\n");

	nr_hibernates++;

	save_avenrun();

	toi_io_time[0][0] = toi_io_time[0][1] =
		toi_io_time[1][0] =	toi_io_time[1][1] = 0;

	if (!test_toi_state(TOI_CAN_HIBERNATE) ||
	    allocate_bitmaps())
		return 1;

	mark_nosave_pages();

	toi_prepare_console();
	if (test_action_state(TOI_LATE_CPU_HOTPLUG) ||
			!disable_nonboot_cpus())
		return 1;

	set_abort_result(TOI_CPU_HOTPLUG_FAILED);
	return 0;
}

/**
 * can_hibernate: Perform basic 'Can we hibernate?' tests.
 *
 * Perform basic tests that must pass if we're going to be able to hibernate:
 * Can we get the pm_mutex? Is resume= valid (we need to know where to write
 * the image header).
 */
static int can_hibernate(void)
{
	if (get_pmsem) {
		if (!mutex_trylock(&pm_mutex)) {
			printk("TuxOnIce: Failed to obtain pm_mutex.\n");
			dump_stack();
			set_abort_result(TOI_PM_SEM);
			return 0;
		}
		got_pmsem = 1;
	}

	if (!test_toi_state(TOI_CAN_HIBERNATE))
		toi_attempt_to_parse_resume_device(0);

	if (!test_toi_state(TOI_CAN_HIBERNATE)) {
		printk("TuxOnIce: Hibernation is disabled.\n"
			"This may be because you haven't put something along "
			"the lines of\n\nresume=swap:/dev/hda1\n\n"
			"in lilo.conf or equivalent. (Where /dev/hda1 is your "
			"swap partition).\n");
		set_abort_result(TOI_CANT_SUSPEND);
		if (!got_pmsem) {
			mutex_unlock(&pm_mutex);
			got_pmsem = 0;
		}
		return 0;
	}

	return 1;
}

/**
 * do_post_image_write: Having written an image, figure out what to do next.
 *
 * After writing an image, we might load an alternate image or power down.
 * Powering down might involve hibernating to ram, in which case we also
 * need to handle reloading pageset2.
 */
static int do_post_image_write(void)
{
	/* If switching images fails, do normal powerdown */
	if (alt_resume_param[0])
		do_toi_step(STEP_RESUME_ALT_IMAGE);

	toi_cond_pause(1, "About to power down or reboot.");
	toi_power_down();

	/* If we return, it's because we hibernated to ram */
	if (read_pageset2(1))
		panic("Attempt to reload pagedir 2 failed. Try rebooting.");

	barrier();
	mb();
	do_cleanup(1);
	return 0;
}

/**
 * __save_image: Do the hard work of saving the image.
 *
 * High level routine for getting the image saved. The key assumptions made
 * are that processes have been frozen and sufficient memory is available.
 *
 * We also exit through here at resume time, coming back from toi_hibernate
 * after the atomic restore. This is the reason for the toi_in_hibernate
 * test.
 */
static int __save_image(void)
{
	int temp_result, did_copy = 0;

	toi_prepare_status(DONT_CLEAR_BAR, "Starting to save the image..");

	toi_message(TOI_ANY_SECTION, TOI_LOW, 1,
		" - Final values: %d and %d.\n",
		pagedir1.size, pagedir2.size);

	toi_cond_pause(1, "About to write pagedir2.");

	calculate_check_checksums(0);

	temp_result = write_pageset(&pagedir2);

	if (temp_result == -1 || test_result_state(TOI_ABORTED))
		return 1;

	toi_cond_pause(1, "About to copy pageset 1.");

	if (test_result_state(TOI_ABORTED))
		return 1;

	toi_deactivate_storage(1);

	toi_prepare_status(DONT_CLEAR_BAR, "Doing atomic copy.");

	toi_in_hibernate = 1;

	if (toi_go_atomic(PMSG_FREEZE, 1))
		goto Failed;

	temp_result = toi_hibernate();
	did_copy = 1;

	/* We return here at resume time too! */
	toi_end_atomic(ATOMIC_ALL_STEPS, toi_in_hibernate);

Failed:
	if (toi_activate_storage(1))
		panic("Failed to reactivate our storage.");

	if (temp_result || test_result_state(TOI_ABORTED)) {
		if (did_copy)
			goto abort_reloading_pagedir_two;
		else
			return 1;
	}

	/* Resume time? */
	if (!toi_in_hibernate) {
		copyback_post();
		return 0;
	}

	/* Nope. Hibernating. So, see if we can save the image... */

	toi_update_status(pagedir2.size,
			pagedir1.size + pagedir2.size,
			NULL);

	if (test_result_state(TOI_ABORTED))
		goto abort_reloading_pagedir_two;

	toi_cond_pause(1, "About to write pageset1.");

	toi_message(TOI_ANY_SECTION, TOI_LOW, 1,
			"-- Writing pageset1\n");

	temp_result = write_pageset(&pagedir1);

	/* We didn't overwrite any memory, so no reread needs to be done. */
	if (test_action_state(TOI_TEST_FILTER_SPEED))
		return 1;

	if (temp_result == 1 || test_result_state(TOI_ABORTED))
		goto abort_reloading_pagedir_two;

	toi_cond_pause(1, "About to write header.");

	if (test_result_state(TOI_ABORTED))
		goto abort_reloading_pagedir_two;

	temp_result = write_image_header();

	if (test_action_state(TOI_TEST_BIO))
		return 1;

	if (!temp_result && !test_result_state(TOI_ABORTED))
		return 0;

abort_reloading_pagedir_two:
	temp_result = read_pageset2(1);

	/* If that failed, we're sunk. Panic! */
	if (temp_result)
		panic("Attempt to reload pagedir 2 while aborting "
				"a hibernate failed.");

	return 1;
}

/**
 * do_save_image: Save the image and handle the result.
 *
 * Save the prepared image. If we fail or we're in the path returning
 * from the atomic restore, cleanup.
 */

static int do_save_image(void)
{
	int result = __save_image();
	if (!toi_in_hibernate || result)
		do_cleanup(1);
	return result;
}


/**
 * do_prepare_image: Try to prepare an image.
 *
 * Seek to initialise and prepare an image to be saved. On failure,
 * cleanup.
 */

static int do_prepare_image(void)
{
	if (toi_activate_storage(0))
		return 1;

	/*
	 * If kept image and still keeping image and hibernating to RAM, we will
	 * return 1 after hibernating and resuming (provided the power doesn't
	 * run out. In that case, we skip directly to cleaning up and exiting.
	 */

	if (!can_hibernate() ||
	    (test_result_state(TOI_KEPT_IMAGE) &&
	     check_still_keeping_image()))
		goto cleanup;

	if (toi_init() && !toi_prepare_image() &&
			!test_result_state(TOI_ABORTED))
		return 0;

cleanup:
	do_cleanup(0);
	return 1;
}

/**
 * do_check_can_resume: Find out whether an image has been stored.
 *
 * Read whether an image exists. We use the same routine as the
 * image_exists sysfs entry, and just look to see whether the
 * first character in the resulting buffer is a '1'.
 */
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

/**
 * do_load_atomic_copy: Load the first part of an image, if it exists.
 *
 * Check whether we have an image. If one exists, do sanity checking
 * (possibly invalidating the image or even rebooting if the user
 * requests that) before loading it into memory in preparation for the
 * atomic restore.
 *
 * If and only if we have an image loaded and ready to restore, we return 1.
 */
static int do_load_atomic_copy(void)
{
	int read_image_result = 0;

	if (sizeof(swp_entry_t) != sizeof(long)) {
		printk(KERN_WARNING "TuxOnIce: The size of swp_entry_t != size"
			" of long. Please report this!\n");
		return 1;
	}

	if (!resume_file[0])
		printk(KERN_WARNING "TuxOnIce: "
			"You need to use a resume= command line parameter to "
			"tell TuxOnIce where to look for an image.\n");

	toi_activate_storage(0);

	if (!(test_toi_state(TOI_RESUME_DEVICE_OK)) &&
		!toi_attempt_to_parse_resume_device(0)) {
		/*
		 * Without a usable storage device we can do nothing -
		 * even if noresume is given
		 */

		if (!toiNumAllocators)
			printk(KERN_ALERT "TuxOnIce: "
			  "No storage allocators have been registered.\n");
		else
			printk(KERN_ALERT "TuxOnIce: "
				"Missing or invalid storage location "
				"(resume= parameter). Please correct and "
				"rerun lilo (or equivalent) before "
				"hibernating.\n");
		toi_deactivate_storage(0);
		return 1;
	}

	read_image_result = read_pageset1(); /* non fatal error ignored */

	if (test_toi_state(TOI_NORESUME_SPECIFIED))
		clear_toi_state(TOI_NORESUME_SPECIFIED);

	toi_deactivate_storage(0);

	if (read_image_result)
		return 1;

	return 0;
}

/**
 * prepare_restore_load_alt_image: Save & restore alt image variables.
 *
 * Save and restore the pageset1 maps, when loading an alternate image.
 */
static void prepare_restore_load_alt_image(int prepare)
{
	static dyn_pageflags_t pageset1_map_save, pageset1_copy_map_save;

	if (prepare) {
		pageset1_map_save = pageset1_map;
		pageset1_map = NULL;
		pageset1_copy_map_save = pageset1_copy_map;
		pageset1_copy_map = NULL;
		set_toi_state(TOI_LOADING_ALT_IMAGE);
		toi_reset_alt_image_pageset2_pfn();
	} else {
		if (pageset1_map)
			free_dyn_pageflags(&pageset1_map);
		pageset1_map = pageset1_map_save;
		if (pageset1_copy_map)
			free_dyn_pageflags(&pageset1_copy_map);
		pageset1_copy_map = pageset1_copy_map_save;
		clear_toi_state(TOI_NOW_RESUMING);
		clear_toi_state(TOI_LOADING_ALT_IMAGE);
	}
}

/**
 * pre_resume_freeze: Freeze the system, before doing an atomic restore.
 *
 * Hot unplug cpus (if we didn't do it early) and freeze processes, in
 * preparation for doing an atomic restore.
 */
int pre_resume_freeze(void)
{
	if (!test_action_state(TOI_LATE_CPU_HOTPLUG)) {
		toi_prepare_status(DONT_CLEAR_BAR,	"Disable nonboot cpus.");
		if (disable_nonboot_cpus()) {
			set_abort_result(TOI_CPU_HOTPLUG_FAILED);
			return 1;
		}
	}

	toi_prepare_status(DONT_CLEAR_BAR,	"Freeze processes.");

	if (freeze_processes()) {
		printk("Some processes failed to hibernate\n");
		return 1;
	}

	return 0;
}

/**
 * do_toi_step: Perform a step in hibernating or resuming.
 *
 * Perform a step in hibernating or resuming an image. This abstraction
 * is in preparation for implementing cluster support, and perhaps replacing
 * uswsusp too (haven't looked whether that's possible yet).
 */
int do_toi_step(int step)
{
	switch (step) {
		case STEP_HIBERNATE_PREPARE_IMAGE:
			return do_prepare_image();
		case STEP_HIBERNATE_SAVE_IMAGE:
			return do_save_image();
		case STEP_HIBERNATE_POWERDOWN:
			return do_post_image_write();
		case STEP_RESUME_CAN_RESUME:
			return do_check_can_resume();
		case STEP_RESUME_LOAD_PS1:
			return do_load_atomic_copy();
		case STEP_RESUME_DO_RESTORE:
			/*
			 * If we succeed, this doesn't return.
			 * Instead, we return from do_save_image() in the
			 * hibernated kernel.
			 */
			return toi_atomic_restore();
		case STEP_RESUME_ALT_IMAGE:
			printk("Trying to resume alternate image.\n");
			toi_in_hibernate = 0;
			save_restore_alt_param(SAVE, NOQUIET);
			prepare_restore_load_alt_image(1);
			if (!do_check_can_resume()) {
				printk("Nothing to resume from.\n");
				goto out;
			}
			if (!do_load_atomic_copy()) {
				printk("Failed to load image.\n");
				toi_atomic_restore();
			}
out:
			prepare_restore_load_alt_image(0);
			save_restore_alt_param(RESTORE, NOQUIET);
			break;
	}

	return 0;
}

/* -- Functions for kickstarting a hibernate or resume --- */

/**
 * __toi_try_resume: Try to do the steps in resuming.
 *
 * Check if we have an image and if so try to resume. Clear the status
 * flags too.
 */
void __toi_try_resume(void)
{
	set_toi_state(TOI_TRYING_TO_RESUME);
	resume_attempted = 1;

	toi_print_modules();

	if (do_toi_step(STEP_RESUME_CAN_RESUME) &&
	    !do_toi_step(STEP_RESUME_LOAD_PS1))
	    do_toi_step(STEP_RESUME_DO_RESTORE);

	do_cleanup(0);

	clear_toi_state(TOI_IGNORE_LOGLEVEL);
	clear_toi_state(TOI_TRYING_TO_RESUME);
	clear_toi_state(TOI_NOW_RESUMING);
}

/**
 * _toi_try_resume: Wrapper calling __toi_try_resume from do_mounts.
 *
 * Wrapper for when __toi_try_resume is called from init/do_mounts.c,
 * rather than from echo > /sys/power/tuxonice/do_resume.
 */
void _toi_try_resume(void)
{
	resume_attempted = 1;

	if (toi_start_anything(SYSFS_RESUMING))
		return;

	/* Unlock will be done in do_cleanup */
	mutex_lock(&pm_mutex);
	got_pmsem = 1;

	__toi_try_resume();

	/*
	 * For initramfs, we have to clear the boot time
	 * flag after trying to resume
	 */
	clear_toi_state(TOI_BOOT_TIME);
	toi_finish_anything(SYSFS_RESUMING);
}

/**
 * _toi_try_hibernate: Try to start a hibernation cycle.
 *
 * have_pmsem: Whther the pm_sem is already taken.
 *
 * Start a hibernation cycle, coming in from either
 * echo > /sys/power/tuxonice/do_suspend
 *
 * or
 *
 * echo disk > /sys/power/state
 *
 * In the later case, we come in without pm_sem taken; in the
 * former, it has been taken.
 */
int _toi_try_hibernate(int have_pmsem)
{
	int result = 0, sys_power_disk = 0;

	if (!atomic_read(&actions_running)) {
		/* Came in via /sys/power/disk */
		if (toi_start_anything(SYSFS_HIBERNATING))
			return -EBUSY;
		sys_power_disk = 1;
	}

	get_pmsem = !have_pmsem;

	if (strlen(alt_resume_param)) {
		attempt_to_parse_alt_resume_param();

		if (!strlen(alt_resume_param)) {
			printk("Alternate resume parameter now invalid. Aborting.\n");
			goto out;
		}
	}

	if ((result = do_toi_step(STEP_HIBERNATE_PREPARE_IMAGE)))
		goto out;

	if (test_action_state(TOI_FREEZER_TEST)) {
		do_cleanup(0);
		goto out;
	}

	if ((result = do_toi_step(STEP_HIBERNATE_SAVE_IMAGE)))
		goto out;

	/* This code runs at resume time too! */
	if (toi_in_hibernate)
		result = do_toi_step(STEP_HIBERNATE_POWERDOWN);
out:
	if (sys_power_disk)
		toi_finish_anything(SYSFS_HIBERNATING);
	return result;
}

/*
 * This array contains entries that are automatically registered at
 * boot. Modules and the console code register their own entries separately.
 */
static struct toi_sysfs_data sysfs_params[] = {
	{ TOI_ATTR("extra_pages_allowance", SYSFS_RW),
	  SYSFS_INT(&extra_pd1_pages_allowance, MIN_EXTRA_PAGES_ALLOWANCE,
			  INT_MAX, 0)
	},

	{ TOI_ATTR("image_exists", SYSFS_RW),
	  SYSFS_CUSTOM(image_exists_read, image_exists_write,
			  SYSFS_NEEDS_SM_FOR_BOTH)
	},

	{ TOI_ATTR("resume", SYSFS_RW),
	  SYSFS_STRING(resume_file, 255, SYSFS_NEEDS_SM_FOR_WRITE),
	  .write_side_effect = attempt_to_parse_resume_device2,
	},

	{ TOI_ATTR("alt_resume_param", SYSFS_RW),
	  SYSFS_STRING(alt_resume_param, 255, SYSFS_NEEDS_SM_FOR_WRITE),
	  .write_side_effect = attempt_to_parse_alt_resume_param,
	},
	{ TOI_ATTR("debug_info", SYSFS_READONLY),
	  SYSFS_CUSTOM(get_toi_debug_info, NULL, 0)
	},

	{ TOI_ATTR("ignore_rootfs", SYSFS_RW),
	  SYSFS_BIT(&toi_action, TOI_IGNORE_ROOTFS, 0)
	},

	{ TOI_ATTR("image_size_limit", SYSFS_RW),
	  SYSFS_INT(&image_size_limit, -2, INT_MAX, 0)
	},

	{ TOI_ATTR("last_result", SYSFS_RW),
	  SYSFS_UL(&toi_result, 0, 0, 0)
	},

	{ TOI_ATTR("no_multithreaded_io", SYSFS_RW),
	  SYSFS_BIT(&toi_action, TOI_NO_MULTITHREADED_IO, 0)
	},

	{ TOI_ATTR("full_pageset2", SYSFS_RW),
	  SYSFS_BIT(&toi_action, TOI_PAGESET2_FULL, 0)
	},

	{ TOI_ATTR("reboot", SYSFS_RW),
	  SYSFS_BIT(&toi_action, TOI_REBOOT, 0)
	},

	{ TOI_ATTR("replace_swsusp", SYSFS_RW),
	  SYSFS_BIT(&toi_action, TOI_REPLACE_SWSUSP, 0)
	},

	{ TOI_ATTR("resume_commandline", SYSFS_RW),
	  SYSFS_STRING(toi_nosave_commandline, COMMAND_LINE_SIZE, 0)
	},

	{ TOI_ATTR("version", SYSFS_READONLY),
	  SYSFS_STRING(TOI_CORE_VERSION, 0, 0)
	},

	{ TOI_ATTR("no_load_direct", SYSFS_RW),
	  SYSFS_BIT(&toi_action, TOI_NO_DIRECT_LOAD, 0)
	},

	{ TOI_ATTR("freezer_test", SYSFS_RW),
	  SYSFS_BIT(&toi_action, TOI_FREEZER_TEST, 0)
	},

	{ TOI_ATTR("test_bio", SYSFS_RW),
	  SYSFS_BIT(&toi_action, TOI_TEST_BIO, 0)
	},

	{ TOI_ATTR("test_filter_speed", SYSFS_RW),
	  SYSFS_BIT(&toi_action, TOI_TEST_FILTER_SPEED, 0)
	},

	{ TOI_ATTR("slow", SYSFS_RW),
	  SYSFS_BIT(&toi_action, TOI_SLOW, 0)
	},

	{ TOI_ATTR("no_pageset2", SYSFS_RW),
	  SYSFS_BIT(&toi_action, TOI_NO_PAGESET2, 0)
	},

	{ TOI_ATTR("late_cpu_hotplug", SYSFS_RW),
	  SYSFS_BIT(&toi_action, TOI_LATE_CPU_HOTPLUG, 0)
	},

#ifdef CONFIG_TOI_KEEP_IMAGE
	{ TOI_ATTR("keep_image", SYSFS_RW),
	  SYSFS_BIT(&toi_action, TOI_KEEP_IMAGE, 0)
	},
#endif
};

struct toi_core_fns my_fns = {
	.get_nonconflicting_page = __toi_get_nonconflicting_page,
	.post_context_save = __toi_post_context_save,
	.try_hibernate = _toi_try_hibernate,
	.try_resume = _toi_try_resume,
};

/**
 * core_load: Initialisation of TuxOnIce core.
 *
 * Initialise the core, beginning with sysfs. Checksum and so on are part of
 * the core, but have their own initialisation routines because they either
 * aren't compiled in all the time or have their own subdirectories.
 */
static __init int core_load(void)
{
	int i,
	    numfiles = sizeof(sysfs_params) / sizeof(struct toi_sysfs_data);

	if (toi_sysfs_init())
		return 1;

	for (i=0; i< numfiles; i++)
		toi_register_sysfs_file(&toi_subsys.kobj,
				&sysfs_params[i]);

	toi_core_fns = &my_fns;

	if (toi_checksum_init())
		return 1;
	if (toi_cluster_init())
		return 1;
	if (toi_usm_init())
		return 1;
	if (toi_ui_init())
		return 1;
	if (toi_poweroff_init())
		return 1;

	return 0;
}

#ifdef MODULE
/**
 * core_unload: Prepare to unload the core code.
 */
static __exit void core_unload(void)
{
	int i,
	    numfiles = sizeof(sysfs_params) / sizeof(struct toi_sysfs_data);

	toi_poweroff_exit();
	toi_ui_exit();
	toi_checksum_exit();
	toi_cluster_exit();
	toi_usm_exit();

	for (i=0; i< numfiles; i++)
		toi_unregister_sysfs_file(&toi_subsys.kobj,
				&sysfs_params[i]);

	toi_core_fns = NULL;

	toi_sysfs_exit();
}
MODULE_LICENSE("GPL");
module_init(core_load);
module_exit(core_unload);
#else
late_initcall(core_load);
#endif

#ifdef CONFIG_TOI_EXPORTS
EXPORT_SYMBOL_GPL(pagedir2);
#endif
