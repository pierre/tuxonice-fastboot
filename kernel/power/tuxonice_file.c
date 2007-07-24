/*
 * kernel/power/tuxonice_file.c
 *
 * Copyright (C) 2005-2007 Nigel Cunningham (nigel at suspend2 net)
 *
 * Distributed under GPLv2.
 * 
 * This file encapsulates functions for usage of a simple file as a
 * backing store. It is based upon the swapallocator, and shares the
 * same basic working. Here, though, we have nothing to do with
 * swapspace, and only one device to worry about.
 *
 * The user can just
 *
 * echo TuxOnIce > /path/to/my_file
 *
 * dd if=/dev/zero bs=1M count=<file_size_desired> >> /path/to/my_file
 *
 * and
 *
 * echo /path/to/my_file > /sys/power/tuxonice/file/target
 *
 * then put what they find in /sys/power/tuxonice/resume
 * as their resume= parameter in lilo.conf (and rerun lilo if using it).
 *
 * Having done this, they're ready to hibernate and resume.
 *
 * TODO:
 * - File resizing.
 */

#include <linux/suspend.h>
#include <linux/module.h>
#include <linux/blkdev.h>
#include <linux/file.h>
#include <linux/stat.h>
#include <linux/mount.h>
#include <linux/statfs.h>
#include <linux/syscalls.h>
#include <linux/namei.h>
#include <linux/fs.h>
#include <linux/root_dev.h>

#include "tuxonice.h"
#include "tuxonice_sysfs.h"
#include "tuxonice_modules.h"
#include "tuxonice_ui.h"
#include "tuxonice_extent.h"
#include "tuxonice_io.h"
#include "tuxonice_storage.h"
#include "tuxonice_block_io.h"

static struct toi_module_ops toi_fileops;

/* Details of our target.  */

char toi_file_target[256];
static struct inode *target_inode;
static struct file *target_file;
static struct block_device *toi_file_target_bdev;
static dev_t resume_file_dev_t;
static int used_devt = 0;
static int setting_toi_file_target = 0;
static sector_t target_firstblock = 0, target_header_start = 0;
static int target_storage_available = 0;
static int target_claim = 0;

static char HaveImage[] = "HaveImage\n";
static char NoImage[] =   "TuxOnIce\n";
#define sig_size (sizeof(HaveImage) + 1)

struct toi_file_header {
	char sig[sig_size];
	int resumed_before;
	unsigned long first_header_block;
};

/* Header Page Information */
static int header_pages_allocated;

/* Main Storage Pages */
static int main_pages_allocated, main_pages_requested;

#define target_is_normal_file() (S_ISREG(target_inode->i_mode))

static struct toi_bdev_info devinfo;

/* Extent chain for blocks */
static struct extent_chain block_chain;

/* Signature operations */
enum {
	GET_IMAGE_EXISTS,
	INVALIDATE,
	MARK_RESUME_ATTEMPTED,
	UNMARK_RESUME_ATTEMPTED,
};

static void set_devinfo(struct block_device *bdev, int target_blkbits)
{
	devinfo.bdev = bdev;
	if (!target_blkbits) {
		devinfo.bmap_shift = devinfo.blocks_per_page = 0;
	} else {
		devinfo.bmap_shift = target_blkbits - 9;
		devinfo.blocks_per_page = (1 << (PAGE_SHIFT - target_blkbits));
	}
}

static int adjust_for_extra_pages(int unadjusted)
{
	return (unadjusted << PAGE_SHIFT) / (PAGE_SIZE + sizeof(unsigned long)
			+ sizeof(int));
}

