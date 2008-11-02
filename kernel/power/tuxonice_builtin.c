/*
 * Copyright (C) 2004-2007 Nigel Cunningham (nigel at tuxonice net)
 *
 * This file is released under the GPLv2.
 */
#include <linux/module.h>
#include <linux/resume-trace.h>
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
#include <linux/ctype.h>
#include "tuxonice_io.h"
#include "tuxonice.h"
#include "tuxonice_extent.h"
#include "tuxonice_block_io.h"
#include "tuxonice_netlink.h"
#include "tuxonice_prepare_image.h"
#include "tuxonice_ui.h"
#include "tuxonice_sysfs.h"
#include "tuxonice_pagedir.h"
#include "tuxonice_modules.h"
#include "tuxonice_builtin.h"
#include "tuxonice_power_off.h"
#include "power.h"

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
	struct page *pbe_page = (struct page *) restore_highmem_pblist;
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

char toi_wait_for_keypress_dev_console(int timeout)
{
	int fd, this_timeout = 255;
	char key = '\0';
	struct termios t, t_backup;

	/* We should be guaranteed /dev/console exists after populate_rootfs()
	 * in init/main.c.
	 */
	fd = sys_open("/dev/console", O_RDONLY, 0);
	if (fd < 0) {
		printk(KERN_INFO "Couldn't open /dev/console.\n");
		return key;
	}

	if (sys_ioctl(fd, TCGETS, (long)&t) < 0)
		goto out_close;

	memcpy(&t_backup, &t, sizeof(t));

	t.c_lflag &= ~(ISIG|ICANON|ECHO);
	t.c_cc[VMIN] = 0;

new_timeout:
	if (timeout > 0) {
		this_timeout = timeout < 26 ? timeout : 25;
		timeout -= this_timeout;
		this_timeout *= 10;
	}

	t.c_cc[VTIME] = this_timeout;

	if (sys_ioctl(fd, TCSETS, (long)&t) < 0)
		goto out_restore;

	while (1) {
		if (sys_read(fd, &key, 1) <= 0) {
			if (timeout)
				goto new_timeout;
			key = '\0';
			break;
		}
		key = tolower(key);
		if (test_toi_state(TOI_SANITY_CHECK_PROMPT)) {
			if (key == 'c') {
				set_toi_state(TOI_CONTINUE_REQ);
				break;
			} else if (key == ' ')
				break;
		} else
			break;
	}

out_restore:
	sys_ioctl(fd, TCSETS, (long)&t_backup);
out_close:
	sys_close(fd);

	return key;
}

struct toi_boot_kernel_data toi_bkd __nosavedata
		__attribute__((aligned(PAGE_SIZE))) = {
	MY_BOOT_KERNEL_DATA_VERSION,
	0,
#ifdef CONFIG_TOI_REPLACE_SWSUSP
	(1 << TOI_REPLACE_SWSUSP) |
#endif
	(1 << TOI_NO_FLUSHER_THREAD) |
	(1 << TOI_PAGESET2_FULL) | (1 << TOI_LATE_CPU_HOTPLUG),
};
EXPORT_IF_TOI_MODULAR(toi_bkd);

struct block_device *toi_open_by_devnum(dev_t dev, fmode_t mode)
{
	struct block_device *bdev = bdget(dev);
	int err = -ENOMEM;
	if (bdev)
		err = blkdev_get(bdev, mode);
	return err ? ERR_PTR(err) : bdev;
}
EXPORT_IF_TOI_MODULAR(toi_open_by_devnum);

