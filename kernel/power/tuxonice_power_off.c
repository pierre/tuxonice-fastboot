/*
 * kernel/power/tuxonice_power_off.c
 *
 * Copyright (C) 2006-2007 Nigel Cunningham (nigel at suspend2 net)
 *
 * This file is released under the GPLv2.
 *
 * Support for powering down.
 */

#include <linux/device.h>
#include <linux/suspend.h>
#include <linux/mm.h>
#include <linux/pm.h>
#include <linux/reboot.h>
#include <linux/cpu.h>
#include <linux/console.h>
#include <linux/fs.h>
#include "tuxonice.h"
#include "tuxonice_ui.h"
#include "tuxonice_power_off.h"
#include "tuxonice_sysfs.h"
#include "tuxonice_modules.h"

unsigned long toi_poweroff_method = 0; /* 0 - Kernel power off */
int wake_delay = 0;
static char lid_state_file[256], wake_alarm_dir[256];
static struct file *lid_file, *alarm_file, *epoch_file;
int post_wake_state = -1;

extern struct hibernation_ops *hibernation_ops;

int toi_platform_prepare(void)
{
	return (toi_poweroff_method == 4 && hibernation_ops) ?
		hibernation_ops->prepare() : 0;
}

/*
 * __toi_power_down
 * Functionality   : Powers down or reboots the computer once the image
 *                   has been written to disk.
 * Key Assumptions : Able to reboot/power down via code called or that
 *                   the warning emitted if the calls fail will be visible
 *                   to the user (ie printk resumes devices).
 */

static void __toi_power_down(int method)
{
	int result = 0;

	if (test_action_state(TOI_REBOOT)) {
		toi_prepare_status(DONT_CLEAR_BAR, "Ready to reboot.");
		kernel_restart(NULL);
	}

	toi_prepare_status(DONT_CLEAR_BAR, "Powering down.");

	switch (method) {
		case 0:
			break;
		case 3:
			result = suspend_devices_and_enter(PM_SUSPEND_MEM);

			/* If suspended to ram and later woke. */
			if (!result)
				return;
			break;
		case 4:
			if (hibernation_platform_enter())
				return;
			break;
		case 5:
			/* Historic entry only now */
			break;
	}

	if (method)
		toi_prepare_status(DONT_CLEAR_BAR,
				"Falling back to alternate power off method.");
	kernel_power_off();
	kernel_halt();
	toi_prepare_status(DONT_CLEAR_BAR, "Powerdown failed.");
	while (1)
		cpu_relax();
}

void toi_platform_finish(void)
{
	if (toi_poweroff_method == 4 && hibernation_ops)
		hibernation_ops->finish();
}

#define CLOSE_FILE(file) \
 if (file) { filp_close(file, NULL); file = NULL; }

static void powerdown_files_close(int toi_or_resume)
{
	if (!toi_or_resume)
		return;

	CLOSE_FILE(lid_file);
	CLOSE_FILE(alarm_file);
	CLOSE_FILE(epoch_file);
}

static void open_file(char *format, char *arg, struct file **var, int mode,
		char *desc)
{
	char buf[256];

	if (strlen(arg)) {
		sprintf(buf, format, arg);
		*var = filp_open(buf, mode, 0);
		if (IS_ERR(*var) || !*var) {
			printk("Failed to open %s file '%s' (%p).\n",
				desc, buf, *var);
			*var = 0;
		}
	}
}

static int powerdown_files_open(int toi_or_resume)
{
	if (!toi_or_resume)
		return 0;

	open_file("/proc/acpi/button/%s/state", lid_state_file, &lid_file, O_RDONLY, "lid");

	if (strlen(wake_alarm_dir)) {
		open_file("/sys/class/rtc/%s/wakealarm", wake_alarm_dir,
				&alarm_file, O_WRONLY, "alarm");

		open_file("/sys/class/rtc/%s/since_epoch", wake_alarm_dir,
				&epoch_file, O_RDONLY, "epoch");
	}

	return 0;
}

static int lid_closed(void)
{
	char array[25];
	ssize_t size;
	loff_t pos = 0;

	if (!lid_file)
		return 0;

	size = vfs_read(lid_file, (char __user *) array, 25, &pos);
	if ((int) size < 1) {
		printk("Failed to read lid state file (%d).\n", (int) size);
		return 0;
	}

	if (!strcmp(array, "state:      closed\n"))
		return 1;

	return 0;
}

static void write_alarm_file(int value)
{
	ssize_t size;
	char buf[40];
	loff_t pos = 0;

	if (!alarm_file)
		return;

	sprintf(buf, "%d\n", value);

	size = vfs_write(alarm_file, (char __user *)buf, strlen(buf), &pos);

	if (size < 0)
		printk("Error %d writing alarm value %s.\n", (int) size, buf);
}

/**
 * toi_check_resleep: See whether to powerdown again after waking.
 *
 * After waking, check whether we should powerdown again in a (usually
 * different) way. We only do this if the lid switch is still closed.
 */
void toi_check_resleep(void)
{
	/* We only return if we suspended to ram and woke. */
	if (lid_closed() && post_wake_state >= 0)
		__toi_power_down(post_wake_state);
}

void toi_power_down(void)
{
	if (alarm_file && wake_delay) {
		char array[25];
		loff_t pos = 0;
		size_t size = vfs_read(epoch_file, (char __user *) array, 25, &pos);

		if (((int) size) < 1)
			printk("Failed to read epoch file (%d).\n", (int) size);
		else {
			unsigned long since_epoch = simple_strtol(array, NULL, 0);

			/* Clear any wakeup time. */
			write_alarm_file(0);

			/* Set new wakeup time. */
			write_alarm_file(since_epoch + wake_delay);
		}
	}

	__toi_power_down(toi_poweroff_method);

	toi_check_resleep();
}

static struct toi_sysfs_data sysfs_params[] = {
#if defined(CONFIG_ACPI)
	{
	 TOI_ATTR("lid_file", SYSFS_RW),
	 SYSFS_STRING(lid_state_file, 256, 0),
	},

	{
	  TOI_ATTR("wake_delay", SYSFS_RW),
	  SYSFS_INT(&wake_delay, 0, INT_MAX, 0)
	},

	{
	  TOI_ATTR("wake_alarm_dir", SYSFS_RW),
	  SYSFS_STRING(wake_alarm_dir, 256, 0)
	},

	{ TOI_ATTR("post_wake_state", SYSFS_RW),
	  SYSFS_INT(&post_wake_state, -1, 5, 0)
	},

	{ TOI_ATTR("powerdown_method", SYSFS_RW),
	  SYSFS_UL(&toi_poweroff_method, 0, 5, 0)
	},
#endif
};

static struct toi_module_ops powerdown_ops = {
	.type				= MISC_HIDDEN_MODULE,
	.name				= "poweroff",
	.initialise			= powerdown_files_open,
	.cleanup			= powerdown_files_close,
	.directory			= "[ROOT]",
	.module				= THIS_MODULE,
	.sysfs_data			= sysfs_params,
	.num_sysfs_entries		= sizeof(sysfs_params) / sizeof(struct toi_sysfs_data),
};

int toi_poweroff_init(void)
{
	return toi_register_module(&powerdown_ops);
}

void toi_poweroff_exit(void)
{
	toi_unregister_module(&powerdown_ops);
}

EXPORT_SYMBOL_GPL(toi_poweroff_method);
EXPORT_SYMBOL_GPL(toi_power_down);
