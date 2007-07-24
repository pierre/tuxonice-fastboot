/*
 * kernel/power/tuxonice_pageflags.c
 *
 * Copyright (C) 2004-2007 Nigel Cunningham (nigel at suspend2 net)
 * 
 * This file is released under the GPLv2.
 *
 * Routines for serialising and relocating pageflags in which we
 * store our image metadata.
 */

#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/bitops.h>
#include <linux/list.h>
#include <linux/suspend.h>
#include "tuxonice_pageflags.h"
#include "tuxonice_modules.h"
#include "tuxonice_pagedir.h"
#include "tuxonice.h"

struct dyn_pageflags pageset2_map, page_resave_map, io_map, nosave_map,
		     free_map;

static int pages_for_zone(struct zone *zone)
{
	return DIV_ROUND_UP(zone->spanned_pages, (PAGE_SIZE << 3));
}

int toi_pageflags_space_needed(void)
{
	int total = 0;
	struct zone *zone;

	for_each_zone(zone)
		if (populated_zone(zone))
			total += sizeof(int) * 3 + pages_for_zone(zone) * PAGE_SIZE;

	total += sizeof(int);

	return total;
}

/* save_dyn_pageflags
 *
 * Description: Save a set of pageflags.
 * Arguments:   struct dyn_pageflags *: Pointer to the bitmap being saved.
 */

void save_dyn_pageflags(struct dyn_pageflags *pagemap)
{
	int i, zone_idx, size, node = 0;
	struct zone *zone;
	struct pglist_data *pgdat;

	if (!pagemap)
		return;

	for_each_online_pgdat(pgdat) {
		for (zone_idx = 0; zone_idx < MAX_NR_ZONES; zone_idx++) {
			zone = &pgdat->node_zones[zone_idx];

			if (!populated_zone(zone))
				continue;

			toiActiveAllocator->rw_header_chunk(WRITE, NULL,
					(char *) &node, sizeof(int));
			toiActiveAllocator->rw_header_chunk(WRITE, NULL,
					(char *) &zone_idx, sizeof(int));
			size = pages_for_zone(zone);
			toiActiveAllocator->rw_header_chunk(WRITE, NULL,
					(char *) &size, sizeof(int));

			for (i = 0; i < size; i++) {
				if (!pagemap->bitmap[node][zone_idx][i+2]) {
					printk("Sparse pagemap?\n");
					dump_pagemap(pagemap);
					BUG();
				}
				toiActiveAllocator->rw_header_chunk(WRITE,
					NULL, (char *) pagemap->bitmap[node][zone_idx][i+2],
					PAGE_SIZE);
			}
		}
		node++;
	}
	node = -1;
	toiActiveAllocator->rw_header_chunk(WRITE, NULL,
			(char *) &node, sizeof(int));
}

/* load_dyn_pageflags
 *
 * Description: Load a set of pageflags.
 * Arguments:   struct dyn_pageflags *: Pointer to the bitmap being loaded.
 *              (It must be allocated before calling this routine).
 */

int load_dyn_pageflags(struct dyn_pageflags *pagemap)
{
	int i, zone_idx, zone_check = 0, size, node = 0;
	struct zone *zone;
	struct pglist_data *pgdat;

	if (!pagemap)
		return 1;

	for_each_online_pgdat(pgdat) {
		for (zone_idx = 0; zone_idx < MAX_NR_ZONES; zone_idx++) {
			zone = &pgdat->node_zones[zone_idx];

			if (!populated_zone(zone))
				continue;

			/* Same node? */
			toiActiveAllocator->rw_header_chunk(READ, NULL,
					(char *) &zone_check, sizeof(int));
			if (zone_check != node) {
				printk("Node read (%d) != node (%d).\n",
						zone_check, node);
				return 1;
			}

			/* Same zone? */
			toiActiveAllocator->rw_header_chunk(READ, NULL,
					(char *) &zone_check, sizeof(int));
			if (zone_check != zone_idx) {
				printk("Zone read (%d) != node (%d).\n",
						zone_check, zone_idx);
				return 1;
			}


			toiActiveAllocator->rw_header_chunk(READ, NULL,
				(char *) &size, sizeof(int));

			for (i = 0; i < size; i++)
				toiActiveAllocator->rw_header_chunk(READ, NULL,
					(char *) pagemap->bitmap[node][zone_idx][i+2],
					PAGE_SIZE);
		}
		node++;
	}
	toiActiveAllocator->rw_header_chunk(READ, NULL, (char *) &zone_check,
			sizeof(int));
	if (zone_check != -1) {
		printk("Didn't read end of dyn pageflag data marker.(%x)\n",
				zone_check);
		return 1;
	}

	return 0;
}
