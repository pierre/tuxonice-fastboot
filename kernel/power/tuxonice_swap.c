/*
 * kernel/power/tuxonice_swap.c
 *
 * Copyright (C) 2004-2008 Nigel Cunningham (nigel at tuxonice net)
 *
 * Distributed under GPLv2.
 *
 * This file encapsulates functions for usage of swap space as a
 * backing store.
 */

#include <linux/suspend.h>
#include <linux/blkdev.h>
#include <linux/swapops.h>
#include <linux/swap.h>
#include <linux/syscalls.h>

#include "tuxonice.h"
#include "tuxonice_sysfs.h"
#include "tuxonice_modules.h"
#include "tuxonice_io.h"
#include "tuxonice_ui.h"
#include "tuxonice_extent.h"
#include "tuxonice_block_io.h"
#include "tuxonice_alloc.h"
#include "tuxonice_builtin.h"

static struct toi_module_ops toi_swapops;

/* --- Struct of pages stored on disk */

struct sig_data {
	dev_t device;
	unsigned long sector;
	int resume_attempted;
	int orig_sig_type;
};

union diskpage {
	union swap_header swh;	/* swh.magic is the only member used */
	struct sig_data sig_data;
};

union p_diskpage {
	union diskpage *pointer;
	char *ptr;
	unsigned long address;
};

enum {
	IMAGE_SIGNATURE,
	NO_IMAGE_SIGNATURE,
	TRIED_RESUME,
	NO_TRIED_RESUME,
};

/*
 * Both of these point to versions of the swap header page. original_sig points
 * to the data we read from disk at the start of hibernating or checking whether
 * to resume. no_image is the page stored in the image header, showing what the
 * swap header page looked like at the start of hibernating.
 */
static char *current_signature_page;
static char no_image_signature_contents[sizeof(struct sig_data)];

/* Devices used for swap */
static struct toi_bdev_info devinfo[MAX_SWAPFILES];

/* Extent chains for swap & blocks */
static struct hibernate_extent_chain swapextents;
static struct hibernate_extent_chain block_chain[MAX_SWAPFILES];

static dev_t header_dev_t;
static struct block_device *header_block_device;
static unsigned long headerblock;

/* For swapfile automatically swapon/off'd. */
static char swapfilename[32] = "";
static int toi_swapon_status;

/* Header Page Information */
static long header_pages_reserved;

/* Swap Pages */
static long swap_pages_allocated;

/* User Specified Parameters. */

static unsigned long resume_firstblock;
static dev_t resume_swap_dev_t;
static struct block_device *resume_block_device;

static struct sysinfo swapinfo;

/* Block devices open. */
struct bdev_opened {
	dev_t device;
	struct block_device *bdev;
};

/*
 * Entry MAX_SWAPFILES is the resume block device, which may
 * be a swap device not enabled when we hibernate.
 * Entry MAX_SWAPFILES + 1 is the header block device, which
 * is needed before we find out which slot it occupies.
 *
 * We use a separate struct to devInfo so that we can track
 * the bdevs we open, because if we need to abort resuming
 * prior to the atomic restore, they need to be closed, but
 * closing them after sucessfully resuming would be wrong.
 */
static struct bdev_opened *bdevs_opened[MAX_SWAPFILES + 2];

/**
 * close_bdev: Close a swap bdev.
 *
 * int: The swap entry number to close.
 */
static void close_bdev(int i)
{
	struct bdev_opened *this = bdevs_opened[i];

	if (!this)
		return;

	blkdev_put(this->bdev, FMODE_READ | FMODE_NDELAY);
	toi_kfree(8, this);
	bdevs_opened[i] = NULL;
}

/**
 * close_bdevs: Close all bdevs we opened.
 *
 * Close all bdevs that we opened and reset the related vars.
 */
static void close_bdevs(void)
{
	int i;

	for (i = 0; i < MAX_SWAPFILES + 2; i++)
		close_bdev(i);

	resume_block_device = NULL;
	header_block_device = NULL;
}

/**
 * open_bdev: Open a bdev at resume time.
 *
 * index: The swap index. May be MAX_SWAPFILES for the resume_dev_t
 * (the user can have resume= pointing at a swap partition/file that isn't
 * swapon'd when they hibernate. MAX_SWAPFILES+1 for the first page of the
 * header. It will be from a swap partition that was enabled when we hibernated,
 * but we don't know it's real index until we read that first page.
 * dev_t: The device major/minor.
 * display_errs: Whether to try to do this quietly.
 *
 * We stored a dev_t in the image header. Open the matching device without
 * requiring /dev/<whatever> in most cases and record the details needed
 * to close it later and avoid duplicating work.
 */
static struct block_device *open_bdev(int index, dev_t device, int display_errs)
{
	struct bdev_opened *this;
	struct block_device *bdev;

	if (bdevs_opened[index]) {
		if (bdevs_opened[index]->device == device)
			return bdevs_opened[index]->bdev;

		close_bdev(index);
	}

