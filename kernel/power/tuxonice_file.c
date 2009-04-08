/*
 * kernel/power/tuxonice_file.c
 *
 * Copyright (C) 2005-2008 Nigel Cunningham (nigel at tuxonice net)
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
#include "tuxonice_alloc.h"
#include "tuxonice_builtin.h"

static struct toi_module_ops toi_fileops;

/* Details of our target.  */

static char toi_file_target[256];
static struct inode *target_inode;
static struct file *target_file;
static struct block_device *toi_file_target_bdev;
static dev_t resume_file_dev_t;
static int used_devt;
static int setting_toi_file_target;
static sector_t target_firstblock, target_header_start;
static int target_storage_available;
static int target_claim;

/* Old signatures */
static char HaveImage[] = "HaveImage\n";
static char NoImage[] =   "TuxOnIce\n";
#define sig_size (sizeof(HaveImage) + 1)

struct toi_file_header {
	char sig[sig_size];
	int resumed_before;
	unsigned long first_header_block;
	int have_image;
	int devinfo_sz;
	int num_nodes;
	int num_zones;
};

/* Header Page Information */
static int header_pages_reserved;

/* Main Storage Pages */
static int main_pages_allocated, main_pages_requested;

#define target_is_normal_file() (S_ISREG(target_inode->i_mode))

static struct toi_bdev_info devinfo;

/* Extent chain for blocks */
static struct hibernate_extent_chain block_chain;

/* Signature operations */
enum {
	GET_IMAGE_EXISTS,
	INVALIDATE,
	MARK_RESUME_ATTEMPTED,
	UNMARK_RESUME_ATTEMPTED,
};

/**
 * set_devinfo - populate device information
 * @bdev:		Block device on which the file is.
 * @target_blkbits:	Number of bits in the page block size of the target
 *			file inode.
 *
 * Populate the devinfo structure about the target device.
 *
 * Background: a sector represents a fixed amount of data (generally 512 bytes).
 * The hard drive sector size and the filesystem block size may be different.
 * If fs_blksize mesures the filesystem block size and hd_blksize the hard drive
 * sector size:
 *
 * sector << (fs_blksize - hd_blksize) converts hd sector into fs block
 * fs_block >> (fs_blksize - hd_blksize) converts fs block into hd sector number
 *
 * Here target_blkbits == fs_blksize and hd_blksize == 9, hence:
 *
 *	(fs_blksize - hd_blksize) == devinfo.bmap_shift
 *
 * The memory page size is defined by PAGE_SHIFT. devinfo.blocks_per_page is the
 * number of filesystem blocks per memory page.
 *
 * Note that blocks are stored after >>. They are used after being <<.
 * We always only use PAGE_SIZE aligned blocks.
 *
 * Side effects:
 *	devinfo.bdev, devinfo.bmap_shift and devinfo.blocks_per_page are set.
 */
static void set_devinfo(struct block_device *bdev, int target_blkbits)
{
	devinfo.bdev = bdev;
	if (!target_blkbits) {
		devinfo.bmap_shift = 0;
		devinfo.blocks_per_page = 0;
	} else {
		/* We are assuming a hard disk with 512 (2^9) bytes/sector */
		devinfo.bmap_shift = target_blkbits - 9;
		devinfo.blocks_per_page = (1 << (PAGE_SHIFT - target_blkbits));
	}
}

static long raw_to_real(long raw)
{
	long result;

	result = raw - (raw * (sizeof(unsigned long) + sizeof(int)) +
		(PAGE_SIZE + sizeof(unsigned long) + sizeof(int) + 1)) /
		(PAGE_SIZE + sizeof(unsigned long) + sizeof(int));

	return result < 0 ? 0 : result;
}