static int toi_file_storage_available(void)
{
	int result = 0;
	struct block_device *bdev=toi_file_target_bdev;

	if (!target_inode)
		return 0;

	switch (target_inode->i_mode & S_IFMT) {
		case S_IFSOCK:
		case S_IFCHR:
		case S_IFIFO: /* Socket, Char, Fifo */
			return -1;
		case S_IFREG: /* Regular file: current size - holes + free
				 space on part */
			result = target_storage_available;
			break;
		case S_IFBLK: /* Block device */
			if (!bdev->bd_disk) {
				printk("bdev->bd_disk null.\n");
				return 0;
			}

			result = (bdev->bd_part ?
				bdev->bd_part->nr_sects :
				bdev->bd_disk->capacity) >> (PAGE_SHIFT - 9);
	}

	return adjust_for_extra_pages(result);
}

static int has_contiguous_blocks(int page_num)
{
	int j;
	sector_t last = 0;

	for (j = 0; j < devinfo.blocks_per_page; j++) {
		sector_t this = bmap(target_inode,
				page_num * devinfo.blocks_per_page + j);

		if (!this || (last && (last + 1) != this))
			break;

		last = this;
	}
			
	return (j == devinfo.blocks_per_page);
}

static int size_ignoring_ignored_pages(void)
{
	int mappable = 0, i;
	
	if (!target_is_normal_file())
		return toi_file_storage_available();

	for (i = 0; i < (target_inode->i_size >> PAGE_SHIFT) ; i++)
		if (has_contiguous_blocks(i))
			mappable++;
	
	return mappable;
}

static void __populate_block_list(int min, int max)
{
	if (test_action_state(TOI_TEST_BIO))
		printk("Adding extent %d-%d.\n", min << devinfo.bmap_shift,
		        ((max + 1) << devinfo.bmap_shift) - 1);

	toi_add_to_extent_chain(&block_chain, min, max);
}

static void populate_block_list(void)
{
	int i;
	int extent_min = -1, extent_max = -1, got_header = 0;
	
	if (block_chain.first)
		toi_put_extent_chain(&block_chain);

	if (!target_is_normal_file()) {
		if (target_storage_available > 0)
			__populate_block_list(devinfo.blocks_per_page, 
				(target_storage_available + 1) *
			 	devinfo.blocks_per_page - 1);
		return;
	}

	for (i = 0; i < (target_inode->i_size >> PAGE_SHIFT); i++) {
		sector_t new_sector;

		if (!has_contiguous_blocks(i))
			continue;

		new_sector = bmap(target_inode,
		(i * devinfo.blocks_per_page));

		/* 
		 * Ignore the first block in the file.
		 * It gets the header.
		 */
		if (new_sector == target_firstblock >> devinfo.bmap_shift) {
			got_header = 1;
			continue;
		}

		/* 
		 * I'd love to be able to fill in holes and resize 
		 * files, but not yet...
		 */

		if (new_sector == extent_max + 1)
			extent_max+= devinfo.blocks_per_page;
		else {
			if (extent_min > -1)
				__populate_block_list(extent_min,
						extent_max);

			extent_min = new_sector;
			extent_max = extent_min +
				devinfo.blocks_per_page - 1;
		}
	}

	if (extent_min > -1)
		__populate_block_list(extent_min, extent_max);
}

static void toi_file_cleanup(int finishing_cycle)
{
	if (toi_file_target_bdev) {
		if (target_claim) {
			bd_release(toi_file_target_bdev);
			target_claim = 0;
		}

		if (used_devt) {
			blkdev_put(toi_file_target_bdev);
			used_devt = 0;
		}
		toi_file_target_bdev = NULL;
		target_inode = NULL;
		set_devinfo(NULL, 0);
		target_storage_available = 0;
	}

	if (target_file > 0) {
		filp_close(target_file, NULL);
		target_file = NULL;
	}
}

/* 
 * reopen_resume_devt
 *
 * Having opened resume= once, we remember the major and
 * minor nodes and use them to reopen the bdev for checking
 * whether an image exists (possibly when starting a resume).
 */
static void reopen_resume_devt(void)
{
	toi_file_target_bdev = open_by_devnum(resume_file_dev_t, FMODE_READ);
	if (IS_ERR(toi_file_target_bdev)) {
		printk("Got a dev_num (%lx) but failed to open it.\n",
				(unsigned long) resume_file_dev_t);
		return;
	}
	target_inode = toi_file_target_bdev->bd_inode;
	set_devinfo(toi_file_target_bdev, target_inode->i_blkbits);
}