	bdev = toi_open_by_devnum(device, FMODE_READ | FMODE_NDELAY);

	if (IS_ERR(bdev) || !bdev) {
		if (display_errs)
			toi_early_boot_message(1, TOI_CONTINUE_REQ,
				"Failed to get access to block device "
				"\"%x\" (error %d).\n Maybe you need "
				"to run mknod and/or lvmsetup in an "
				"initrd/ramfs?", device, bdev);
		return ERR_PTR(-EINVAL);
	}

	this = toi_kzalloc(8, sizeof(struct bdev_opened), GFP_KERNEL);
	if (!this) {
		printk(KERN_WARNING "TuxOnIce: Failed to allocate memory for "
				"opening a bdev.");
		blkdev_put(bdev, FMODE_READ | FMODE_NDELAY);
		return ERR_PTR(-ENOMEM);
	}

	bdevs_opened[index] = this;
	this->device = device;
	this->bdev = bdev;

	return bdev;
}

/**
 * enable_swapfile: Swapon the user specified swapfile prior to hibernating.
 *
 * Activate the given swapfile if it wasn't already enabled. Remember whether
 * we really did swapon it for swapoffing later.
 */
static void enable_swapfile(void)
{
	int activateswapresult = -EINVAL;

	if (swapfilename[0]) {
		/* Attempt to swap on with maximum priority */
		activateswapresult = sys_swapon(swapfilename, 0xFFFF);
		if (activateswapresult && activateswapresult != -EBUSY)
			printk("TuxOnIce: The swapfile/partition specified by "
				"/sys/power/tuxonice/swap/swapfile "
				"(%s) could not be turned on (error %d). "
				"Attempting to continue.\n",
				swapfilename, activateswapresult);
		if (!activateswapresult)
			toi_swapon_status = 1;
	}
}

/**
 * disable_swapfile: Swapoff any file swaponed at the start of the cycle.
 *
 * If we did successfully swapon a file at the start of the cycle, swapoff
 * it now (finishing up).
 */
static void disable_swapfile(void)
{
	if (!toi_swapon_status)
		return;

	sys_swapoff(swapfilename);
	toi_swapon_status = 0;
}

/**
 * try_to_parse_resume_device: Try to parse resume=
 *
 * Any "swap:" has been stripped away and we just have the path to deal with.
 * We attempt to do name_to_dev_t, open and stat the file. Having opened the
 * file, get the struct block_device * to match.
 */
static int try_to_parse_resume_device(char *commandline, int quiet)
{
	struct kstat stat;
	int error = 0;

	wait_for_device_probe();
	resume_swap_dev_t = name_to_dev_t(commandline);

	if (!resume_swap_dev_t) {
		struct file *file = filp_open(commandline,
				O_RDONLY|O_LARGEFILE, 0);

		if (!IS_ERR(file) && file) {
			vfs_getattr(file->f_vfsmnt, file->f_dentry, &stat);
			filp_close(file, NULL);
		} else
			error = vfs_stat(commandline, &stat);
		if (!error)
			resume_swap_dev_t = stat.rdev;
	}

	if (!resume_swap_dev_t) {
		if (quiet)
			return 1;

		if (test_toi_state(TOI_TRYING_TO_RESUME))
			toi_early_boot_message(1, TOI_CONTINUE_REQ,
			  "Failed to translate \"%s\" into a device id.\n",
			  commandline);
		else
			printk("TuxOnIce: Can't translate \"%s\" into a device "
					"id yet.\n", commandline);
		return 1;
	}

	resume_block_device = open_bdev(MAX_SWAPFILES, resume_swap_dev_t, 0);
	if (IS_ERR(resume_block_device)) {
		if (!quiet)
			toi_early_boot_message(1, TOI_CONTINUE_REQ,
				"Failed to get access to \"%s\", where"
				" the swap header should be found.",
				commandline);
		return 1;
	}

	return 0;
}

/*
 * If we have read part of the image, we might have filled  memory with
 * data that should be zeroed out.
 */
static void toi_swap_noresume_reset(void)
{
	toi_bio_ops.rw_cleanup(READ);
	memset((char *) &devinfo, 0, sizeof(devinfo));
}

static int get_current_signature(void)
{
	if (!current_signature_page) {
		current_signature_page = (char *) toi_get_zeroed_page(38,
			TOI_ATOMIC_GFP);
		if (!current_signature_page)
			return -ENOMEM;
	}

	return toi_bio_ops.bdev_page_io(READ, resume_block_device,
		resume_firstblock, virt_to_page(current_signature_page));
}

