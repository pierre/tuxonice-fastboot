/*
 * Copyright (C) 2004-2007 Nigel Cunningham (nigel at suspend2 net)
 *
 * This file is released under the GPLv2.
 */
#include <linux/module.h>
#include <linux/resume-trace.h>
#include <linux/syscalls.h>
#include <linux/kernel.h>
#include <linux/swap.h>
#include <linux/syscalls.h>
#include <linux/bio.h>
#include <linux/root_dev.h>
#include <linux/freezer.h>
#include <linux/reboot.h>
#include <linux/writeback.h>
#include <linux/tty.h>
#include <linux/crypto.h>
#include <linux/cpu.h>
#include <linux/dyn_pageflags.h>
#include "io.h"
#include "suspend.h"
#include "extent.h"
#include "block_io.h"
#include "netlink.h"
#include "prepare_image.h"
#include "ui.h"
#include "sysfs.h"
#include "pagedir.h"
#include "modules.h"
#include "suspend2_builtin.h"

#ifndef CONFIG_SOFTWARE_SUSPEND
struct hibernation_ops *hibernation_ops;

/**
 * hibernation_set_ops - set the global hibernate operations
 * @ops: the hibernation operations to use in subsequent hibernation transitions
 */

void hibernation_set_ops(struct hibernation_ops *ops)
{
	if (ops && !(ops->prepare && ops->enter && ops->finish)) {
		WARN_ON(1);
		return;
	}
	mutex_lock(&pm_mutex);
	hibernation_ops = ops;
	mutex_unlock(&pm_mutex);
}
EXPORT_SYMBOL_GPL(hibernation_set_ops);
#endif

EXPORT_SYMBOL_GPL(hibernation_ops);

#ifdef CONFIG_SUSPEND2_CORE_EXPORTS
#ifdef CONFIG_SOFTWARE_SUSPEND
EXPORT_SYMBOL_GPL(resume_file);
#endif

EXPORT_SYMBOL_GPL(max_pfn);
EXPORT_SYMBOL_GPL(free_dyn_pageflags);
EXPORT_SYMBOL_GPL(clear_dynpageflag);
EXPORT_SYMBOL_GPL(test_dynpageflag);
EXPORT_SYMBOL_GPL(set_dynpageflag);
EXPORT_SYMBOL_GPL(get_next_bit_on);
EXPORT_SYMBOL_GPL(allocate_dyn_pageflags);
EXPORT_SYMBOL_GPL(clear_dyn_pageflags);

#ifdef CONFIG_X86_64
EXPORT_SYMBOL_GPL(restore_processor_state);
EXPORT_SYMBOL_GPL(save_processor_state);
#endif

EXPORT_SYMBOL_GPL(kernel_shutdown_prepare);
EXPORT_SYMBOL_GPL(drop_pagecache);
EXPORT_SYMBOL_GPL(restore_pblist);
EXPORT_SYMBOL_GPL(pm_mutex);
EXPORT_SYMBOL_GPL(pm_restore_console);
EXPORT_SYMBOL_GPL(super_blocks);
EXPORT_SYMBOL_GPL(next_zone);

EXPORT_SYMBOL_GPL(freeze_processes);
EXPORT_SYMBOL_GPL(thaw_processes);
EXPORT_SYMBOL_GPL(thaw_kernel_threads);
EXPORT_SYMBOL_GPL(shrink_all_memory);
EXPORT_SYMBOL_GPL(shrink_one_zone);
EXPORT_SYMBOL_GPL(saveable_page);
EXPORT_SYMBOL_GPL(swsusp_arch_suspend);
EXPORT_SYMBOL_GPL(swsusp_arch_resume);
EXPORT_SYMBOL_GPL(pm_ops);
EXPORT_SYMBOL_GPL(pm_prepare_console);
EXPORT_SYMBOL_GPL(follow_page);
EXPORT_SYMBOL_GPL(machine_halt);
EXPORT_SYMBOL_GPL(block_dump);
EXPORT_SYMBOL_GPL(unlink_lru_lists);
EXPORT_SYMBOL_GPL(relink_lru_lists);
EXPORT_SYMBOL_GPL(power_subsys);
EXPORT_SYMBOL_GPL(machine_power_off);
EXPORT_SYMBOL_GPL(suspend_enter);
EXPORT_SYMBOL_GPL(first_online_pgdat);
EXPORT_SYMBOL_GPL(next_online_pgdat);
EXPORT_SYMBOL_GPL(machine_restart);
EXPORT_SYMBOL_GPL(saved_command_line);
EXPORT_SYMBOL_GPL(tasklist_lock);
#ifdef CONFIG_SUSPEND_SMP
EXPORT_SYMBOL_GPL(disable_nonboot_cpus);
EXPORT_SYMBOL_GPL(enable_nonboot_cpus);
#endif
#endif