static void toi_file_get_target_info(char *target, int get_size,
		int resume_param)
{
	if (target_file)
		toi_file_cleanup(0);

	if (!target || !strlen(target))
		return;

	target_file = filp_open(target, O_RDWR|O_LARGEFILE, 0);

	if (IS_ERR(target_file) || !target_file) {

		if (!resume_param) {
			printk("Open file %s returned %p.\n",
					target, target_file);
			target_file = NULL;
			return;
		}

		target_file = NULL;
		resume_file_dev_t = name_to_dev_t(target);
		if (!resume_file_dev_t) {
			struct kstat stat;
			int error = vfs_stat(target, &stat);
			printk("Open file %s returned %p and name_to_devt "
					"failed.\n", target, target_file);
			if (error)
				printk("Stating the file also failed."
					" Nothing more we can do.\n");
			else
				resume_file_dev_t = stat.rdev;
			return;
		}

	     	toi_file_target_bdev = open_by_devnum(resume_file_dev_t,
				FMODE_READ);
		if (IS_ERR(toi_file_target_bdev)) {
			printk("Got a dev_num (%lx) but failed to open it.\n",
					(unsigned long) resume_file_dev_t);
			return;
		}
		used_devt = 1;
		target_inode = toi_file_target_bdev->bd_inode;
	} else
		target_inode = target_file->f_mapping->host;

	if (S_ISLNK(target_inode->i_mode) || S_ISDIR(target_inode->i_mode) ||
	    S_ISSOCK(target_inode->i_mode) || S_ISFIFO(target_inode->i_mode)) {
		printk("File support works with regular files, character "
				"files and block devices.\n");
		goto cleanup;
	}

	if (!used_devt) {
		if (S_ISBLK(target_inode->i_mode)) {
			toi_file_target_bdev = I_BDEV(target_inode);
			if (!bd_claim(toi_file_target_bdev, &toi_fileops))
				target_claim = 1;
		} else
			toi_file_target_bdev = target_inode->i_sb->s_bdev;
		resume_file_dev_t = toi_file_target_bdev->bd_dev;
	}

	set_devinfo(toi_file_target_bdev, target_inode->i_blkbits);

	if (get_size)
		target_storage_available = size_ignoring_ignored_pages();

	if (!resume_param)
		target_firstblock = bmap(target_inode, 0) << devinfo.bmap_shift;
	
	return;
cleanup:
	target_inode = NULL;
	if (target_file) {
		filp_close(target_file, NULL);
		target_file = NULL;
	}
	set_devinfo(NULL, 0);
	target_storage_available = 0;
}

static int parse_signature(struct toi_file_header *header)
{
	int have_image = !memcmp(HaveImage, header->sig, sizeof(HaveImage) - 1);
	int no_image_header = !memcmp(NoImage, header->sig, sizeof(NoImage) - 1);

	if (no_image_header)
		return 0;

	if (!have_image)
		return -1;

	if (header->resumed_before)
		set_toi_state(TOI_RESUMED_BEFORE);
	else
		clear_toi_state(TOI_RESUMED_BEFORE);

	target_header_start = header->first_header_block;
	return 1;
}

/* prepare_signature */

static int prepare_signature(struct toi_file_header *current_header,
		unsigned long first_header_block)
{
	strncpy(current_header->sig, HaveImage, sizeof(HaveImage));
	current_header->resumed_before = 0;
	current_header->first_header_block = first_header_block;
	return 0;
}

static int toi_file_storage_allocated(void)
{
	if (!target_inode)
		return 0;

	if (target_is_normal_file())
		return (int) target_storage_available;
	else
		return header_pages_allocated + main_pages_requested;
}

