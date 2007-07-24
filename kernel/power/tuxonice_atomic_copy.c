/*
 * kernel/power/tuxonice_atomic_copy.c
 *
 * Copyright 2004-2007 Nigel Cunningham (nigel at suspend2 net)
 * Copyright (C) 2006 Red Hat, inc.
 *
 * Distributed under GPLv2.
 *
 * Routines for doing the atomic save/restore.
 */

#include <linux/suspend.h>
#include <linux/highmem.h>
#include <linux/cpu.h>
#include <linux/freezer.h>
#include <linux/console.h>
#include "tuxonice.h"
#include "tuxonice_storage.h"
#include "tuxonice_power_off.h"
#include "tuxonice_ui.h"
#include "power.h"
#include "tuxonice_io.h"
#include "tuxonice_prepare_image.h"
#include "tuxonice_pageflags.h"
#include "tuxonice_checksum.h"
#include "tuxonice_builtin.h"
#include "tuxonice_atomic_copy.h"

int extra_pd1_pages_used;

/*
 * Highmem related functions (x86 only).
 */

#ifdef CONFIG_HIGHMEM

/**
 * copyback_high: Restore highmem pages.
 *
 * Highmem data and pbe lists are/can be stored in highmem.
 * The format is slightly different to the lowmem pbe lists
 * used for the assembly code: the last pbe in each page is
 * a struct page * instead of struct pbe *, pointing to the
 * next page where pbes are stored (or NULL if happens to be
 * the end of the list). Since we don't want to generate
 * unnecessary deltas against swsusp code, we use a cast
 * instead of a union.
 **/

static void copyback_high(void)
{
	struct page * pbe_page = (struct page *) restore_highmem_pblist;
	struct pbe *this_pbe, *first_pbe;
	unsigned long *origpage, *copypage;
	int pbe_index = 1;

	if (!pbe_page)
		return;

	this_pbe = (struct pbe *) kmap_atomic(pbe_page, KM_BOUNCE_READ);
	first_pbe = this_pbe;

	while (this_pbe) {
		int loop = (PAGE_SIZE / sizeof(unsigned long)) - 1;

		origpage = kmap_atomic((struct page *) this_pbe->orig_address,
			KM_BIO_DST_IRQ);
		copypage = kmap_atomic((struct page *) this_pbe->address,
			KM_BIO_SRC_IRQ);

		while (loop >= 0) {
			*(origpage + loop) = *(copypage + loop);
			loop--;
		}

		kunmap_atomic(origpage, KM_BIO_DST_IRQ);
		kunmap_atomic(copypage, KM_BIO_SRC_IRQ);

		if (!this_pbe->next)
			break;

		if (pbe_index < PBES_PER_PAGE) {
			this_pbe++;
			pbe_index++;
		} else {
			pbe_page = (struct page *) this_pbe->next;
			kunmap_atomic(first_pbe, KM_BOUNCE_READ);
			if (!pbe_page)
				return;
			this_pbe = (struct pbe *) kmap_atomic(pbe_page,
					KM_BOUNCE_READ);
			first_pbe = this_pbe;
			pbe_index = 1;
		}
	}
	kunmap_atomic(first_pbe, KM_BOUNCE_READ);
}

#else /* CONFIG_HIGHMEM */
void copyback_high(void) { }
#endif

/**
 * free_pbe_list: Free page backup entries used by the atomic copy code.
 *
 * Normally, this function isn't used. If, however, we need to abort before
 * doing the atomic copy, we use this to free the pbes previously allocated.
 **/
static void free_pbe_list(struct pbe **list, int highmem)
{
	while(*list) {
		int i;
		struct pbe *free_pbe, *next_page = NULL;
		struct page *page;

		if (highmem) {
			page = (struct page *) *list;
			free_pbe = (struct pbe *) kmap(page);
		} else {
			page = virt_to_page(*list);
			free_pbe = *list;
		}

		for (i = 0; i < PBES_PER_PAGE; i++) {
			if (!free_pbe)
				break;
			if (highmem)
				__free_page(free_pbe->address);
			else
				free_page((unsigned long) free_pbe->address);
			free_pbe = free_pbe->next;
		}

		if (highmem) {
			if (free_pbe)
				next_page = free_pbe;
			kunmap(page);
		} else {
			if (free_pbe)
				next_page = free_pbe;
		}

		__free_page(page);
		*list = (struct pbe *) next_page;
	};
}

/**
 * copyback_post: Post atomic-restore actions.
 *
 * After doing the atomic restore, we have a few more things to do:
 * 1) We want to retain some values across the restore, so we now copy
 * these from the nosave variables to the normal ones.
 * 2) Set the status flags.
 * 3) Resume devices.
 * 4) Tell userui so it can redraw & restore settings.
 * 5) Reread the page cache.
 **/

