/*
 * kernel/power/tuxonice_modules.c
 *
 * Copyright (C) 2004-2007 Nigel Cunningham (nigel at suspend2 net)
 *
 */

#include <linux/suspend.h>
#include <linux/module.h>
#include "tuxonice.h"
#include "tuxonice_modules.h"
#include "tuxonice_sysfs.h"
#include "tuxonice_ui.h"

LIST_HEAD(suspend_filters);
LIST_HEAD(suspendAllocators);
LIST_HEAD(suspend_modules);

struct suspend_module_ops *suspendActiveAllocator;
int suspend_num_filters;
int suspendNumAllocators, suspend_num_modules;
 
/*
 * suspend_header_storage_for_modules
 *
 * Returns the amount of space needed to store configuration
 * data needed by the modules prior to copying back the original
 * kernel. We can exclude data for pageset2 because it will be
 * available anyway once the kernel is copied back.
 */
int suspend_header_storage_for_modules(void)
{
	struct suspend_module_ops *this_module;
	int bytes = 0;
	
	list_for_each_entry(this_module, &suspend_modules, module_list) {
		if (!this_module->enabled ||
		    (this_module->type == WRITER_MODULE &&
		     suspendActiveAllocator != this_module))
			continue;
		if (this_module->storage_needed) {
			int this = this_module->storage_needed() +
				sizeof(struct suspend_module_header) +
				sizeof(int);
			this_module->header_requested = this;
			bytes += this;
		}
	}

	/* One more for the empty terminator */
	return bytes + sizeof(struct suspend_module_header);
}

/*
 * suspend_memory_for_modules
 *
 * Returns the amount of memory requested by modules for
 * doing their work during the cycle.
 */

int suspend_memory_for_modules(void)
{
	int bytes = 0;
	struct suspend_module_ops *this_module;

	list_for_each_entry(this_module, &suspend_modules, module_list) {
		if (!this_module->enabled)
			continue;
		if (this_module->memory_needed)
			bytes += this_module->memory_needed();
	}

	return ((bytes + PAGE_SIZE - 1) >> PAGE_SHIFT);
}

/*
 * suspend_expected_compression_ratio
 *
 * Returns the compression ratio expected when saving the image.
 */

int suspend_expected_compression_ratio(void)
{
	int ratio = 100;
	struct suspend_module_ops *this_module;

	list_for_each_entry(this_module, &suspend_modules, module_list) {
		if (!this_module->enabled)
			continue;
		if (this_module->expected_compression)
			ratio = ratio * this_module->expected_compression() / 100;
	}

	return ratio;
}

/* suspend_find_module_given_dir
 * Functionality :	Return a module (if found), given a pointer
 * 			to its directory name
 */

static struct suspend_module_ops *suspend_find_module_given_dir(char *name)
{
	struct suspend_module_ops *this_module, *found_module = NULL;
	
	list_for_each_entry(this_module, &suspend_modules, module_list) {
		if (!strcmp(name, this_module->directory)) {
			found_module = this_module;
			break;
		}			
	}

	return found_module;
}

/* suspend_find_module_given_name
 * Functionality :	Return a module (if found), given a pointer
 * 			to its name
 */

struct suspend_module_ops *suspend_find_module_given_name(char *name)
{
	struct suspend_module_ops *this_module, *found_module = NULL;
	
	list_for_each_entry(this_module, &suspend_modules, module_list) {
		if (!strcmp(name, this_module->name)) {
			found_module = this_module;
			break;
		}			
	}

	return found_module;
}

/*
 * suspend_print_module_debug_info
 * Functionality   : Get debugging info from modules into a buffer.
 */
int suspend_print_module_debug_info(char *buffer, int buffer_size)
{
	struct suspend_module_ops *this_module;
	int len = 0;

	list_for_each_entry(this_module, &suspend_modules, module_list) {
		if (!this_module->enabled)
			continue;
		if (this_module->print_debug_info) {
			int result;
			result = this_module->print_debug_info(buffer + len, 
					buffer_size - len);
			len += result;
		}
	}

	/* Ensure null terminated */
	buffer[buffer_size] = 0;

	return len;
}

