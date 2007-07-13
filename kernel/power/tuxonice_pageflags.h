/*
 * kernel/power/tuxonice_pageflags.h
 *
 * Copyright (C) 2004-2007 Nigel Cunningham (nigel at suspend2 net)
 *
 * This file is released under the GPLv2.
 *
 * Suspend2 needs a few pageflags while working that aren't otherwise
 * used. To save the struct page pageflags, we dynamically allocate
 * a bitmap and use that. These are the only non order-0 allocations
 * we do.
 *
 * NOTE!!!
 * We assume that PAGE_SIZE - sizeof(void *) is a multiple of
 * sizeof(unsigned long). Is this ever false?
 */

#include <linux/dyn_pageflags.h>
#include <linux/suspend.h>

extern dyn_pageflags_t pageset1_map;
extern dyn_pageflags_t pageset1_copy_map;
extern dyn_pageflags_t pageset2_map;
extern dyn_pageflags_t page_resave_map;
extern dyn_pageflags_t io_map;
extern dyn_pageflags_t nosave_map;
extern dyn_pageflags_t free_map;

#define PagePageset1(page) (test_dynpageflag(&pageset1_map, page))
#define SetPagePageset1(page) (set_dynpageflag(&pageset1_map, page))
#define ClearPagePageset1(page) (clear_dynpageflag(&pageset1_map, page))

#define PagePageset1Copy(page) (test_dynpageflag(&pageset1_copy_map, page))
#define SetPagePageset1Copy(page) (set_dynpageflag(&pageset1_copy_map, page))
#define ClearPagePageset1Copy(page) (clear_dynpageflag(&pageset1_copy_map, page))

#define PagePageset2(page) (test_dynpageflag(&pageset2_map, page))
#define SetPagePageset2(page) (set_dynpageflag(&pageset2_map, page))
#define ClearPagePageset2(page) (clear_dynpageflag(&pageset2_map, page))

#define PageWasRW(page) (test_dynpageflag(&pageset2_map, page))
#define SetPageWasRW(page) (set_dynpageflag(&pageset2_map, page))
#define ClearPageWasRW(page) (clear_dynpageflag(&pageset2_map, page))

#define PageResave(page) (page_resave_map ? test_dynpageflag(&page_resave_map, page) : 0)
#define SetPageResave(page) (set_dynpageflag(&page_resave_map, page))
#define ClearPageResave(page) (clear_dynpageflag(&page_resave_map, page))

#define PageNosave(page) (nosave_map ? test_dynpageflag(&nosave_map, page) : 0)
#define SetPageNosave(page) (set_dynpageflag(&nosave_map, page))
#define ClearPageNosave(page) (clear_dynpageflag(&nosave_map, page))

#define PageNosaveFree(page) (free_map ? test_dynpageflag(&free_map, page) : 0)
#define SetPageNosaveFree(page) (set_dynpageflag(&free_map, page))
#define ClearPageNosaveFree(page) (clear_dynpageflag(&free_map, page))

extern void save_dyn_pageflags(dyn_pageflags_t pagemap);
extern int load_dyn_pageflags(dyn_pageflags_t pagemap);
extern int suspend_pageflags_space_needed(void);