static int toi_file_storage_available(void)
{
	int result = 0;
	struct block_device *bdev = toi_file_target_bdev;

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
			printk(KERN_INFO "bdev->bd_disk null.\n");
			return 0;
		}

		result = (bdev->bd_part ?
			bdev->bd_part->nr_sects :
			get_capacity(bdev->bd_disk)) >> (PAGE_SHIFT - 9);
	}

	return raw_to_real(result);
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

	return j == devinfo.blocks_per_page;
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

/**
 * __populate_block_list - add an extent to the chain
 * @min:	Start of the extent (first physical block = sector)
 * @max:	End of the extent (last physical block = sector)
 *
 * If TOI_TEST_BIO is set, print a debug message, outputting the min and max
 * fs block numbers.
 **/
static int __populate_block_list(int min, int max)
{
	if (test_action_state(TOI_TEST_BIO))
		printk(KERN_INFO "Adding extent %d-%d.\n",
			min << devinfo.bmap_shift,
			((max + 1) << devinfo.bmap_shift) - 1);

	return toi_add_to_extent_chain(&block_chain, min, max);
}

static int apply_header_reservation(void)
{
	int i;

	/* Apply header space reservation */
	toi_extent_state_goto_start(&toi_writer_posn);

	for (i = 0; i < header_pages_reserved; i++)
		if (toi_bio_ops.forward_one_page(1, 0))
			return -ENOSPC;

	/* The end of header pages will be the start of pageset 2 */
	toi_extent_state_save(&toi_writer_posn, &toi_writer_posn_save[2]);

	return 0;
}

static int populate_block_list(void)
{
	int i, extent_min = -1, extent_max = -1, got_header = 0, result = 0;

	if (block_chain.first)
		toi_put_extent_chain(&block_chain);

	if (!target_is_normal_file()) {
		result = (target_storage_available > 0) ?
			__populate_block_list(devinfo.blocks_per_page,
				(target_storage_available + 1) *
				devinfo.blocks_per_page - 1) : 0;
		if (result)
			return result;
		goto out;
	}

	for (i = 0; i < (target_inode->i_size >> PAGE_SHIFT); i++) {
		sector_t new_sector;

		if (!has_contiguous_blocks(i))
			continue;

		new_sector = bmap(target_inode, (i * devinfo.blocks_per_page));

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
			extent_max += devinfo.blocks_per_page;
		else {
			if (extent_min > -1) {
				result = __populate_block_list(extent_min,
						extent_max);
				if (result)
					return result;
			}

			extent_min = new_sector;
			extent_max = extent_min +
				devinfo.blocks_per_page - 1;
		}
	}

	if (extent_min > -1) {
		result = __populate_block_list(extent_min, extent_max);
		if (result)
			return result;
	}

out:
	return apply_header_reservation();
}

static void toi_file_cleanup(int finishing_cycle)
{
	if (toi_file_target_bdev) {
		if (target_claim) {
			bd_release(toi_file_target_bdev);
			target_claim = 0;
		}

		if (used_devt) {
			blkdev_put(toi_file_target_bdev,
					FMODE_READ | FMODE_NDELAY);
			used_devt = 0;
		}
		toi_file_target_bdev = NULL;
		target_inode = NULL;
		set_devinfo(NULL, 0);
		target_storage_available = 0;
	}

	if (target_file && !IS_ERR(target_file))
		filp_close(target_file, NULL);

	target_file = NULL;
}

/**
 * reopen_resume_devt - reset the devinfo struct
 *
 * Having opened resume= once, we remember the major and
 * minor nodes and use them to reopen the bdev for checking
 * whether an image exists (possibly when starting a resume).
 **/
