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
#include <linux/console.h>
#include "suspend.h"
#include "ui.h"
#include "power_off.h"
#include "power.h"

unsigned long suspend2_poweroff_method = 0; /* 0 - Kernel power off */

/*
 * suspend2_power_down
 * Functionality   : Powers down or reboots the computer once the image
 *                   has been written to disk.
 * Key Assumptions : Able to reboot/power down via code called or that
 *                   the warning emitted if the calls fail will be visible
 *                   to the user (ie printk resumes devices).
 * Called From     : do_suspend2_suspend_2
 */

void suspend2_power_down(void)
{
	int result = 0;

	if (test_action_state(SUSPEND_REBOOT)) {
		suspend_prepare_status(DONT_CLEAR_BAR, "Ready to reboot.");
		kernel_restart(NULL);
	}

	suspend_prepare_status(DONT_CLEAR_BAR, "Powering down.");

	switch (suspend2_poweroff_method) {
		case 0:
			break;
		case 3:
			suspend_console();

			if (device_suspend(PMSG_SUSPEND)) {
				suspend_prepare_status(DONT_CLEAR_BAR, "Device "
					"suspend failure. Doing poweroff.");
				goto ResumeConsole;
			}

			if (!pm_ops ||
			    (pm_ops->prepare && pm_ops->prepare(PM_SUSPEND_MEM)))
				goto DeviceResume;

			if (test_action_state(SUSPEND_LATE_CPU_HOTPLUG) &&
				disable_nonboot_cpus())
				goto PmOpsFinish;
	
			if (!suspend_enter(PM_SUSPEND_MEM))
				result = 1;

			if (test_action_state(SUSPEND_LATE_CPU_HOTPLUG))
				enable_nonboot_cpus();

PmOpsFinish:
			if (pm_ops->finish)
				pm_ops->finish(PM_SUSPEND_MEM);

DeviceResume:
			device_resume();

ResumeConsole:
			resume_console();

			/* If suspended to ram and later woke. */
			if (result)
				return;
			break;
		case 4:
		case 5:
			if (!pm_ops ||
			    (pm_ops->prepare && pm_ops->prepare(PM_SUSPEND_MAX)))
				break;

			kernel_shutdown_prepare(SYSTEM_SUSPEND_DISK);
			suspend_enter(suspend2_poweroff_method);

			/* Failed. Fall back to kernel_power_off etc. */
			if (pm_ops->finish)
				pm_ops->finish(PM_SUSPEND_MAX);
	}

	suspend_prepare_status(DONT_CLEAR_BAR, "Falling back to alternate power off method.");
	kernel_power_off();
	kernel_halt();
	suspend_prepare_status(DONT_CLEAR_BAR, "Powerdown failed.");
	while (1)
		cpu_relax();
}

EXPORT_SYMBOL_GPL(suspend2_poweroff_method);
EXPORT_SYMBOL_GPL(suspend2_power_down);
