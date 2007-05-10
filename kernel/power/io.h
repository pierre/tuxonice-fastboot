/*
 * kernel/power/io.h
 *
 * Copyright (C) 2005-2007 Nigel Cunningham (nigel at suspend2 net)
 *
 * This file is released under the GPLv2.
 *
 * It contains high level IO routines for suspending.
 *
 */

#include <linux/utsname.h>
#include "pagedir.h"

/* Non-module data saved in our image header */
struct suspend_header {
	u32 version_code;
	unsigned long num_physpages;
	unsigned long orig_mem_free;
	struct new_utsname uts;
	int num_cpus;
	int page_size;
	int pageset_2_size;
	int param0;
	int param1;
	int param2;
	int param3;
	int progress0;
	int progress1;
	int progress2;
	int progress3;
	int io_time[2][2];
	struct pagedir pagedir;
	dev_t root_fs;
};

extern int write_pageset(struct pagedir *pagedir);
extern int write_image_header(void);
extern int read_pageset1(void);
extern int read_pageset2(int overwrittenpagesonly);

extern int suspend_attempt_to_parse_resume_device(int quiet);
extern void attempt_to_parse_resume_device2(void);
extern void attempt_to_parse_po_resume_device2(void);
int image_exists_read(const char *page, int count);
int image_exists_write(const char *buffer, int count);
extern void replace_restore_resume2(int replace, int quiet);

extern dev_t name_to_dev_t(char *line);