static int parse_signature(void)
{
	union p_diskpage swap_header_page;
	struct sig_data *sig;
	int type;
	char *swap_header;
	const char *sigs[] = {
		"SWAP-SPACE", "SWAPSPACE2", "S1SUSP", "S2SUSP", "S1SUSPEND"
	};

	int result = get_current_signature();
	if (result)
		return result;

	swap_header_page = (union p_diskpage) current_signature_page;
	sig = (struct sig_data *) current_signature_page;
	swap_header = swap_header_page.pointer->swh.magic.magic;

	for (type = 0; type < 5; type++)
		if (!memcmp(sigs[type], swap_header, strlen(sigs[type])))
			return type;

	if (memcmp(tuxonice_signature, swap_header, sizeof(tuxonice_signature)))
		return -1;

	header_dev_t = sig->device;
	clear_toi_state(TOI_RESUMED_BEFORE);
	if (sig->resume_attempted)
		set_toi_state(TOI_RESUMED_BEFORE);
	headerblock = sig->sector;

	return 10;
}

static void forget_signatures(void)
{
	if (current_signature_page) {
		toi_free_page(38, (unsigned long) current_signature_page);
		current_signature_page = NULL;
	}
}

/*
 * write_modified_signature
 *
 * Write a (potentially) modified signature page without forgetting the
 * original contents.
 */
static int write_modified_signature(int modification)
{
	union p_diskpage swap_header_page;
	struct swap_info_struct *si;
	int result;
	char *orig_sig;

	/* In case we haven't already */
	result = get_current_signature();

	if (result)
		return result;

	swap_header_page.address = toi_get_zeroed_page(38, TOI_ATOMIC_GFP);

	if (!swap_header_page.address)
		return -ENOMEM;

	memcpy(swap_header_page.ptr, current_signature_page, PAGE_SIZE);

	switch (modification) {
	case IMAGE_SIGNATURE:

		memcpy(no_image_signature_contents, swap_header_page.ptr,
				sizeof(no_image_signature_contents));

		/* Get the details of the header first page. */
		toi_extent_state_goto_start(&toi_writer_posn);
		toi_bio_ops.forward_one_page(1, 1);

		si = get_swap_info_struct(toi_writer_posn.current_chain);

		/* Prepare the signature */
		swap_header_page.pointer->sig_data.device = si->bdev->bd_dev;
		swap_header_page.pointer->sig_data.sector =
			toi_writer_posn.current_offset;
		swap_header_page.pointer->sig_data.resume_attempted = 0;
		swap_header_page.pointer->sig_data.orig_sig_type =
			parse_signature();

		memcpy(swap_header_page.pointer->swh.magic.magic,
				tuxonice_signature, sizeof(tuxonice_signature));

		break;
	case NO_IMAGE_SIGNATURE:
		if (!swap_header_page.pointer->sig_data.orig_sig_type)
			orig_sig = "SWAP-SPACE";
		else
			orig_sig = "SWAPSPACE2";

		memcpy(swap_header_page.pointer->swh.magic.magic, orig_sig, 10);
		memcpy(swap_header_page.ptr, no_image_signature_contents,
				sizeof(no_image_signature_contents));
		break;
	case TRIED_RESUME:
		swap_header_page.pointer->sig_data.resume_attempted = 1;
		break;
	case NO_TRIED_RESUME:
		swap_header_page.pointer->sig_data.resume_attempted = 0;
		break;
	}

	result = toi_bio_ops.bdev_page_io(WRITE, resume_block_device,
		resume_firstblock, virt_to_page(swap_header_page.address));

	memcpy(current_signature_page, swap_header_page.ptr, PAGE_SIZE);

	toi_free_page(38, swap_header_page.address);

	return result;
}

/*
 * apply_header_reservation
 */
static int apply_header_reservation(void)
{
	int i;

	toi_extent_state_goto_start(&toi_writer_posn);

	for (i = 0; i < header_pages_reserved; i++)
		if (toi_bio_ops.forward_one_page(1, 0))
			return -ENOSPC;

	/* The end of header pages will be the start of pageset 2;
	 * we are now sitting on the first pageset2 page. */
	toi_extent_state_save(&toi_writer_posn, &toi_writer_posn_save[2]);
	return 0;
}

static void toi_swap_reserve_header_space(int request)
{
	header_pages_reserved = (long) request;
}

static void free_block_chains(void)
{
	int i;

	for (i = 0; i < MAX_SWAPFILES; i++)
		if (block_chain[i].first)
			toi_put_extent_chain(&block_chain[i]);
}

static int add_blocks_to_extent_chain(int chain, int start, int end)
{
	if (test_action_state(TOI_TEST_BIO))
		printk(KERN_INFO "Adding extent chain %d %d-%d.\n", chain,
				start << devinfo[chain].bmap_shift,
				end << devinfo[chain].bmap_shift);

	if (toi_add_to_extent_chain(&block_chain[chain], start, end)) {
		free_block_chains();
		return -ENOMEM;
	}

	return 0;
}


