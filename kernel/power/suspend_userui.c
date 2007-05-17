/*
 * kernel/power/user_ui.c
 *
 * Copyright (C) 2005-2007 Bernard Blackham
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
 
#include "sysfs.h"
#include "modules.h"
#include "suspend.h"
#include "ui.h"
#include "netlink.h"
#include "power_off.h"

static char local_printf_buf[1024];	/* Same as printk - should be safe */

static struct user_helper_data ui_helper_data;
static struct suspend_module_ops userui_ops;
static int orig_kmsg;

static char lastheader[512];
static int lastheader_message_len;
static int ui_helper_changed; /* Used at resume-time so don't overwrite value
				set from initrd/ramfs. */

/* Number of distinct progress amounts that userspace can display */
static int progress_granularity = 30;

DECLARE_WAIT_QUEUE_HEAD(userui_wait_for_key);

static void ui_nl_set_state(int n)
{
	/* Only let them change certain settings */
	static const int suspend_action_mask =
		(1 << SUSPEND_REBOOT) | (1 << SUSPEND_PAUSE) | (1 << SUSPEND_SLOW) |
		(1 << SUSPEND_LOGALL) | (1 << SUSPEND_SINGLESTEP) |
		(1 << SUSPEND_PAUSE_NEAR_PAGESET_END);

	suspend_action = (suspend_action & (~suspend_action_mask)) |
		(n & suspend_action_mask);

	if (!test_action_state(SUSPEND_PAUSE) &&
			!test_action_state(SUSPEND_SINGLESTEP))
		wake_up_interruptible(&userui_wait_for_key);
}

static void userui_redraw(void)
{
	suspend_send_netlink_message(&ui_helper_data,
			USERUI_MSG_REDRAW, NULL, 0);
}

static int userui_storage_needed(void)
{
	return sizeof(ui_helper_data.program) + 1 + sizeof(int);
}

static int userui_save_config_info(char *buf)
{
	*((int *) buf) = progress_granularity;
	memcpy(buf + sizeof(int), ui_helper_data.program, sizeof(ui_helper_data.program));
	return sizeof(ui_helper_data.program) + sizeof(int) + 1;
}

static void userui_load_config_info(char *buf, int size)
{
	progress_granularity = *((int *) buf);
	size -= sizeof(int);

	/* Don't load the saved path if one has already been set */
	if (ui_helper_changed)
		return;

	if (size > sizeof(ui_helper_data.program))
		size = sizeof(ui_helper_data.program);

	memcpy(ui_helper_data.program, buf + sizeof(int), size);
	ui_helper_data.program[sizeof(ui_helper_data.program)-1] = '\0';
}

static void set_ui_program_set(void)
{
	ui_helper_changed = 1;
}

static int userui_memory_needed(void)
{
	/* ball park figure of 128 pages */
	return (128 * PAGE_SIZE);
}

/* suspend_update_status
 *
 * Description: Update the progress bar and (if on) in-bar message.
 * Arguments:	UL value, maximum: Current progress percentage (value/max).
 * 		const char *fmt, ...: Message to be displayed in the middle
 * 		of the progress bar.
 * 		Note that a NULL message does not mean that any previous
 * 		message is erased! For that, you need suspend_prepare_status with
 * 		clearbar on.
 * Returns:	Unsigned long: The next value where status needs to be updated.
 * 		This is to reduce unnecessary calls to update_status.
 */
