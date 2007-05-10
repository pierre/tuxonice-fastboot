/*
 * lib/dyn_pageflags.c
 *
 * Copyright (C) 2004-2006 Nigel Cunningham <nigel@suspend2.net>
 * 
 * This file is released under the GPLv2.
 *
 * Routines for dynamically allocating and releasing bitmaps
 * used as pseudo-pageflags.
 */

#include <linux/module.h>
#include <linux/dyn_pageflags.h>
#include <linux/bootmem.h>
#include <linux/mm.h>

#if 0
#define PR_DEBUG(a, b...) do { printk(a, ##b); } while(0)
#else
#define PR_DEBUG(a, b...) do { } while(0)
#endif

#define pages_for_zone(zone) \
	(DIV_ROUND_UP((zone)->spanned_pages, (PAGE_SIZE << 3)))

/* 
 * clear_dyn_pageflags(dyn_pageflags_t pagemap)
 *
 * Clear an array used to store local page flags.
 *
 */

void clear_dyn_pageflags(dyn_pageflags_t pagemap)
{
	int i = 0, zone_idx, node_id = 0;
	struct zone *zone;
	struct pglist_data *pgdat;
	
	BUG_ON(!pagemap);

	for_each_online_pgdat(pgdat) {
		for (zone_idx = 0; zone_idx < MAX_NR_ZONES; zone_idx++) {
			zone = &pgdat->node_zones[zone_idx];

			if (!populated_zone(zone))
				continue;

			for (i = 0; i < pages_for_zone(zone); i++)
				memset((pagemap[node_id][zone_idx][i]), 0,
						PAGE_SIZE);
		}
		node_id++;
	}
}

/* 
 * free_dyn_pageflags(dyn_pageflags_t pagemap)
 *
 * Free a dynamically allocated pageflags bitmap. For Suspend2 usage, we
 * support data being relocated from slab to pages that don't conflict
 * with the image that will be copied back. This is the reason for the
 * PageSlab tests below.
 *
 */
void free_dyn_pageflags(dyn_pageflags_t *pagemap)
{
	int i = 0, zone_pages, node_id = -1, zone_idx;
	struct zone *zone;
	struct pglist_data *pgdat;

	if (!*pagemap)
		return;
	
	PR_DEBUG("Seeking to free dyn_pageflags %p.\n", pagemap);

	for_each_online_pgdat(pgdat) {
		node_id++;

		PR_DEBUG("Node id %d.\n", node_id);

		if (!(*pagemap)[node_id]) {
			PR_DEBUG("Node %d unallocated.\n", node_id);
			continue;
		}
		
		for (zone_idx = 0; zone_idx < MAX_NR_ZONES; zone_idx++) {
			zone = &pgdat->node_zones[zone_idx];
			if (!populated_zone(zone)) {
				PR_DEBUG("Node %d zone %d unpopulated.\n", node_id, zone_idx);
				continue;
			}

			if (!(*pagemap)[node_id][zone_idx]) {
				PR_DEBUG("Node %d zone %d unallocated.\n", node_id, zone_idx);
				continue;
			}

			PR_DEBUG("Node id %d. Zone %d.\n", node_id, zone_idx);

			zone_pages = pages_for_zone(zone);

			for (i = 0; i < zone_pages; i++) {
				PR_DEBUG("Node id %d. Zone %d. Page %d.\n", node_id, zone_idx, i);
				free_page((unsigned long)(*pagemap)[node_id][zone_idx][i]);
			}

			kfree((*pagemap)[node_id][zone_idx]);
		}
		PR_DEBUG("Free node %d (%p).\n", node_id, pagemap[node_id]);
		kfree((*pagemap)[node_id]);
	}

	PR_DEBUG("Free map pgdat list at %p.\n", pagemap);
	kfree(*pagemap);

	*pagemap = NULL;
	PR_DEBUG("Done.\n");
	return;
}

static int try_alloc_dyn_pageflag_part(int nr_ptrs, void **ptr)
{
	*ptr = kzalloc(sizeof(void *) * nr_ptrs, GFP_ATOMIC);
	PR_DEBUG("Got %p. Putting it in %p.\n", *ptr, ptr);

	if (*ptr)
		return 0;

	printk("Error. Unable to allocate memory for dynamic pageflags.");
	return -ENOMEM;
}

/* 
 * allocate_dyn_pageflags
 *
 * Allocate a bitmap for dynamic page flags.
 *
 */