static int get_main_pool_phys_params(void)
{
	struct hibernate_extent *extentpointer = NULL;
	unsigned long address;
	int extent_min = -1, extent_max = -1, last_chain = -1;

	free_block_chains();

	toi_extent_for_each(&swapextents, extentpointer, address) {
		swp_entry_t swap_address = (swp_entry_t) { address };
		pgoff_t offset = swp_offset(swap_address);
		unsigned swapfilenum = swp_type(swap_address);
		struct swap_info_struct *sis =
			get_swap_info_struct(swapfilenum);
		sector_t new_sector = map_swap_page(sis, offset);

		if (devinfo[swapfilenum].ignored)
			continue;

		if ((new_sector == extent_max + 1) &&
		    (last_chain == swapfilenum)) {
			extent_max++;
			continue;
		}

		if (extent_min > -1 && add_blocks_to_extent_chain(last_chain,
					extent_min, extent_max)) {
			printk("Out of memory while making block chains.\n");
			return -ENOMEM;
		}

		extent_min = new_sector;
		extent_max = new_sector;
		last_chain = swapfilenum;
	}

	if (extent_min > -1 && add_blocks_to_extent_chain(last_chain,
				extent_min, extent_max)) {
			printk("Out of memory while making block chains.\n");
			return -ENOMEM;
	}

	return apply_header_reservation();
}

static long raw_to_real(long raw)
{
	long result;

	result = raw - (raw * (sizeof(unsigned long) + sizeof(int)) +
		(PAGE_SIZE + sizeof(unsigned long) + sizeof(int) + 1)) /
		(PAGE_SIZE + sizeof(unsigned long) + sizeof(int));

	return result < 0 ? 0 : result;
}

static int toi_swap_storage_allocated(void)
{
	return (int) raw_to_real(swap_pages_allocated - header_pages_reserved);
}

/*
 * Like si_swapinfo, except that we don't include ram backed swap (compcache!)
 * and don't need to use the spinlocks (userspace is stopped when this
 * function is called).
 */
void si_swapinfo_no_compcache(struct sysinfo *val)
{
	unsigned int i;

	si_swapinfo(&swapinfo);
	val->freeswap = 0;
	val->totalswap = 0;

	for (i = 0; i < MAX_SWAPFILES; i++) {
		struct swap_info_struct *si = get_swap_info_struct(i);
		if ((si->flags & SWP_USED) &&
		    (si->flags & SWP_WRITEOK) &&
		    (strncmp(si->bdev->bd_disk->disk_name, "ram", 3))) {
			val->totalswap += si->inuse_pages;
			val->freeswap += si->pages - si->inuse_pages;
		}
	}
}
/*
 * We can't just remember the value from allocation time, because other
 * processes might have allocated swap in the mean time.
 */
static int toi_swap_storage_available(void)
{
	si_swapinfo_no_compcache(&swapinfo);
	return (int) raw_to_real((long) swapinfo.freeswap +
			swap_pages_allocated - header_pages_reserved);
}

static int toi_swap_initialise(int starting_cycle)
{
	int result = 0;

	if (!starting_cycle)
		return 0;

	enable_swapfile();

	if (resume_swap_dev_t && !resume_block_device) {
		resume_block_device = open_bdev(MAX_SWAPFILES,
				resume_swap_dev_t, 1);
		if (IS_ERR(resume_block_device))
			result = 1;
	}

	return result;
}

static void toi_swap_cleanup(int ending_cycle)
{
	if (ending_cycle)
		disable_swapfile();

	close_bdevs();

	forget_signatures();
}

static int toi_swap_release_storage(void)
{
	header_pages_reserved = 0;
	swap_pages_allocated = 0;

	if (swapextents.first) {
		/* Free swap entries */
		struct hibernate_extent *extentpointer;
		unsigned long extentvalue;
		toi_extent_for_each(&swapextents, extentpointer,
				extentvalue)
			swap_free((swp_entry_t) { extentvalue });

		toi_put_extent_chain(&swapextents);

		free_block_chains();
	}

	return 0;
}

static void free_swap_range(unsigned long min, unsigned long max)
{
	int j;

	for (j = min; j <= max; j++)
		swap_free((swp_entry_t) { j });
}

/*
 * Round robin allocation (where swap storage has the same priority).
 * could make this very inefficient, so we track extents allocated on
 * a per-swapfile basis.
 */