static int toi_file_release_storage(void)
{
	if (test_action_state(TOI_KEEP_IMAGE) &&
	    test_toi_state(TOI_NOW_RESUMING))
		return 0;

	toi_put_extent_chain(&block_chain);

	header_pages_allocated = 0;
	main_pages_allocated = 0;
	main_pages_requested = 0;
	return 0;
}

static int __toi_file_allocate_storage(int main_storage_requested,
		int header_storage);

static int toi_file_allocate_header_space(int space_requested)
{
	int i;

	if (!block_chain.first && __toi_file_allocate_storage(
				main_pages_requested, space_requested)) {
		printk("Failed to allocate space for the header.\n");
		return -ENOSPC;
	}

	toi_extent_state_goto_start(&toi_writer_posn);
	toi_bio_ops.forward_one_page(); /* To first page */

	for (i = 0; i < space_requested; i++) {
		if (toi_bio_ops.forward_one_page()) {
			printk("Out of space while seeking to allocate "
					"header pages,\n");
			header_pages_allocated = i;
			return -ENOSPC;
		}
	}

	header_pages_allocated = space_requested;

	/* The end of header pages will be the start of pageset 2 */
	toi_extent_state_save(&toi_writer_posn,
			&toi_writer_posn_save[2]);
	return 0;
}

static int toi_file_allocate_storage(int space_requested)
{
	if (__toi_file_allocate_storage(space_requested,
				header_pages_allocated))
		return -ENOSPC;

	main_pages_requested = space_requested;
	return -ENOSPC;
}

static int __toi_file_allocate_storage(int main_space_requested,
		int header_space_requested)
{
	int result = 0;

	int extra_pages = DIV_ROUND_UP(main_space_requested *
			(sizeof(unsigned long) + sizeof(int)), PAGE_SIZE);
	int pages_to_get = main_space_requested + extra_pages +
		header_space_requested;
	int blocks_to_get = pages_to_get - block_chain.size;
	
	/* Only release_storage reduces the size */
	if (blocks_to_get < 1)
		return 0;

	populate_block_list();

	toi_message(TOI_WRITER, TOI_MEDIUM, 0,
		"Finished with block_chain.size == %d.\n",
		block_chain.size);

	if (block_chain.size < pages_to_get) {
		printk("Block chain size (%d) < header pages (%d) + extra pages (%d) + main pages (%d) (=%d pages).\n",
			block_chain.size, header_pages_allocated, extra_pages,
			main_space_requested, pages_to_get);
		result = -ENOSPC;
	}

	main_pages_requested = main_space_requested;
	main_pages_allocated = main_space_requested + extra_pages;

	toi_file_allocate_header_space(header_pages_allocated);
	return result;
}

static int toi_file_write_header_init(void)
{
	toi_extent_state_goto_start(&toi_writer_posn);

	toi_writer_buffer_posn = toi_header_bytes_used = 0;

	/* Info needed to bootstrap goes at the start of the header.
	 * First we save the basic info needed for reading, including the number
	 * of header pages. Then we save the structs containing data needed
	 * for reading the header pages back.
	 * Note that even if header pages take more than one page, when we
	 * read back the info, we will have restored the location of the
	 * next header page by the time we go to use it.
	 */

	toi_bio_ops.rw_header_chunk(WRITE, &toi_fileops,
			(char *) &toi_writer_posn_save, 
			sizeof(toi_writer_posn_save));

	toi_bio_ops.rw_header_chunk(WRITE, &toi_fileops,
			(char *) &devinfo, sizeof(devinfo));

	toi_serialise_extent_chain(&toi_fileops, &block_chain);
	
	return 0;
}

