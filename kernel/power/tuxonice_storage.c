/*
 * kernel/power/tuxonice_storage.c
 *
 * Copyright (C) 2005-2007 Nigel Cunningham (nigel at suspend2 net)
 *
 * This file is released under the GPLv2.
 *
 * Routines for talking to a userspace program that manages storage.
 *
 * The kernel side:
 * - starts the userspace program;
 * - sends messages telling it when to open and close the connection;
 * - tells it when to quit;
 *
 * The user space side:
 * - passes messages regarding status;
 *
 */

#include <linux/suspend.h>
#include <linux/freezer.h>
 
#include "tuxonice_sysfs.h"
#include "tuxonice_modules.h"
#include "tuxonice_netlink.h"
#include "tuxonice_storage.h"
#include "tuxonice_ui.h"

static struct user_helper_data usm_helper_data;
static struct suspend_module_ops usm_ops;
static int message_received = 0;
static int usm_prepare_count = 0;
static int storage_manager_last_action = 0;
static int storage_manager_action = 0;
       
static int usm_user_rcv_msg(struct sk_buff *skb, struct nlmsghdr *nlh)
{
	int type;
	int *data;

	type = nlh->nlmsg_type;

	/* A control message: ignore them */
	if (type < NETLINK_MSG_BASE)
		return 0;

	/* Unknown message: reply with EINVAL */
	if (type >= USM_MSG_MAX)
		return -EINVAL;

	/* All operations require privileges, even GET */
	if (security_netlink_recv(skb, CAP_NET_ADMIN))
		return -EPERM;

	/* Only allow one task to receive NOFREEZE privileges */
	if (type == NETLINK_MSG_NOFREEZE_ME && usm_helper_data.pid != -1)
		return -EBUSY;

	data = (int*)NLMSG_DATA(nlh);

	switch (type) {
		case USM_MSG_SUCCESS:
		case USM_MSG_FAILED:
			message_received = type;
			complete(&usm_helper_data.wait_for_process);
			break;
		default:
			printk("Storage manager doesn't recognise message %d.\n", type);
	}

	return 1;
}

#ifdef CONFIG_NET
static int activations = 0;

int suspend_activate_storage(int force)
{
	int tries = 1;

	if (usm_helper_data.pid == -1 || !usm_ops.enabled)
		return 0;

	message_received = 0;
	activations++;

	if (activations > 1 && !force)
		return 0;

	while ((!message_received || message_received == USM_MSG_FAILED) && tries < 2) {
		suspend_prepare_status(DONT_CLEAR_BAR, "Activate storage attempt %d.\n", tries);

		init_completion(&usm_helper_data.wait_for_process);

		suspend_send_netlink_message(&usm_helper_data,
			USM_MSG_CONNECT,
			NULL, 0);

		/* Wait 2 seconds for the userspace process to make contact */
		wait_for_completion_timeout(&usm_helper_data.wait_for_process, 2*HZ);

		tries++;
	}

	return 0;
}

int suspend_deactivate_storage(int force)
{
	if (usm_helper_data.pid == -1 || !usm_ops.enabled)
		return 0;
	
	message_received = 0;
	activations--;

	if (activations && !force)
		return 0;

	init_completion(&usm_helper_data.wait_for_process);

	suspend_send_netlink_message(&usm_helper_data,
			USM_MSG_DISCONNECT,
			NULL, 0);

	wait_for_completion_timeout(&usm_helper_data.wait_for_process, 2*HZ);

	if (!message_received || message_received == USM_MSG_FAILED) {
		printk("Returning failure disconnecting storage.\n");
		return 1;
	}

	return 0;
}
#endif

static void storage_manager_simulate(void)
{
	printk("--- Storage manager simulate ---\n");
	suspend_prepare_usm();
	schedule();
	printk("--- Activate storage 1 ---\n");
	suspend_activate_storage(1);
	schedule();
	printk("--- Deactivate storage 1 ---\n");
	suspend_deactivate_storage(1);
	schedule();
	printk("--- Cleanup usm ---\n");
	suspend_cleanup_usm();
	schedule();
	printk("--- Storage manager simulate ends ---\n");
}