void copyback_post(void)
{
	int loop;

	toi_action = toi_nosave_state1;
	toi_debug_state = toi_nosave_state2;
	toi_default_console_level = toi_nosave_state3;

	for (loop = 0; loop < 4; loop++)
		toi_io_time[loop/2][loop%2] =
			toi_nosave_io_speed[loop/2][loop%2];

	set_toi_state(TOI_NOW_RESUMING);

	if (toi_activate_storage(1))
		panic("Failed to reactivate our storage.");

	toi_ui_post_atomic_restore();

	toi_cond_pause(1, "About to reload secondary pagedir.");

	if (read_pageset2(0))
		panic("Unable to successfully reread the page cache.");

	/*
	 * If the user wants to sleep again after resuming from full-off,
	 * it's most likely to be in order to suspend to ram, so we'll
	 * do this check after loading pageset2, to give them the fastest
	 * wakeup when they are ready to use the computer again.
	 */
	toi_check_resleep();
}

/**
 * toi_copy_pageset1: Do the atomic copy of pageset1.
 *
 * Make the atomic copy of pageset1. We can't use copy_page (as we once did)
 * because we can't be sure what side effects it has. On my old Duron, with
 * 3DNOW, kernel_fpu_begin increments preempt count, making our preempt
 * count at resume time 4 instead of 3.
 * 
 * We don't want to call kmap_atomic unconditionally because it has the side
 * effect of incrementing the preempt count, which will leave it one too high
 * post resume (the page containing the preempt count will be copied after
 * its incremented. This is essentially the same problem.
 **/

void toi_copy_pageset1(void)
{
	int i;
	unsigned long source_index, dest_index;

	source_index = get_next_bit_on(&pageset1_map, max_pfn + 1);
	dest_index = get_next_bit_on(&pageset1_copy_map, max_pfn + 1);

	for (i = 0; i < pagedir1.size; i++) {
		unsigned long *origvirt, *copyvirt;
		struct page *origpage, *copypage;
		int loop = (PAGE_SIZE / sizeof(unsigned long)) - 1;

		origpage = pfn_to_page(source_index);
		copypage = pfn_to_page(dest_index);
		
		origvirt = PageHighMem(origpage) ?
			kmap_atomic(origpage, KM_USER0) :
			page_address(origpage);

	       	copyvirt = PageHighMem(copypage) ?
			kmap_atomic(copypage, KM_USER1) :
			page_address(copypage);

		while (loop >= 0) {
			*(copyvirt + loop) = *(origvirt + loop);
			loop--;
		}
		
		if (PageHighMem(origpage))
			kunmap_atomic(origvirt, KM_USER0);
		else if (toi_faulted) {
			printk("%p (%lu) being unmapped after faulting during atomic copy.\n", origpage, source_index);
			kernel_map_pages(origpage, 1, 0);
			clear_toi_fault();
		}

		if (PageHighMem(copypage))
			kunmap_atomic(copyvirt, KM_USER1);
		
		source_index = get_next_bit_on(&pageset1_map, source_index);
		dest_index = get_next_bit_on(&pageset1_copy_map, dest_index);
	}
}

/**
 * __toi_post_context_save: Steps after saving the cpu context.
 *
 * Steps taken after saving the CPU state to make the actual
 * atomic copy.
 *
 * Called from swsusp_save in snapshot.c via toi_post_context_save.
 **/

int __toi_post_context_save(void)
{
	int old_ps1_size = pagedir1.size;
	
	calculate_check_checksums(1);

	free_checksum_pages();

	toi_recalculate_image_contents(1);

	extra_pd1_pages_used = pagedir1.size - old_ps1_size;

	if (extra_pd1_pages_used > extra_pd1_pages_allowance) {
		printk("Pageset1 has grown by %d pages. "
			"extra_pages_allowance is currently only %d.\n",
			pagedir1.size - old_ps1_size,
			extra_pd1_pages_allowance);
		set_abort_result(TOI_EXTRA_PAGES_ALLOW_TOO_SMALL);
		return -1;
	}

	if (!test_action_state(TOI_TEST_FILTER_SPEED) &&
	    !test_action_state(TOI_TEST_BIO))
		toi_copy_pageset1();

	return 0;
}

/**
 * toi_hibernate: High level code for doing the atomic copy.
 *
 * High-level code which prepares to do the atomic copy. Loosely based
 * on the swsusp version, but with the following twists:
 * - We set toi_running so the swsusp code uses our code paths.
 * - We give better feedback regarding what goes wrong if there is a problem.
 * - We use an extra function to call the assembly, just in case this code
 *   is in a module (return address).
 **/

