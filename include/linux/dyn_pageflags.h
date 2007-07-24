/*
 * include/linux/dyn_pageflags.h
 *
 * Copyright (C) 2004-2007 Nigel Cunningham <nigel@suspend2.net>
 *
 * This file is released under the GPLv2.
 *
 * It implements support for dynamically allocated bitmaps that are
 * used for temporary or infrequently used pageflags, in lieu of
 * bits in the struct page flags entry.
 */

#ifndef DYN_PAGEFLAGS_H
#define DYN_PAGEFLAGS_H

#include <linux/mm.h>

struct dyn_pageflags {
	unsigned long ****bitmap; /* [pg_dat][zone][page_num] */
	int sparse, initialised;
	struct list_head list;
	spinlock_t struct_lock;
};

#define DYN_PAGEFLAGS_INIT(name) { \
	.list = LIST_HEAD_INIT(name.list), \
	.struct_lock = __SPIN_LOCK_UNLOCKED(name.lock) \
}

#define DECLARE_DYN_PAGEFLAGS(name) \
	struct dyn_pageflags name = DYN_PAGEFLAGS_INIT(name);

#define BITMAP_FOR_EACH_SET(bitmap, counter) \
	for (counter = get_next_bit_on(bitmap, max_pfn + 1); counter <= max_pfn; \
		counter = get_next_bit_on(bitmap, counter))

extern void clear_dyn_pageflags(struct dyn_pageflags *pagemap);
extern int allocate_dyn_pageflags(struct dyn_pageflags *pagemap, int sparse);
extern void free_dyn_pageflags(struct dyn_pageflags *pagemap);
extern unsigned long get_next_bit_on(struct dyn_pageflags *bitmap, unsigned long counter);

extern int test_dynpageflag(struct dyn_pageflags *bitmap, struct page *page);
/*
 * In sparse bitmaps, setting a flag can fail (we can fail to allocate
 * the page to store the bit. If this happens, we will BUG(). If you don't
 * want this behaviour, don't allocate sparse pageflags.
 */
extern void set_dynpageflag(struct dyn_pageflags *bitmap, struct page *page);
extern void clear_dynpageflag(struct dyn_pageflags *bitmap, struct page *page);
extern void dump_pagemap(struct dyn_pageflags *pagemap);

/* 
 * With the above macros defined, you can do...
 * #define PagePageset1(page) (test_dynpageflag(&pageset1_map, page))
 * #define SetPagePageset1(page) (set_dynpageflag(&pageset1_map, page))
 * #define ClearPagePageset1(page) (clear_dynpageflag(&pageset1_map, page))
 */

extern void __init dyn_pageflags_init(void);
extern void __init dyn_pageflags_use_kzalloc(void);

#ifdef CONFIG_MEMORY_HOTPLUG_SPARSE
extern void dyn_pageflags_hotplug(struct zone *zone);
#endif
#endif