static int usm_storage_needed(void)
{
	return strlen(usm_helper_data.program);
}

static int usm_save_config_info(char *buf)
{
	int len = strlen(usm_helper_data.program);
	memcpy(buf, usm_helper_data.program, len);
	return len;
}

static void usm_load_config_info(char *buf, int size)
{
	/* Don't load the saved path if one has already been set */
	if (usm_helper_data.program[0])
		return;

	memcpy(usm_helper_data.program, buf, size);
}

static int usm_memory_needed(void)
{
	/* ball park figure of 32 pages */
	return (32 * PAGE_SIZE);
}

/* suspend_prepare_usm
 */
int suspend_prepare_usm(void)
{
	usm_prepare_count++;

	if (usm_prepare_count > 1 || !usm_ops.enabled)
		return 0;
	
	usm_helper_data.pid = -1;

	if (!*usm_helper_data.program)
		return 0;

	suspend_netlink_setup(&usm_helper_data);

	if (usm_helper_data.pid == -1)
		printk("Suspend2 Storage Manager wanted, but couldn't start it.\n");

	suspend_activate_storage(0);

	return (usm_helper_data.pid != -1);
}

void suspend_cleanup_usm(void)
{
	usm_prepare_count--;

	if (usm_helper_data.pid > -1 && !usm_prepare_count) {
		suspend_deactivate_storage(0);
		suspend_netlink_close(&usm_helper_data);
	}
}

static void storage_manager_activate(void)
{
	if (storage_manager_action == storage_manager_last_action)
		return;

	if (storage_manager_action)
		suspend_prepare_usm();
	else
		suspend_cleanup_usm();

	storage_manager_last_action = storage_manager_action;
}

/*
 * User interface specific /sys/power/suspend2 entries.
 */

static struct suspend_sysfs_data sysfs_params[] = {
	{ SUSPEND2_ATTR("simulate_atomic_copy", SYSFS_RW),
	  .type				= SUSPEND_SYSFS_DATA_NONE,
	  .write_side_effect		= storage_manager_simulate,
	},

	{ SUSPEND2_ATTR("enabled", SYSFS_RW),
	  SYSFS_INT(&usm_ops.enabled, 0, 1, 0)
	},

	{ SUSPEND2_ATTR("program", SYSFS_RW),
	  SYSFS_STRING(usm_helper_data.program, 254, 0)
	},

	{ SUSPEND2_ATTR("activate_storage", SYSFS_RW),
	  SYSFS_INT(&storage_manager_action, 0, 1, 0),
	  .write_side_effect		= storage_manager_activate,
	}
};

static struct suspend_module_ops usm_ops = {
	.type				= MISC_MODULE,
	.name				= "usm",
	.directory			= "storage_manager",
	.module				= THIS_MODULE,
	.storage_needed			= usm_storage_needed,
	.save_config_info		= usm_save_config_info,
	.load_config_info		= usm_load_config_info,
	.memory_needed			= usm_memory_needed,

	.sysfs_data			= sysfs_params,
	.num_sysfs_entries		= sizeof(sysfs_params) / sizeof(struct suspend_sysfs_data),
};

/* suspend_usm_sysfs_init
 * Description: Boot time initialisation for user interface.
 */
int s2_usm_init(void)
{
	usm_helper_data.nl = NULL;
	usm_helper_data.program[0] = '\0';
	usm_helper_data.pid = -1;
	usm_helper_data.skb_size = 0;
	usm_helper_data.pool_limit = 6;
	usm_helper_data.netlink_id = NETLINK_TOI_USM;
	usm_helper_data.name = "userspace storage manager";
	usm_helper_data.rcv_msg = usm_user_rcv_msg;
	usm_helper_data.interface_version = 1;
	usm_helper_data.must_init = 0;
	init_completion(&usm_helper_data.wait_for_process);

	return suspend_register_module(&usm_ops);
}

void s2_usm_exit(void)
{
	suspend_unregister_module(&usm_ops);
}