static int toi_swap_allocate_storage(int request)
{
	int i, result = 0, to_add[MAX_SWAPFILES], pages_to_get, extra_pages,
	    gotten = 0, result2;
	unsigned long extent_min[MAX_SWAPFILES], extent_max[MAX_SWAPFILES];

	extra_pages = DIV_ROUND_UP(request * (sizeof(unsigned long)
			       + sizeof(int)), PAGE_SIZE);
	pages_to_get = request + extra_pages - swapextents.size +
		header_pages_reserved;

	if (pages_to_get < 1)
		return apply_header_reservation();

	for (i = 0; i < MAX_SWAPFILES; i++) {
		struct swap_info_struct *si = get_swap_info_struct(i);
		to_add[i] = 0;
		if (!si->bdev)
			continue;
		if (!strncmp(si->bdev->bd_disk->disk_name, "ram", 3)) {
			devinfo[i].ignored = 1;
			continue;
		}
		devinfo[i].ignored = 0;
		devinfo[i].bdev = si->bdev;
		devinfo[i].dev_t = si->bdev->bd_dev;
		devinfo[i].bmap_shift = 3;
		devinfo[i].blocks_per_page = 1;
	}

	while (gotten < pages_to_get) {
		swp_entry_t entry;
		unsigned long new_value;
		unsigned swapfilenum;

		entry = get_swap_page();
		if (!entry.val)
			break;

		swapfilenum = swp_type(entry);
		new_value = entry.val;

		if (!to_add[swapfilenum]) {
			to_add[swapfilenum] = 1;
			extent_min[swapfilenum] = new_value;
			extent_max[swapfilenum] = new_value;
			if (!devinfo[swapfilenum].ignored)
				gotten++;
			continue;
		}

		if (new_value == extent_max[swapfilenum] + 1) {
			extent_max[swapfilenum]++;
			if (!devinfo[swapfilenum].ignored)
				gotten++;
			continue;
		}

		if (toi_add_to_extent_chain(&swapextents,
					extent_min[swapfilenum],
					extent_max[swapfilenum])) {
			printk(KERN_INFO "Failed to allocate extent for "
					"%lu-%lu.\n", extent_min[swapfilenum],
					extent_max[swapfilenum]);
			free_swap_range(extent_min[swapfilenum],
					extent_max[swapfilenum]);
			swap_free(entry);
			if (!devinfo[swapfilenum].ignored)
				gotten -= (extent_max[swapfilenum] -
					extent_min[swapfilenum] + 1);
			/* Don't try to add again below */
			to_add[swapfilenum] = 0;
			break;
		} else {
			extent_min[swapfilenum] = new_value;
			extent_max[swapfilenum] = new_value;
			if (!devinfo[swapfilenum].ignored)
				gotten++;
		}
	}

	for (i = 0; i < MAX_SWAPFILES; i++) {
		if (!to_add[i] || !toi_add_to_extent_chain(&swapextents,
					extent_min[i], extent_max[i]))
			continue;

		free_swap_range(extent_min[i], extent_max[i]);
		if (!devinfo[i].ignored)
			gotten -= (extent_max[i] - extent_min[i] + 1);
		break;
	}

	if (gotten < pages_to_get) {
		printk("Got fewer pages than required "
				"(%d wanted, %d gotten).\n",
				pages_to_get, gotten);
		result = -ENOSPC;
	}

	swap_pages_allocated += (long) gotten;

	result2 = get_main_pool_phys_params();

	return result ? result : result2;
}

static int toi_swap_write_header_init(void)
{
	int i, result;
	struct swap_info_struct *si;

	toi_bio_ops.rw_init(WRITE, 0);
	toi_writer_buffer_posn = 0;

	/* Info needed to bootstrap goes at the start of the header.
	 * First we save the positions and devinfo, including the number
	 * of header pages. Then we save the structs containing data needed
	 * for reading the header pages back.
	 * Note that even if header pages take more than one page, when we
	 * read back the info, we will have restored the location of the
	 * next header page by the time we go to use it.
	 */

	result = toi_bio_ops.rw_header_chunk(WRITE, &toi_swapops,
			(char *) &no_image_signature_contents,
			sizeof(struct sig_data));

	if (result)
		return result;

	/* Forward one page will be done prior to the read */
	for (i = 0; i < MAX_SWAPFILES; i++) {
		si = get_swap_info_struct(i);
		if (si->swap_file)
			devinfo[i].dev_t = si->bdev->bd_dev;
		else
			devinfo[i].dev_t = (dev_t) 0;
	}

	result = toi_bio_ops.rw_header_chunk(WRITE, &toi_swapops,
			(char *) &toi_writer_posn_save,
			sizeof(toi_writer_posn_save));

	if (result)
		return result;

	result = toi_bio_ops.rw_header_chunk(WRITE, &toi_swapops,
			(char *) &devinfo, sizeof(devinfo));

	if (result)
		return result;

	for (i = 0; i < MAX_SWAPFILES; i++)
		toi_serialise_extent_chain(&toi_swapops, &block_chain[i]);

	return 0;
}

static int toi_swap_write_header_cleanup(void)
{
	int result = toi_bio_ops.write_header_chunk_finish();

	/* Set signature to save we have an image */
	if (!result)
		result = write_modified_signature(IMAGE_SIGNATURE);

	return result;
}

/* ------------------------- HEADER READING ------------------------- */

/*
 * read_header_init()
 *
 * Description:
 * 1. Attempt to read the device specified with resume=.
 * 2. Check the contents of the swap header for our signature.
 * 3. Warn, ignore, reset and/or continue as appropriate.
 * 4. If continuing, read the toi_swap configuration section
 *    of the header and set up block device info so we can read
 *    the rest of the header & image.
 *
 * Returns:
 * May not return if user choose to reboot at a warning.
 * -EINVAL if cannot resume at this time. Booting should continue
 * normally.
 */

