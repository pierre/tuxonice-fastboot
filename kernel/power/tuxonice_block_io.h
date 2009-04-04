/*
 * kernel/power/tuxonice_block_io.h
 *
 * Copyright (C) 2004-2008 Nigel Cunningham (nigel at tuxonice net)
 * Copyright (C) 2006 Red Hat, inc.
 *
 * Distributed under GPLv2.
 *
 * This file contains declarations for functions exported from
 * tuxonice_block_io.c, which contains low level io functions.
 */

#include <linux/buffer_head.h>
#include "tuxonice_extent.h"

struct toi_bdev_info {
	struct block_device *bdev;
	dev_t dev_t;
	int bmap_shift;
	int blocks_per_page;
	int ignored;
};

/*
 * Our exported interface so the swapwriter and filewriter don't
 * need these functions duplicated.
 */
struct toi_bio_ops {
	int (*bdev_page_io) (int rw, struct block_device *bdev, long pos,
			struct page *page);
	void (*check_io_stats) (void);
	void (*reset_io_stats) (void);
	void (*update_throughput_throttle) (int jif_index);
	int (*finish_all_io) (void);
	int (*forward_one_page) (int writing, int section_barrier);
	void (*set_extra_page_forward) (void);
	void (*set_devinfo) (struct toi_bdev_info *info);
	int (*read_page) (unsigned long *index, struct page *buffer_page,
			unsigned int *buf_size);
	int (*write_page) (unsigned long index, struct page *buffer_page,
			unsigned int buf_size);
	void (*read_header_init) (void);
	int (*rw_header_chunk) (int rw, struct toi_module_ops *owner,
			char *buffer, int buffer_size);
	int (*rw_header_chunk_noreadahead) (int rw,
			struct toi_module_ops *owner,
			char *buffer, int buffer_size);
	int (*write_header_chunk_finish) (void);
	int (*rw_init) (int rw, int stream_number);
	int (*rw_cleanup) (int rw);
	int (*io_flusher) (int rw);
};

extern struct toi_bio_ops toi_bio_ops;

extern char *toi_writer_buffer;
extern int toi_writer_buffer_posn;
extern struct hibernate_extent_iterate_saved_state toi_writer_posn_save[4];
extern struct toi_extent_iterate_state toi_writer_posn;
