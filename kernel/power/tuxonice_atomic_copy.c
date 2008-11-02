/*
 * kernel/power/tuxonice_atomic_copy.c
 *
 * Copyright 2004-2007 Nigel Cunningham (nigel at tuxonice net)
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
#include <linux/ftrace.h>
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
#include "tuxonice_alloc.h"

long extra_pd1_pages_used;
static int ftrace_save;

/**
 * free_pbe_list: Free page backup entries used by the atomic copy code.
 *
 * Normally, this function isn't used. If, however, we need to abort before
 * doing the atomic copy, we use this to free the pbes previously allocated.
 **/
static void free_pbe_list(struct pbe **list, int highmem)
{
	while (*list) {
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
				toi__free_page(29, free_pbe->address);
			else
				toi_free_page(29,
					(unsigned long) free_pbe->address);
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

		toi__free_page(29, page);
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
	struct toi_boot_kernel_data *bkd =
		(struct toi_boot_kernel_data *) boot_kernel_data_buffer;

	/*
	 * The boot kernel's data may be larger (newer version) or
	 * smaller (older version) than ours. Copy the minimum
	 * of the two sizes, so that we don't overwrite valid values
	 * from pre-atomic copy.
	 */

	memcpy(&toi_bkd, (char *) boot_kernel_data_buffer,
			min_t(int, sizeof(struct toi_boot_kernel_data),
				bkd->size));

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
		int loop = (PAGE_SIZE / sizeof(unsigned long)) - 1,
		    was_present;

		origpage = pfn_to_page(source_index);
		copypage = pfn_to_page(dest_index);

		origvirt = PageHighMem(origpage) ?
			kmap_atomic(origpage, KM_USER0) :
			page_address(origpage);

		copyvirt = PageHighMem(copypage) ?
			kmap_atomic(copypage, KM_USER1) :
			page_address(copypage);

		was_present = kernel_page_present(origpage);
		if (!was_present)
			kernel_map_pages(origpage, 1, 1);

		while (loop >= 0) {
			*(copyvirt + loop) = *(origvirt + loop);
			loop--;
		}

		if (!was_present)
			kernel_map_pages(origpage, 1, 0);

		if (PageHighMem(origpage))
			kunmap_atomic(origvirt, KM_USER0);

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
	long old_ps1_size = pagedir1.size;

	check_checksums();

	free_checksum_pages();

	toi_recalculate_image_contents(1);

	extra_pd1_pages_used = pagedir1.size - old_ps1_size;

	if (extra_pd1_pages_used > extra_pd1_pages_allowance) {
		printk(KERN_INFO "Pageset1 has grown by %ld pages. "
			"extra_pages_allowance is currently only %lu.\n",
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
	int error;

	toi_running = 1;

	toi_prepare_status(DONT_CLEAR_BAR,	"Atomic restore.");

	if (add_boot_kernel_data_pbe())
		goto Failed;

	if (toi_go_atomic(PMSG_QUIESCE, 0))
		goto Failed;

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
	toi_prepare_status(DONT_CLEAR_BAR, "Doing atomic copy/restore.");

	if (suspend_time && toi_platform_begin()) {
		set_abort_result(TOI_PLATFORM_PREP_FAILED);
		toi_end_atomic(ATOMIC_STEP_PLATFORM_END, suspend_time, 0);
		return 1;
	}

	suspend_console();
	ftrace_save = __ftrace_enabled_save();

	if (device_suspend(state)) {
		set_abort_result(TOI_DEVICE_REFUSED);
		toi_end_atomic(ATOMIC_STEP_RESUME_CONSOLE, suspend_time, 1);
		return 1;
	}

	if (suspend_time && toi_platform_pre_snapshot()) {
		set_abort_result(TOI_PRE_SNAPSHOT_FAILED);
		toi_end_atomic(ATOMIC_STEP_PLATFORM_FINISH, suspend_time, 0);
		return 1;
	}

	if (!suspend_time && toi_platform_pre_restore()) {
		set_abort_result(TOI_PRE_RESTORE_FAILED);
		toi_end_atomic(ATOMIC_STEP_DEVICE_RESUME, suspend_time, 0);
		return 1;
	}

	if (test_action_state(TOI_LATE_CPU_HOTPLUG)) {
		if (disable_nonboot_cpus()) {
			set_abort_result(TOI_CPU_HOTPLUG_FAILED);
			toi_end_atomic(ATOMIC_STEP_CPU_HOTPLUG,
					suspend_time, 0);
			return 1;
		}
	}

	if (suspend_time && arch_prepare_suspend()) {
		set_abort_result(TOI_ARCH_PREPARE_FAILED);
		toi_end_atomic(ATOMIC_STEP_CPU_HOTPLUG, suspend_time, 0);
		return 1;
	}

	device_pm_lock();
	local_irq_disable();

	/* At this point, device_suspend() has been called, but *not*
	 * device_power_down(). We *must* device_power_down() now.
	 * Otherwise, drivers for some devices (e.g. interrupt controllers)
	 * become desynchronized with the actual state of the hardware
	 * at resume time, and evil weirdness ensues.
	 */

	if (device_power_down(state)) {
		set_abort_result(TOI_DEVICE_REFUSED);
		toi_end_atomic(ATOMIC_STEP_IRQS, suspend_time, 0);
		return 1;
	}

	return 0;
}

void toi_end_atomic(int stage, int suspend_time, int error)
{
	switch (stage) {
	case ATOMIC_ALL_STEPS:
		if (!suspend_time)
			toi_platform_leave();
		device_power_up(error ? PMSG_RECOVER :
			(suspend_time ? PMSG_THAW : PMSG_RESTORE));
	case ATOMIC_STEP_IRQS:
		local_irq_enable();
		device_pm_unlock();
	case ATOMIC_STEP_CPU_HOTPLUG:
		if (test_action_state(TOI_LATE_CPU_HOTPLUG))
			enable_nonboot_cpus();
	case ATOMIC_STEP_PLATFORM_FINISH:
		toi_platform_finish();
	case ATOMIC_STEP_DEVICE_RESUME:
		if (suspend_time && error)
			toi_platform_recover();
		device_resume(error ? PMSG_RECOVER :
			(suspend_time ? PMSG_THAW : PMSG_RESTORE));
	case ATOMIC_STEP_RESUME_CONSOLE:
		__ftrace_enabled_restore(ftrace_save);
		resume_console();
	case ATOMIC_STEP_PLATFORM_END:
		toi_platform_end();

		toi_prepare_status(DONT_CLEAR_BAR, "Post atomic.");
	}
}