#ifdef CONFIG_SUSPEND2_USERUI_EXPORTS
EXPORT_SYMBOL_GPL(kmsg_redirect);
#ifndef CONFIG_COMPAT
EXPORT_SYMBOL_GPL(sys_ioctl);
#endif
#endif

#if defined(CONFIG_SUSPEND2_USERUI_EXPORTS) || defined(CONFIG_SUSPEND2_CORE_EXPORTS)
EXPORT_SYMBOL_GPL(console_printk);
#endif
#ifdef CONFIG_SUSPEND2_SWAP_EXPORTS	/* Suspend swap specific */
EXPORT_SYMBOL_GPL(sys_swapon);
EXPORT_SYMBOL_GPL(sys_swapoff);
EXPORT_SYMBOL_GPL(si_swapinfo);
EXPORT_SYMBOL_GPL(map_swap_page);
EXPORT_SYMBOL_GPL(get_swap_page);
EXPORT_SYMBOL_GPL(swap_free);
EXPORT_SYMBOL_GPL(get_swap_info_struct);
#endif

#ifdef CONFIG_SUSPEND2_FILE_EXPORTS
/* Suspend_file specific */
extern char * __initdata root_device_name;

EXPORT_SYMBOL_GPL(ROOT_DEV);
EXPORT_SYMBOL_GPL(root_device_name);
EXPORT_SYMBOL_GPL(sys_unlink);
EXPORT_SYMBOL_GPL(sys_mknod);
#endif

/* Swap or file */
#if defined(CONFIG_SUSPEND2_FILE_EXPORTS) || defined(CONFIG_SUSPEND2_SWAP_EXPORTS)
EXPORT_SYMBOL_GPL(bio_set_pages_dirty);
EXPORT_SYMBOL_GPL(name_to_dev_t);
#endif

#if defined(CONFIG_SUSPEND2_EXPORTS) || defined(CONFIG_SUSPEND2_CORE_EXPORTS)
EXPORT_SYMBOL_GPL(snprintf_used);
#endif
struct suspend2_core_fns *s2_core_fns;
EXPORT_SYMBOL_GPL(s2_core_fns);

dyn_pageflags_t pageset1_map;
dyn_pageflags_t pageset1_copy_map;
EXPORT_SYMBOL_GPL(pageset1_map);
EXPORT_SYMBOL_GPL(pageset1_copy_map);

unsigned long suspend_result = 0;
unsigned long suspend_debug_state = 0;
int suspend_io_time[2][2];
struct pagedir pagedir1 = {1};

EXPORT_SYMBOL_GPL(suspend_io_time);
EXPORT_SYMBOL_GPL(suspend_debug_state);
EXPORT_SYMBOL_GPL(suspend_result);
EXPORT_SYMBOL_GPL(pagedir1);

unsigned long suspend_get_nonconflicting_page(void)
{
	return s2_core_fns->get_nonconflicting_page();
}

int suspend_post_context_save(void)
{
	return s2_core_fns->post_context_save();
}

int suspend2_try_suspend(int have_pmsem)
{
	if (!s2_core_fns)
		return -ENODEV;

	return s2_core_fns->try_suspend(have_pmsem);
}

void suspend2_try_resume(void)
{
	if (s2_core_fns)
		s2_core_fns->try_resume();
}

int suspend2_lowlevel_builtin(void)
{
	int error = 0;

	save_processor_state();
	if ((error = swsusp_arch_suspend()))
		printk(KERN_ERR "Error %d suspending\n", error);
	/* Restore control flow appears here */
	restore_processor_state();

	return error;
}

#ifndef CONFIG_SOFTWARE_SUSPEND
int hibernate(void)
{
	return suspend2_try_suspend(0);
}
#endif

