/*
 * kernel/power/tuxonice_netlink.c
 *
 * Copyright (C) 2004-2007 Nigel Cunningham (nigel at suspend2 net)
 *
 * This file is released under the GPLv2.
 *
 * Functions for communicating with a userspace helper via netlink.
 */


#include <linux/suspend.h>
#include "tuxonice_netlink.h"
#include "tuxonice.h"
#include "tuxonice_modules.h"

struct user_helper_data *uhd_list = NULL;

/* 
 * Refill our pool of SKBs for use in emergencies (eg, when eating memory and none
 * can be allocated).
 */
static void toi_fill_skb_pool(struct user_helper_data *uhd)
{
	while (uhd->pool_level < uhd->pool_limit) {
		struct sk_buff *new_skb =
			alloc_skb(NLMSG_SPACE(uhd->skb_size), TOI_ATOMIC_GFP);

		if (!new_skb)
			break;

		new_skb->next = uhd->emerg_skbs;
		uhd->emerg_skbs = new_skb;
		uhd->pool_level++;
	}
}

/* 
 * Try to allocate a single skb. If we can't get one, try to use one from
 * our pool.
 */
static struct sk_buff *toi_get_skb(struct user_helper_data *uhd)
{
	struct sk_buff *skb =
		alloc_skb(NLMSG_SPACE(uhd->skb_size), TOI_ATOMIC_GFP);

	if (skb)
		return skb;

	skb = uhd->emerg_skbs;
	if (skb) {
		uhd->pool_level--;
		uhd->emerg_skbs = skb->next;
		skb->next = NULL;
	}

	return skb;
}

static void put_skb(struct user_helper_data *uhd, struct sk_buff *skb)
{
	if (uhd->pool_level < uhd->pool_limit) {
		skb->next = uhd->emerg_skbs;
		uhd->emerg_skbs = skb;
	} else
		kfree_skb(skb);
}

void toi_send_netlink_message(struct user_helper_data *uhd,
		int type, void* params, size_t len)
{
	struct sk_buff *skb;
	struct nlmsghdr *nlh;
	void *dest;
	struct task_struct *t;

	if (uhd->pid == -1)
		return;

	skb = toi_get_skb(uhd);
	if (!skb) {
		printk("toi_netlink: Can't allocate skb!\n");
		return;
	}

	/* NLMSG_PUT contains a hidden goto nlmsg_failure */
	nlh = NLMSG_PUT(skb, 0, uhd->sock_seq, type, len);
	uhd->sock_seq++;

	dest = NLMSG_DATA(nlh);
	if (params && len > 0)
		memcpy(dest, params, len);

	netlink_unicast(uhd->nl, skb, uhd->pid, 0);

	read_lock(&tasklist_lock);
	if ((t = find_task_by_pid(uhd->pid)) == NULL) {
		read_unlock(&tasklist_lock);
		if (uhd->pid > -1)
			printk("Hmm. Can't find the userspace task %d.\n", uhd->pid);
		return;
	}
	wake_up_process(t);
	read_unlock(&tasklist_lock);

	yield();

	return;

nlmsg_failure:
	if (skb)
		put_skb(uhd, skb);
}

static void send_whether_debugging(struct user_helper_data *uhd)
{
	static int is_debugging = 1;

	toi_send_netlink_message(uhd, NETLINK_MSG_IS_DEBUGGING,
			&is_debugging, sizeof(int));
}

/*
 * Set the PF_NOFREEZE flag on the given process to ensure it can run whilst we
 * are hibernating.
 */
static int nl_set_nofreeze(struct user_helper_data *uhd, int pid)
{
	struct task_struct *t;

	read_lock(&tasklist_lock);
	if ((t = find_task_by_pid(pid)) == NULL) {
		read_unlock(&tasklist_lock);
		printk("Strange. Can't find the userspace task %d.\n", pid);
		return -EINVAL;
	}

	t->flags |= PF_NOFREEZE;

	read_unlock(&tasklist_lock);
	uhd->pid = pid;

	toi_send_netlink_message(uhd, NETLINK_MSG_NOFREEZE_ACK, NULL, 0);

	return 0;
}

/*
 * Called when the userspace process has informed us that it's ready to roll.
 */
static int nl_ready(struct user_helper_data *uhd, int version)
{
	if (version != uhd->interface_version) {
		printk("%s userspace process using invalid interface version."
				" Trying to continue without it.\n",
				uhd->name);
		if (uhd->not_ready)
			uhd->not_ready();
		return 1;
	}

	complete(&uhd->wait_for_process);

	return 0;
}

void toi_netlink_close_complete(struct user_helper_data *uhd)
{
	if (uhd->nl) {
		sock_release(uhd->nl->sk_socket);
		uhd->nl = NULL;
	}

	while (uhd->emerg_skbs) {
		struct sk_buff *next = uhd->emerg_skbs->next;
		kfree_skb(uhd->emerg_skbs);
		uhd->emerg_skbs = next;
	}

	uhd->pid = -1;

	toi_put_modules();
}

static int toi_nl_gen_rcv_msg(struct user_helper_data *uhd,
		struct sk_buff *skb, struct nlmsghdr *nlh)
{
	int type;
	int *data;
	int err;

	/* Let the more specific handler go first. It returns
	 * 1 for valid messages that it doesn't know. */
	if ((err = uhd->rcv_msg(skb, nlh)) != 1)
		return err;
	
	type = nlh->nlmsg_type;

	/* Only allow one task to receive NOFREEZE privileges */
	if (type == NETLINK_MSG_NOFREEZE_ME && uhd->pid != -1) {
		printk("Received extra nofreeze me requests.\n");
		return -EBUSY;
	}