static int toi_swap_read_header_init(void)
{
	int i, result = 0;
	toi_writer_buffer_posn = 0;

	if (!header_dev_t) {
		printk(KERN_INFO "read_header_init called when we haven't "
				"verified there is an image!\n");
		return -EINVAL;
	}

	/*
	 * If the header is not on the resume_swap_dev_t, get the resume device
	 * first.
	 */
	if (header_dev_t != resume_swap_dev_t) {
		header_block_device = open_bdev(MAX_SWAPFILES + 1,
				header_dev_t, 1);

		if (IS_ERR(header_block_device))
			return PTR_ERR(header_block_device);
	} else
		header_block_device = resume_block_device;

	toi_bio_ops.read_header_init();

	/*
	 * Read toi_swap configuration.
	 * Headerblock size taken into account already.
	 */
	result = toi_bio_ops.bdev_page_io(READ, header_block_device,
			headerblock << 3,
			virt_to_page((unsigned long) toi_writer_buffer));
	if (result)
		return result;

	memcpy(&no_image_signature_contents, toi_writer_buffer,
			sizeof(no_image_signature_contents));

	toi_writer_buffer_posn = sizeof(no_image_signature_contents);

	memcpy(&toi_writer_posn_save, toi_writer_buffer +
			toi_writer_buffer_posn, sizeof(toi_writer_posn_save));

	toi_writer_buffer_posn += sizeof(toi_writer_posn_save);

	memcpy(&devinfo, toi_writer_buffer + toi_writer_buffer_posn,
			sizeof(devinfo));

	toi_writer_buffer_posn += sizeof(devinfo);

	/* Restore device info */
	for (i = 0; i < MAX_SWAPFILES; i++) {
		dev_t thisdevice = devinfo[i].dev_t;
		struct block_device *bdev_result;

		devinfo[i].bdev = NULL;

		if (!thisdevice || devinfo[i].ignored)
			continue;

		if (thisdevice == resume_swap_dev_t) {
			devinfo[i].bdev = resume_block_device;
			continue;
		}

		if (thisdevice == header_dev_t) {
			devinfo[i].bdev = header_block_device;
			continue;
		}

		bdev_result = open_bdev(i, thisdevice, 1);
		if (IS_ERR(bdev_result))
			return PTR_ERR(bdev_result);
		devinfo[i].bdev = bdevs_opened[i]->bdev;
	}

	toi_extent_state_goto_start(&toi_writer_posn);
	toi_bio_ops.set_extra_page_forward();

	for (i = 0; i < MAX_SWAPFILES && !result; i++)
		result = toi_load_extent_chain(&block_chain[i]);

	return result;
}

static int toi_swap_read_header_cleanup(void)
{
	toi_bio_ops.rw_cleanup(READ);
	return 0;
}

/*
 * workspace_size
 *
 * Description:
 * Returns the number of bytes of RAM needed for this
 * code to do its work. (Used when calculating whether
 * we have enough memory to be able to hibernate & resume).
 *
 */
static int toi_swap_memory_needed(void)
{
	return 1;
}

/*
 * Print debug info
 *
 * Description:
 */
static int toi_swap_print_debug_stats(char *buffer, int size)
{
	int len = 0;
	struct sysinfo sysinfo;

	if (toiActiveAllocator != &toi_swapops) {
		len = scnprintf(buffer, size,
				"- SwapAllocator inactive.\n");
		return len;
	}

	len = scnprintf(buffer, size, "- SwapAllocator active.\n");
	if (swapfilename[0])
		len += scnprintf(buffer+len, size-len,
			"  Attempting to automatically swapon: %s.\n",
			swapfilename);

	si_swapinfo_no_compcache(&sysinfo);

	len += scnprintf(buffer+len, size-len,
			"  Swap available for image: %d pages.\n",
			(int) sysinfo.freeswap + toi_swap_storage_allocated());

	return len;
}

/*
 * Storage needed
 *
 * Returns amount of space in the swap header required
 * for the toi_swap's data. This ignores the links between
 * pages, which we factor in when allocating the space.
 *
 * We ensure the space is allocated, but actually save the
 * data from write_header_init and therefore don't also define a
 * save_config_info routine.
 */
static int toi_swap_storage_needed(void)
{
	int i, result;
	result = sizeof(struct sig_data) + sizeof(toi_writer_posn_save) +
		sizeof(devinfo);

	for (i = 0; i < MAX_SWAPFILES; i++) {
		result += 2 * sizeof(int);
		result += (2 * sizeof(unsigned long) *
			block_chain[i].num_extents);
	}

	return result;
}

/*
 * Image_exists
 *
 * Returns -1 if don't know, otherwise 0 (no) or 1 (yes).
 */