/*
 * suspend_register_module
 *
 * Register a module.
 */
int suspend_register_module(struct suspend_module_ops *module)
{
	int i;
	struct kobject *kobj;

	module->enabled = 1;
	
	if (suspend_find_module_given_name(module->name)) {
		printk("Suspend2: Trying to load module %s,"
				" which is already registered.\n",
				module->name);
		return -EBUSY;
	}

	switch (module->type) {
		case FILTER_MODULE:
			list_add_tail(&module->type_list,
					&suspend_filters);
			suspend_num_filters++;
			break;

		case WRITER_MODULE:
			list_add_tail(&module->type_list,
					&suspendAllocators);
			suspendNumAllocators++;
			break;

		case MISC_MODULE:
		case MISC_HIDDEN_MODULE:
			break;

		default:
			printk("Hmmm. Module '%s' has an invalid type."
				" It has been ignored.\n", module->name);
			return -EINVAL;
	}
	list_add_tail(&module->module_list, &suspend_modules);
	suspend_num_modules++;

	if (module->directory || module->shared_directory) {
		/* 
		 * Modules may share a directory, but those with shared_dir
		 * set must be loaded (via symbol dependencies) after parents
		 * and unloaded beforehand.
		 */
		if (module->shared_directory) {
			struct suspend_module_ops *shared =
				suspend_find_module_given_dir(module->shared_directory);
			if (!shared) {
				printk("Suspend2: Module %s wants to share %s's directory but %s isn't loaded.\n",
						module->name,
						module->shared_directory,
						module->shared_directory);
				suspend_unregister_module(module);
				return -ENODEV;
			}
			kobj = shared->dir_kobj;
		} else {
			if (!strncmp(module->directory, "[ROOT]", 6))
				kobj = &suspend2_subsys.kobj;
			else
				kobj = make_suspend2_sysdir(module->directory);
		}
		module->dir_kobj = kobj;
		for (i=0; i < module->num_sysfs_entries; i++) {
			int result = suspend_register_sysfs_file(kobj, &module->sysfs_data[i]);
			if (result)
				return result;
		}
	}

	return 0;
}

/*
 * suspend_unregister_module
 *
 * Remove a module.
 */
void suspend_unregister_module(struct suspend_module_ops *module)
{
	int i;

	if (module->dir_kobj)
		for (i=0; i < module->num_sysfs_entries; i++)
			suspend_unregister_sysfs_file(module->dir_kobj, &module->sysfs_data[i]);

	if (!module->shared_directory && module->directory &&
			strncmp(module->directory, "[ROOT]", 6))
		remove_suspend2_sysdir(module->dir_kobj);

	switch (module->type) {
		case FILTER_MODULE:
			list_del(&module->type_list);
			suspend_num_filters--;
			break;

		case WRITER_MODULE:
			list_del(&module->type_list);
			suspendNumAllocators--;
			if (suspendActiveAllocator == module) {
				suspendActiveAllocator = NULL;
				clear_suspend_state(TOI_CAN_RESUME);
				clear_suspend_state(TOI_CAN_HIBERNATE);
			}
			break;
		
		case MISC_MODULE:
		case MISC_HIDDEN_MODULE:
			break;

		default:
			printk("Hmmm. Module '%s' has an invalid type."
				" It has been ignored.\n", module->name);
			return;
	}
	list_del(&module->module_list);
	suspend_num_modules--;
}

/*
 * suspend_move_module_tail
 *
 * Rearrange modules when reloading the config.
 */
void suspend_move_module_tail(struct suspend_module_ops *module)
{
	switch (module->type) {
		case FILTER_MODULE:
			if (suspend_num_filters > 1)
				list_move_tail(&module->type_list,
						&suspend_filters);
			break;

		case WRITER_MODULE:
			if (suspendNumAllocators > 1)
				list_move_tail(&module->type_list,
						&suspendAllocators);
			break;
		
		case MISC_MODULE:
		case MISC_HIDDEN_MODULE:
			break;
		default:
			printk("Hmmm. Module '%s' has an invalid type."
				" It has been ignored.\n", module->name);
			return;
	}
	if ((suspend_num_filters + suspendNumAllocators) > 1)
		list_move_tail(&module->module_list, &suspend_modules);
}