EXPORT_IF_TOI_MODULAR(toi_wait_for_keypress_dev_console);
EXPORT_IF_TOI_MODULAR(hibernation_platform_enter);
EXPORT_IF_TOI_MODULAR(platform_begin);
EXPORT_IF_TOI_MODULAR(platform_pre_snapshot);
EXPORT_IF_TOI_MODULAR(platform_recover);
EXPORT_IF_TOI_MODULAR(platform_leave);
EXPORT_IF_TOI_MODULAR(platform_end);
EXPORT_IF_TOI_MODULAR(platform_finish);
EXPORT_IF_TOI_MODULAR(platform_pre_restore);
EXPORT_IF_TOI_MODULAR(platform_restore_cleanup);
EXPORT_IF_TOI_MODULAR(power_kobj);
EXPORT_IF_TOI_MODULAR(pm_notifier_call_chain);
EXPORT_IF_TOI_MODULAR(init_swsusp_header);

#ifdef CONFIG_ARCH_HIBERNATION_HEADER
EXPORT_IF_TOI_MODULAR(arch_hibernation_header_restore);
#endif

#ifdef CONFIG_TOI_CORE_EXPORTS
#ifdef CONFIG_X86_64
EXPORT_SYMBOL_GPL(save_processor_state);
#endif

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
EXPORT_SYMBOL_GPL(saveable_page);
EXPORT_SYMBOL_GPL(swsusp_arch_resume);
EXPORT_SYMBOL_GPL(follow_page);
EXPORT_SYMBOL_GPL(block_dump);
EXPORT_SYMBOL_GPL(suspend_devices_and_enter);
EXPORT_SYMBOL_GPL(first_online_pgdat);
EXPORT_SYMBOL_GPL(next_online_pgdat);
EXPORT_SYMBOL_GPL(machine_restart);
EXPORT_SYMBOL_GPL(tasklist_lock);
#ifdef CONFIG_PM_SLEEP_SMP
EXPORT_SYMBOL_GPL(disable_nonboot_cpus);
EXPORT_SYMBOL_GPL(enable_nonboot_cpus);
#endif
#endif

int toi_wait = CONFIG_TOI_DEFAULT_WAIT;

#ifdef CONFIG_TOI_USERUI_EXPORTS
EXPORT_SYMBOL_GPL(kmsg_redirect);
#endif
EXPORT_IF_TOI_MODULAR(toi_wait);

#if defined(CONFIG_TOI_USERUI_EXPORTS) || defined(CONFIG_TOI_CORE_EXPORTS)
EXPORT_SYMBOL_GPL(console_printk);
#endif
#ifdef CONFIG_TOI_SWAP_EXPORTS	/* TuxOnIce swap specific */
EXPORT_SYMBOL_GPL(sys_swapon);
EXPORT_SYMBOL_GPL(sys_swapoff);
EXPORT_SYMBOL_GPL(si_swapinfo);
EXPORT_SYMBOL_GPL(map_swap_page);
EXPORT_SYMBOL_GPL(get_swap_page);
EXPORT_SYMBOL_GPL(swap_free);
EXPORT_SYMBOL_GPL(get_swap_info_struct);
#endif

/* Swap or file */
#if defined(CONFIG_TOI_FILE_EXPORTS) || defined(CONFIG_TOI_SWAP_EXPORTS)
EXPORT_SYMBOL_GPL(name_to_dev_t);
#endif

#if defined(CONFIG_TOI_FILE_EXPORTS) || defined(CONFIG_TOI_SWAP_EXPORTS) || \
	defined(CONFIG_TOI_CORE_EXPORTS)
EXPORT_SYMBOL_GPL(resume_file);
#endif
struct toi_core_fns *toi_core_fns;
EXPORT_IF_TOI_MODULAR(toi_core_fns);

DECLARE_DYN_PAGEFLAGS(pageset1_map);
DECLARE_DYN_PAGEFLAGS(pageset1_copy_map);
EXPORT_IF_TOI_MODULAR(pageset1_map);
EXPORT_IF_TOI_MODULAR(pageset1_copy_map);

unsigned long toi_result;
struct pagedir pagedir1 = {1};

EXPORT_IF_TOI_MODULAR(toi_result);
EXPORT_IF_TOI_MODULAR(pagedir1);

