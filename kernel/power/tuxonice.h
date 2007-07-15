/*
 * kernel/power/tuxonice.h
 *
 * Copyright (C) 2004-2007 Nigel Cunningham (nigel at suspend2 net)
 *
 * This file is released under the GPLv2.
 *
 * It contains declarations used throughout swsusp.
 *
 */

#ifndef KERNEL_POWER_SUSPEND_H
#define KERNEL_POWER_SUSPEND_H

#include <linux/delay.h>
#include <linux/bootmem.h>
#include <linux/suspend.h>
#include <linux/dyn_pageflags.h>
#include <asm/setup.h>
#include "tuxonice_pageflags.h"

#define TOI_CORE_VERSION "2.2.10.2"

/*		 == Action states == 		*/

enum {
	TOI_REBOOT,
	TOI_PAUSE,
	TOI_SLOW,
	TOI_LOGALL,
	TOI_CAN_CANCEL,
	TOI_KEEP_IMAGE,
	TOI_FREEZER_TEST,
	TOI_SINGLESTEP,
	TOI_PAUSE_NEAR_PAGESET_END,
	TOI_TEST_FILTER_SPEED,
	TOI_TEST_BIO,
	TOI_NO_PAGESET2,
	TOI_PM_PREPARE_CONSOLE,
	TOI_IGNORE_ROOTFS,
	TOI_REPLACE_SWSUSP,
	TOI_PAGESET2_FULL,
	TOI_ABORT_ON_RESAVE_NEEDED,
	TOI_NO_MULTITHREADED_IO,
	TOI_NO_DIRECT_LOAD,
	TOI_LATE_CPU_HOTPLUG,
};

extern unsigned long suspend_action;

#define clear_action_state(bit) (test_and_clear_bit(bit, &suspend_action))
#define test_action_state(bit) (test_bit(bit, &suspend_action))

/*		 == Result states == 		*/

enum {
	TOI_ABORTED,
	TOI_ABORT_REQUESTED,
	TOI_NOSTORAGE_AVAILABLE,
	TOI_INSUFFICIENT_STORAGE,
	TOI_FREEZING_FAILED,
	TOI_UNEXPECTED_ALLOC,
	TOI_KEPT_IMAGE,
	TOI_WOULD_EAT_MEMORY,
	TOI_UNABLE_TO_FREE_ENOUGH_MEMORY,
	TOI_PM_SEM,
	TOI_DEVICE_REFUSED,
	TOI_EXTRA_PAGES_ALLOW_TOO_SMALL,
	TOI_UNABLE_TO_PREPARE_IMAGE,
	TOI_FAILED_MODULE_INIT,
	TOI_FAILED_MODULE_CLEANUP,
	TOI_FAILED_IO,
	TOI_OUT_OF_MEMORY,
	TOI_IMAGE_ERROR,
	TOI_PLATFORM_PREP_FAILED,
	TOI_CPU_HOTPLUG_FAILED,
	TOI_ARCH_PREPARE_FAILED,
	TOI_RESAVE_NEEDED,
	TOI_CANT_SUSPEND,
};

extern unsigned long suspend_result;

#define set_result_state(bit) (test_and_set_bit(bit, &suspend_result))
#define set_abort_result(bit) (	test_and_set_bit(TOI_ABORTED, &suspend_result), \
				test_and_set_bit(bit, &suspend_result))
#define clear_result_state(bit) (test_and_clear_bit(bit, &suspend_result))
#define test_result_state(bit) (test_bit(bit, &suspend_result))

/*	 == Debug sections and levels == 	*/

/* debugging levels. */
enum {
	TOI_STATUS = 0,
	TOI_ERROR = 2,
	TOI_LOW,
	TOI_MEDIUM,
	TOI_HIGH,
	TOI_VERBOSE,
};

enum {
	TOI_ANY_SECTION,
	TOI_EAT_MEMORY,
	TOI_IO,
	TOI_HEADER,
	TOI_WRITER,
	TOI_MEMORY,
};

extern unsigned long suspend_debug_state;

#define set_debug_state(bit) (test_and_set_bit(bit, &suspend_debug_state))
#define clear_debug_state(bit) (test_and_clear_bit(bit, &suspend_debug_state))
#define test_debug_state(bit) (test_bit(bit, &suspend_debug_state))

/*		== Steps in suspending ==	*/

enum {
	STEP_HIBERNATE_PREPARE_IMAGE,
	STEP_HIBERNATE_SAVE_IMAGE,
	STEP_HIBERNATE_POWERDOWN,
	STEP_RESUME_CAN_RESUME,
	STEP_RESUME_LOAD_PS1,
	STEP_RESUME_DO_RESTORE,
	STEP_RESUME_READ_PS2,
	STEP_RESUME_GO,
	STEP_RESUME_ALT_IMAGE,
};

/*		== Suspend states ==
	(see also include/linux/suspend.h)	*/

#define get_suspend_state()  (suspend_state)
#define restore_suspend_state(saved_state) \
	do { suspend_state = saved_state; } while(0)

/*		== Module support ==		*/

struct suspend2_core_fns {
	int (*post_context_save)(void);
	unsigned long (*get_nonconflicting_page)(void);
	int (*try_suspend)(int have_pmsem);
	void (*try_resume)(void);
};

extern struct suspend2_core_fns *s2_core_fns;

/*		== All else ==			*/
#define KB(x) ((x) << (PAGE_SHIFT - 10))
#define MB(x) ((x) >> (20 - PAGE_SHIFT))

extern int suspend_start_anything(int suspend_or_resume);
extern void suspend_finish_anything(int suspend_or_resume);

extern int save_image_part1(void);
extern int suspend_atomic_restore(void);

extern int _suspend2_try_suspend(int have_pmsem);
extern void __suspend2_try_resume(void);

extern int __suspend_post_context_save(void);

extern unsigned int nr_suspends;
extern char alt_resume_param[256];

extern void copyback_post(void);
extern int suspend2_suspend(void);
extern int extra_pd1_pages_used;

extern int suspend_io_time[2][2];

#define SECTOR_SIZE 512

extern void suspend_early_boot_message 
	(int can_erase_image, int default_answer, char *warning_reason, ...);

static inline int load_direct(struct page *page)
{
	return test_action_state(TOI_NO_DIRECT_LOAD) ? 0 : PagePageset1Copy(page);
}

extern int pre_resume_freeze(void);

#define S2_WAIT_GFP (GFP_KERNEL | __GFP_NOWARN)
#define S2_ATOMIC_GFP (GFP_ATOMIC | __GFP_NOWARN)
#endif