static int toi_file_write_header_cleanup(void)
{
	struct toi_file_header *header;

	/* Write any unsaved data */
	if (toi_writer_buffer_posn)
		toi_bio_ops.write_header_chunk_finish();

	toi_bio_ops.finish_all_io();

	toi_extent_state_goto_start(&toi_writer_posn);
	toi_bio_ops.forward_one_page();

	/* Adjust image header */
	toi_bio_ops.bdev_page_io(READ, toi_file_target_bdev,
			target_firstblock,
			virt_to_page(toi_writer_buffer));

	header = (struct toi_file_header *) toi_writer_buffer;

	prepare_signature(header,
			toi_writer_posn.current_offset <<
			devinfo.bmap_shift);
		
	toi_bio_ops.bdev_page_io(WRITE, toi_file_target_bdev,
			target_firstblock,
			virt_to_page(toi_writer_buffer));

	toi_bio_ops.finish_all_io();

	return 0;
}

/* HEADER READING */

static int file_init(void)
{
	toi_writer_buffer_posn = 0;

	/* Read toi_file configuration */
	toi_bio_ops.bdev_page_io(READ, toi_file_target_bdev,
			target_header_start,
			virt_to_page((unsigned long) toi_writer_buffer));
	
	return 0;
}

/*
 * read_header_init()
 * 
 * Description:
 * 1. Attempt to read the device specified with resume=.
 * 2. Check the contents of the header for our signature.
 * 3. Warn, ignore, reset and/or continue as appropriate.
 * 4. If continuing, read the toi_file configuration section
 *    of the header and set up block device info so we can read
 *    the rest of the header & image.
 *
 * Returns:
 * May not return if user choose to reboot at a warning.
 * -EINVAL if cannot resume at this time. Booting should continue
 * normally.
 */

static int toi_file_read_header_init(void)
{
	int result;
	struct block_device *tmp;

	result = file_init();
	
	if (result) {
		printk("FileAllocator read header init: Failed to initialise "
				"reading the first page of data.\n");
		return result;
	}

	memcpy(&toi_writer_posn_save,
	       toi_writer_buffer + toi_writer_buffer_posn,
	       sizeof(toi_writer_posn_save));
	
	toi_writer_buffer_posn += sizeof(toi_writer_posn_save);

	tmp = devinfo.bdev;

	memcpy(&devinfo,
	       toi_writer_buffer + toi_writer_buffer_posn,
	       sizeof(devinfo));

	devinfo.bdev = tmp;
	toi_writer_buffer_posn += sizeof(devinfo);

	toi_bio_ops.read_header_init();
	toi_extent_state_goto_start(&toi_writer_posn);
	toi_bio_ops.set_extra_page_forward();

	toi_header_bytes_used = toi_writer_buffer_posn;

	return toi_load_extent_chain(&block_chain);
}

static int toi_file_read_header_cleanup(void)
{
	toi_bio_ops.rw_cleanup(READ);
	return 0;
}

static int toi_file_signature_op(int op)
{
	char *cur;
	int result = 0, changed = 0;
	struct toi_file_header *header;
	
	if(toi_file_target_bdev <= 0)
		return -1;

	cur = (char *) get_zeroed_page(TOI_ATOMIC_GFP);
	if (!cur) {
		printk("Unable to allocate a page for reading the image "
				"signature.\n");
		return -ENOMEM;
	}

	toi_bio_ops.bdev_page_io(READ, toi_file_target_bdev,
			target_firstblock,
			virt_to_page(cur));

	header = (struct toi_file_header *) cur;
	result = parse_signature(header);
		
	switch (op) {
		case INVALIDATE:
			if (result == -1)
				goto out;

			strcpy(header->sig, NoImage);
			header->resumed_before = 0;
			result = changed = 1;
			break;
		case MARK_RESUME_ATTEMPTED:
			if (result == 1) {
				header->resumed_before = 1;
				changed = 1;
			}
			break;
		case UNMARK_RESUME_ATTEMPTED:
			if (result == 1) {
				header->resumed_before = 0;
				changed = 1;
			}
			break;
	}

	if (changed)
		toi_bio_ops.bdev_page_io(WRITE, toi_file_target_bdev,
				target_firstblock,
				virt_to_page(cur));

out:
	toi_bio_ops.finish_all_io();
	free_page((unsigned long) cur);
	return result;
}

/* Print debug info
 *
 * Description:
 */

