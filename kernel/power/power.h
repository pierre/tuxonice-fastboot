/*
 * Copyright (C) 2004-2008 Nigel Cunningham (nigel at tuxonice net)
 */

#ifndef KERNEL_POWER_POWER_H
#define KERNEL_POWER_POWER_H

#include <linux/suspend.h>
#include <linux/suspend_ioctls.h>
#include <linux/utsname.h>
#include <linux/freezer.h>

struct swsusp_info {
	struct new_utsname	uts;
	u32			version_code;
	unsigned long		num_physpages;
	int			cpus;
	unsigned long		image_pages;
	unsigned long		pages;
	unsigned long		size;
} __attribute__((aligned(PAGE_SIZE)));

#ifdef CONFIG_HIBERNATION
#ifdef CONFIG_ARCH_HIBERNATION_HEADER
/* Maximum size of architecture specific data in a hibernation header */
#define MAX_ARCH_HEADER_SIZE	(sizeof(struct new_utsname) + 4)

extern int arch_hibernation_header_save(void *addr, unsigned int max_size);
extern int arch_hibernation_header_restore(void *addr);

static inline int init_swsusp_header_complete(struct swsusp_info *info)
{
	return arch_hibernation_header_save(info, MAX_ARCH_HEADER_SIZE);
}

static inline char *check_swsusp_image_kernel(struct swsusp_info *info)
{
	return arch_hibernation_header_restore(info) ?
			"architecture specific data" : NULL;
}
#else
extern char *check_swsusp_image_kernel(struct swsusp_info *info);
#endif /* CONFIG_ARCH_HIBERNATION_HEADER */
extern int init_swsusp_header(struct swsusp_info *info);

extern char resume_file[256];
/*
 * Keep some memory free so that I/O operations can succeed without paging
 * [Might this be more than 4 MB?]
 */
#define PAGES_FOR_IO	((4096 * 1024) >> PAGE_SHIFT)

/*
 * Keep 1 MB of memory free so that device drivers can allocate some pages in
 * their .suspend() routines without breaking the suspend to disk.
 */
#define SPARE_PAGES	((1024 * 1024) >> PAGE_SHIFT)

/* kernel/power/disk.c */
extern int hibernation_snapshot(int platform_mode);
extern int hibernation_restore(int platform_mode);
extern int hibernation_platform_enter(void);
extern void platform_recover(int platform_mode);
#endif

extern int pfn_is_nosave(unsigned long);

#define power_attr(_name) \
static struct kobj_attribute _name##_attr = {	\
	.attr	= {				\
		.name = __stringify(_name),	\
		.mode = 0644,			\
	},					\
	.show	= _name##_show,			\
	.store	= _name##_store,		\
}

extern struct pbe *restore_pblist;

/* Preferred image size in bytes (default 500 MB) */
extern unsigned long image_size;
extern int in_suspend;
extern dev_t swsusp_resume_device;
extern sector_t swsusp_resume_block;

extern asmlinkage int swsusp_arch_suspend(void);
extern asmlinkage int swsusp_arch_resume(void);

extern int create_basic_memory_bitmaps(void);
extern void free_basic_memory_bitmaps(void);
extern unsigned int count_data_pages(void);

/**
 *	Auxiliary structure used for reading the snapshot image data and
 *	metadata from and writing them to the list of page backup entries
 *	(PBEs) which is the main data structure of swsusp.
 *
 *	Using struct snapshot_handle we can transfer the image, including its
 *	metadata, as a continuous sequence of bytes with the help of
 *	snapshot_read_next() and snapshot_write_next().
 *
 *	The code that writes the image to a storage or transfers it to
 *	the user land is required to use snapshot_read_next() for this
 *	purpose and it should not make any assumptions regarding the internal
 *	structure of the image.  Similarly, the code that reads the image from
 *	a storage or transfers it from the user land is required to use
 *	snapshot_write_next().
 *
 *	This may allow us to change the internal structure of the image
 *	in the future with considerably less effort.
 */

struct snapshot_handle {
	loff_t		offset;	/* number of the last byte ready for reading
				 * or writing in the sequence
				 */
	unsigned int	cur;	/* number of the block of PAGE_SIZE bytes the
				 * next operation will refer to (ie. current)
				 */
	unsigned int	cur_offset;	/* offset with respect to the current
					 * block (for the next operation)
					 */
	unsigned int	prev;	/* number of the block of PAGE_SIZE bytes that
				 * was the current one previously
				 */
	void		*buffer;	/* address of the block to read from
					 * or write to
					 */
	unsigned int	buf_offset;	/* location to read from or write to,
					 * given as a displacement from 'buffer'
					 */
	int		sync_read;	/* Set to one to notify the caller of
					 * snapshot_write_next() that it may
					 * need to call wait_on_bio_chain()
					 */
};

/* This macro returns the address from/to which the caller of
 * snapshot_read_next()/snapshot_write_next() is allowed to
 * read/write data after the function returns
 */
#define data_of(handle)	((handle).buffer + (handle).buf_offset)

extern unsigned int snapshot_additional_pages(struct zone *zone);
extern unsigned long snapshot_get_image_size(void);
extern int snapshot_read_next(struct snapshot_handle *handle, size_t count);
extern int snapshot_write_next(struct snapshot_handle *handle, size_t count);
extern void snapshot_write_finalize(struct snapshot_handle *handle);
extern int snapshot_image_loaded(struct snapshot_handle *handle);

/* If unset, the snapshot device cannot be open. */
extern atomic_t snapshot_device_available;

extern sector_t alloc_swapdev_block(int swap);
extern void free_all_swap_pages(int swap);
extern int swsusp_swap_in_use(void);

