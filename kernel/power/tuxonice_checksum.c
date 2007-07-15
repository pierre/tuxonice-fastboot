/*
 * kernel/power/tuxonice_checksum.c
 *
 * Copyright (C) 2006-2007 Nigel Cunningham (nigel at suspend2 net)
 * Copyright (C) 2006 Red Hat, inc.
 *
 * This file is released under the GPLv2.
 *
 * This file contains data checksum routines for suspend2,
 * using cryptoapi. They are used to locate any modifications
 * made to pageset 2 while we're saving it.
 */

#include <linux/suspend.h>
#include <linux/module.h>
#include <linux/highmem.h>
#include <linux/vmalloc.h>
#include <linux/crypto.h>
#include <linux/scatterlist.h>

#include "tuxonice.h"
#include "tuxonice_modules.h"
#include "tuxonice_sysfs.h"
#include "tuxonice_io.h"
#include "tuxonice_pageflags.h"
#include "tuxonice_checksum.h"
#include "tuxonice_pagedir.h"

static struct suspend_module_ops suspend_checksum_ops;

/* Constant at the mo, but I might allow tuning later */
static char suspend_checksum_name[32] = "md5";
/* Bytes per checksum */
#define CHECKSUM_SIZE (128 / 8)

#define CHECKSUMS_PER_PAGE ((PAGE_SIZE - sizeof(void *)) / CHECKSUM_SIZE)

static struct crypto_hash *suspend_checksum_transform;
static struct hash_desc desc;
static int pages_allocated;
static unsigned long page_list;

static int suspend_num_resaved = 0;

#if 1
#define PRINTK(a, b...) do { } while(0)
#else
#define PRINTK(a, b...) do { printk(a, ##b); } while(0)
#endif

/* ---- Local buffer management ---- */

/* 
 * suspend_checksum_cleanup
 *
 * Frees memory allocated for our labours.
 */
static void suspend_checksum_cleanup(int ending_cycle)
{
	if (ending_cycle && suspend_checksum_transform) {
		crypto_free_hash(suspend_checksum_transform);
		suspend_checksum_transform = NULL;
		desc.tfm = NULL;
	}
}

/* 
 * suspend_crypto_prepare
 *
 * Prepare to do some work by allocating buffers and transforms.
 * Returns: Int: Zero. Even if we can't set up checksum, we still
 * seek to suspend.
 */
static int suspend_checksum_prepare(int starting_cycle)
{
	if (!starting_cycle || !suspend_checksum_ops.enabled)
		return 0;

	if (!*suspend_checksum_name) {
		printk("Suspend2: No checksum algorithm name set.\n");
		return 1;
	}

	suspend_checksum_transform = crypto_alloc_hash(suspend_checksum_name, 0, 0);
	if (IS_ERR(suspend_checksum_transform)) {
		printk("Suspend2: Failed to initialise the %s checksum algorithm: %ld.\n",
				suspend_checksum_name,
				(long) suspend_checksum_transform);
		suspend_checksum_transform = NULL;
		return 1;
	}

	desc.tfm = suspend_checksum_transform;
	desc.flags = 0;

	return 0;
}

static int suspend_print_task_if_using_page(struct task_struct *t, struct page *seeking)
{
	struct vm_area_struct *vma;
	struct mm_struct *mm;
	int result = 0;

	mm = t->active_mm;

	if (!mm || !mm->mmap) return 0;

	/* Don't try to take the sem when processes are frozen, 
	 * drivers are suspended and irqs are disabled. We're
	 * not racing with anything anyway.  */
	if (!irqs_disabled())
		down_read(&mm->mmap_sem);
	
	for (vma = mm->mmap; vma; vma = vma->vm_next) {
		if (vma->vm_flags & VM_PFNMAP)
			continue;
		if (vma->vm_start) {
			unsigned long posn;
			for (posn = vma->vm_start; posn < vma->vm_end;
					posn += PAGE_SIZE) {
				struct page *page = 
					follow_page(vma, posn, 0);
				if (page == seeking) {
					printk("%s(%d)", t->comm, t->pid);
					result = 1;
					goto out;
				}
			}
		}
	}

out:
	if (!irqs_disabled())
		up_read(&mm->mmap_sem);

	return result;
}

static void print_tasks_using_page(struct page *seeking)
{
	struct task_struct *p;

	read_lock(&tasklist_lock);
	for_each_process(p) {
		if (suspend_print_task_if_using_page(p, seeking))
			printk(" ");
	}
	read_unlock(&tasklist_lock);
}

/* 
 * suspend_checksum_print_debug_stats
 * @buffer: Pointer to a buffer into which the debug info will be printed.
 * @size: Size of the buffer.
 *
 * Print information to be recorded for debugging purposes into a buffer.
 * Returns: Number of characters written to the buffer.
 */

static int suspend_checksum_print_debug_stats(char *buffer, int size)
{
	int len;

	if (!suspend_checksum_ops.enabled)
		return snprintf_used(buffer, size,
			"- Checksumming disabled.\n");
	
	len = snprintf_used(buffer, size, "- Checksum method is '%s'.\n",
			suspend_checksum_name);
	len+= snprintf_used(buffer + len, size - len,
		"  %d pages resaved in atomic copy.\n", suspend_num_resaved);
	return len;
}

static int suspend_checksum_storage_needed(void)
{
	if (suspend_checksum_ops.enabled)
		return strlen(suspend_checksum_name) + sizeof(int) + 1;
	else
		return 0;
}

/* 
 * suspend_checksum_save_config_info
 * @buffer: Pointer to a buffer of size PAGE_SIZE.
 *
 * Save informaton needed when reloading the image at resume time.
 * Returns: Number of bytes used for saving our data.
 */
