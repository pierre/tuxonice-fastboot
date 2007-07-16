/*
 * Copyright (C) 2004-2007 Nigel Cunningham (nigel at suspend2 net)
 *
 * This file is released under the GPLv2.
 */
#include <linux/dyn_pageflags.h>
#include <asm/setup.h>

extern struct toi_core_fns *toi_core_fns;
extern unsigned long toi_compress_bytes_in, toi_compress_bytes_out;
extern unsigned long toi_action;
extern unsigned int nr_hibernates;
extern int toi_in_hibernate;

extern unsigned long toi_nosave_state1 __nosavedata;
extern unsigned long toi_nosave_state2 __nosavedata;
extern int toi_nosave_state3 __nosavedata;
extern int toi_nosave_io_speed[2][2] __nosavedata;
extern __nosavedata char toi_nosave_commandline[COMMAND_LINE_SIZE];
extern __nosavedata struct pbe *restore_highmem_pblist;

int toi_lowlevel_builtin(void);

extern dyn_pageflags_t __nosavedata toi_nosave_origmap;
extern dyn_pageflags_t __nosavedata toi_nosave_copymap;

#ifdef CONFIG_HIGHMEM
extern __nosavedata struct zone_data *toi_nosave_zone_list;
extern __nosavedata unsigned long toi_nosave_max_pfn;
#endif

extern unsigned long toi_get_nonconflicting_page(void);
extern int toi_post_context_save(void);
extern int toi_try_hibernate(int have_pmsem);
