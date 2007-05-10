/*
 * include/linux/dyn_pageflags.h
 *
 * Copyright (C) 2004-2006 Nigel Cunningham <nigel@suspend2.net>
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

/* [pg_dat][zone][page_num] */
typedef unsigned long **** dyn_pageflags_t;

#if BITS_PER_LONG == 32
#define UL_SHIFT 5
#else 
#if BITS_PER_LONG == 64
#define UL_SHIFT 6
#else
#error Bits per long not 32 or 64?
#endif
#endif

#define BIT_NUM_MASK (sizeof(unsigned long) * 8 - 1)
#define PAGE_NUM_MASK (~((1 << (PAGE_SHIFT + 3)) - 1))
#define UL_NUM_MASK (~(BIT_NUM_MASK | PAGE_NUM_MASK))

/*
 * PAGENUMBER gives the index of the page within the zone.
 * PAGEINDEX gives the index of the unsigned long within that page.
 * PAGEBIT gives the index of the bit within the unsigned long.
 */
#define BITS_PER_PAGE (PAGE_SIZE << 3)
#define PAGENUMBER(zone_offset) ((int) (zone_offset >> (PAGE_SHIFT + 3)))
#define PAGEINDEX(zone_offset) ((int) ((zone_offset & UL_NUM_MASK) >> UL_SHIFT))
#define PAGEBIT(zone_offset) ((int) (zone_offset & BIT_NUM_MASK))

#define PAGE_UL_PTR(bitmap, node, zone_num, zone_pfn) \
       ((bitmap[node][zone_num][PAGENUMBER(zone_pfn)])+PAGEINDEX(zone_pfn))

#define BITMAP_FOR_EACH_SET(bitmap, counter) \
	for (counter = get_next_bit_on(bitmap, max_pfn + 1); counter <= max_pfn; \
		counter = get_next_bit_on(bitmap, counter))

extern void clear_dyn_pageflags(dyn_pageflags_t pagemap);
extern int allocate_dyn_pageflags(dyn_pageflags_t *pagemap);
extern void free_dyn_pageflags(dyn_pageflags_t *pagemap);
extern unsigned long get_next_bit_on(dyn_pageflags_t bitmap, unsigned long counter);

extern int test_dynpageflag(dyn_pageflags_t *bitmap, struct page *page);
extern void set_dynpageflag(dyn_pageflags_t *bitmap, struct page *page);
extern void clear_dynpageflag(dyn_pageflags_t *bitmap, struct page *page);
#endif

/* 
 * With the above macros defined, you can do...
 * #define PagePageset1(page) (test_dynpageflag(&pageset1_map, page))
 * #define SetPagePageset1(page) (set_dynpageflag(&pageset1_map, page))
 * #define ClearPagePageset1(page) (clear_dynpageflag(&pageset1_map, page))
 */

