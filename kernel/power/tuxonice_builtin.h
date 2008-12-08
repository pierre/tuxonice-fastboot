/*
 * Copyright (C) 2004-2008 Nigel Cunningham (nigel at tuxonice net)
 *
 * This file is released under the GPLv2.
 */
#include <linux/dyn_pageflags.h>
#include <asm/setup.h>

extern struct toi_core_fns *toi_core_fns;
extern unsigned long toi_compress_bytes_in, toi_compress_bytes_out;
extern unsigned int nr_hibernates;
extern int toi_in_hibernate;

extern __nosavedata struct pbe *restore_highmem_pblist;

int toi_lowlevel_builtin(void);

extern struct dyn_pageflags __nosavedata toi_nosave_origmap;
extern struct dyn_pageflags __nosavedata toi_nosave_copymap;

#ifdef CONFIG_HIGHMEM
extern __nosavedata struct zone_data *toi_nosave_zone_list;
extern __nosavedata unsigned long toi_nosave_max_pfn;
#endif

extern unsigned long toi_get_nonconflicting_page(void);
extern int toi_post_context_save(void);
extern int toi_try_hibernate(void);
extern char toi_wait_for_keypress_dev_console(int timeout);
extern struct block_device *toi_open_by_devnum(dev_t dev, fmode_t mode);
extern int toi_wait;
