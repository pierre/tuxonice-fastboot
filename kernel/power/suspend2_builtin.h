/*
 * Copyright (C) 2004-2007 Nigel Cunningham (nigel at suspend2 net)
 *
 * This file is released under the GPLv2.
 */
#include <linux/dyn_pageflags.h>
#include <asm/setup.h>

extern struct suspend2_core_fns *s2_core_fns;
extern unsigned long suspend_compress_bytes_in, suspend_compress_bytes_out;
extern unsigned long suspend_action;
extern unsigned int nr_suspends;
extern int suspend2_in_suspend;

extern unsigned long suspend2_nosave_state1 __nosavedata;
extern unsigned long suspend2_nosave_state2 __nosavedata;
extern int suspend2_nosave_state3 __nosavedata;
extern int suspend2_nosave_io_speed[2][2] __nosavedata;
extern __nosavedata char suspend2_nosave_commandline[COMMAND_LINE_SIZE];
extern __nosavedata struct pbe *restore_highmem_pblist;

int suspend2_lowlevel_builtin(void);

extern dyn_pageflags_t __nosavedata suspend2_nosave_origmap;
extern dyn_pageflags_t __nosavedata suspend2_nosave_copymap;

#ifdef CONFIG_HIGHMEM
extern __nosavedata struct zone_data *suspend2_nosave_zone_list;
extern __nosavedata unsigned long suspend2_nosave_max_pfn;
#endif

extern unsigned long suspend_get_nonconflicting_page(void);
extern int suspend_post_context_save(void);
extern int suspend2_try_suspend(int have_pmsem);