static int toi_file_print_debug_stats(char *buffer, int size)
{
	int len = 0;
	
	if (toiActiveAllocator != &toi_fileops) {
		len = snprintf_used(buffer, size, "- FileAllocator inactive.\n");
		return len;
	}

	len = snprintf_used(buffer, size, "- FileAllocator active.\n");

	len+= snprintf_used(buffer+len, size-len, "  Storage available for image: "
			"%ld pages.\n",
			toi_file_storage_allocated());

	return len;
}

/*
 * Storage needed
 *
 * Returns amount of space in the image header required
 * for the toi_file's data.
 *
 * We ensure the space is allocated, but actually save the
 * data from write_header_init and therefore don't also define a
 * save_config_info routine.
 */
static int toi_file_storage_needed(void)
{
	return sig_size + strlen(toi_file_target) + 1 +
		3 * sizeof(struct extent_iterate_saved_state) +
		sizeof(devinfo) +
		sizeof(struct extent_chain) - 2 * sizeof(void *) +
		(2 * sizeof(unsigned long) * block_chain.num_extents);
}

/* 
 * toi_file_remove_image
 * 
 */
static int toi_file_remove_image(void)
{
	toi_file_release_storage();
	return toi_file_signature_op(INVALIDATE);
}

/*
 * Image_exists
 *
 */

static int toi_file_image_exists(void)
{
	if (!toi_file_target_bdev)
		reopen_resume_devt();

	return toi_file_signature_op(GET_IMAGE_EXISTS);
}

/*
 * Mark resume attempted.
 *
 * Record that we tried to resume from this image.
 */

static void toi_file_mark_resume_attempted(int mark)
{
	toi_file_signature_op(mark ? MARK_RESUME_ATTEMPTED:
		UNMARK_RESUME_ATTEMPTED);
}

static void toi_file_set_resume_param(void)
{
	char *buffer = (char *) get_zeroed_page(TOI_ATOMIC_GFP);
	char *buffer2 = (char *) get_zeroed_page(TOI_ATOMIC_GFP);
	unsigned long sector = bmap(target_inode, 0);
	int offset = 0;

	if (toi_file_target_bdev) {
		set_devinfo(toi_file_target_bdev, target_inode->i_blkbits);

		bdevname(toi_file_target_bdev, buffer2);
		offset += snprintf(buffer + offset, PAGE_SIZE - offset, 
				"/dev/%s", buffer2);
		
		if (sector)
			offset += snprintf(buffer + offset, PAGE_SIZE - offset,
				":0x%lx", sector << devinfo.bmap_shift);
	} else
		offset += snprintf(buffer + offset, PAGE_SIZE - offset,
				"%s is not a valid target.", toi_file_target);
			
	sprintf(resume_file, "file:%s", buffer);

	free_page((unsigned long) buffer);
	free_page((unsigned long) buffer2);

	toi_attempt_to_parse_resume_device(1);
}

static int __test_toi_file_target(char *target, int resume_time, int quiet)
{
	toi_file_get_target_info(target, 0, resume_time);
	if (toi_file_signature_op(GET_IMAGE_EXISTS) > -1) {
		if (!quiet)
			printk("TuxOnIce: FileAllocator: File signature found.\n");
		if (!resume_time)
			toi_file_set_resume_param();
		
		toi_bio_ops.set_devinfo(&devinfo);
		toi_writer_posn.chains = &block_chain;
		toi_writer_posn.num_chains = 1;

		if (!resume_time)
			set_toi_state(TOI_CAN_HIBERNATE);
		return 0;
	}

	clear_toi_state(TOI_CAN_HIBERNATE);

	if (quiet)
		return 1;

	if (*target)
		printk("TuxOnIce: FileAllocator: Sorry. No signature found at"
					" %s.\n", target);
	else
		if (!resume_time)
			printk("TuxOnIce: FileAllocator: Sorry. Target is not"
						" set for hibernating.\n");

	return 1;
}