EXPORT_SYMBOL_GPL(suspend2_lowlevel_builtin);

unsigned long suspend_compress_bytes_in, suspend_compress_bytes_out;
EXPORT_SYMBOL_GPL(suspend_compress_bytes_in);
EXPORT_SYMBOL_GPL(suspend_compress_bytes_out);

#ifdef CONFIG_SUSPEND2_REPLACE_SWSUSP
unsigned long suspend_action = (1 << SUSPEND_REPLACE_SWSUSP) | \
			       (1 << SUSPEND_PAGESET2_FULL) | \
			       (1 << SUSPEND_LATE_CPU_HOTPLUG);
#else
unsigned long suspend_action = 	(1 << SUSPEND_PAGESET2_FULL) | \
				(1 << SUSPEND_LATE_CPU_HOTPLUG);
#endif
EXPORT_SYMBOL_GPL(suspend_action);

unsigned long suspend_state = ((1 << SUSPEND_BOOT_TIME) |
		(1 << SUSPEND_IGNORE_LOGLEVEL) |
		(1 << SUSPEND_IO_STOPPED));
EXPORT_SYMBOL_GPL(suspend_state);

/* The number of suspends we have started (some may have been cancelled) */
unsigned int nr_suspends;
EXPORT_SYMBOL_GPL(nr_suspends);

char resume2_file[256] = CONFIG_SUSPEND2_DEFAULT_RESUME2;
EXPORT_SYMBOL_GPL(resume2_file);

int suspend2_running = 0;
EXPORT_SYMBOL_GPL(suspend2_running);

int suspend2_in_suspend __nosavedata;
EXPORT_SYMBOL_GPL(suspend2_in_suspend);

unsigned long suspend2_nosave_state1 __nosavedata = 0;
unsigned long suspend2_nosave_state2 __nosavedata = 0;
int suspend2_nosave_state3 __nosavedata = 0;
int suspend2_nosave_io_speed[2][2] __nosavedata;
__nosavedata char suspend2_nosave_commandline[COMMAND_LINE_SIZE];

__nosavedata struct pbe *restore_highmem_pblist;

#ifdef CONFIG_SUSPEND2_CORE_EXPORTS
#ifdef CONFIG_HIGHMEM
EXPORT_SYMBOL_GPL(nr_free_highpages);
EXPORT_SYMBOL_GPL(saveable_highmem_page);
EXPORT_SYMBOL_GPL(restore_highmem_pblist);
#endif

EXPORT_SYMBOL_GPL(suspend2_nosave_state1);
EXPORT_SYMBOL_GPL(suspend2_nosave_state2);
EXPORT_SYMBOL_GPL(suspend2_nosave_state3);
EXPORT_SYMBOL_GPL(suspend2_nosave_io_speed);
EXPORT_SYMBOL_GPL(suspend2_nosave_commandline);
#endif

/* --  Commandline Parameter Handling ---
 *
 * Resume setup: obtain the storage device.
 */
static int __init resume2_setup(char *str)
{
	if (!*str)
		return 0;
	
	strncpy(resume2_file, str, 255);
	return 0;
}

/*
 * Allow the user to specify that we should ignore any image found and
 * invalidate the image if necesssary. This is equivalent to running
 * the task queue and a sync and then turning off the power. The same
 * precautions should be taken: fsck if you're not journalled.
 */
static int __init noresume2_setup(char *str)
{
	set_suspend_state(SUSPEND_NORESUME_SPECIFIED);
	return 0;
}

static int __init suspend_retry_resume_setup(char *str)
{
	set_suspend_state(SUSPEND_RETRY_RESUME);
	return 0;
}

#ifndef CONFIG_SOFTWARE_SUSPEND
static int __init resume_setup(char *str)
{
	if (!*str)
		return 0;
	
	strncpy(resume2_file, str, 255);
	return 0;
}

static int __init noresume_setup(char *str)
{
	set_suspend_state(SUSPEND_NORESUME_SPECIFIED);
	return 0;
}
__setup("noresume", noresume_setup);
__setup("resume=", resume_setup);
#endif

__setup("noresume2", noresume2_setup);
__setup("resume2=", resume2_setup);
__setup("suspend_retry_resume", suspend_retry_resume_setup);