static int toi_swap_image_exists(int quiet)
{
	int signature_found;

	if (!resume_swap_dev_t) {
		if (!quiet)
			printk(KERN_INFO "Not even trying to read header "
				"because resume_swap_dev_t is not set.\n");
		return -1;
	}

	if (!resume_block_device) {
	    resume_block_device = open_bdev(MAX_SWAPFILES, resume_swap_dev_t,
			    1);
	    if (IS_ERR(resume_block_device)) {
		if (!quiet)
			printk(KERN_INFO "Failed to open resume dev_t (%x).\n",
				resume_swap_dev_t);
		return -1;
	    }
	}

	signature_found = parse_signature();

	switch (signature_found) {
	case -ENOMEM:
		return -1;
	case -1:
		if (!quiet)
			printk(KERN_ERR "TuxOnIce: Unable to find a signature."
				" Could you have moved a swap file?\n");
		return -1;
	case 0:
	case 1:
		if (!quiet)
			printk(KERN_INFO "TuxOnIce: Normal swapspace found.\n");
		return 0;
	case 2:
	case 3:
	case 4:
		if (!quiet)
			printk(KERN_INFO "TuxOnIce: Detected another "
				"implementation's signature.\n");
		return 0;
	case 10:
		if (!quiet)
			printk(KERN_INFO "TuxOnIce: Detected TuxOnIce binary "
				"signature.\n");
		return 1;
	}

	printk("Unrecognised parse_signature result (%d).\n", signature_found);
	return 0;
}

/* toi_swap_remove_image
 *
 */
static int toi_swap_remove_image(void)
{
	/*
	 * If nr_hibernates == 0, we must be booting, so no swap pages
	 * will be recorded as used yet.
	 */

	if (nr_hibernates)
		toi_swap_release_storage();

	/*
	 * We don't do a sanity check here: we want to restore the swap
	 * whatever version of kernel made the hibernate image.
	 *
	 * We need to write swap, but swap may not be enabled so
	 * we write the device directly
	 *
	 * If we don't have an current_signature_page, we didn't
	 * read an image header, so don't change anything.
	 */

	return toi_swap_image_exists(1) ?
		write_modified_signature(NO_IMAGE_SIGNATURE) : 0;
}

/*
 * Mark resume attempted.
 *
 * Record that we tried to resume from this image. We have already read the
 * signature in. We just need to write the modified version.
 */
static int toi_swap_mark_resume_attempted(int mark)
{
	if (!resume_swap_dev_t) {
		printk(KERN_INFO "Not even trying to record attempt at resuming"
				" because resume_swap_dev_t is not set.\n");
		return -ENODEV;
	}

	return write_modified_signature(mark ? TRIED_RESUME : NO_TRIED_RESUME);
}

/*
 * Parse Image Location
 *
 * Attempt to parse a resume= parameter.
 * Swap Writer accepts:
 * resume=swap:DEVNAME[:FIRSTBLOCK][@BLOCKSIZE]
 *
 * Where:
 * DEVNAME is convertable to a dev_t by name_to_dev_t
 * FIRSTBLOCK is the location of the first block in the swap file
 * (specifying for a swap partition is nonsensical but not prohibited).
 * Data is validated by attempting to read a swap header from the
 * location given. Failure will result in toi_swap refusing to
 * save an image, and a reboot with correct parameters will be
 * necessary.
 */
static int toi_swap_parse_sig_location(char *commandline,
		int only_allocator, int quiet)
{
	char *thischar, *devstart, *colon = NULL;
	int signature_found, result = -EINVAL, temp_result;

	if (strncmp(commandline, "swap:", 5)) {
		/*
		 * Failing swap:, we'll take a simple
		 * resume=/dev/hda2, but fall through to
		 * other allocators if /dev/ isn't matched.
		 */
		if (strncmp(commandline, "/dev/", 5))
			return 1;
	} else
		commandline += 5;

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

	while ((thischar - commandline) < 250 && *thischar)
		thischar++;

	if (colon)
		resume_firstblock = (int) simple_strtoul(colon + 1, NULL, 0);
	else
		resume_firstblock = 0;

	clear_toi_state(TOI_CAN_HIBERNATE);
	clear_toi_state(TOI_CAN_RESUME);

	temp_result = try_to_parse_resume_device(devstart, quiet);

	if (colon)
		*colon = ':';

	if (temp_result)
		return -EINVAL;

	signature_found = toi_swap_image_exists(quiet);

	if (signature_found != -1) {
		result = 0;

		toi_bio_ops.set_devinfo(devinfo);
		toi_writer_posn.chains = &block_chain[0];
		toi_writer_posn.num_chains = MAX_SWAPFILES;
		set_toi_state(TOI_CAN_HIBERNATE);
		set_toi_state(TOI_CAN_RESUME);
	} else
		if (!quiet)
			printk(KERN_ERR "TuxOnIce: SwapAllocator: No swap "
				"signature found at %s.\n", devstart);
	return result;
}