static void test_toi_file_target(void)
{
	setting_toi_file_target = 1;
       	
	printk("TuxOnIce: Hibernating %sabled.\n",
			__test_toi_file_target(toi_file_target, 0, 1) ?
			"dis" : "en");
	
	setting_toi_file_target = 0;
}

/*
 * Parse Image Location
 *
 * Attempt to parse a resume= parameter.
 * File Allocator accepts:
 * resume=file:DEVNAME[:FIRSTBLOCK]
 *
 * Where:
 * DEVNAME is convertable to a dev_t by name_to_dev_t
 * FIRSTBLOCK is the location of the first block in the file.
 * BLOCKSIZE is the logical blocksize >= SECTOR_SIZE & <= PAGE_SIZE, 
 * mod SECTOR_SIZE == 0 of the device.
 * Data is validated by attempting to read a header from the
 * location given. Failure will result in toi_file refusing to
 * save an image, and a reboot with correct parameters will be
 * necessary.
 */

static int toi_file_parse_sig_location(char *commandline,
		int only_writer, int quiet)
{
	char *thischar, *devstart = NULL, *colon = NULL, *at_symbol = NULL;
	int result = -EINVAL, target_blocksize = 0;

	if (strncmp(commandline, "file:", 5)) {
		if (!only_writer)
			return 1;
	} else
		commandline += 5;

	/* 
	 * Don't check signature again if we're beginning a cycle. If we already
	 * did the initialisation successfully, assume we'll be okay when it comes
	 * to resuming.
	 */
	if (toi_file_target_bdev)
		return 0;
	
	devstart = thischar = commandline;
	while ((*thischar != ':') && (*thischar != '@') &&
		((thischar - commandline) < 250) && (*thischar))
		thischar++;

	if (*thischar == ':') {
		colon = thischar;
		*colon = 0;
		thischar++;
	}

	while ((*thischar != '@') && ((thischar - commandline) < 250) && (*thischar))
		thischar++;

	if (*thischar == '@') {
		at_symbol = thischar;
		*at_symbol = 0;
	}
	
	/* 
	 * For the toi_file, you can be able to resume, but not hibernate,
	 * because the resume= is set correctly, but the toi_file_target
	 * isn't. 
	 *
	 * We may have come here as a result of setting resume or
	 * toi_file_target. We only test the toi_file target in the
	 * former case (it's already done in the later), and we do it before
	 * setting the block number ourselves. It will overwrite the values
	 * given on the command line if we don't.
	 */

	if (!setting_toi_file_target)
		__test_toi_file_target(toi_file_target, 1, 0);

	if (colon)
		target_firstblock = (int) simple_strtoul(colon + 1, NULL, 0);
	else
		target_firstblock = 0;

	if (at_symbol) {
		target_blocksize = (int) simple_strtoul(at_symbol + 1, NULL, 0);
		if (target_blocksize & (SECTOR_SIZE - 1)) {
			printk("FileAllocator: Blocksizes are multiples of %d.\n", SECTOR_SIZE);
			result = -EINVAL;
			goto out;
		}
	}
	
	if (!quiet)
		printk("TuxOnIce FileAllocator: Testing whether you can resume:\n");

	toi_file_get_target_info(commandline, 0, 1);

	if (!toi_file_target_bdev || IS_ERR(toi_file_target_bdev)) {
		toi_file_target_bdev = NULL;
		result = -1;
		goto out;
	}

	if (target_blocksize)
		set_devinfo(toi_file_target_bdev, ffs(target_blocksize));

	result = __test_toi_file_target(commandline, 1, 0);

out:
	if (result)
		clear_toi_state(TOI_CAN_HIBERNATE);

	if (!quiet)
		printk("Resuming %sabled.\n",  result ? "dis" : "en");

	if (colon)
		*colon = ':';
	if (at_symbol)
		*at_symbol = '@';

	return result;
}

/* toi_file_save_config_info
 *
 * Description:	Save the target's name, not for resume time, but for all_settings.
 * Arguments:	Buffer:		Pointer to a buffer of size PAGE_SIZE.
 * Returns:	Number of bytes used for saving our data.
 */

