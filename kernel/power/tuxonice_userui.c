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
 
#include "tuxonice_sysfs.h"
#include "tuxonice_modules.h"
#include "tuxonice.h"
#include "tuxonice_ui.h"
#include "tuxonice_netlink.h"
#include "tuxonice_power_off.h"

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

/**
 * ui_nl_set_state - Update suspend_action based on a message from userui.
 *
 * @n: The bit (1 << bit) to set.
 */
static void ui_nl_set_state(int n)
{
	/* Only let them change certain settings */
	static const int suspend_action_mask =
		(1 << SUSPEND_REBOOT) | (1 << SUSPEND_PAUSE) |
		(1 << SUSPEND_SLOW) | (1 << SUSPEND_LOGALL) |
		(1 << SUSPEND_SINGLESTEP) |
		(1 << SUSPEND_PAUSE_NEAR_PAGESET_END);

	suspend_action = (suspend_action & (~suspend_action_mask)) |
		(n & suspend_action_mask);

	if (!test_action_state(SUSPEND_PAUSE) &&
			!test_action_state(SUSPEND_SINGLESTEP))
		wake_up_interruptible(&userui_wait_for_key);
}

/**
 * userui_post_atomic_restore - Tell userui that atomic restore just happened.
 *
 * Tell userui that atomic restore just occured, so that it can do things like
 * redrawing the screen, re-getting settings and so on.
 */
static void userui_post_atomic_restore(void)
{
	suspend_send_netlink_message(&ui_helper_data,
			USERUI_MSG_POST_ATOMIC_RESTORE, NULL, 0);
}

/**
 * userui_storage_needed - Report how much memory in image header is needed.
 */
static int userui_storage_needed(void)
{
	return sizeof(ui_helper_data.program) + 1 + sizeof(int);
}

/**
 * userui_save_config_info - Fill buffer with config info for image header.
 *
 * @buf: Buffer into which to put the config info we want to save.
 */
static int userui_save_config_info(char *buf)
{
	*((int *) buf) = progress_granularity;
	memcpy(buf + sizeof(int), ui_helper_data.program, sizeof(ui_helper_data.program));
	return sizeof(ui_helper_data.program) + sizeof(int) + 1;
}

/**
 * userui_load_config_info - Restore config info from buffer.
 *
 * @buf: Buffer containing header info loaded.
 * @size: Size of data loaded for this module.
 */
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

/**
 * set_ui_program_set: Record that userui program was changed.
 *
 * Side effect routine for when the userui program is set. In an initrd or
 * ramfs, the user may set a location for the userui program. If this happens,
 * we don't want to reload the value that was saved in the image header. This
 * routine allows us to flag that we shouldn't restore the program name from
 * the image header.
 */
static void set_ui_program_set(void)
{
	ui_helper_changed = 1;
}

/**
 * userui_memory_needed - Tell core how much memory to reserve for us.
 */
static int userui_memory_needed(void)
{
	/* ball park figure of 128 pages */
	return (128 * PAGE_SIZE);
}

/**
 * userui_update_status - Update the progress bar and (if on) in-bar message.
 *
 * @value: Current progress percentage numerator.
 * @maximum: Current progress percentage denominator.
 * @fmt: Message to be displayed in the middle of the progress bar.
 *
 * Note that a NULL message does not mean that any previous message is erased!
 * For that, you need suspend_prepare_status with clearbar on.
 *
 * Returns an unsigned long, being the next numerator (as determined by the
 * maximum and progress granularity) where status needs to be updated.
 * This is to reduce unnecessary calls to update_status.
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

/**
 * userui_message - Display a message without necessarily logging it.
 *
 * @section: Type of message. Messages can be filtered by type.
 * @level: Degree of importance of the message. Lower values = higher priority.
 * @normally_logged: Whether logged even if log_everything is off.
 * @fmt: Message (and parameters).
 *
 * This function is intended to do the same job as printk, but without normally
 * logging what is printed. The point is to be able to get debugging info on
 * screen without filling the logs with "1/534. ^M 2/534^M. 3/534^M"
 *
 * It may be called from an interrupt context - can't sleep!
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

/**
 * wait_for_key_via_userui - Wait for userui to receive a keypress.
 */
static void wait_for_key_via_userui(void)
{
	DECLARE_WAITQUEUE(wait, current);

	add_wait_queue(&userui_wait_for_key, &wait);
	set_current_state(TASK_INTERRUPTIBLE);

	interruptible_sleep_on(&userui_wait_for_key);

	set_current_state(TASK_RUNNING);
	remove_wait_queue(&userui_wait_for_key, &wait);
}

