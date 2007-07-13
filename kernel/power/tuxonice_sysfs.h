/*
 * kernel/power/tuxonice_sysfs.h
 *
 * Copyright (C) 2004-2007 Nigel Cunningham (nigel at suspend2 net)
 *
 * This file is released under the GPLv2.
 *
 * It provides declarations for suspend to use in managing
 * /sysfs/suspend2. When we switch to kobjects,
 * this will become redundant.
 *
 */

#include <linux/sysfs.h>
#include "power.h"

struct suspend_sysfs_data {
	struct attribute attr;
	int type;
	int flags;
	union {
		struct {
			unsigned long *bit_vector;
			int bit;
		} bit;
		struct {
			int *variable;
			int minimum;
			int maximum;
		} integer;
		struct {
			long *variable;
			long minimum;
			long maximum;
		} a_long;
		struct {
			unsigned long *variable;
			unsigned long minimum;
			unsigned long maximum;
		} ul;
		struct {
			char *variable;
			int max_length;
		} string;
		struct {
			int (*read_sysfs) (const char *buffer, int count);
			int (*write_sysfs) (const char *buffer, int count);
			void *data;
		} special;
	} data;
	
	/* Side effects routines. Used, eg, for reparsing the
	 * resume= entry when it changes */
	void (*read_side_effect) (void);
	void (*write_side_effect) (void); 
	struct list_head sysfs_data_list;
};

enum {
	SUSPEND_SYSFS_DATA_NONE = 1,
	SUSPEND_SYSFS_DATA_CUSTOM,
	SUSPEND_SYSFS_DATA_BIT,
	SUSPEND_SYSFS_DATA_INTEGER,
	SUSPEND_SYSFS_DATA_UL,
	SUSPEND_SYSFS_DATA_LONG,
	SUSPEND_SYSFS_DATA_STRING
};

#define SUSPEND2_ATTR(_name, _mode)      \
        .attr = {.name  = _name , .mode   = _mode }

#define SYSFS_BIT(_ul, _bit, _flags) \
	.type = SUSPEND_SYSFS_DATA_BIT, \
	.flags = _flags, \
	.data = { .bit = { .bit_vector = _ul, .bit = _bit } }

#define SYSFS_INT(_int, _min, _max, _flags) \
	.type = SUSPEND_SYSFS_DATA_INTEGER, \
	.flags = _flags, \
	.data = { .integer = { .variable = _int, .minimum = _min, \
			.maximum = _max } }

#define SYSFS_UL(_ul, _min, _max, _flags) \
	.type = SUSPEND_SYSFS_DATA_UL, \
	.flags = _flags, \
	.data = { .ul = { .variable = _ul, .minimum = _min, \
			.maximum = _max } }

#define SYSFS_LONG(_long, _min, _max, _flags) \
	.type = SUSPEND_SYSFS_DATA_LONG, \
	.flags = _flags, \
	.data = { .a_long = { .variable = _long, .minimum = _min, \
			.maximum = _max } }

#define SYSFS_STRING(_string, _max_len, _flags) \
	.type = SUSPEND_SYSFS_DATA_STRING, \
	.flags = _flags, \
	.data = { .string = { .variable = _string, .max_length = _max_len } }

#define SYSFS_CUSTOM(_read, _write, _flags) \
	.type = SUSPEND_SYSFS_DATA_CUSTOM, \
	.flags = _flags, \
	.data = { .special = { .read_sysfs = _read, .write_sysfs = _write } }

#define SYSFS_WRITEONLY 0200
#define SYSFS_READONLY 0444
#define SYSFS_RW 0644

/* Flags */
#define SYSFS_NEEDS_SM_FOR_READ 1
#define SYSFS_NEEDS_SM_FOR_WRITE 2
#define SYSFS_SUSPEND 4
#define SYSFS_RESUME 8
#define SYSFS_SUSPEND_OR_RESUME (SYSFS_SUSPEND | SYSFS_RESUME)
#define SYSFS_SUSPENDING (SYSFS_SUSPEND | SYSFS_NEEDS_SM_FOR_WRITE)
#define SYSFS_RESUMING (SYSFS_RESUME | SYSFS_NEEDS_SM_FOR_WRITE)
#define SYSFS_NEEDS_SM_FOR_BOTH \
 (SYSFS_NEEDS_SM_FOR_READ | SYSFS_NEEDS_SM_FOR_WRITE)

int suspend_register_sysfs_file(struct kobject *kobj,
		struct suspend_sysfs_data *suspend_sysfs_data);
void suspend_unregister_sysfs_file(struct kobject *kobj,
		struct suspend_sysfs_data *suspend_sysfs_data);

extern struct kset suspend2_subsys;

struct kobject *make_suspend2_sysdir(char *name);
void remove_suspend2_sysdir(struct kobject *obj);
extern void suspend_cleanup_sysfs(void);

extern int s2_sysfs_init(void);
extern void s2_sysfs_exit(void);