static unsigned long userui_update_status(unsigned long value,
		unsigned long maximum, const char *fmt, ...)
{
	static int last_step = -1;
	struct userui_msg_params msg;
	int bitshift;
	int this_step;
	unsigned long next_update;

	if (ui_helper_data.pid == -1)
		return 0;

	if ((!maximum) || (!progress_granularity))
		return maximum;

	if (value < 0)
		value = 0;

	if (value > maximum)
		value = maximum;

	/* Try to avoid math problems - we can't do 64 bit math here
	 * (and shouldn't need it - anyone got screen resolution
	 * of 65536 pixels or more?) */
	bitshift = fls(maximum) - 16;
	if (bitshift > 0) {
		unsigned long temp_maximum = maximum >> bitshift;
		unsigned long temp_value = value >> bitshift;
		this_step = (int)
			(temp_value * progress_granularity / temp_maximum);
		next_update = (((this_step + 1) * temp_maximum /
					progress_granularity) + 1) << bitshift;
	} else {
		this_step = (int) (value * progress_granularity / maximum);
		next_update = ((this_step + 1) * maximum /
				progress_granularity) + 1;
	}

	if (this_step == last_step)
		return next_update;

	memset(&msg, 0, sizeof(msg));

	msg.a = this_step;
	msg.b = progress_granularity;

	if (fmt) {
		va_list args;
		va_start(args, fmt);
		vsnprintf(msg.text, sizeof(msg.text), fmt, args);
		va_end(args);
		msg.text[sizeof(msg.text)-1] = '\0';
	}

	suspend_send_netlink_message(&ui_helper_data, USERUI_MSG_PROGRESS,
			&msg, sizeof(msg));
	last_step = this_step;

	return next_update;
}

/* userui_message.
 *
 * Description:	This function is intended to do the same job as printk, but
 * 		without normally logging what is printed. The point is to be
 * 		able to get debugging info on screen without filling the logs
 * 		with "1/534. ^M 2/534^M. 3/534^M"
 *
 * 		It may be called from an interrupt context - can't sleep!
 *
 * Arguments:	int mask: The debugging section(s) this message belongs to.
 * 		int level: The level of verbosity of this message.
 * 		int restartline: Whether to output a \r or \n with this line
 * 			(\n if we're logging all output).
 * 		const char *fmt, ...: Message to be displayed a la printk.
 */
static void userui_message(unsigned long section, unsigned long level,
		int normally_logged, const char *fmt, ...)
{
	struct userui_msg_params msg;

	if ((level) && (level > console_loglevel))
		return;

	memset(&msg, 0, sizeof(msg));

	msg.a = section;
	msg.b = level;
	msg.c = normally_logged;

	if (fmt) {
		va_list args;
		va_start(args, fmt);
		vsnprintf(msg.text, sizeof(msg.text), fmt, args);
		va_end(args);
		msg.text[sizeof(msg.text)-1] = '\0';
	}

	if (test_action_state(SUSPEND_LOGALL))
		printk("%s\n", msg.text);

	suspend_send_netlink_message(&ui_helper_data, USERUI_MSG_MESSAGE,
			&msg, sizeof(msg));
}

static void wait_for_key_via_userui(void)
{
	DECLARE_WAITQUEUE(wait, current);

	add_wait_queue(&userui_wait_for_key, &wait);
	set_current_state(TASK_INTERRUPTIBLE);

	interruptible_sleep_on(&userui_wait_for_key);

	set_current_state(TASK_RUNNING);
	remove_wait_queue(&userui_wait_for_key, &wait);
}

