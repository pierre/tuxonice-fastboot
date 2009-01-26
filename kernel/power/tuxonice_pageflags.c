/*
 * kernel/power/tuxonice_pageflags.c
 *
 * Copyright (C) 2004-2008 Nigel Cunningham (nigel at tuxonice net)
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

struct memory_bitmap pageset2_map;
struct memory_bitmap page_resave_map;
struct memory_bitmap io_map;
struct memory_bitmap nosave_map;
struct memory_bitmap free_map;

int toi_pageflags_space_needed(void)
{
	int total = 0;
	struct bm_block *bb;

	total = sizeof(unsigned int);

	list_for_each_entry(bb, &pageset1_map.blocks, hook)
		total += 2 * sizeof(unsigned long) + PAGE_SIZE;

	return total;
}
