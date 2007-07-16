/*
 * kernel/power/tuxonice_checksum.c
 *
 * Copyright (C) 2006-2007 Nigel Cunningham (nigel at suspend2 net)
 * Copyright (C) 2006 Red Hat, inc.
 *
 * This file is released under the GPLv2.
 *
 * This file contains data checksum routines for TuxOnIce,
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

static struct toi_module_ops toi_checksum_ops;

/* Constant at the mo, but I might allow tuning later */
static char toi_checksum_name[32] = "md5";
/* Bytes per checksum */
#define CHECKSUM_SIZE (128 / 8)

#define CHECKSUMS_PER_PAGE ((PAGE_SIZE - sizeof(void *)) / CHECKSUM_SIZE)

static struct crypto_hash *toi_checksum_transform;
static struct hash_desc desc;
static int pages_allocated;
static unsigned long page_list;

static int toi_num_resaved = 0;

#if 1
#define PRINTK(a, b...) do { } while(0)
#else
#define PRINTK(a, b...) do { printk(a, ##b); } while(0)
#endif

/* ---- Local buffer management ---- */

/* 
 * toi_checksum_cleanup
 *
 * Frees memory allocated for our labours.
 */
static void toi_checksum_cleanup(int ending_cycle)
{
	if (ending_cycle && toi_checksum_transform) {
		crypto_free_hash(toi_checksum_transform);
		toi_checksum_transform = NULL;
		desc.tfm = NULL;
	}
}

/* 
 * toi_crypto_prepare
 *
 * Prepare to do some work by allocating buffers and transforms.
 * Returns: Int: Zero. Even if we can't set up checksum, we still
 * seek to hibernate.
 */
static int toi_checksum_prepare(int starting_cycle)
{
	if (!starting_cycle || !toi_checksum_ops.enabled)
		return 0;

	if (!*toi_checksum_name) {
		printk("TuxOnIce: No checksum algorithm name set.\n");
		return 1;
	}

	toi_checksum_transform = crypto_alloc_hash(toi_checksum_name, 0, 0);
	if (IS_ERR(toi_checksum_transform)) {
		printk("TuxOnIce: Failed to initialise the %s checksum algorithm: %ld.\n",
				toi_checksum_name,
				(long) toi_checksum_transform);
		toi_checksum_transform = NULL;
		return 1;
	}

	desc.tfm = toi_checksum_transform;
	desc.flags = 0;

	return 0;
}

static int toi_print_task_if_using_page(struct task_struct *t, struct page *seeking)
{
	struct vm_area_struct *vma;
	struct mm_struct *mm;
	int result = 0;

	mm = t->active_mm;

	if (!mm || !mm->mmap) return 0;

	/* Don't try to take the sem when processes are frozen, 
	 * drivers are hibernated and irqs are disabled. We're
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
		if (toi_print_task_if_using_page(p, seeking))
			printk(" ");
	}
	read_unlock(&tasklist_lock);
}

/* 
 * toi_checksum_print_debug_stats
 * @buffer: Pointer to a buffer into which the debug info will be printed.
 * @size: Size of the buffer.
 *
 * Print information to be recorded for debugging purposes into a buffer.
 * Returns: Number of characters written to the buffer.
 */

static int toi_checksum_print_debug_stats(char *buffer, int size)
{
	int len;

	if (!toi_checksum_ops.enabled)
		return snprintf_used(buffer, size,
			"- Checksumming disabled.\n");
	
	len = snprintf_used(buffer, size, "- Checksum method is '%s'.\n",
			toi_checksum_name);
	len+= snprintf_used(buffer + len, size - len,
		"  %d pages resaved in atomic copy.\n", toi_num_resaved);
	return len;
}

static int toi_checksum_storage_needed(void)
{
	if (toi_checksum_ops.enabled)
		return strlen(toi_checksum_name) + sizeof(int) + 1;
	else
		return 0;
}

/* 
 * toi_checksum_save_config_info
 * @buffer: Pointer to a buffer of size PAGE_SIZE.
 *
 * Save informaton needed when reloading the image at resume time.
 * Returns: Number of bytes used for saving our data.
 */
static int toi_checksum_save_config_info(char *buffer)
{
	int namelen = strlen(toi_checksum_name) + 1;
	int total_len;
	
	*((unsigned int *) buffer) = namelen;
	strncpy(buffer + sizeof(unsigned int), toi_checksum_name, 
								namelen);
	total_len = sizeof(unsigned int) + namelen;
	return total_len;
}

/* toi_checksum_load_config_info
 * @buffer: Pointer to the start of the data.
 * @size: Number of bytes that were saved.
 *
 * Description:	Reload information needed for dechecksuming the image at
 * resume time.
 */
static void toi_checksum_load_config_info(char *buffer, int size)
{
	int namelen;

	namelen = *((unsigned int *) (buffer));
	strncpy(toi_checksum_name, buffer + sizeof(unsigned int),
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

	if (!toi_checksum_ops.enabled)
		return 0;

	PRINTK("Need %d checksum pages for %ld pageset2 pages.\n", pages_needed, pagedir2.size);
	while (pages_allocated < pages_needed) {
		unsigned long *new_page =
			(unsigned long *) get_zeroed_page(TOI_ATOMIC_GFP);
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

	if (!toi_checksum_ops.enabled)
		return;

	next_page = (unsigned long) page_list;

	if (check)
		toi_num_resaved = 0;

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
				toi_num_resaved++;
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

static struct toi_sysfs_data sysfs_params[] = {
	{ TOI_ATTR("enabled", SYSFS_RW),
	  SYSFS_INT(&toi_checksum_ops.enabled, 0, 1, 0)
	},

	{ TOI_ATTR("abort_if_resave_needed", SYSFS_RW),
	  SYSFS_BIT(&toi_action, TOI_ABORT_ON_RESAVE_NEEDED, 0)
	}
};

/*
 * Ops structure.
 */
static struct toi_module_ops toi_checksum_ops = {
	.type			= MISC_MODULE,
	.name			= "checksumming",
	.directory		= "checksum",
	.module			= THIS_MODULE,
	.initialise		= toi_checksum_prepare,
	.cleanup		= toi_checksum_cleanup,
	.print_debug_info	= toi_checksum_print_debug_stats,
	.save_config_info	= toi_checksum_save_config_info,
	.load_config_info	= toi_checksum_load_config_info,
	.storage_needed		= toi_checksum_storage_needed,

	.sysfs_data		= sysfs_params,
	.num_sysfs_entries	= sizeof(sysfs_params) / sizeof(struct toi_sysfs_data),
};

/* ---- Registration ---- */
int toi_checksum_init(void)
{
	int result = toi_register_module(&toi_checksum_ops);

	/* Disabled by default */
	toi_checksum_ops.enabled = 0;
	return result;
}

void toi_checksum_exit(void)
{
	toi_unregister_module(&toi_checksum_ops);
}