static void reopen_resume_devt(void)
{
	toi_file_target_bdev = toi_open_by_devnum(resume_file_dev_t,
			FMODE_READ | FMODE_NDELAY);
	if (IS_ERR(toi_file_target_bdev)) {
		printk(KERN_INFO "Got a dev_num (%lx) but failed to open it.\n",
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

	target_file = filp_open(target, O_RDONLY|O_LARGEFILE, 0);

	if (IS_ERR(target_file) || !target_file) {

		if (!resume_param) {
			printk(KERN_INFO "Open file %s returned %p.\n",
					target, target_file);
			target_file = NULL;
			return;
		}

		target_file = NULL;
		wait_for_device_probe();
		resume_file_dev_t = name_to_dev_t(target);
		if (!resume_file_dev_t) {
			struct kstat stat;
			int error = vfs_stat(target, &stat);
			printk(KERN_INFO "Open file %s returned %p and "
					"name_to_devt failed.\n", target,
					target_file);
			if (error)
				printk(KERN_INFO "Stating the file also failed."
					" Nothing more we can do.\n");
			else
				resume_file_dev_t = stat.rdev;
			return;
		}

		toi_file_target_bdev = toi_open_by_devnum(resume_file_dev_t,
				FMODE_READ | FMODE_NDELAY);
		if (IS_ERR(toi_file_target_bdev)) {
			printk(KERN_INFO "Got a dev_num (%lx) but failed to "
					"open it.\n",
					(unsigned long) resume_file_dev_t);
			return;
		}
		used_devt = 1;
		target_inode = toi_file_target_bdev->bd_inode;
	} else
		target_inode = target_file->f_mapping->host;

	if (S_ISLNK(target_inode->i_mode) || S_ISDIR(target_inode->i_mode) ||
	    S_ISSOCK(target_inode->i_mode) || S_ISFIFO(target_inode->i_mode)) {
		printk(KERN_INFO "File support works with regular files,"
				" character files and block devices.\n");
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

static void toi_file_noresume_reset(void)
{
	toi_bio_ops.rw_cleanup(READ);
}

/**
 * parse_signature - check if the file is suitable for resuming
 * @header:	Signature of the file
 *
 * Given a file header, check the content of the file. Return true if it
 * contains a valid hibernate image.
 * TOI_RESUMED_BEFORE is set accordingly.
 **/
static int parse_signature(struct toi_file_header *header)
{
	int have_image = !memcmp(HaveImage, header->sig, sizeof(HaveImage) - 1);
	int no_image_header = !memcmp(NoImage, header->sig,
			sizeof(NoImage) - 1);
	int binary_sig = !memcmp(tuxonice_signature, header->sig,
			sizeof(tuxonice_signature));

	if (no_image_header || (binary_sig && !header->have_image))
		return 0;

	if (!have_image && !binary_sig)
		return -1;

	if (header->resumed_before)
		set_toi_state(TOI_RESUMED_BEFORE);
	else
		clear_toi_state(TOI_RESUMED_BEFORE);

	target_header_start = header->first_header_block;
	return 1;
}

/**
 * prepare_signature - populate the signature structure
 * @current_header:	Signature structure to populate
 * @first_header_block:	Sector with the header containing the extents
 **/
static int prepare_signature(struct toi_file_header *current_header,
		unsigned long first_header_block)
{
	memcpy(current_header->sig, tuxonice_signature,
			sizeof(tuxonice_signature));
	current_header->resumed_before = 0;
	current_header->first_header_block = first_header_block;
	current_header->have_image = 1;
	current_header->devinfo_sz = sizeof(devinfo);
	current_header->num_nodes = MAX_NUMNODES;
	current_header->num_zones = MAX_NR_ZONES;
	return 0;
}

static int toi_file_storage_allocated(void)
{
	if (!target_inode)
		return 0;

	if (target_is_normal_file())
		return (int) raw_to_real(target_storage_available);
	else
		return (int) raw_to_real(main_pages_requested);
}

/**
 * toi_file_release_storage - deallocate the block chain
 **/
static int toi_file_release_storage(void)
{
	toi_put_extent_chain(&block_chain);

	header_pages_reserved = 0;
	main_pages_allocated = 0;
	main_pages_requested = 0;
	return 0;
}

static void toi_file_reserve_header_space(int request)
{
	header_pages_reserved = request;
}

static int toi_file_allocate_storage(int main_space_requested)
{
	int result = 0;

	int extra_pages = DIV_ROUND_UP(main_space_requested *
			(sizeof(unsigned long) + sizeof(int)), PAGE_SIZE);
	int pages_to_get = main_space_requested + extra_pages +
		header_pages_reserved;
	int blocks_to_get = pages_to_get - block_chain.size;

	/* Only release_storage reduces the size */
	if (blocks_to_get < 1)
		return apply_header_reservation();

	result = populate_block_list();

	if (result)
		return result;

	toi_message(TOI_WRITER, TOI_MEDIUM, 0,
		"Finished with block_chain.size == %d.\n",
		block_chain.size);

	if (block_chain.size < pages_to_get) {
		printk(KERN_INFO "Block chain size (%d) < header pages (%d) + "
				 "extra pages (%d) + main pages (%d) (=%d "
				 "pages).\n",
				 block_chain.size, header_pages_reserved,
				 extra_pages, main_space_requested,
				 pages_to_get);
		result = -ENOSPC;
	}

	main_pages_requested = main_space_requested;
	main_pages_allocated = main_space_requested + extra_pages;
	return result;
}

/**
 * toi_file_write_header_init - save the header on the image
 **/
static int toi_file_write_header_init(void)
{
	int result;

	toi_bio_ops.rw_init(WRITE, 0);
	toi_writer_buffer_posn = 0;

	/* Info needed to bootstrap goes at the start of the header.
	 * First we save the basic info needed for reading, including the number
	 * of header pages. Then we save the structs containing data needed
	 * for reading the header pages back.
	 * Note that even if header pages take more than one page, when we
	 * read back the info, we will have restored the location of the
	 * next header page by the time we go to use it.
	 */

	result = toi_bio_ops.rw_header_chunk(WRITE, &toi_fileops,
			(char *) &toi_writer_posn_save,
			sizeof(toi_writer_posn_save));

	if (result)
		return result;

	result = toi_bio_ops.rw_header_chunk(WRITE, &toi_fileops,
			(char *) &devinfo, sizeof(devinfo));

	if (result)
		return result;

	/* Flush the chain */
	toi_serialise_extent_chain(&toi_fileops, &block_chain);

	return 0;
}

static int toi_file_write_header_cleanup(void)
{
	struct toi_file_header *header;
	int result, result2;
	unsigned long sig_page = toi_get_zeroed_page(38, TOI_ATOMIC_GFP);

	/* Write any unsaved data */
	result = toi_bio_ops.write_header_chunk_finish();

	if (result)
		goto out;

	toi_extent_state_goto_start(&toi_writer_posn);
	toi_bio_ops.forward_one_page(1, 1);

	/* Adjust image header */
	result = toi_bio_ops.bdev_page_io(READ, toi_file_target_bdev,
			target_firstblock,
			virt_to_page(sig_page));
	if (result)
		goto out;

	header = (struct toi_file_header *) sig_page;

	prepare_signature(header,
			toi_writer_posn.current_offset <<
			devinfo.bmap_shift);

	result = toi_bio_ops.bdev_page_io(WRITE, toi_file_target_bdev,
			target_firstblock,
			virt_to_page(sig_page));

out:
	result2 = toi_bio_ops.finish_all_io();
	toi_free_page(38, sig_page);

	return result ? result : result2;
}

/* HEADER READING */

/**
 * toi_file_read_header_init - check content of signature
 *
 * Entry point of the resume path.
 * 1. Attempt to read the device specified with resume=.
 * 2. Check the contents of the header for our signature.
 * 3. Warn, ignore, reset and/or continue as appropriate.
 * 4. If continuing, read the toi_file configuration section
 *    of the header and set up block device info so we can read
 *    the rest of the header & image.
 *
 * Returns:
 *	May not return if user choose to reboot at a warning.
 *	-EINVAL if cannot resume at this time. Booting should continue
 *	normally.
 **/
static int toi_file_read_header_init(void)
{
	int result;
	struct block_device *tmp;

	/* Allocate toi_writer_buffer */
	toi_bio_ops.read_header_init();

	/*
	 * Read toi_file configuration (header containing metadata).
	 * target_header_start is the first sector of the header. It has been
	 * set when checking if the file was suitable for resuming, see
	 * do_toi_step(STEP_RESUME_CAN_RESUME).
	 */
	result = toi_bio_ops.bdev_page_io(READ, toi_file_target_bdev,
			target_header_start,
			virt_to_page((unsigned long) toi_writer_buffer));

	if (result) {
		printk(KERN_ERR "FileAllocator read header init: Failed to "
				"initialise reading the first page of data.\n");
		toi_bio_ops.rw_cleanup(READ);
		return result;
	}

	/* toi_writer_posn_save[0] contains the header */
	memcpy(&toi_writer_posn_save, toi_writer_buffer,
	       sizeof(toi_writer_posn_save));

	/* Save the position in the buffer */
	toi_writer_buffer_posn = sizeof(toi_writer_posn_save);

	tmp = devinfo.bdev;

	/* See tuxonice_block_io.h */
	memcpy(&devinfo,
	       toi_writer_buffer + toi_writer_buffer_posn,
	       sizeof(devinfo));

	devinfo.bdev = tmp;
	toi_writer_buffer_posn += sizeof(devinfo);

	/* Reinitialize the extent pointer */
	toi_extent_state_goto_start(&toi_writer_posn);
	/* Jump to the next page */
	toi_bio_ops.set_extra_page_forward();

	/* Bring back the chain from disk: this will read
	 * all extents.
	 */
	return toi_load_extent_chain(&block_chain);
}

static int toi_file_read_header_cleanup(void)
{
	toi_bio_ops.rw_cleanup(READ);
	return 0;
}

/**
 * toi_file_signature_op - perform an operation on the file signature
 * @op:	operation to perform
 *
 * op is either GET_IMAGE_EXISTS, INVALIDATE, MARK_RESUME_ATTEMPTED or
 * UNMARK_RESUME_ATTEMPTED.
 * If the signature is changed, an I/O operation is performed.
 * The signature exists iff toi_file_signature_op(GET_IMAGE_EXISTS)>-1.
 **/
static int toi_file_signature_op(int op)
{
	char *cur;
	int result = 0, result2, changed = 0;
	struct toi_file_header *header;

	if (!toi_file_target_bdev || IS_ERR(toi_file_target_bdev))
		return -1;

	cur = (char *) toi_get_zeroed_page(17, TOI_ATOMIC_GFP);
	if (!cur) {
		printk(KERN_INFO "Unable to allocate a page for reading the "
				 "image signature.\n");
		return -ENOMEM;
	}

	result = toi_bio_ops.bdev_page_io(READ, toi_file_target_bdev,
			target_firstblock,
			virt_to_page(cur));

	if (result)
		goto out;

	header = (struct toi_file_header *) cur;
	result = parse_signature(header);

	switch (op) {
	case INVALIDATE:
		if (result == -1)
			goto out;

		memcpy(header->sig, tuxonice_signature,
				sizeof(tuxonice_signature));
		header->resumed_before = 0;
		header->have_image = 0;
		result = 1;
		changed = 1;
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

	if (changed) {
		int io_result = toi_bio_ops.bdev_page_io(WRITE,
				toi_file_target_bdev, target_firstblock,
				virt_to_page(cur));
		if (io_result)
			result = io_result;
	}

out:
	result2 = toi_bio_ops.finish_all_io();
	toi_free_page(17, (unsigned long) cur);
	return result ? result : result2;
}

/**
 * toi_file_print_debug_stats - print debug info
 * @buffer:	Buffer to data to populate
 * @size:	Size of the buffer
 **/
static int toi_file_print_debug_stats(char *buffer, int size)
{
	int len = 0;

	if (toiActiveAllocator != &toi_fileops) {
		len = scnprintf(buffer, size,
				"- FileAllocator inactive.\n");
		return len;
	}

	len = scnprintf(buffer, size, "- FileAllocator active.\n");

	len += scnprintf(buffer+len, size-len, "  Storage available for "
			"image: %d pages.\n",
			toi_file_storage_allocated());

	return len;
}

/**
 * toi_file_storage_needed - storage needed
 *
 * Returns amount of space in the image header required
 * for the toi_file's data.
 *
 * We ensure the space is allocated, but actually save the
 * data from write_header_init and therefore don't also define a
 * save_config_info routine.
 **/
static int toi_file_storage_needed(void)
{
	return strlen(toi_file_target) + 1 +
		sizeof(toi_writer_posn_save) +
		sizeof(devinfo) +
		2 * sizeof(int) +
		(2 * sizeof(unsigned long) * block_chain.num_extents);
}

/**
 * toi_file_remove_image - invalidate the image
 **/
static int toi_file_remove_image(void)
{
	toi_file_release_storage();
	return toi_file_signature_op(INVALIDATE);
}

/**
 * toi_file_image_exists - test if an image exists
 *
 * Repopulate toi_file_target_bdev if needed.
 **/
static int toi_file_image_exists(int quiet)
{
	if (!toi_file_target_bdev)
		reopen_resume_devt();
	return toi_file_signature_op(GET_IMAGE_EXISTS);
}

/**
 * toi_file_mark_resume_attempted - mark resume attempted if so
 * @mark:	attempted flag
 *
 * Record that we tried to resume from this image. Resuming
 * multiple times from the same image may be dangerous
 * (possible filesystem corruption).
 **/
static int toi_file_mark_resume_attempted(int mark)
{
	return toi_file_signature_op(mark ? MARK_RESUME_ATTEMPTED :
		UNMARK_RESUME_ATTEMPTED);
}

/**
 * toi_file_set_resume_param - validate the specified resume file
 *
 * Given a target filename, populate the resume parameter. This is
 * meant to be used by the user to populate the kernel command line.
 * By setting /sys/power/tuxonice/file/target, the valid resume
 * parameter to use is set and accessible through
 * /sys/power/tuxonice/resume.
 *
 * If the file could be located, we check if it contains a valid
 * signature.
 **/
static void toi_file_set_resume_param(void)
{
	char *buffer = (char *) toi_get_zeroed_page(18, TOI_ATOMIC_GFP);
	char *buffer2 = (char *) toi_get_zeroed_page(19, TOI_ATOMIC_GFP);
	unsigned long sector = bmap(target_inode, 0);
	int offset = 0;

	if (!buffer || !buffer2) {
		if (buffer)
			toi_free_page(18, (unsigned long) buffer);
		if (buffer2)
			toi_free_page(19, (unsigned long) buffer2);
		printk(KERN_ERR "TuxOnIce: Failed to allocate memory while "
				"setting resume= parameter.\n");
		return;
	}

	if (toi_file_target_bdev) {
		set_devinfo(toi_file_target_bdev, target_inode->i_blkbits);

		bdevname(toi_file_target_bdev, buffer2);
		offset += snprintf(buffer + offset, PAGE_SIZE - offset,
				"/dev/%s", buffer2);

		if (sector)
			/* The offset is: sector << (inode->i_blkbits - 9) */
			offset += snprintf(buffer + offset, PAGE_SIZE - offset,
				":0x%lx", sector << devinfo.bmap_shift);
	} else
		offset += snprintf(buffer + offset, PAGE_SIZE - offset,
				"%s is not a valid target.", toi_file_target);

	sprintf(resume_file, "file:%s", buffer);

	toi_free_page(18, (unsigned long) buffer);
	toi_free_page(19, (unsigned long) buffer2);

	toi_attempt_to_parse_resume_device(1);
}

/**
 * __test_toi_file_target - is the file target valid for hibernating?
 * @target:		target file
 * @resume_param:	whether resume= has been specified
 * @quiet:		quiet flag
 *
 * Test whether the file target can be used for hibernating: valid target
 * and signature.
 * The resume parameter is set if needed.
 **/
static int __test_toi_file_target(char *target, int resume_param, int quiet)
{
	toi_file_get_target_info(target, 0, resume_param);
	if (toi_file_signature_op(GET_IMAGE_EXISTS) > -1) {
		if (!quiet)
			printk(KERN_INFO "TuxOnIce: FileAllocator: File "
					 "signature found.\n");
		if (!resume_param)
			toi_file_set_resume_param();

		toi_bio_ops.set_devinfo(&devinfo);
		toi_writer_posn.chains = &block_chain;
		toi_writer_posn.num_chains = 1;

		if (!resume_param)
			set_toi_state(TOI_CAN_HIBERNATE);
		return 0;
	}

	/*
	 * Target unaccessible or no signature found
	 * Most errors have already been reported
	 */

	clear_toi_state(TOI_CAN_HIBERNATE);

	if (quiet)
		return 1;

	if (*target)
		printk(KERN_INFO "TuxOnIce: FileAllocator: Sorry. No signature "
				 "found at  %s.\n", target);
	else
		if (!resume_param)
			printk(KERN_INFO "TuxOnIce: FileAllocator: Sorry. "
					"Target is not set for hibernating.\n");

	return 1;
}

/**
 * test_toi_file_target - sysfs callback for /sys/power/tuxonince/file/target
 *
 * Test wheter the target file is valid for hibernating.
 **/
static void test_toi_file_target(void)
{
	setting_toi_file_target = 1;

	printk(KERN_INFO "TuxOnIce: Hibernating %sabled.\n",
			__test_toi_file_target(toi_file_target, 0, 1) ?
			"dis" : "en");

	setting_toi_file_target = 0;
}

/**
 * toi_file_parse_sig_location - parse image Location
 * @commandline:	the resume parameter
 * @only_writer:	??
 * @quiet:		quiet flag
 *
 * Attempt to parse a resume= parameter.
 * File Allocator accepts:
 *	resume=file:DEVNAME[:FIRSTBLOCK]
 *
 * Where:
 *	DEVNAME is convertable to a dev_t by name_to_dev_t
 *	FIRSTBLOCK is the location of the first block in the file.
 *	BLOCKSIZE is the logical blocksize >= SECTOR_SIZE &
 *					<= PAGE_SIZE,
 *	mod SECTOR_SIZE == 0 of the device.
 *
 * Data is validated by attempting to read a header from the
 * location given. Failure will result in toi_file refusing to
 * save an image, and a reboot with correct parameters will be
 * necessary.
 **/
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
	 * did the initialisation successfully, assume we'll be okay when it
	 * comes to resuming.
	 */
	if (toi_file_target_bdev)
		return 0;

	devstart = commandline;
	thischar = commandline;
	while ((*thischar != ':') && (*thischar != '@') &&
		((thischar - commandline) < 250) && (*thischar))
		thischar++;

	if (*thischar == ':') {
		colon = thischar;
		*colon = 0;
		thischar++;
	}

	while ((*thischar != '@') && ((thischar - commandline) < 250)
			&& (*thischar))
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

	if (!setting_toi_file_target) /* Concurrent write via /sys? */
		__test_toi_file_target(toi_file_target, 1, 0);

	if (colon)
		target_firstblock = (int) simple_strtoul(colon + 1, NULL, 0);
	else
		target_firstblock = 0;

	if (at_symbol) {
		target_blocksize = (int) simple_strtoul(at_symbol + 1, NULL, 0);
		if (target_blocksize & (SECTOR_SIZE - 1)) {
			printk(KERN_INFO "FileAllocator: Blocksizes are "
					 "multiples of %d.\n", SECTOR_SIZE);
			result = -EINVAL;
			goto out;
		}
	}

	if (!quiet)
		printk(KERN_INFO "TuxOnIce FileAllocator: Testing whether you "
				 "can resume:\n");

	toi_file_get_target_info(commandline, 0, 1);

	if (!toi_file_target_bdev || IS_ERR(toi_file_target_bdev)) {
		toi_file_target_bdev = NULL;
		result = -1;
		goto out;
	}

	if (target_blocksize)
		set_devinfo(toi_file_target_bdev, ffs(target_blocksize));

	result = __test_toi_file_target(commandline, 1, quiet);

out:
	if (result)
		clear_toi_state(TOI_CAN_HIBERNATE);

	if (!quiet)
		printk(KERN_INFO "Resuming %sabled.\n",  result ? "dis" : "en");

	if (colon)
		*colon = ':';
	if (at_symbol)
		*at_symbol = '@';

	return result;
}

/**
 * toi_file_save_config_info - populate toi_file_target
 * @buffer:	Pointer to a buffer of size PAGE_SIZE.
 *
 * Save the target's name, not for resume time, but for
 * all_settings.
 * Returns:
 *	Number of bytes used for saving our data.
 **/
static int toi_file_save_config_info(char *buffer)
{
	strcpy(buffer, toi_file_target);
	return strlen(toi_file_target) + 1;
}

/**
 * toi_file_load_config_info - reload target's name
 * @buffer:	pointer to the start of the data
 * @size:	number of bytes that were saved
 *
 * toi_file_target is set to buffer.
 **/
static void toi_file_load_config_info(char *buffer, int size)
{
	strlcpy(toi_file_target, buffer, size);
}

static int toi_file_initialise(int starting_cycle)
{
	if (starting_cycle) {
		if (toiActiveAllocator != &toi_fileops)
			return 0;

		if (starting_cycle & SYSFS_HIBERNATE && !*toi_file_target) {
			printk(KERN_INFO "FileAllocator is the active writer,  "
					"but no filename has been set.\n");
			return 1;
		}
	}

	if (*toi_file_target)
		toi_file_get_target_info(toi_file_target, starting_cycle, 0);

	if (starting_cycle && (toi_file_image_exists(1) == -1)) {
		printk("%s is does not have a valid signature for "
				"hibernating.\n", toi_file_target);
		return 1;
	}

	return 0;
}

static struct toi_sysfs_data sysfs_params[] = {

	SYSFS_STRING("target", SYSFS_RW, toi_file_target, 256,
		SYSFS_NEEDS_SM_FOR_WRITE, test_toi_file_target),
	SYSFS_INT("enabled", SYSFS_RW, &toi_fileops.enabled, 0, 1, 0,
		attempt_to_parse_resume_device2)
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

	.noresume_reset		= toi_file_noresume_reset,
	.storage_available 	= toi_file_storage_available,
	.storage_allocated	= toi_file_storage_allocated,
	.reserve_header_space	= toi_file_reserve_header_space,
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
	.num_sysfs_entries	= sizeof(sysfs_params) /
		sizeof(struct toi_sysfs_data),
};

/* ---- Registration ---- */
static __init int toi_file_load(void)
{
	toi_fileops.rw_init = toi_bio_ops.rw_init;
	toi_fileops.rw_cleanup = toi_bio_ops.rw_cleanup;
	toi_fileops.read_page = toi_bio_ops.read_page;
	toi_fileops.write_page = toi_bio_ops.write_page;
	toi_fileops.rw_header_chunk = toi_bio_ops.rw_header_chunk;
	toi_fileops.rw_header_chunk_noreadahead =
		toi_bio_ops.rw_header_chunk_noreadahead;
	toi_fileops.io_flusher = toi_bio_ops.io_flusher;
	toi_fileops.update_throughput_throttle =
		toi_bio_ops.update_throughput_throttle;
	toi_fileops.finish_all_io = toi_bio_ops.finish_all_io;

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
