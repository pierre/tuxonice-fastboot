/*
 * kernel/power/tuxonice_sysfs.c
 *
 * Copyright (C) 2002-2007 Nigel Cunningham (nigel at suspend2 net)
 *
 * This file is released under the GPLv2.
 *
 * This file contains support for sysfs entries for tuning Suspend2.
 *
 * We have a generic handler that deals with the most common cases, and
 * hooks for special handlers to use.
 */

#include <linux/suspend.h>
#include <linux/module.h>
#include <asm/uaccess.h>

#include "tuxonice_sysfs.h"
#include "tuxonice.h"
#include "tuxonice_storage.h"

static int suspend_sysfs_initialised = 0;

static void suspend_initialise_sysfs(void);

static struct suspend_sysfs_data sysfs_params[];

#define to_sysfs_data(_attr) container_of(_attr, struct suspend_sysfs_data, attr)

static void suspend2_main_wrapper(void)
{
	_suspend2_try_suspend(0);
}

static ssize_t suspend2_attr_show(struct kobject *kobj, struct attribute *attr,
			      char *page)
{
	struct suspend_sysfs_data *sysfs_data = to_sysfs_data(attr);
	int len = 0;

	if (suspend_start_anything(0))
		return -EBUSY;

	if (sysfs_data->flags & SYSFS_NEEDS_SM_FOR_READ)
		suspend_prepare_usm();
	
	switch (sysfs_data->type) {
		case SUSPEND_SYSFS_DATA_CUSTOM:
			len = (sysfs_data->data.special.read_sysfs) ?
				(sysfs_data->data.special.read_sysfs)(page, PAGE_SIZE)
				: 0;
			break;
		case SUSPEND_SYSFS_DATA_BIT:
			len = sprintf(page, "%d\n", 
				-test_bit(sysfs_data->data.bit.bit,
					sysfs_data->data.bit.bit_vector));
			break;
		case SUSPEND_SYSFS_DATA_INTEGER:
			len = sprintf(page, "%d\n",
				*(sysfs_data->data.integer.variable));
			break;
		case SUSPEND_SYSFS_DATA_LONG:
			len = sprintf(page, "%ld\n",
				*(sysfs_data->data.a_long.variable));
			break;
		case SUSPEND_SYSFS_DATA_UL:
			len = sprintf(page, "%lu\n",
				*(sysfs_data->data.ul.variable));
			break;
		case SUSPEND_SYSFS_DATA_STRING:
			len = sprintf(page, "%s\n",
				sysfs_data->data.string.variable);
			break;
	}
	/* Side effect routine? */
	if (sysfs_data->read_side_effect)
		sysfs_data->read_side_effect();

	if (sysfs_data->flags & SYSFS_NEEDS_SM_FOR_READ)
		suspend_cleanup_usm();

	suspend_finish_anything(0);

	return len;
}

#define BOUND(_variable, _type) \
	if (*_variable < sysfs_data->data._type.minimum) \
		*_variable = sysfs_data->data._type.minimum; \
	else if (*_variable > sysfs_data->data._type.maximum) \
		*_variable = sysfs_data->data._type.maximum;

static ssize_t suspend2_attr_store(struct kobject *kobj, struct attribute *attr,
		const char *my_buf, size_t count)
{
	int assigned_temp_buffer = 0, result = count;
	struct suspend_sysfs_data *sysfs_data = to_sysfs_data(attr);

	if (suspend_start_anything((sysfs_data->flags & SYSFS_SUSPEND_OR_RESUME)))
		return -EBUSY;

	((char *) my_buf)[count] = 0;

	if (sysfs_data->flags & SYSFS_NEEDS_SM_FOR_WRITE)
		suspend_prepare_usm();

	switch (sysfs_data->type) {
		case SUSPEND_SYSFS_DATA_CUSTOM:
			if (sysfs_data->data.special.write_sysfs)
				result = (sysfs_data->data.special.write_sysfs)
					(my_buf, count);
			break;
		case SUSPEND_SYSFS_DATA_BIT:
			{
			int value = simple_strtoul(my_buf, NULL, 0);
			if (value)
				set_bit(sysfs_data->data.bit.bit, 
					(sysfs_data->data.bit.bit_vector));
			else
				clear_bit(sysfs_data->data.bit.bit,
					(sysfs_data->data.bit.bit_vector));
			}
			break;
		case SUSPEND_SYSFS_DATA_INTEGER:
			{
				int *variable = sysfs_data->data.integer.variable;
				*variable = simple_strtol(my_buf, NULL, 0);
				BOUND(variable, integer);
				break;
			}
		case SUSPEND_SYSFS_DATA_LONG:
			{
				long *variable = sysfs_data->data.a_long.variable;
				*variable = simple_strtol(my_buf, NULL, 0);
				BOUND(variable, a_long);
				break;
			}
		case SUSPEND_SYSFS_DATA_UL:
			{
				unsigned long *variable = sysfs_data->data.ul.variable;
				*variable = simple_strtoul(my_buf, NULL, 0);
				BOUND(variable, ul);
				break;
			}
			break;
		case SUSPEND_SYSFS_DATA_STRING:
			{
				int copy_len = count;
				char *variable =
					sysfs_data->data.string.variable;

				if (sysfs_data->data.string.max_length &&
				    (copy_len > sysfs_data->data.string.max_length))
					copy_len = sysfs_data->data.string.max_length;

				if (!variable) {
					sysfs_data->data.string.variable =
						variable = (char *) get_zeroed_page(S2_ATOMIC_GFP);
					assigned_temp_buffer = 1;
				}
				strncpy(variable, my_buf, copy_len);
				if ((copy_len) &&
					 (my_buf[copy_len - 1] == '\n'))
					variable[count - 1] = 0;
				variable[count] = 0;
			}
			break;
	}

	/* Side effect routine? */
	if (sysfs_data->write_side_effect)
		sysfs_data->write_side_effect();

	/* Free temporary buffers */
	if (assigned_temp_buffer) {
		free_page((unsigned long) sysfs_data->data.string.variable);
		sysfs_data->data.string.variable = NULL;
	}

	if (sysfs_data->flags & SYSFS_NEEDS_SM_FOR_WRITE)
		suspend_cleanup_usm();

	suspend_finish_anything(sysfs_data->flags & SYSFS_SUSPEND_OR_RESUME);

	return result;
}