/*
 * suspend_initialise_modules
 *
 * Get ready to do some work!
 */
int suspend_initialise_modules(int starting_cycle)
{
	struct suspend_module_ops *this_module;
	int result;
	
	list_for_each_entry(this_module, &suspend_modules, module_list) {
		this_module->header_requested = 0;
		this_module->header_used = 0;
		if (!this_module->enabled)
			continue;
		if (this_module->initialise) {
			suspend_message(TOI_MEMORY, TOI_MEDIUM, 1,
				"Initialising module %s.\n",
				this_module->name);
			if ((result = this_module->initialise(starting_cycle))) {
				printk("%s didn't initialise okay.\n",
						this_module->name);
				return result;
			}
		}
	}

	return 0;
}

/* 
 * suspend_cleanup_modules
 *
 * Tell modules the work is done.
 */
void suspend_cleanup_modules(int finishing_cycle)
{
	struct suspend_module_ops *this_module;
	
	list_for_each_entry(this_module, &suspend_modules, module_list) {
		if (!this_module->enabled)
			continue;
		if (this_module->cleanup) {
			suspend_message(TOI_MEMORY, TOI_MEDIUM, 1,
				"Cleaning up module %s.\n",
				this_module->name);
			this_module->cleanup(finishing_cycle);
		}
	}
}

/*
 * suspend_get_next_filter
 *
 * Get the next filter in the pipeline.
 */
struct suspend_module_ops *suspend_get_next_filter(struct suspend_module_ops *filter_sought)
{
	struct suspend_module_ops *last_filter = NULL, *this_filter = NULL;

	list_for_each_entry(this_filter, &suspend_filters, type_list) {
		if (!this_filter->enabled)
			continue;
		if ((last_filter == filter_sought) || (!filter_sought))
			return this_filter;
		last_filter = this_filter;
	}

	return suspendActiveAllocator;
}

/**
 * suspend_show_modules: Printk what support is loaded.
 */
void suspend_print_modules(void)
{
	struct suspend_module_ops *this_module;
	int prev = 0;

	printk("Suspend2 " TOI_CORE_VERSION ", with support for");
	
	list_for_each_entry(this_module, &suspend_modules, module_list) {
		if (this_module->type == MISC_HIDDEN_MODULE)
			continue;
		printk("%s %s%s%s", prev ? "," : "",
				this_module->enabled ? "" : "[",
				this_module->name,
				this_module->enabled ? "" : "]");
		prev = 1;
	}

	printk(".\n");
}

/* suspend_get_modules
 * 
 * Take a reference to modules so they can't go away under us.
 */

int suspend_get_modules(void)
{
	struct suspend_module_ops *this_module;
	
	list_for_each_entry(this_module, &suspend_modules, module_list) {
		if (!try_module_get(this_module->module)) {
			/* Failed! Reverse gets and return error */
			struct suspend_module_ops *this_module2;
			list_for_each_entry(this_module2, &suspend_modules, module_list) {
				if (this_module == this_module2)
					return -EINVAL;
				module_put(this_module2->module);
			}
		}
	}

	return 0;
}

/* suspend_put_modules
 *
 * Release our references to modules we used.
 */

void suspend_put_modules(void)
{
	struct suspend_module_ops *this_module;
	
	list_for_each_entry(this_module, &suspend_modules, module_list)
		module_put(this_module->module);
}

#ifdef CONFIG_TOI_EXPORTS
EXPORT_SYMBOL_GPL(suspend_register_module);
EXPORT_SYMBOL_GPL(suspend_unregister_module);
EXPORT_SYMBOL_GPL(suspend_get_next_filter);
EXPORT_SYMBOL_GPL(suspendActiveAllocator);
#endif
