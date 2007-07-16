/*
 * kernel/power/tuxonice_ui.c
 *
 * Copyright (C) 1998-2001 Gabor Kuti <seasons@fornax.hu>
 * Copyright (C) 1998,2001,2002 Pavel Machek <pavel@suse.cz>
 * Copyright (C) 2002-2003 Florent Chabaud <fchabaud@free.fr>
 * Copyright (C) 2002-2007 Nigel Cunningham (nigel at suspend2 net)
 *
 * This file is released under the GPLv2.
 *
 * Routines for Suspend2's user interface.
 *
 * The user interface code talks to a userspace program via a
 * netlink socket.
 *
 * The kernel side:
 * - starts the userui program;
 * - sends text messages and progress bar status;
 *
 * The user space side:
 * - passes messages regarding user requests (abort, toggle reboot etc)
 *
 */

#define __KERNEL_SYSCALLS__

#include <linux/suspend.h>
#include <linux/freezer.h>
#include <linux/console.h>
#include <linux/ctype.h>
#include <linux/tty.h>
#include <linux/vt_kern.h>
#include <linux/module.h>
#include <linux/reboot.h>
#include <linux/kmod.h>
#include <linux/security.h>
#include <linux/syscalls.h>
 
#include "tuxonice_sysfs.h"
#include "tuxonice_modules.h"
#include "tuxonice.h"
#include "tuxonice_ui.h"
#include "tuxonice_netlink.h"
#include "tuxonice_power_off.h"

static char local_printf_buf[1024];	/* Same as printk - should be safe */
extern int suspend2_wait;
struct ui_ops *s2_current_ui;

/*! The console log level we default to. */
int suspend_default_console_level = 0;

/**
 * suspend_wait_for_keypress - Wait for keypress via userui or /dev/console.
 *
 * @timeout: Maximum time to wait.
 *
 * Wait for a keypress, either from userui or /dev/console if userui isn't
 * available. The non-userui path is particularly for at boot-time, prior
 * to userui being started, when we have an important warning to give to
 * the user.
 */
static char suspend_wait_for_keypress(int timeout)
{
	int fd, this_timeout = 255;
	char key = '\0';
	struct termios t, t_backup;

	if (s2_current_ui && s2_current_ui->wait_for_key(timeout)) {
		key = ' ';
		goto out;
	}
	
	/* We should be guaranteed /dev/console exists after populate_rootfs() in
	 * init/main.c
	 */
	if ((fd = sys_open("/dev/console", O_RDONLY, 0)) < 0) {
		printk("Couldn't open /dev/console.\n");
		goto out;
	}

	if (sys_ioctl(fd, TCGETS, (long)&t) < 0)
		goto out_close;

	memcpy(&t_backup, &t, sizeof(t));

	t.c_lflag &= ~(ISIG|ICANON|ECHO);
	t.c_cc[VMIN] = 0;

new_timeout:
	if (timeout > 0) {
		this_timeout = timeout < 26 ? timeout : 25;
		timeout -= this_timeout;
		this_timeout *= 10;
	}

	t.c_cc[VTIME] = this_timeout;

	if (sys_ioctl(fd, TCSETS, (long)&t) < 0)
		goto out_restore;

	while (1) {
		if (sys_read(fd, &key, 1) <= 0) {
			if (timeout)
				goto new_timeout;
			key = '\0';
			break;
		}
		key = tolower(key);
		if (test_toi_state(TOI_SANITY_CHECK_PROMPT)) {
			if (key == 'c') {
				set_toi_state(TOI_CONTINUE_REQ);
				break;
			} else if (key == ' ')
				break;
		} else
			break;
	}

out_restore:
	sys_ioctl(fd, TCSETS, (long)&t_backup);
out_close:
	sys_close(fd);
out:
	return key;
}

/* suspend_early_boot_message()
 * Description:	Handle errors early in the process of booting.
 * 		The user may press C to continue booting, perhaps
 * 		invalidating the image,  or space to reboot. 
 * 		This works from either the serial console or normally 
 * 		attached keyboard.
 *
 * 		Note that we come in here from init, while the kernel is
 * 		locked. If we want to get events from the serial console,
 * 		we need to temporarily unlock the kernel.
 *
 * 		suspend_early_boot_message may also be called post-boot.
 * 		In this case, it simply printks the message and returns.
 *
 * Arguments:	int	Whether we are able to erase the image.
 * 		int	default_answer. What to do when we timeout. This
 * 			will normally be continue, but the user might
 * 			provide command line options (__setup) to override
 * 			particular cases.
 * 		Char *. Pointer to a string explaining why we're moaning.
 */

#define say(message, a...) printk(KERN_EMERG message, ##a)