static char userui_wait_for_keypress(int timeout)
{
	int fd;
	char key = '\0';
	struct termios t, t_backup;

	if (ui_helper_data.pid != -1) {
		wait_for_key_via_userui();
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
	if (timeout)
		t.c_cc[VTIME] = timeout*10;

	if (sys_ioctl(fd, TCSETS, (long)&t) < 0)
		goto out_restore;

	while (1) {
		if (sys_read(fd, &key, 1) <= 0) {
			key = '\0';
			break;
		}
		key = tolower(key);
		if (test_suspend_state(SUSPEND_SANITY_CHECK_PROMPT)) {
			if (key == 'c') {
				set_suspend_state(SUSPEND_CONTINUE_REQ);
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

/* suspend_prepare_status
 * Description:	Prepare the 'nice display', drawing the header and version,
 * 		along with the current action and perhaps also resetting the
 * 		progress bar.
 * Arguments:	
 * 		int clearbar: Whether to reset the progress bar.
 * 		const char *fmt, ...: The action to be displayed.
 */
static void userui_prepare_status(int clearbar, const char *fmt, ...)
{
	va_list args;

	if (fmt) {
		va_start(args, fmt);
		lastheader_message_len = vsnprintf(lastheader, 512, fmt, args);
		va_end(args);
	}

	if (clearbar)
		suspend_update_status(0, 1, NULL);

	suspend_message(0, SUSPEND_STATUS, 1, lastheader, NULL);

	if (ui_helper_data.pid == -1)
		printk(KERN_EMERG "%s\n", lastheader);
}

/* abort_suspend
 *
 * Description: Begin to abort a cycle. If this wasn't at the user's request
 * 		(and we're displaying output), tell the user why and wait for
 * 		them to acknowledge the message.
 * Arguments:	A parameterised string (imagine this is printk) to display,
 *	 	telling the user why we're aborting.
 */

static void userui_abort_suspend(int result_code, const char *fmt, ...)
{
	va_list args;
	int printed_len = 0;

	set_result_state(result_code);
	if (!test_result_state(SUSPEND_ABORTED)) {
		if (!test_result_state(SUSPEND_ABORT_REQUESTED)) {
			va_start(args, fmt);
			printed_len = vsnprintf(local_printf_buf, 
					sizeof(local_printf_buf), fmt, args);
			va_end(args);
			if (ui_helper_data.pid != -1)
				printed_len = sprintf(local_printf_buf + printed_len,
					" (Press SPACE to continue)");
			suspend_prepare_status(CLEAR_BAR, local_printf_buf);

			if (ui_helper_data.pid != -1)
				suspend_wait_for_keypress(0);
		}
		/* Turn on aborting flag */
		set_result_state(SUSPEND_ABORTED);
	}
}

/* request_abort_suspend
 *
 * Description:	Handle the user requesting the cancellation of a suspend by
 * 		pressing escape.
 * Callers:	Invoked from a netlink packet from userspace when the user presses
 * 	 	escape.
 */
static void request_abort_suspend(void)
{
	if (test_result_state(SUSPEND_ABORT_REQUESTED))
		return;

	if (test_suspend_state(SUSPEND_NOW_RESUMING)) {
		suspend_prepare_status(CLEAR_BAR, "Escape pressed. "
					"Powering down again.");
		set_suspend_state(SUSPEND_STOP_RESUME);
		while (!test_suspend_state(SUSPEND_IO_STOPPED))
			schedule();
		if (suspendActiveAllocator->mark_resume_attempted)
			suspendActiveAllocator->mark_resume_attempted(0);
		suspend2_power_down();
	} else {
		suspend_prepare_status(CLEAR_BAR, "--- ESCAPE PRESSED :"
					" ABORTING SUSPEND ---");
		set_result_state(SUSPEND_ABORTED);
		set_result_state(SUSPEND_ABORT_REQUESTED);
	
		wake_up_interruptible(&userui_wait_for_key);
	}
}

static int userui_user_rcv_msg(struct sk_buff *skb, struct nlmsghdr *nlh)
{
	int type;
	int *data;

	type = nlh->nlmsg_type;

	/* A control message: ignore them */
	if (type < NETLINK_MSG_BASE)
		return 0;

	/* Unknown message: reply with EINVAL */
	if (type >= USERUI_MSG_MAX)
		return -EINVAL;

	/* All operations require privileges, even GET */
	if (security_netlink_recv(skb, CAP_NET_ADMIN))
		return -EPERM;

	/* Only allow one task to receive NOFREEZE privileges */
	if (type == NETLINK_MSG_NOFREEZE_ME && ui_helper_data.pid != -1) {
		printk("Got NOFREEZE_ME request when ui_helper_data.pid is %d.\n", ui_helper_data.pid);
		return -EBUSY;
	}

	data = (int*)NLMSG_DATA(nlh);

	switch (type) {
		case USERUI_MSG_ABORT:
			request_abort_suspend();
			break;
		case USERUI_MSG_GET_STATE:
			suspend_send_netlink_message(&ui_helper_data, 
					USERUI_MSG_GET_STATE, &suspend_action,
					sizeof(suspend_action));
			break;
		case USERUI_MSG_GET_DEBUG_STATE:
			suspend_send_netlink_message(&ui_helper_data,
					USERUI_MSG_GET_DEBUG_STATE,
					&suspend_debug_state,
					sizeof(suspend_debug_state));
			break;
		case USERUI_MSG_SET_STATE:
			if (nlh->nlmsg_len < NLMSG_LENGTH(sizeof(int)))
				return -EINVAL;
			ui_nl_set_state(*data);
			break;
		case USERUI_MSG_SET_DEBUG_STATE:
			if (nlh->nlmsg_len < NLMSG_LENGTH(sizeof(int)))
				return -EINVAL;
			suspend_debug_state = (*data);
			break;
		case USERUI_MSG_SPACE:
			wake_up_interruptible(&userui_wait_for_key);
			break;
		case USERUI_MSG_GET_POWERDOWN_METHOD:
			suspend_send_netlink_message(&ui_helper_data,
					USERUI_MSG_GET_POWERDOWN_METHOD,
					&suspend2_poweroff_method,
					sizeof(suspend2_poweroff_method));
			break;
		case USERUI_MSG_SET_POWERDOWN_METHOD:
			if (nlh->nlmsg_len < NLMSG_LENGTH(sizeof(int)))
				return -EINVAL;
			suspend2_poweroff_method = (*data);
			break;
		case USERUI_MSG_GET_LOGLEVEL:
			suspend_send_netlink_message(&ui_helper_data,
					USERUI_MSG_GET_LOGLEVEL,
					&suspend_default_console_level,
					sizeof(suspend_default_console_level));
			break;
		case USERUI_MSG_SET_LOGLEVEL:
			if (nlh->nlmsg_len < NLMSG_LENGTH(sizeof(int)))
				return -EINVAL;
			suspend_default_console_level = (*data);
			break;
	}

	return 1;
}

/* userui_cond_pause
 * 
 * Description:	Potentially pause and wait for the user to tell us to continue.
 * 		We normally only pause when @pause is set.
 * Arguments:	int pause: Whether we normally pause.
 * 		char *message: The message to display. Not parameterised
 * 		 because it's normally a constant.
 */

static void userui_cond_pause(int pause, char *message)
{
	int displayed_message = 0, last_key = 0;
	
	while (last_key != 32 &&
		ui_helper_data.pid != -1 &&
		(!test_result_state(SUSPEND_ABORTED)) &&
		((test_action_state(SUSPEND_PAUSE) && pause) || 
		 (test_action_state(SUSPEND_SINGLESTEP)))) {
		if (!displayed_message) {
			suspend_prepare_status(DONT_CLEAR_BAR, 
			   "%s Press SPACE to continue.%s",
			   message ? message : "",
			   (test_action_state(SUSPEND_SINGLESTEP)) ? 
			   " Single step on." : "");
			displayed_message = 1;
		}
		last_key = suspend_wait_for_keypress(0);
	}
	schedule();
}

/* userui_prepare_console
 *
 * Description:	Prepare a console for use, save current settings.
 * Returns:	Boolean: Whether an error occured. Errors aren't
 * 		treated as fatal, but a warning is printed.
 */
static void userui_prepare_console(void)
{
	orig_kmsg = kmsg_redirect;
	kmsg_redirect = fg_console + 1;

	ui_helper_data.pid = -1;

	if (!userui_ops.enabled)
		return;

	if (!*ui_helper_data.program) {
		printk("suspend_userui: program not configured. suspend_userui disabled.\n");
		return;
	}

	suspend_netlink_setup(&ui_helper_data);

	return;
}

/* userui_cleanup_console
 *
 * Description: Restore the settings we saved above.
 */

static void userui_cleanup_console(void)
{
	if (ui_helper_data.pid > -1)
		suspend_netlink_close(&ui_helper_data);

	kmsg_redirect = orig_kmsg;
}

/*
 * User interface specific /sys/power/suspend2 entries.
 */

static struct suspend_sysfs_data sysfs_params[] = {
#if defined(CONFIG_NET) && defined(CONFIG_SYSFS)
	{ SUSPEND2_ATTR("enable_escape", SYSFS_RW),
	  SYSFS_BIT(&suspend_action, SUSPEND_CAN_CANCEL, 0)
	},

	{ SUSPEND2_ATTR("pause_between_steps", SYSFS_RW),
	  SYSFS_BIT(&suspend_action, SUSPEND_PAUSE, 0)
	},

	{ SUSPEND2_ATTR("enabled", SYSFS_RW),
	  SYSFS_INT(&userui_ops.enabled, 0, 1, 0)
	},

	{ SUSPEND2_ATTR("progress_granularity", SYSFS_RW),
	  SYSFS_INT(&progress_granularity, 1, 2048, 0)
	},

	{ SUSPEND2_ATTR("program", SYSFS_RW),
	  SYSFS_STRING(ui_helper_data.program, 255, 0),
	  .write_side_effect = set_ui_program_set,
	},
#endif
};

static struct suspend_module_ops userui_ops = {
	.type				= MISC_MODULE,
	.name				= "Userspace UI",
	.shared_directory		= "Basic User Interface",
	.module				= THIS_MODULE,
	.storage_needed			= userui_storage_needed,
	.save_config_info		= userui_save_config_info,
	.load_config_info		= userui_load_config_info,
	.memory_needed			= userui_memory_needed,
	.sysfs_data			= sysfs_params,
	.num_sysfs_entries		= sizeof(sysfs_params) / sizeof(struct suspend_sysfs_data),
};

static struct ui_ops my_ui_ops = {
	.redraw				= userui_redraw,
	.update_status			= userui_update_status,
	.message			= userui_message,
	.prepare_status			= userui_prepare_status,
	.abort				= userui_abort_suspend,
	.cond_pause			= userui_cond_pause,
	.prepare			= userui_prepare_console,
	.cleanup			= userui_cleanup_console,
	.wait_for_key			= userui_wait_for_keypress,
};

/* suspend_console_sysfs_init
 * Description: Boot time initialisation for user interface.
 */

static __init int s2_user_ui_init(void)
{
	int result;

	ui_helper_data.nl = NULL;
	ui_helper_data.program[0] = '\0';
	ui_helper_data.pid = -1;
	ui_helper_data.skb_size = sizeof(struct userui_msg_params);
	ui_helper_data.pool_limit = 6;
	ui_helper_data.netlink_id = NETLINK_SUSPEND2_USERUI;
	ui_helper_data.name = "userspace ui";
	ui_helper_data.rcv_msg = userui_user_rcv_msg;
	ui_helper_data.interface_version = 7;
	ui_helper_data.must_init = 0;
	ui_helper_data.not_ready = userui_cleanup_console;
	init_completion(&ui_helper_data.wait_for_process);
	result = suspend_register_module(&userui_ops);
	if (!result)
		result = s2_register_ui_ops(&my_ui_ops);
	if (result)
		suspend_unregister_module(&userui_ops);

	return result;
}

#ifdef MODULE
static __exit void s2_user_ui_exit(void)
{
	s2_remove_ui_ops(&my_ui_ops);
	suspend_unregister_module(&userui_ops);
}

module_init(s2_user_ui_init);
module_exit(s2_user_ui_exit);
MODULE_AUTHOR("Nigel Cunningham");
MODULE_DESCRIPTION("Suspend2 Userui Support");
MODULE_LICENSE("GPL");
#else
late_initcall(s2_user_ui_init);
#endif