int toi_hibernate(void)
{
	int error;

	toi_running = 1; /* For the swsusp code we use :< */

	error = toi_lowlevel_builtin();

	if (!toi_in_hibernate)
		copyback_high();

	toi_running = 0;
	return error;
}

/**
 * toi_atomic_restore: Prepare to do the atomic restore.
 *
 * Get ready to do the atomic restore. This part gets us into the same
 * state we are in prior to do calling do_toi_lowlevel while
 * hibernating: hot-unplugging secondary cpus and freeze processes,
 * before starting the thread that will do the restore.
 **/

int toi_atomic_restore(void)
{
	int error, loop;

	toi_running = 1;

	toi_prepare_status(DONT_CLEAR_BAR,	"Atomic restore.");

	if (toi_go_atomic(PMSG_PRETHAW, 0))
		goto Failed;

	toi_nosave_state1 = toi_action;
	toi_nosave_state2 = toi_debug_state;
	toi_nosave_state3 = toi_default_console_level;
	
	for (loop = 0; loop < 4; loop++)
		toi_nosave_io_speed[loop/2][loop%2] =
			toi_io_time[loop/2][loop%2];
	memcpy(toi_nosave_commandline, saved_command_line, COMMAND_LINE_SIZE);

	/* We'll ignore saved state, but this gets preempt count (etc) right */
	save_processor_state();

	error = swsusp_arch_resume();
	/* 
	 * Code below is only ever reached in case of failure. Otherwise
	 * execution continues at place where swsusp_arch_suspend was called.
	 *
	 * We don't know whether it's safe to continue (this shouldn't happen),
	 * so lets err on the side of caution.
         */
	BUG();

Failed:
	free_pbe_list(&restore_pblist, 0);
#ifdef CONFIG_HIGHMEM
	free_pbe_list(&restore_highmem_pblist, 1);
#endif
	if (test_action_state(TOI_PM_PREPARE_CONSOLE))
		pm_restore_console();
	toi_running = 0;
	return 1;
}

int toi_go_atomic(pm_message_t state, int suspend_time)
{
	if (test_action_state(TOI_PM_PREPARE_CONSOLE))
		pm_prepare_console();

	if (suspend_time && toi_platform_prepare()) {
		set_abort_result(TOI_PLATFORM_PREP_FAILED);
		toi_end_atomic(ATOMIC_STEP_RESTORE_CONSOLE, suspend_time);
		return 1;
	}

	suspend_console();

	if (device_suspend(state)) {
		set_abort_result(TOI_DEVICE_REFUSED);
		toi_end_atomic(ATOMIC_STEP_RESUME_CONSOLE, suspend_time);
		return 1;
	}

	if (test_action_state(TOI_LATE_CPU_HOTPLUG)) {
		toi_prepare_status(DONT_CLEAR_BAR,	"Disable nonboot cpus.");
		if (disable_nonboot_cpus()) {
			set_abort_result(TOI_CPU_HOTPLUG_FAILED);
			toi_end_atomic(ATOMIC_STEP_DEVICE_RESUME,
					suspend_time);
			return 1;
		}
	}

	if (suspend_time && arch_prepare_suspend()) {
		set_abort_result(TOI_ARCH_PREPARE_FAILED);
		toi_end_atomic(ATOMIC_STEP_CPU_HOTPLUG, suspend_time);
		return 1;
	}

	local_irq_disable();

	/* At this point, device_suspend() has been called, but *not*
	 * device_power_down(). We *must* device_power_down() now.
	 * Otherwise, drivers for some devices (e.g. interrupt controllers)
	 * become desynchronized with the actual state of the hardware
	 * at resume time, and evil weirdness ensues.
	 */

	if (device_power_down(state)) {
		set_abort_result(TOI_DEVICE_REFUSED);
		toi_end_atomic(ATOMIC_STEP_IRQS, suspend_time);
		return 1;
	}

	return 0;
}

void toi_end_atomic(int stage, int suspend_time)
{
	switch (stage) {
		case ATOMIC_ALL_STEPS:
			device_power_up();
		case ATOMIC_STEP_IRQS:
			local_irq_enable();
		case ATOMIC_STEP_CPU_HOTPLUG:
			if (test_action_state(TOI_LATE_CPU_HOTPLUG))
				enable_nonboot_cpus();
		case ATOMIC_STEP_DEVICE_RESUME:
			toi_platform_finish();
			device_resume();
		case ATOMIC_STEP_RESUME_CONSOLE:
			resume_console();
		case ATOMIC_STEP_RESTORE_CONSOLE:
			if (test_action_state(TOI_PM_PREPARE_CONSOLE))
				pm_restore_console();
	}
}