int allocate_dyn_pageflags(dyn_pageflags_t *pagemap)
{
	int i, zone_idx, zone_pages, node_id = 0;
	struct zone *zone;
	struct pglist_data *pgdat;

	if (*pagemap) {
		PR_DEBUG("Pagemap %p already allocated.\n", pagemap);
		return 0;
	}

	PR_DEBUG("Seeking to allocate dyn_pageflags %p.\n", pagemap);

	for_each_online_pgdat(pgdat)
		node_id++;

	if (try_alloc_dyn_pageflag_part(node_id, (void **) pagemap))
		return -ENOMEM;

	node_id = 0;

	for_each_online_pgdat(pgdat) {
		PR_DEBUG("Node %d.\n", node_id);

		if (try_alloc_dyn_pageflag_part(MAX_NR_ZONES,
					(void **) &(*pagemap)[node_id]))
				return -ENOMEM;

		for (zone_idx = 0; zone_idx < MAX_NR_ZONES; zone_idx++) {
			PR_DEBUG("Zone %d of %d.\n", zone_idx, MAX_NR_ZONES);

			zone = &pgdat->node_zones[zone_idx];

			if (!populated_zone(zone)) {
				PR_DEBUG("Node %d zone %d unpopulated - won't allocate.\n", node_id, zone_idx);
				continue;
			}

			zone_pages = pages_for_zone(zone);

			PR_DEBUG("Node %d zone %d (needs %d pages).\n", node_id, zone_idx, zone_pages);

			if (try_alloc_dyn_pageflag_part(zone_pages,
				(void **) &(*pagemap)[node_id][zone_idx]))
				return -ENOMEM;

			for (i = 0; i < zone_pages; i++) {
				unsigned long address = get_zeroed_page(GFP_ATOMIC);
				if (!address) {
					PR_DEBUG("Error. Unable to allocate memory for "
						"dynamic pageflags.");
					free_dyn_pageflags(pagemap);
					return -ENOMEM;
				}
				PR_DEBUG("Node %d zone %d. Page %d.\n", node_id, zone_idx, i);
				(*pagemap)[node_id][zone_idx][i] =
					(unsigned long *) address;
			}
		}
		node_id++;
	}

	PR_DEBUG("Done.\n");
	return 0;
}

#define GET_BIT_AND_UL(bitmap, page) \
	struct zone *zone = page_zone(page); \
	unsigned long zone_pfn = page_to_pfn(page) - zone->zone_start_pfn; \
	int node = page_to_nid(page); \
	int zone_num = zone_idx(zone); \
	int pagenum = PAGENUMBER(zone_pfn); \
	int page_offset = PAGEINDEX(zone_pfn); \
	unsigned long *ul = ((*bitmap)[node][zone_num][pagenum]) + page_offset; \
	int bit = PAGEBIT(zone_pfn);

/*
 * test_dynpageflag(dyn_pageflags_t *bitmap, struct page *page)
 *
 * Is the page flagged in the given bitmap?
 *
 */

int test_dynpageflag(dyn_pageflags_t *bitmap, struct page *page)
{
	GET_BIT_AND_UL(bitmap, page);
	return test_bit(bit, ul);
}

/*
 * set_dynpageflag(dyn_pageflags_t *bitmap, struct page *page)
 *
 * Set the flag for the page in the given bitmap.
 *
 */

void set_dynpageflag(dyn_pageflags_t *bitmap, struct page *page)
{
	GET_BIT_AND_UL(bitmap, page);
	set_bit(bit, ul);
}

/*
 * clear_dynpageflags(dyn_pageflags_t *bitmap, struct page *page)
 *
 * Clear the flag for the page in the given bitmap.
 *
 */

void clear_dynpageflag(dyn_pageflags_t *bitmap, struct page *page)
{
	GET_BIT_AND_UL(bitmap, page);
	clear_bit(bit, ul);
}

/*
 * get_next_bit_on(dyn_pageflags_t bitmap, int counter)
 *
 * Given a pfn (possibly -1), find the next pfn in the bitmap that
 * is set. If there are no more flags set, return -1.
 *
 */

unsigned long get_next_bit_on(dyn_pageflags_t bitmap, unsigned long counter)
{
	struct page *page;
	struct zone *zone;
	unsigned long *ul = NULL;
	unsigned long zone_offset;
	int pagebit, zone_num, first = (counter == (max_pfn + 1)), node;

	if (first)
		counter = first_online_pgdat()->node_zones->zone_start_pfn;

	page = pfn_to_page(counter);
	zone = page_zone(page);
	node = zone->zone_pgdat->node_id;
	zone_num = zone_idx(zone);
	zone_offset = counter - zone->zone_start_pfn;

	if (first)
		goto test;

	do {
		zone_offset++;
	
		if (zone_offset >= zone->spanned_pages) {
			do {
				zone = next_zone(zone);
				if (!zone)
					return max_pfn + 1;
			} while(!zone->spanned_pages);
			
			zone_num = zone_idx(zone);
			node = zone->zone_pgdat->node_id;
			zone_offset = 0;
		}
test:
		pagebit = PAGEBIT(zone_offset);

		if (!pagebit || !ul)
			ul = (bitmap[node][zone_num][PAGENUMBER(zone_offset)])
				+ PAGEINDEX(zone_offset);

		if (!(*ul & ~((1 << pagebit) - 1))) {
			zone_offset += BITS_PER_LONG - pagebit - 1;
			continue;
		}

	} while(!test_bit(pagebit, ul));

	return zone->zone_start_pfn + zone_offset;
}

