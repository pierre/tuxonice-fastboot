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

unsigned long suspend2_poweroff_method = 0; /* 0 - Kernel power off */
int wake_delay = 0;
static char lid_state_file[256], wake_alarm_dir[256];
static struct file *lid_file, *alarm_file, *epoch_file;
int post_wake_state = -1;

extern struct hibernation_ops *hibernation_ops;

int suspend2_platform_prepare(void)
{
	return (suspend2_poweroff_method == 4 && hibernation_ops) ?
		hibernation_ops->prepare() : 0;
}

/*
 * __suspend2_power_down
 * Functionality   : Powers down or reboots the computer once the image
 *                   has been written to disk.
 * Key Assumptions : Able to reboot/power down via code called or that
 *                   the warning emitted if the calls fail will be visible
 *                   to the user (ie printk resumes devices).
 * Called From     : do_suspend2_suspend_2
 */

static void __suspend2_power_down(int method)
{
	int result = 0;

	if (test_action_state(TOI_REBOOT)) {
		suspend_prepare_status(DONT_CLEAR_BAR, "Ready to reboot.");
		kernel_restart(NULL);
	}

	suspend_prepare_status(DONT_CLEAR_BAR, "Powering down.");

	switch (method) {
		case 0:
			break;
		case 3:
			if (!pm_ops ||
			    (pm_ops->prepare && pm_ops->prepare(PM_SUSPEND_MEM))) {
				printk("No pm_ops or prepare failed.\n");
				break;
			}

			suspend_console();

			if (device_suspend(PMSG_SUSPEND)) {
				suspend_prepare_status(DONT_CLEAR_BAR, "Device "
					"suspend failure.");
				goto ResumeConsole;
			}

			if (test_action_state(TOI_LATE_CPU_HOTPLUG) &&
				disable_nonboot_cpus())
				goto DeviceResume;
	
			if (!suspend_enter(PM_SUSPEND_MEM)) {
				printk("Failed to enter suspend to ram state.\n");
				result = 1;
			}

			if (test_action_state(TOI_LATE_CPU_HOTPLUG))
				enable_nonboot_cpus();

DeviceResume:
			device_resume();

ResumeConsole:
			resume_console();

			if (pm_ops->finish)
				pm_ops->finish(PM_SUSPEND_MEM);

			/* If suspended to ram and later woke. */
			if (result)
				return;
			break;
		case 4:
			kernel_shutdown_prepare(SYSTEM_SUSPEND_DISK);
			hibernation_ops->enter();
			break;
		case 5:
			/* Historic entry only now */
			break;
	}

	if (method)
		suspend_prepare_status(DONT_CLEAR_BAR,
				"Falling back to alternate power off method.");
	kernel_power_off();
	kernel_halt();
	suspend_prepare_status(DONT_CLEAR_BAR, "Powerdown failed.");
	while (1)
		cpu_relax();
}

void suspend2_platform_finish(void)
{
	if (suspend2_poweroff_method == 4 && hibernation_ops)
		hibernation_ops->finish();
}

#define CLOSE_FILE(file) \
 if (file) { filp_close(file, NULL); file = NULL; }

static void powerdown_files_close(int suspend_or_resume)
{
	if (!suspend_or_resume)
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

static int powerdown_files_open(int suspend_or_resume)
{
	if (!suspend_or_resume)
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
 * suspend2_check_resleep: See whether to powerdown again after waking.
 *
 * After waking, check whether we should powerdown again in a (usually
 * different) way. We only do this if the lid switch is still closed.
 */
void suspend2_check_resleep(void)
{
	/* We only return if we suspended to ram and woke. */
	if (lid_closed() && post_wake_state >= 0)
		__suspend2_power_down(post_wake_state);
}

void suspend2_power_down(void)
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

	__suspend2_power_down(suspend2_poweroff_method);

	suspend2_check_resleep();
}

static struct suspend_sysfs_data sysfs_params[] = {
#if defined(CONFIG_ACPI)
	{
	 SUSPEND2_ATTR("lid_file", SYSFS_RW),
	 SYSFS_STRING(lid_state_file, 256, 0),
	},

	{
	  SUSPEND2_ATTR("wake_delay", SYSFS_RW),
	  SYSFS_INT(&wake_delay, 0, INT_MAX, 0)
	},

	{
	  SUSPEND2_ATTR("wake_alarm_dir", SYSFS_RW),
	  SYSFS_STRING(wake_alarm_dir, 256, 0)
	},

	{ SUSPEND2_ATTR("post_wake_state", SYSFS_RW),
	  SYSFS_INT(&post_wake_state, -1, 5, 0)
	},

	{ SUSPEND2_ATTR("powerdown_method", SYSFS_RW),
	  SYSFS_UL(&suspend2_poweroff_method, 0, 5, 0)
	},
#endif
};

static struct suspend_module_ops powerdown_ops = {
	.type				= MISC_HIDDEN_MODULE,
	.name				= "poweroff",
	.initialise			= powerdown_files_open,
	.cleanup			= powerdown_files_close,
	.directory			= "[ROOT]",
	.module				= THIS_MODULE,
	.sysfs_data			= sysfs_params,
	.num_sysfs_entries		= sizeof(sysfs_params) / sizeof(struct suspend_sysfs_data),
};

int s2_poweroff_init(void)
{
	return suspend_register_module(&powerdown_ops);
}

void s2_poweroff_exit(void)
{
	suspend_unregister_module(&powerdown_ops);
}

EXPORT_SYMBOL_GPL(suspend2_poweroff_method);
EXPORT_SYMBOL_GPL(suspend2_power_down);