static struct sysfs_ops suspend2_sysfs_ops = {
	.show	= &suspend2_attr_show,
	.store	= &suspend2_attr_store,
};

static struct kobj_type suspend2_ktype = {
	.sysfs_ops	= &suspend2_sysfs_ops,
};

decl_subsys(suspend2, &suspend2_ktype, NULL);

/* Non-module sysfs entries.
 *
 * This array contains entries that are automatically registered at
 * boot. Modules and the console code register their own entries separately.
 *
 * NB: If you move do_suspend, change suspend_write_sysfs's test so that
 * suspend_start_anything still gets a 1 when the user echos > do_suspend!
 */

static struct suspend_sysfs_data sysfs_params[] = {
	{ SUSPEND2_ATTR("do_suspend", SYSFS_WRITEONLY),
	  SYSFS_CUSTOM(NULL, NULL, SYSFS_SUSPENDING),
	  .write_side_effect = suspend2_main_wrapper
	},

	{ SUSPEND2_ATTR("do_resume", SYSFS_WRITEONLY),
	  SYSFS_CUSTOM(NULL, NULL, SYSFS_RESUMING),
	  .write_side_effect = __suspend2_try_resume
	},

};

void remove_suspend2_sysdir(struct kobject *kobj)
{
	if (!kobj)
		return;

	kobject_unregister(kobj);

	kfree(kobj);
}

struct kobject *make_suspend2_sysdir(char *name)
{
	struct kobject *kobj = kzalloc(sizeof(struct kobject), GFP_KERNEL);
	int err;

	if(!kobj) {
		printk("Suspend2: Can't allocate kobject for sysfs dir!\n");
		return NULL;
	}

	err = kobject_set_name(kobj, "%s", name);

	if (err) {
		kfree(kobj);
		return NULL;
	}

	kobj->kset = &suspend2_subsys;

	err = kobject_register(kobj);

	if (err)
		kfree(kobj);

	return err ? NULL : kobj;
}

/* suspend_register_sysfs_file
 *
 * Helper for registering a new /sysfs/suspend2 entry.
 */

int suspend_register_sysfs_file(
		struct kobject *kobj,
		struct suspend_sysfs_data *suspend_sysfs_data)
{
	int result;

	if (!suspend_sysfs_initialised)
		suspend_initialise_sysfs();

	if ((result = sysfs_create_file(kobj, &suspend_sysfs_data->attr)))
		printk("Suspend2: sysfs_create_file for %s returned %d.\n",
			suspend_sysfs_data->attr.name, result);

	return result;
}

/* suspend_unregister_sysfs_file
 *
 * Helper for removing unwanted /sys/power/suspend2 entries.
 *
 */
void suspend_unregister_sysfs_file(struct kobject *kobj,
		struct suspend_sysfs_data *suspend_sysfs_data)
{
	sysfs_remove_file(kobj, &suspend_sysfs_data->attr);
}

void suspend_cleanup_sysfs(void)
{
	int i,
	    numfiles = sizeof(sysfs_params) / sizeof(struct suspend_sysfs_data);

	if (!suspend_sysfs_initialised)
		return;

	for (i=0; i< numfiles; i++)
		suspend_unregister_sysfs_file(&suspend2_subsys.kobj,
				&sysfs_params[i]);

	kobj_set_kset_s(&suspend2_subsys, power_subsys);
	subsystem_unregister(&suspend2_subsys);

	suspend_sysfs_initialised = 0;
}

/* suspend_initialise_sysfs
 *
 * Initialise the /sysfs/suspend2 directory.
 */

static void suspend_initialise_sysfs(void)
{
	int i, error;
	int numfiles = sizeof(sysfs_params) / sizeof(struct suspend_sysfs_data);
	
	if (suspend_sysfs_initialised)
		return;

	/* Make our suspend2 directory a child of /sys/power */
	kobj_set_kset_s(&suspend2_subsys, power_subsys);
	error = subsystem_register(&suspend2_subsys);

	if (error)
		return;

	/* Make it use the .store and .show routines above */
	kobj_set_kset_s(&suspend2_subsys, suspend2_subsys);

	suspend_sysfs_initialised = 1;

	for (i=0; i< numfiles; i++)
		suspend_register_sysfs_file(&suspend2_subsys.kobj,
				&sysfs_params[i]);
}

int s2_sysfs_init(void)
{
	suspend_initialise_sysfs();
	return 0;
}

void s2_sysfs_exit(void)
{
	suspend_cleanup_sysfs();
}
