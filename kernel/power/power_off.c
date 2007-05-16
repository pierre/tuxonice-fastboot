/*
 * kernel/power/power_off.c
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
#include "suspend.h"
#include "ui.h"
#include "power_off.h"
#include "power.h"

unsigned long suspend2_poweroff_method = 0; /* 0 - Kernel power off */

static int try_pm_state_powerdown(void)
{
	int result = 0;

	if (pm_ops && pm_ops->prepare && suspend2_poweroff_method &&
	    pm_ops->prepare(suspend2_poweroff_method))
			return 0;

	if (suspend2_poweroff_method > 3)
		kernel_shutdown_prepare(SYSTEM_SUSPEND_DISK);
	else {
		if (device_suspend(PMSG_SUSPEND)) {
			printk(KERN_ERR "Some devices failed to suspend\n");
			return 0;
		}

		disable_nonboot_cpus();
	}

	mdelay(1000); /* Give time for devices to power down */
	
	if (!suspend_enter(suspend2_poweroff_method))
		result = 1;

	enable_nonboot_cpus();

	if (pm_ops && pm_ops->finish && suspend2_poweroff_method)
		pm_ops->finish(suspend2_poweroff_method);

	device_resume();

	return result;
}

/*
 * suspend_power_down
 * Functionality   : Powers down or reboots the computer once the image
 *                   has been written to disk.
 * Key Assumptions : Able to reboot/power down via code called or that
 *                   the warning emitted if the calls fail will be visible
 *                   to the user (ie printk resumes devices).
 * Called From     : do_suspend2_suspend_2
 */

void suspend_power_down(void)
{
	if (test_action_state(SUSPEND_REBOOT)) {
		suspend_prepare_status(DONT_CLEAR_BAR, "Ready to reboot.");
		kernel_restart(NULL);
	}

	suspend_prepare_status(DONT_CLEAR_BAR, "Powering down.");

	if (pm_ops && pm_ops->enter && suspend2_poweroff_method && try_pm_state_powerdown())
		return;

	kernel_shutdown_prepare(SYSTEM_POWER_OFF);

	mdelay(1000); /* Give time for devices to power down */

	machine_power_off();
	machine_halt();
	suspend_prepare_status(DONT_CLEAR_BAR, "Powerdown failed");
	while (1)
		cpu_relax();
}

EXPORT_SYMBOL_GPL(suspend2_poweroff_method);
EXPORT_SYMBOL_GPL(suspend_power_down);