static int header_locations_read_sysfs(const char *page, int count)
{
	int i, printedpartitionsmessage = 0, len = 0, haveswap = 0;
	struct inode *swapf = NULL;
	int zone;
	char *path_page = (char *) toi_get_free_page(10, GFP_KERNEL);
	char *path, *output = (char *) page;
	int path_len;

	if (!page)
		return 0;

	for (i = 0; i < MAX_SWAPFILES; i++) {
		struct swap_info_struct *si =  get_swap_info_struct(i);

		if (!si->swap_file)
			continue;

		if (S_ISBLK(si->swap_file->f_mapping->host->i_mode)) {
			haveswap = 1;
			if (!printedpartitionsmessage) {
				len += sprintf(output + len,
					"For swap partitions, simply use the "
					"format: resume=swap:/dev/hda1.\n");
				printedpartitionsmessage = 1;
			}
		} else {
			path_len = 0;

			path = d_path(&si->swap_file->f_path, path_page,
					PAGE_SIZE);
			path_len = snprintf(path_page, 31, "%s", path);

			haveswap = 1;
			swapf = si->swap_file->f_mapping->host;
			zone = bmap(swapf, 0);
			if (!zone) {
				len += sprintf(output + len,
					"Swapfile %s has been corrupted. Reuse"
					" mkswap on it and try again.\n",
					path_page);
			} else {
				char name_buffer[255];
				len += sprintf(output + len,
					"For swapfile `%s`,"
					" use resume=swap:/dev/%s:0x%x.\n",
					path_page,
					bdevname(si->bdev, name_buffer),
					zone << (swapf->i_blkbits - 9));
			}
		}
	}

	if (!haveswap)
		len = sprintf(output, "You need to turn on swap partitions "
				"before examining this file.\n");

	toi_free_page(10, (unsigned long) path_page);
	return len;
}

static struct toi_sysfs_data sysfs_params[] = {
	SYSFS_STRING("swapfilename", SYSFS_RW, swapfilename, 255, 0, NULL),
	SYSFS_CUSTOM("headerlocations", SYSFS_READONLY,
			header_locations_read_sysfs, NULL, 0, NULL),
	SYSFS_INT("enabled", SYSFS_RW, &toi_swapops.enabled, 0, 1, 0,
			attempt_to_parse_resume_device2),
};

static struct toi_module_ops toi_swapops = {
	.type					= WRITER_MODULE,
	.name					= "swap storage",
	.directory				= "swap",
	.module					= THIS_MODULE,
	.memory_needed				= toi_swap_memory_needed,
	.print_debug_info			= toi_swap_print_debug_stats,
	.storage_needed				= toi_swap_storage_needed,
	.initialise				= toi_swap_initialise,
	.cleanup				= toi_swap_cleanup,

	.noresume_reset		= toi_swap_noresume_reset,
	.storage_available 	= toi_swap_storage_available,
	.storage_allocated	= toi_swap_storage_allocated,
	.reserve_header_space	= toi_swap_reserve_header_space,
	.allocate_storage	= toi_swap_allocate_storage,
	.image_exists		= toi_swap_image_exists,
	.mark_resume_attempted	= toi_swap_mark_resume_attempted,
	.write_header_init	= toi_swap_write_header_init,
	.write_header_cleanup	= toi_swap_write_header_cleanup,
	.read_header_init	= toi_swap_read_header_init,
	.read_header_cleanup	= toi_swap_read_header_cleanup,
	.remove_image		= toi_swap_remove_image,
	.parse_sig_location	= toi_swap_parse_sig_location,

	.sysfs_data		= sysfs_params,
	.num_sysfs_entries	= sizeof(sysfs_params) /
		sizeof(struct toi_sysfs_data),
};

/* ---- Registration ---- */
static __init int toi_swap_load(void)
{
	toi_swapops.rw_init = toi_bio_ops.rw_init;
	toi_swapops.rw_cleanup = toi_bio_ops.rw_cleanup;
	toi_swapops.read_page = toi_bio_ops.read_page;
	toi_swapops.write_page = toi_bio_ops.write_page;
	toi_swapops.rw_header_chunk = toi_bio_ops.rw_header_chunk;
	toi_swapops.rw_header_chunk_noreadahead =
		toi_bio_ops.rw_header_chunk_noreadahead;
	toi_swapops.io_flusher = toi_bio_ops.io_flusher;
	toi_swapops.update_throughput_throttle =
		toi_bio_ops.update_throughput_throttle;
	toi_swapops.finish_all_io = toi_bio_ops.finish_all_io;

	return toi_register_module(&toi_swapops);
}

#ifdef MODULE
static __exit void toi_swap_unload(void)
{
	toi_unregister_module(&toi_swapops);
}

module_init(toi_swap_load);
module_exit(toi_swap_unload);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Nigel Cunningham");
MODULE_DESCRIPTION("TuxOnIce SwapAllocator");
#else
late_initcall(toi_swap_load);
#endif