/*
 * Flags that can be passed from the hibernatig hernel to the "boot" kernel in
 * the image header.
 */
#define SF_PLATFORM_MODE	1

/* kernel/power/disk.c */
extern int swsusp_check(void);
extern int swsusp_shrink_memory(void);
extern void swsusp_free(void);
extern int swsusp_read(unsigned int *flags_p);
extern int swsusp_write(unsigned int flags);
extern void swsusp_close(fmode_t);

struct timeval;
/* kernel/power/swsusp.c */
extern void swsusp_show_speed(struct timeval *, struct timeval *,
				unsigned int, char *);

#ifdef CONFIG_SUSPEND
/* kernel/power/main.c */
extern int suspend_devices_and_enter(suspend_state_t state);
#else /* !CONFIG_SUSPEND */
static inline int suspend_devices_and_enter(suspend_state_t state)
{
	return -ENOSYS;
}
#endif /* !CONFIG_SUSPEND */

#ifdef CONFIG_PM_SLEEP
/* kernel/power/main.c */
extern int pm_notifier_call_chain(unsigned long val);
#endif

#ifdef CONFIG_HIGHMEM
unsigned int count_highmem_pages(void);
int restore_highmem(void);
#else
static inline unsigned int count_highmem_pages(void) { return 0; }
static inline int restore_highmem(void) { return 0; }
#endif

/*
 * Suspend test levels
 */
enum {
	/* keep first */
	TEST_NONE,
	TEST_CORE,
	TEST_CPUS,
	TEST_PLATFORM,
	TEST_DEVICES,
	TEST_FREEZER,
	/* keep last */
	__TEST_AFTER_LAST
};

#define TEST_FIRST	TEST_NONE
#define TEST_MAX	(__TEST_AFTER_LAST - 1)

extern int pm_test_level;

#ifdef CONFIG_SUSPEND_FREEZER
static inline int suspend_freeze_processes(void)
{
	return freeze_processes();
}

static inline void suspend_thaw_processes(void)
{
	thaw_processes();
}
#else
static inline int suspend_freeze_processes(void)
{
	return 0;
}

static inline void suspend_thaw_processes(void)
{
}
#endif

extern struct page *saveable_page(struct zone *z, unsigned long p);
#ifdef CONFIG_HIGHMEM
extern struct page *saveable_highmem_page(struct zone *z, unsigned long p);
#else
static inline struct page *saveable_highmem_page(struct zone *z, unsigned long p)
{
	return NULL;
}
#endif

#define PBES_PER_PAGE (PAGE_SIZE / sizeof(struct pbe))
extern struct list_head nosave_regions;

/**
 *	This structure represents a range of page frames the contents of which
 *	should not be saved during the suspend.
 */

struct nosave_region {
	struct list_head list;
	unsigned long start_pfn;
	unsigned long end_pfn;
};

#ifndef PHYS_PFN_OFFSET
#define PHYS_PFN_OFFSET 0
#endif

#define ZONE_START(thiszone) ((thiszone)->zone_start_pfn - PHYS_PFN_OFFSET)

#define BM_END_OF_MAP	(~0UL)

#define BM_BITS_PER_BLOCK	(PAGE_SIZE << 3)

struct bm_block {
	struct list_head hook;		/* hook into a list of bitmap blocks */
	unsigned long start_pfn;	/* pfn represented by the first bit */
	unsigned long end_pfn;	/* pfn represented by the last bit plus 1 */
	unsigned long *data;	/* bitmap representing pages */
};

struct zone_bitmap {
	struct zone_bitmap *next;	/* next element of the list */
	unsigned long start_pfn;	/* minimal pfn in this zone */
	unsigned long end_pfn;		/* maximal pfn in this zone plus 1 */
	struct bm_block *bm_blocks;	/* list of bitmap blocks */
	struct bm_block *cur_block;	/* recently used bitmap block */
};

/* struct bm_position is used for browsing memory bitmaps */

struct bm_position {
	struct bm_block *block;
	int bit;
};

struct memory_bitmap {
	struct list_head blocks;	/* list of bitmap blocks */
	struct linked_page *p_list;	/* list of pages used to store zone
					 * bitmap objects and bitmap block
					 * objects
					 */
	struct bm_position cur;		/* most recently used bit position */
	struct bm_position iter;	/* most recently used bit position
					 * when iterating over a bitmap.
					 */
};


extern int memory_bm_create(struct memory_bitmap *bm, gfp_t gfp_mask,
		int safe_needed);
extern void memory_bm_free(struct memory_bitmap *bm, int clear_nosave_free);
extern void memory_bm_set_bit(struct memory_bitmap *bm, unsigned long pfn);
extern void memory_bm_clear_bit(struct memory_bitmap *bm, unsigned long pfn);
extern int memory_bm_test_bit(struct memory_bitmap *bm, unsigned long pfn);
extern unsigned long memory_bm_next_pfn(struct memory_bitmap *bm);
extern void memory_bm_position_reset(struct memory_bitmap *bm);
extern void memory_bm_clear(struct memory_bitmap *bm);
extern void memory_bm_copy(struct memory_bitmap *source,
		struct memory_bitmap *dest);
extern void memory_bm_dup(struct memory_bitmap *source,
		struct memory_bitmap *dest);

#ifdef CONFIG_TOI
struct toi_module_ops;
extern int memory_bm_read(struct memory_bitmap *bm, int (*rw_chunk)
	(int rw, struct toi_module_ops *owner, char *buffer, int buffer_size));
extern int memory_bm_write(struct memory_bitmap *bm, int (*rw_chunk)
	(int rw, struct toi_module_ops *owner, char *buffer, int buffer_size));
#endif

#endif