static int toi_file_save_config_info(char *buffer)
{
	strcpy(buffer, toi_file_target);
	return strlen(toi_file_target) + 1;
}

/* toi_file_load_config_info
 *
 * Description:	Reload target's name.
 * Arguments:	Buffer:		Pointer to the start of the data.
 *		Size:		Number of bytes that were saved.
 */

static void toi_file_load_config_info(char *buffer, int size)
{
	strcpy(toi_file_target, buffer);
}

static int toi_file_initialise(int starting_cycle)
{
	if (starting_cycle) {
		if (toiActiveAllocator != &toi_fileops)
			return 0;

		if (starting_cycle & SYSFS_HIBERNATE && !*toi_file_target) {
			printk("FileAllocator is the active writer,  "
					"but no filename has been set.\n");
			return 1;
		}
	}

	if (toi_file_target)
		toi_file_get_target_info(toi_file_target, starting_cycle, 0);

	if (starting_cycle && (toi_file_image_exists() == -1)) {
		printk("%s is does not have a valid signature for hibernating.\n",
				toi_file_target);
		return 1;
	}

	return 0;
}

static struct toi_sysfs_data sysfs_params[] = {

	{
	 TOI_ATTR("target", SYSFS_RW),
	 SYSFS_STRING(toi_file_target, 256, SYSFS_NEEDS_SM_FOR_WRITE),
	 .write_side_effect		= test_toi_file_target,
	},

	{
	  TOI_ATTR("enabled", SYSFS_RW),
	  SYSFS_INT(&toi_fileops.enabled, 0, 1, 0),
	  .write_side_effect		= attempt_to_parse_resume_device2,
	}
};

static struct toi_module_ops toi_fileops = {
	.type					= WRITER_MODULE,
	.name					= "file storage",
	.directory				= "file",
	.module					= THIS_MODULE,
	.print_debug_info			= toi_file_print_debug_stats,
	.save_config_info			= toi_file_save_config_info,
	.load_config_info			= toi_file_load_config_info,
	.storage_needed				= toi_file_storage_needed,
	.initialise				= toi_file_initialise,
	.cleanup				= toi_file_cleanup,

	.storage_available 	= toi_file_storage_available,
	.storage_allocated	= toi_file_storage_allocated,
	.release_storage	= toi_file_release_storage,
	.allocate_header_space	= toi_file_allocate_header_space,
	.allocate_storage	= toi_file_allocate_storage,
	.image_exists		= toi_file_image_exists,
	.mark_resume_attempted	= toi_file_mark_resume_attempted,
	.write_header_init	= toi_file_write_header_init,
	.write_header_cleanup	= toi_file_write_header_cleanup,
	.read_header_init	= toi_file_read_header_init,
	.read_header_cleanup	= toi_file_read_header_cleanup,
	.remove_image		= toi_file_remove_image,
	.parse_sig_location	= toi_file_parse_sig_location,

	.sysfs_data		= sysfs_params,
	.num_sysfs_entries	= sizeof(sysfs_params) / sizeof(struct toi_sysfs_data),
};

/* ---- Registration ---- */
static __init int toi_file_load(void)
{
	toi_fileops.rw_init = toi_bio_ops.rw_init;
	toi_fileops.rw_cleanup = toi_bio_ops.rw_cleanup;
	toi_fileops.read_page = toi_bio_ops.read_page;
	toi_fileops.write_page = toi_bio_ops.write_page;
	toi_fileops.rw_header_chunk = toi_bio_ops.rw_header_chunk;

	return toi_register_module(&toi_fileops);
}

#ifdef MODULE
static __exit void toi_file_unload(void)
{
	toi_unregister_module(&toi_fileops);
}

module_init(toi_file_load);
module_exit(toi_file_unload);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Nigel Cunningham");
MODULE_DESCRIPTION("TuxOnIce FileAllocator");
#else
late_initcall(toi_file_load);
#endif