void suspend_early_boot_message(int message_detail, int default_answer, char *warning_reason, ...)
{
#if defined(CONFIG_VT) || defined(CONFIG_SERIAL_CONSOLE)
	unsigned long orig_state = get_toi_state(), continue_req = 0;
	unsigned long orig_loglevel = console_loglevel;
	int can_ask = 1;
#else
	int can_ask = 0;
#endif

	va_list args;
	int printed_len;

	if (!suspend2_wait) {
		set_toi_state(TOI_CONTINUE_REQ);
		can_ask = 0;
	}

	if (warning_reason) {
		va_start(args, warning_reason);
		printed_len = vsnprintf(local_printf_buf, 
				sizeof(local_printf_buf), 
				warning_reason,
				args);
		va_end(args);
	}

	if (!test_toi_state(TOI_BOOT_TIME)) {
		printk("Suspend2: %s\n", local_printf_buf);
		return;
	}

	if (!can_ask) {
		continue_req = !!default_answer;
		goto post_ask;
	}

#if defined(CONFIG_VT) || defined(CONFIG_SERIAL_CONSOLE)
	console_loglevel = 7;

	say("=== Suspend2 ===\n\n");
	if (warning_reason) {
		say("BIG FAT WARNING!! %s\n\n", local_printf_buf);
		switch (message_detail) {
		 case 0:
			say("If you continue booting, note that any image WILL NOT BE REMOVED.\n");
			say("Suspend is unable to do so because the appropriate modules aren't\n");
			say("loaded. You should manually remove the image to avoid any\n");
			say("possibility of corrupting your filesystem(s) later.\n");
			break;
		 case 1:
			say("If you want to use the current suspend image, reboot and try\n");
			say("again with the same kernel that you suspended from. If you want\n");
			say("to forget that image, continue and the image will be erased.\n");
			break;
		}
		say("Press SPACE to reboot or C to continue booting with this kernel\n\n");
		if (suspend2_wait > 0)
			say("Default action if you don't select one in %d seconds is: %s.\n",
				suspend2_wait,
				default_answer == TOI_CONTINUE_REQ ?
				"continue booting" : "reboot");
	} else {
		say("BIG FAT WARNING!!\n\n");
		say("You have tried to resume from this image before.\n");
		say("If it failed once, it may well fail again.\n");
		say("Would you like to remove the image and boot normally?\n");
		say("This will be equivalent to entering noresume on the\n");
		say("kernel command line.\n\n");
		say("Press SPACE to remove the image or C to continue resuming.\n\n");
		if (suspend2_wait > 0)
			say("Default action if you don't select one in %d seconds is: %s.\n",
				suspend2_wait,
				!!default_answer ?
				"continue resuming" : "remove the image");
	}
	console_loglevel = orig_loglevel;
	
	set_toi_state(TOI_SANITY_CHECK_PROMPT);
	clear_toi_state(TOI_CONTINUE_REQ);

	if (suspend_wait_for_keypress(suspend2_wait) == 0) /* We timed out */
		continue_req = !!default_answer;
	else
		continue_req = test_toi_state(TOI_CONTINUE_REQ);

#endif /* CONFIG_VT or CONFIG_SERIAL_CONSOLE */

post_ask:
	if ((warning_reason) && (!continue_req))
		machine_restart(NULL);
	
	restore_toi_state(orig_state);
	if (continue_req)
		set_toi_state(TOI_CONTINUE_REQ);
}
#undef say

/*
 * User interface specific /sys/power/suspend2 entries.
 */

static struct suspend_sysfs_data sysfs_params[] = {
#if defined(CONFIG_NET) && defined(CONFIG_SYSFS)
	{ SUSPEND2_ATTR("default_console_level", SYSFS_RW),
	  SYSFS_INT(&suspend_default_console_level, 0, 7, 0)
	},

	{ SUSPEND2_ATTR("debug_sections", SYSFS_RW),
	  SYSFS_UL(&suspend_debug_state, 0, 1 << 30, 0)
	},

	{ SUSPEND2_ATTR("log_everything", SYSFS_RW),
	  SYSFS_BIT(&suspend_action, TOI_LOGALL, 0)
	},
#endif
	{ SUSPEND2_ATTR("pm_prepare_console", SYSFS_RW),
	  SYSFS_BIT(&suspend_action, TOI_PM_PREPARE_CONSOLE, 0)
	}
};

static struct suspend_module_ops userui_ops = {
	.type				= MISC_HIDDEN_MODULE,
	.name				= "printk ui",
	.directory			= "user_interface",
	.module				= THIS_MODULE,
	.sysfs_data			= sysfs_params,
	.num_sysfs_entries		= sizeof(sysfs_params) / sizeof(struct suspend_sysfs_data),
};

int s2_register_ui_ops(struct ui_ops *this_ui)
{
	if (s2_current_ui) {
		printk("Only one Suspend2 user interface module can be loaded"
			" at a time.");
		return -EBUSY;
	}

	s2_current_ui = this_ui;

	return 0;
}

void s2_remove_ui_ops(struct ui_ops *this_ui)
{
	if (s2_current_ui != this_ui)
		return;

	s2_current_ui = NULL;
}

/* suspend_console_sysfs_init
 * Description: Boot time initialisation for user interface.
 */

int s2_ui_init(void)
{
	return suspend_register_module(&userui_ops);
}

void s2_ui_exit(void)
{
	suspend_unregister_module(&userui_ops);
}

#ifdef CONFIG_TOI_EXPORTS
EXPORT_SYMBOL_GPL(s2_current_ui);
EXPORT_SYMBOL_GPL(suspend_early_boot_message);
EXPORT_SYMBOL_GPL(s2_register_ui_ops);
EXPORT_SYMBOL_GPL(s2_remove_ui_ops);
EXPORT_SYMBOL_GPL(suspend_default_console_level);
#endif
