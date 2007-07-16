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

LIST_HEAD(toi_filters);
LIST_HEAD(toiAllocators);
LIST_HEAD(toi_modules);

struct toi_module_ops *toiActiveAllocator;
int toi_num_filters;
int toiNumAllocators, toi_num_modules;
 
/*
 * toi_header_storage_for_modules
 *
 * Returns the amount of space needed to store configuration
 * data needed by the modules prior to copying back the original
 * kernel. We can exclude data for pageset2 because it will be
 * available anyway once the kernel is copied back.
 */
int toi_header_storage_for_modules(void)
{
	struct toi_module_ops *this_module;
	int bytes = 0;
	
	list_for_each_entry(this_module, &toi_modules, module_list) {
		if (!this_module->enabled ||
		    (this_module->type == WRITER_MODULE &&
		     toiActiveAllocator != this_module))
			continue;
		if (this_module->storage_needed) {
			int this = this_module->storage_needed() +
				sizeof(struct toi_module_header) +
				sizeof(int);
			this_module->header_requested = this;
			bytes += this;
		}
	}

	/* One more for the empty terminator */
	return bytes + sizeof(struct toi_module_header);
}

/*
 * toi_memory_for_modules
 *
 * Returns the amount of memory requested by modules for
 * doing their work during the cycle.
 */

int toi_memory_for_modules(void)
{
	int bytes = 0;
	struct toi_module_ops *this_module;

	list_for_each_entry(this_module, &toi_modules, module_list) {
		if (!this_module->enabled)
			continue;
		if (this_module->memory_needed)
			bytes += this_module->memory_needed();
	}

	return ((bytes + PAGE_SIZE - 1) >> PAGE_SHIFT);
}

/*
 * toi_expected_compression_ratio
 *
 * Returns the compression ratio expected when saving the image.
 */

int toi_expected_compression_ratio(void)
{
	int ratio = 100;
	struct toi_module_ops *this_module;

	list_for_each_entry(this_module, &toi_modules, module_list) {
		if (!this_module->enabled)
			continue;
		if (this_module->expected_compression)
			ratio = ratio * this_module->expected_compression() / 100;
	}

	return ratio;
}

/* toi_find_module_given_dir
 * Functionality :	Return a module (if found), given a pointer
 * 			to its directory name
 */

static struct toi_module_ops *toi_find_module_given_dir(char *name)
{
	struct toi_module_ops *this_module, *found_module = NULL;
	
	list_for_each_entry(this_module, &toi_modules, module_list) {
		if (!strcmp(name, this_module->directory)) {
			found_module = this_module;
			break;
		}			
	}

	return found_module;
}

/* toi_find_module_given_name
 * Functionality :	Return a module (if found), given a pointer
 * 			to its name
 */

struct toi_module_ops *toi_find_module_given_name(char *name)
{
	struct toi_module_ops *this_module, *found_module = NULL;
	
	list_for_each_entry(this_module, &toi_modules, module_list) {
		if (!strcmp(name, this_module->name)) {
			found_module = this_module;
			break;
		}			
	}

	return found_module;
}

/*
 * toi_print_module_debug_info
 * Functionality   : Get debugging info from modules into a buffer.
 */