	data = (int*)NLMSG_DATA(nlh);

	switch (type) {
		case NETLINK_MSG_NOFREEZE_ME:
			if ((err = nl_set_nofreeze(uhd, nlh->nlmsg_pid)) != 0)
				return err;
			break;
		case NETLINK_MSG_GET_DEBUGGING:
			send_whether_debugging(uhd);
			break;
		case NETLINK_MSG_READY:
			if (nlh->nlmsg_len < NLMSG_LENGTH(sizeof(int))) {
				printk("Invalid ready mesage.\n");
				return -EINVAL;
			}
			if ((err = nl_ready(uhd, *data)) != 0)
				return err;
			break;
		case NETLINK_MSG_CLEANUP:
			toi_netlink_close_complete(uhd);
			break;
	}

	return 0;
}

static void toi_user_rcv_skb(struct user_helper_data *uhd,
				  struct sk_buff *skb)
{
	int err;
	struct nlmsghdr *nlh;

	while (skb->len >= NLMSG_SPACE(0)) {
		u32 rlen;

		nlh = (struct nlmsghdr *) skb->data;
		if (nlh->nlmsg_len < sizeof(*nlh) || skb->len < nlh->nlmsg_len)
			return;

		rlen = NLMSG_ALIGN(nlh->nlmsg_len);
		if (rlen > skb->len)
			rlen = skb->len;

		if ((err = toi_nl_gen_rcv_msg(uhd, skb, nlh)) != 0)
			netlink_ack(skb, nlh, err);
		else if (nlh->nlmsg_flags & NLM_F_ACK)
			netlink_ack(skb, nlh, 0);
		skb_pull(skb, rlen);
	}
}

static void toi_netlink_input(struct sock *sk, int len)
{
	struct user_helper_data *uhd = uhd_list;

	while (uhd && uhd->netlink_id != sk->sk_protocol)
		uhd= uhd->next;

	do {
		struct sk_buff *skb;
		while ((skb = skb_dequeue(&sk->sk_receive_queue)) != NULL) {
			toi_user_rcv_skb(uhd, skb);
			put_skb(uhd, skb);
		}
	} while (uhd->nl && uhd->nl->sk_receive_queue.qlen);
}

static int netlink_prepare(struct user_helper_data *uhd)
{
	toi_get_modules();

	uhd->next = uhd_list;
	uhd_list = uhd;

	uhd->sock_seq = 0x42c0ffee;
	uhd->nl = netlink_kernel_create(uhd->netlink_id, 0,
			toi_netlink_input, NULL, THIS_MODULE);
	if (!uhd->nl) {
		printk("Failed to allocate netlink socket for %s.\n",
				uhd->name);
		return -ENOMEM;
	}

	toi_fill_skb_pool(uhd);

	return 0;
}

void toi_netlink_close(struct user_helper_data *uhd)
{
	struct task_struct *t;

	read_lock(&tasklist_lock);
	if ((t = find_task_by_pid(uhd->pid)))
		t->flags &= ~PF_NOFREEZE;
	read_unlock(&tasklist_lock);

	toi_send_netlink_message(uhd, NETLINK_MSG_CLEANUP, NULL, 0);
}

static int toi_launch_userspace_program(char *command, int channel_no)
{
	int retval;
	static char *envp[] = {
			"HOME=/",
			"TERM=linux",
			"PATH=/sbin:/usr/sbin:/bin:/usr/bin",
			NULL };
	static char *argv[] = { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };
	char *channel = kmalloc(6, GFP_KERNEL);
	int arg = 0, size;
	char test_read[255];
	char *orig_posn = command;

	if (!strlen(orig_posn))
		return 1;

	/* Up to 7 args supported */
	while (arg < 7) {
		sscanf(orig_posn, "%s", test_read);
		size = strlen(test_read);
		if (!(size))
			break;
		argv[arg] = kmalloc(size + 1, TOI_ATOMIC_GFP);
		strcpy(argv[arg], test_read);
		orig_posn += size + 1;
		*test_read = 0;
		arg++;
	}
	
	if (channel_no) {
		sprintf(channel, "-c%d", channel_no);
		argv[arg] = channel;
	} else
		arg--;

	retval = call_usermodehelper(argv[0], argv, envp, 0);

	if (retval)
		printk("Failed to launch userspace program '%s': Error %d\n",
				command, retval);

	{
		int i;
		for (i = 0; i < arg; i++)
			if (argv[i] && argv[i] != channel)
				kfree(argv[i]);
	}

	kfree(channel);

	return retval;
}

int toi_netlink_setup(struct user_helper_data *uhd)
{
	if (netlink_prepare(uhd) < 0) {
		printk("Netlink prepare failed.\n");
		return 1;
	}

	if (toi_launch_userspace_program(uhd->program, uhd->netlink_id) < 0) {
		printk("Launch userspace program failed.\n");
		toi_netlink_close_complete(uhd);
		return 1;
	}

	/* Wait 2 seconds for the userspace process to make contact */
	wait_for_completion_timeout(&uhd->wait_for_process, 2*HZ);

	if (uhd->pid == -1) {
		printk("%s: Failed to contact userspace process.\n",
				uhd->name);
		toi_netlink_close_complete(uhd);
		return 1;
	}

	return 0;
}

EXPORT_SYMBOL_GPL(toi_netlink_setup);
EXPORT_SYMBOL_GPL(toi_netlink_close);
EXPORT_SYMBOL_GPL(toi_send_netlink_message);