static int suspend_checksum_save_config_info(char *buffer)
{
	int namelen = strlen(suspend_checksum_name) + 1;
	int total_len;
	
	*((unsigned int *) buffer) = namelen;
	strncpy(buffer + sizeof(unsigned int), suspend_checksum_name, 
								namelen);
	total_len = sizeof(unsigned int) + namelen;
	return total_len;
}

/* suspend_checksum_load_config_info
 * @buffer: Pointer to the start of the data.
 * @size: Number of bytes that were saved.
 *
 * Description:	Reload information needed for dechecksuming the image at
 * resume time.
 */
static void suspend_checksum_load_config_info(char *buffer, int size)
{
	int namelen;

	namelen = *((unsigned int *) (buffer));
	strncpy(suspend_checksum_name, buffer + sizeof(unsigned int),
			namelen);
	return;
}

/*
 * Free Checksum Memory
 */

void free_checksum_pages(void)
{
	PRINTK("Freeing %d checksum pages.\n", pages_allocated);
	while (pages_allocated) {
		unsigned long next = *((unsigned long *) page_list);
		PRINTK("Page %3d is at %lx and points to %lx.\n", pages_allocated, page_list, next);
		ClearPageNosave(virt_to_page(page_list));
		free_page((unsigned long) page_list);
		page_list = next;
		pages_allocated--;
	}
}

/*
 * Allocate Checksum Memory
 */

int allocate_checksum_pages(void)
{
	int pages_needed = DIV_ROUND_UP(pagedir2.size, CHECKSUMS_PER_PAGE);

	if (!suspend_checksum_ops.enabled)
		return 0;

	PRINTK("Need %d checksum pages for %ld pageset2 pages.\n", pages_needed, pagedir2.size);
	while (pages_allocated < pages_needed) {
		unsigned long *new_page =
			(unsigned long *) get_zeroed_page(S2_ATOMIC_GFP);
		if (!new_page)
			return -ENOMEM;
		SetPageNosave(virt_to_page(new_page));
		(*new_page) = page_list;
		page_list = (unsigned long) new_page;
		pages_allocated++;
		PRINTK("Page %3d is at %lx and points to %lx.\n", pages_allocated, page_list, *((unsigned long *) page_list));
	}

	return 0;
}

#if 0
static void print_checksum(char *buf, int size)
{
	int index;

	for (index = 0; index < size; index++)
		printk("%x ", buf[index]);

	printk("\n");
}
#endif

/*
 * Calculate checksums
 */

void calculate_check_checksums(int check)
{
	int pfn, index = 0;
	unsigned long next_page, this_checksum = 0;
	struct scatterlist sg[2];
	char current_checksum[CHECKSUM_SIZE];

	if (!suspend_checksum_ops.enabled)
		return;

	next_page = (unsigned long) page_list;

	if (check)
		suspend_num_resaved = 0;

	BITMAP_FOR_EACH_SET(pageset2_map, pfn) {
		int ret;
		if (index % CHECKSUMS_PER_PAGE) {
			this_checksum += CHECKSUM_SIZE;
		} else {
			this_checksum = next_page + sizeof(void *);
			next_page = *((unsigned long *) next_page);
		}
		PRINTK("Put checksum for page %3d %p in %lx.\n", index, page_address(pfn_to_page(pfn)), this_checksum);
		sg_set_buf(&sg[0], page_address(pfn_to_page(pfn)), PAGE_SIZE);
		if (check) {
			ret = crypto_hash_digest(&desc, sg, 
					PAGE_SIZE, current_checksum);
			if (memcmp(current_checksum, (char *) this_checksum, CHECKSUM_SIZE)) {
				SetPageResave(pfn_to_page(pfn));
				printk("Page %d changed. Saving in atomic copy."
					"Processes using it:", pfn);
				print_tasks_using_page(pfn_to_page(pfn));
				printk("\n");
				suspend_num_resaved++;
				if (test_action_state(TOI_ABORT_ON_RESAVE_NEEDED))
					set_abort_result(TOI_RESAVE_NEEDED);
			}
		} else
			ret = crypto_hash_digest(&desc, sg, 
					PAGE_SIZE, (char *) this_checksum);
		if (ret) {
			printk("Digest failed. Returned %d.\n", ret);
			return;
		}
		index++;
	}
}

static struct suspend_sysfs_data sysfs_params[] = {
	{ SUSPEND2_ATTR("enabled", SYSFS_RW),
	  SYSFS_INT(&suspend_checksum_ops.enabled, 0, 1, 0)
	},

	{ SUSPEND2_ATTR("abort_if_resave_needed", SYSFS_RW),
	  SYSFS_BIT(&suspend_action, TOI_ABORT_ON_RESAVE_NEEDED, 0)
	}
};

/*
 * Ops structure.
 */
static struct suspend_module_ops suspend_checksum_ops = {
	.type			= MISC_MODULE,
	.name			= "checksumming",
	.directory		= "checksum",
	.module			= THIS_MODULE,
	.initialise		= suspend_checksum_prepare,
	.cleanup		= suspend_checksum_cleanup,
	.print_debug_info	= suspend_checksum_print_debug_stats,
	.save_config_info	= suspend_checksum_save_config_info,
	.load_config_info	= suspend_checksum_load_config_info,
	.storage_needed		= suspend_checksum_storage_needed,

	.sysfs_data		= sysfs_params,
	.num_sysfs_entries	= sizeof(sysfs_params) / sizeof(struct suspend_sysfs_data),
};

/* ---- Registration ---- */
int s2_checksum_init(void)
{
	int result = suspend_register_module(&suspend_checksum_ops);

	/* Disabled by default */
	suspend_checksum_ops.enabled = 0;
	return result;
}

void s2_checksum_exit(void)
{
	suspend_unregister_module(&suspend_checksum_ops);
}