unsigned long toi_get_nonconflicting_page(void)
{
	return toi_core_fns->get_nonconflicting_page();
}

int toi_post_context_save(void)
{
	return toi_core_fns->post_context_save();
}

int toi_try_hibernate(int have_pmsem)
{
	if (!toi_core_fns)
		return -ENODEV;

	return toi_core_fns->try_hibernate(have_pmsem);
}

static int num_resume_calls = 0;
#ifdef CONFIG_TOI_IGNORE_LATE_INITCALL
static int ignore_late_initcall = 1;
#else
static int ignore_late_initcall = 0;
#endif

void toi_try_resume(void)
{
	/* Don't let it wrap around eventually */
	if (num_resume_calls < 2)
		num_resume_calls++;

	if (num_resume_calls == 1 && ignore_late_initcall) {
		printk(KERN_INFO "TuxOnIce: Ignoring late initcall, as requested.\n");
		return;
	}

	if (toi_core_fns)
		toi_core_fns->try_resume();
	else
		printk(KERN_INFO "TuxOnIce core not loaded yet.\n");
}

int toi_lowlevel_builtin(void)
{
	int error = 0;

	save_processor_state();
	error = swsusp_arch_suspend();
	if (error)
		printk(KERN_ERR "Error %d hibernating\n", error);

	/* Restore control flow appears here */
	if (!toi_in_hibernate) {
		copyback_high();
		set_toi_state(TOI_NOW_RESUMING);
	}

	restore_processor_state();

	return error;
}

EXPORT_IF_TOI_MODULAR(toi_lowlevel_builtin);

unsigned long toi_compress_bytes_in, toi_compress_bytes_out;
EXPORT_IF_TOI_MODULAR(toi_compress_bytes_in);
EXPORT_IF_TOI_MODULAR(toi_compress_bytes_out);

unsigned long toi_state = ((1 << TOI_BOOT_TIME) |
		(1 << TOI_IGNORE_LOGLEVEL) |
		(1 << TOI_IO_STOPPED));
EXPORT_IF_TOI_MODULAR(toi_state);

/* The number of hibernates we have started (some may have been cancelled) */
unsigned int nr_hibernates;
EXPORT_IF_TOI_MODULAR(nr_hibernates);

int toi_running;
EXPORT_IF_TOI_MODULAR(toi_running);

int toi_in_hibernate __nosavedata;
EXPORT_IF_TOI_MODULAR(toi_in_hibernate);

__nosavedata struct pbe *restore_highmem_pblist;

#ifdef CONFIG_TOI_CORE_EXPORTS
#ifdef CONFIG_HIGHMEM
EXPORT_SYMBOL_GPL(nr_free_highpages);
EXPORT_SYMBOL_GPL(saveable_highmem_page);
EXPORT_SYMBOL_GPL(restore_highmem_pblist);
#endif
#endif

#if defined(CONFIG_TOI_CORE_EXPORTS) || defined(CONFIG_TOI_PAGEFLAGS_EXPORTS)
EXPORT_SYMBOL_GPL(max_pfn);
#endif

#if defined(CONFIG_TOI_EXPORTS) || defined(CONFIG_TOI_CORE_EXPORTS)
EXPORT_SYMBOL_GPL(snprintf_used);
#endif

static int __init toi_wait_setup(char *str)
{
	int value;

	if (sscanf(str, "=%d", &value)) {
		if (value < -1 || value > 255)
			printk(KERN_INFO "TuxOnIce_wait outside range -1 to "
					"255.\n");
		else
			toi_wait = value;
	}

	return 1;
}

__setup("toi_wait", toi_wait_setup);

static int __init toi_ignore_late_initcall_setup(char *str)
{
	int value;

	if (sscanf(str, "=%d", &value))
		ignore_late_initcall = value;

	return 1;
}

__setup("toi_initramfs_resume_only", toi_ignore_late_initcall_setup);

