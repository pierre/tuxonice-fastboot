#ifndef _LINUX_SWSUSP_H
#define _LINUX_SWSUSP_H

#if defined(CONFIG_X86) || defined(CONFIG_FRV) || defined(CONFIG_PPC32) || defined(CONFIG_PPC64)
#include <asm/suspend.h>
#endif
#include <linux/swap.h>
#include <linux/notifier.h>
#include <linux/init.h>
#include <linux/pm.h>
#include <linux/mm.h>

/* struct pbe is used for creating lists of pages that should be restored
 * atomically during the resume from disk, because the page frames they have
 * occupied before the suspend are in use.
 */
struct pbe {
	void *address;		/* address of the copy */
	void *orig_address;	/* original address of a page */
	struct pbe *next;
};

/* mm/page_alloc.c */
extern void drain_local_pages(void);
extern void mark_free_pages(struct zone *zone);

#if defined(CONFIG_PM) && defined(CONFIG_VT) && defined(CONFIG_VT_CONSOLE)
extern int pm_prepare_console(void);
extern void pm_restore_console(void);
#else
static inline int pm_prepare_console(void) { return 0; }
static inline void pm_restore_console(void) {}
#endif

/**
 * struct hibernation_ops - hibernation platform support
 *
 * The methods in this structure allow a platform to override the default
 * mechanism of shutting down the machine during a hibernation transition.
 *
 * All three methods must be assigned.
 *
 * @prepare: prepare system for hibernation
 * @enter: shut down system after state has been saved to disk
 * @finish: finish/clean up after state has been reloaded
 * @pre_restore: prepare system for the restoration from a hibernation image
 * @restore_cleanup: clean up after a failing image restoration
 */
struct hibernation_ops {
	int (*prepare)(void);
	int (*enter)(void);
	void (*finish)(void);
	int (*pre_restore)(void);
	void (*restore_cleanup)(void);
};

#ifdef CONFIG_PM
#ifdef CONFIG_SOFTWARE_SUSPEND
/* kernel/power/snapshot.c */
extern void __register_nosave_region(unsigned long b, unsigned long e, int km);
static inline void register_nosave_region(unsigned long b, unsigned long e)
{
	__register_nosave_region(b, e, 0);
}
static inline void register_nosave_region_late(unsigned long b, unsigned long e)
{
	__register_nosave_region(b, e, 1);
}
extern int swsusp_page_is_forbidden(struct page *);
extern void swsusp_set_page_free(struct page *);
extern void swsusp_unset_page_free(struct page *);
extern unsigned long get_safe_page(gfp_t gfp_mask);

extern void hibernation_set_ops(struct hibernation_ops *ops);
extern int hibernate(void);
#else /* CONFIG_SOFTWARE_SUSPEND */
static inline int swsusp_page_is_forbidden(struct page *p) { return 0; }
static inline void swsusp_set_page_free(struct page *p) {}
static inline void swsusp_unset_page_free(struct page *p) {}

static inline void hibernation_set_ops(struct hibernation_ops *ops) {}
static inline int hibernate(void) { return -ENOSYS; }
#endif /* CONFIG_SOFTWARE_SUSPEND */

void save_processor_state(void);
void restore_processor_state(void);
struct saved_context;
void __save_processor_state(struct saved_context *ctxt);
void __restore_processor_state(struct saved_context *ctxt);

/* kernel/power/main.c */
extern struct blocking_notifier_head pm_chain_head;

static inline int register_pm_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&pm_chain_head, nb);
}

static inline int unregister_pm_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&pm_chain_head, nb);
}

#define pm_notifier(fn, pri) {				\
	static struct notifier_block fn##_nb =			\
		{ .notifier_call = fn, .priority = pri };	\
	register_pm_notifier(&fn##_nb);			\
}
#else /* CONFIG_PM */

static inline int register_pm_notifier(struct notifier_block *nb)
{
	return 0;
}

static inline int unregister_pm_notifier(struct notifier_block *nb)
{
	return 0;
}

#define pm_notifier(fn, pri)	do { (void)(fn); } while (0)
#endif /* CONFIG_PM */

#if !defined CONFIG_SOFTWARE_SUSPEND || !defined(CONFIG_PM)
static inline void register_nosave_region(unsigned long b, unsigned long e)
{
}
static inline void register_nosave_region_late(unsigned long b, unsigned long e)
{
}
#endif

enum {
	TOI_CAN_HIBERNATE,
	TOI_CAN_RESUME,
	TOI_RESUME_DEVICE_OK,
	TOI_NORESUME_SPECIFIED,
	TOI_SANITY_CHECK_PROMPT,
	TOI_CONTINUE_REQ,
	TOI_RESUMED_BEFORE,
	TOI_BOOT_TIME,
	TOI_NOW_RESUMING,
	TOI_IGNORE_LOGLEVEL,
	TOI_TRYING_TO_RESUME,
	TOI_TRY_RESUME_RD,
	TOI_LOADING_ALT_IMAGE,
	TOI_STOP_RESUME,
	TOI_IO_STOPPED,
};

#ifdef CONFIG_TOI

/* Used in init dir files */
extern unsigned long toi_state;
#define set_toi_state(bit) (set_bit(bit, &toi_state))
#define clear_toi_state(bit) (clear_bit(bit, &toi_state))
#define test_toi_state(bit) (test_bit(bit, &toi_state))
extern int toi_running;

#else /* !CONFIG_TOI */

#define toi_state		(0)
#define set_toi_state(bit) do { } while(0)
#define clear_toi_state(bit) do { } while (0)
#define test_toi_state(bit) (0)
#define toi_running (0)
#endif /* CONFIG_TOI */

#ifdef CONFIG_SOFTWARE_SUSPEND
#ifdef CONFIG_TOI
extern void toi_try_resume(void);
#else
#define toi_try_resume() do { } while(0)
#endif

extern int resume_attempted;
extern int software_resume(void);

static inline void check_resume_attempted(void)
{
	if (resume_attempted)
		return;

	software_resume();
}
#else
#define check_resume_attempted() do { } while(0)
#define resume_attempted (0)
#endif

#ifdef CONFIG_PRINTK_NOSAVE
#define POSS_NOSAVE __nosavedata
#else
#define POSS_NOSAVE
#endif

#endif /* _LINUX_SWSUSP_H */