int toi_print_module_debug_info(char *buffer, int buffer_size)
{
	struct toi_module_ops *this_module;
	int len = 0;

	list_for_each_entry(this_module, &toi_modules, module_list) {
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
 * toi_register_module
 *
 * Register a module.
 */
int toi_register_module(struct toi_module_ops *module)
{
	int i;
	struct kobject *kobj;

	module->enabled = 1;
	
	if (toi_find_module_given_name(module->name)) {
		printk("TuxOnIce: Trying to load module %s,"
				" which is already registered.\n",
				module->name);
		return -EBUSY;
	}

	switch (module->type) {
		case FILTER_MODULE:
			list_add_tail(&module->type_list,
					&toi_filters);
			toi_num_filters++;
			break;

		case WRITER_MODULE:
			list_add_tail(&module->type_list,
					&toiAllocators);
			toiNumAllocators++;
			break;

		case MISC_MODULE:
		case MISC_HIDDEN_MODULE:
			break;

		default:
			printk("Hmmm. Module '%s' has an invalid type."
				" It has been ignored.\n", module->name);
			return -EINVAL;
	}
	list_add_tail(&module->module_list, &toi_modules);
	toi_num_modules++;

	if (module->directory || module->shared_directory) {
		/* 
		 * Modules may share a directory, but those with shared_dir
		 * set must be loaded (via symbol dependencies) after parents
		 * and unloaded beforehand.
		 */
		if (module->shared_directory) {
			struct toi_module_ops *shared =
				toi_find_module_given_dir(module->shared_directory);
			if (!shared) {
				printk("TuxOnIce: Module %s wants to share %s's directory but %s isn't loaded.\n",
						module->name,
						module->shared_directory,
						module->shared_directory);
				toi_unregister_module(module);
				return -ENODEV;
			}
			kobj = shared->dir_kobj;
		} else {
			if (!strncmp(module->directory, "[ROOT]", 6))
				kobj = &toi_subsys.kobj;
			else
				kobj = make_toi_sysdir(module->directory);
		}
		module->dir_kobj = kobj;
		for (i=0; i < module->num_sysfs_entries; i++) {
			int result = toi_register_sysfs_file(kobj, &module->sysfs_data[i]);
			if (result)
				return result;
		}
	}

	return 0;
}

/*
 * toi_unregister_module
 *
 * Remove a module.
 */
void toi_unregister_module(struct toi_module_ops *module)
{
	int i;

	if (module->dir_kobj)
		for (i=0; i < module->num_sysfs_entries; i++)
			toi_unregister_sysfs_file(module->dir_kobj, &module->sysfs_data[i]);

	if (!module->shared_directory && module->directory &&
			strncmp(module->directory, "[ROOT]", 6))
		remove_toi_sysdir(module->dir_kobj);

	switch (module->type) {
		case FILTER_MODULE:
			list_del(&module->type_list);
			toi_num_filters--;
			break;

		case WRITER_MODULE:
			list_del(&module->type_list);
			toiNumAllocators--;
			if (toiActiveAllocator == module) {
				toiActiveAllocator = NULL;
				clear_toi_state(TOI_CAN_RESUME);
				clear_toi_state(TOI_CAN_HIBERNATE);
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
	toi_num_modules--;
}

/*
 * toi_move_module_tail
 *
 * Rearrange modules when reloading the config.
 */
void toi_move_module_tail(struct toi_module_ops *module)
{
	switch (module->type) {
		case FILTER_MODULE:
			if (toi_num_filters > 1)
				list_move_tail(&module->type_list,
						&toi_filters);
			break;

		case WRITER_MODULE:
			if (toiNumAllocators > 1)
				list_move_tail(&module->type_list,
						&toiAllocators);
			break;
		
		case MISC_MODULE:
		case MISC_HIDDEN_MODULE:
			break;
		default:
			printk("Hmmm. Module '%s' has an invalid type."
				" It has been ignored.\n", module->name);
			return;
	}
	if ((toi_num_filters + toiNumAllocators) > 1)
		list_move_tail(&module->module_list, &toi_modules);
}

/*
 * toi_initialise_modules
 *
 * Get ready to do some work!
 */
int toi_initialise_modules(int starting_cycle)
{
	struct toi_module_ops *this_module;
	int result;
	
	list_for_each_entry(this_module, &toi_modules, module_list) {
		this_module->header_requested = 0;
		this_module->header_used = 0;
		if (!this_module->enabled)
			continue;
		if (this_module->initialise) {
			toi_message(TOI_MEMORY, TOI_MEDIUM, 1,
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
 * toi_cleanup_modules
 *
 * Tell modules the work is done.
 */
void toi_cleanup_modules(int finishing_cycle)
{
	struct toi_module_ops *this_module;
	
	list_for_each_entry(this_module, &toi_modules, module_list) {
		if (!this_module->enabled)
			continue;
		if (this_module->cleanup) {
			toi_message(TOI_MEMORY, TOI_MEDIUM, 1,
				"Cleaning up module %s.\n",
				this_module->name);
			this_module->cleanup(finishing_cycle);
		}
	}
}

/*
 * toi_get_next_filter
 *
 * Get the next filter in the pipeline.
 */
struct toi_module_ops *toi_get_next_filter(struct toi_module_ops *filter_sought)
{
	struct toi_module_ops *last_filter = NULL, *this_filter = NULL;

	list_for_each_entry(this_filter, &toi_filters, type_list) {
		if (!this_filter->enabled)
			continue;
		if ((last_filter == filter_sought) || (!filter_sought))
			return this_filter;
		last_filter = this_filter;
	}

	return toiActiveAllocator;
}

/**
 * toi_show_modules: Printk what support is loaded.
 */
void toi_print_modules(void)
{
	struct toi_module_ops *this_module;
	int prev = 0;

	printk("TuxOnIce " TOI_CORE_VERSION ", with support for");
	
	list_for_each_entry(this_module, &toi_modules, module_list) {
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

/* toi_get_modules
 * 
 * Take a reference to modules so they can't go away under us.
 */

int toi_get_modules(void)
{
	struct toi_module_ops *this_module;
	
	list_for_each_entry(this_module, &toi_modules, module_list) {
		if (!try_module_get(this_module->module)) {
			/* Failed! Reverse gets and return error */
			struct toi_module_ops *this_module2;
			list_for_each_entry(this_module2, &toi_modules, module_list) {
				if (this_module == this_module2)
					return -EINVAL;
				module_put(this_module2->module);
			}
		}
	}

	return 0;
}

/* toi_put_modules
 *
 * Release our references to modules we used.
 */

void toi_put_modules(void)
{
	struct toi_module_ops *this_module;
	
	list_for_each_entry(this_module, &toi_modules, module_list)
		module_put(this_module->module);
}

#ifdef CONFIG_TOI_EXPORTS
EXPORT_SYMBOL_GPL(toi_register_module);
EXPORT_SYMBOL_GPL(toi_unregister_module);
EXPORT_SYMBOL_GPL(toi_get_next_filter);
EXPORT_SYMBOL_GPL(toiActiveAllocator);
#endif