/**
 * userui_prepare_status - Display high level messages.
 *
 * @clearbar: Whether to clear the progress bar.
 * @fmt...: New message for the title.
 *
 * Prepare the 'nice display', drawing the header and version, along with the
 * current action and perhaps also resetting the progress bar.
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

/**
 * suspend_wait_for_keypress - Wait for keypress via userui.
 *
 * @timeout: Maximum time to wait.
 *
 * Wait for a keypress from userui.
 *
 * FIXME: Implement timeout?
 */
static char userui_wait_for_keypress(int timeout)
{
	char key = '\0';

	if (ui_helper_data.pid != -1) {
		wait_for_key_via_userui();
		key = ' ';
	}
	
	return key;
}

/**
 * userui_abort_suspend - Abort a cycle & tell user if they didn't request it.
 *
 * @result_code: Reason why we're aborting (1 << bit).
 * @fmt: Message to display if telling the user what's going on.
 *
 * Abort a cycle. If this wasn't at the user's request (and we're displaying
 * output), tell the user why and wait for them to acknowledge the message.
 */
static void userui_abort_suspend(int result_code, const char *fmt, ...)
{
	va_list args;
	int printed_len = 0;

	set_result_state(result_code);

	if (test_result_state(SUSPEND_ABORTED))
		return;

	set_result_state(SUSPEND_ABORTED);

	if (test_result_state(SUSPEND_ABORT_REQUESTED))
		return;

	va_start(args, fmt);
	printed_len = vsnprintf(local_printf_buf,  sizeof(local_printf_buf),
			fmt, args);
	va_end(args);
	if (ui_helper_data.pid != -1)
		printed_len = sprintf(local_printf_buf + printed_len,
					" (Press SPACE to continue)");

	suspend_prepare_status(CLEAR_BAR, local_printf_buf);

	if (ui_helper_data.pid != -1)
		userui_wait_for_keypress(0);
}

/**
 * request_abort_suspend - Abort suspending or resuming at user request.
 *
 * Handle the user requesting the cancellation of a suspend or resume by
 * pressing escape.
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
	}

	suspend_prepare_status(CLEAR_BAR, "--- ESCAPE PRESSED :"
					" ABORTING SUSPEND ---");
	set_abort_result(SUSPEND_ABORT_REQUESTED);
	wake_up_interruptible(&userui_wait_for_key);
}

/**
 * userui_user_rcv_msg - Receive a netlink message from userui.
 *
 * @skb: skb received.
 * @nlh: Netlink header received.
 */
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
		case USERUI_MSG_PRINTK:
			printk((char *) data);
	}

	return 1;
}

/**
 * userui_cond_pause - Possibly pause at user request.
 *
 * @pause: Whether to pause or just display the message.
 * @message: Message to display at the start of pausing.
 * 
 * Potentially pause and wait for the user to tell us to continue. We normally
 * only pause when @pause is set. While paused, the user can do things like
 * changing the loglevel, toggling the display of debugging sections and such
 * like.
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
		last_key = userui_wait_for_keypress(0);
	}
	schedule();
}

/**
 * userui_prepare_console - Prepare the console for use.
 *
 * Prepare a console for use, saving current kmsg settings and attempting to
 * start userui. Console loglevel changes are handled by userui.
 */
static void userui_prepare_console(void)
{
	orig_kmsg = kmsg_redirect;
	kmsg_redirect = fg_console + 1;

	ui_helper_data.pid = -1;

	if (!userui_ops.enabled) {
		printk("Suspend2: Userui disabled.\n");
		return;
	}

	if (*ui_helper_data.program)
		suspend_netlink_setup(&ui_helper_data);
	else
		printk("Suspend2: Userui program not configured.\n");
}

/**
 * userui_cleanup_console - Cleanup after a cycle.
 *
 * Tell userui to cleanup, and restore kmsg_redirect to its original value.
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
	.name				= "userui",
	.shared_directory		= "user_interface",
	.module				= THIS_MODULE,
	.storage_needed			= userui_storage_needed,
	.save_config_info		= userui_save_config_info,
	.load_config_info		= userui_load_config_info,
	.memory_needed			= userui_memory_needed,
	.sysfs_data			= sysfs_params,
	.num_sysfs_entries		= sizeof(sysfs_params) / sizeof(struct suspend_sysfs_data),
};

static struct ui_ops my_ui_ops = {
	.post_atomic_restore		= userui_post_atomic_restore,
	.update_status			= userui_update_status,
	.message			= userui_message,
	.prepare_status			= userui_prepare_status,
	.abort				= userui_abort_suspend,
	.cond_pause			= userui_cond_pause,
	.prepare			= userui_prepare_console,
	.cleanup			= userui_cleanup_console,
	.wait_for_key			= userui_wait_for_keypress,
};

/**
 * s2_user_ui_init - Boot time initialisation for user interface.
 *
 * Invoked from the core init routine.
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
/**
 * s2_user_ui_ext - Cleanup code for if the core is unloaded.
 */
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
