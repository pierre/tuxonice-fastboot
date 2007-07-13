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

#define SUSPEND_CORE_VERSION "2.2.10.2"

/*		 == Action states == 		*/

enum {
	SUSPEND_REBOOT,
	SUSPEND_PAUSE,
	SUSPEND_SLOW,
	SUSPEND_LOGALL,
	SUSPEND_CAN_CANCEL,
	SUSPEND_KEEP_IMAGE,
	SUSPEND_FREEZER_TEST,
	SUSPEND_SINGLESTEP,
	SUSPEND_PAUSE_NEAR_PAGESET_END,
	SUSPEND_TEST_FILTER_SPEED,
	SUSPEND_TEST_BIO,
	SUSPEND_NO_PAGESET2,
	SUSPEND_PM_PREPARE_CONSOLE,
	SUSPEND_IGNORE_ROOTFS,
	SUSPEND_REPLACE_SWSUSP,
	SUSPEND_RETRY_RESUME,
	SUSPEND_PAGESET2_FULL,
	SUSPEND_ABORT_ON_RESAVE_NEEDED,
	SUSPEND_NO_MULTITHREADED_IO,
	SUSPEND_NO_DIRECT_LOAD,
	SUSPEND_LATE_CPU_HOTPLUG,
};

extern unsigned long suspend_action;

#define clear_action_state(bit) (test_and_clear_bit(bit, &suspend_action))
#define test_action_state(bit) (test_bit(bit, &suspend_action))

/*		 == Result states == 		*/

enum {
	SUSPEND_ABORTED,
	SUSPEND_ABORT_REQUESTED,
	SUSPEND_NOSTORAGE_AVAILABLE,
	SUSPEND_INSUFFICIENT_STORAGE,
	SUSPEND_FREEZING_FAILED,
	SUSPEND_UNEXPECTED_ALLOC,
	SUSPEND_KEPT_IMAGE,
	SUSPEND_WOULD_EAT_MEMORY,
	SUSPEND_UNABLE_TO_FREE_ENOUGH_MEMORY,
	SUSPEND_ENCRYPTION_SETUP_FAILED,
	SUSPEND_PM_SEM,
	SUSPEND_DEVICE_REFUSED,
	SUSPEND_EXTRA_PAGES_ALLOW_TOO_SMALL,
	SUSPEND_UNABLE_TO_PREPARE_IMAGE,
	SUSPEND_FAILED_MODULE_INIT,
	SUSPEND_FAILED_MODULE_CLEANUP,
	SUSPEND_FAILED_IO,
	SUSPEND_OUT_OF_MEMORY,
	SUSPEND_IMAGE_ERROR,
	SUSPEND_PLATFORM_PREP_FAILED,
	SUSPEND_CPU_HOTPLUG_FAILED,
	SUSPEND_ARCH_PREPARE_FAILED,
	SUSPEND_RESAVE_NEEDED,
	SUSPEND_CANT_SUSPEND,
};

extern unsigned long suspend_result;

#define set_result_state(bit) (test_and_set_bit(bit, &suspend_result))
#define set_abort_result(bit) (	test_and_set_bit(SUSPEND_ABORTED, &suspend_result), \
				test_and_set_bit(bit, &suspend_result))
#define clear_result_state(bit) (test_and_clear_bit(bit, &suspend_result))
#define test_result_state(bit) (test_bit(bit, &suspend_result))

/*	 == Debug sections and levels == 	*/

/* debugging levels. */
enum {
	SUSPEND_STATUS = 0,
	SUSPEND_ERROR = 2,
	SUSPEND_LOW,
	SUSPEND_MEDIUM,
	SUSPEND_HIGH,
	SUSPEND_VERBOSE,
};

enum {
	SUSPEND_ANY_SECTION,
	SUSPEND_EAT_MEMORY,
	SUSPEND_IO,
	SUSPEND_HEADER,
	SUSPEND_WRITER,
	SUSPEND_MEMORY,
};

extern unsigned long suspend_debug_state;

#define set_debug_state(bit) (test_and_set_bit(bit, &suspend_debug_state))
#define clear_debug_state(bit) (test_and_clear_bit(bit, &suspend_debug_state))
#define test_debug_state(bit) (test_bit(bit, &suspend_debug_state))

/*		== Steps in suspending ==	*/

enum {
	STEP_SUSPEND_PREPARE_IMAGE,
	STEP_SUSPEND_SAVE_IMAGE,
	STEP_SUSPEND_POWERDOWN,
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
	return test_action_state(SUSPEND_NO_DIRECT_LOAD) ? 0 : PagePageset1Copy(page);
}

extern int pre_resume_freeze(void);

#define S2_WAIT_GFP (GFP_KERNEL | __GFP_NOWARN)
#define S2_ATOMIC_GFP (GFP_ATOMIC | __GFP_NOWARN)
#endif
