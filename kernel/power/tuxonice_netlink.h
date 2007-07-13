/*
 * kernel/power/tuxonice_netlink.h
 *
 * Copyright (C) 2004-2007 Nigel Cunningham (nigel at suspend2 net)
 *
 * This file is released under the GPLv2.
 *
 * Declarations for functions for communicating with a userspace helper
 * via netlink.
 */

#include <linux/netlink.h>
#include <net/sock.h>

#define NETLINK_MSG_BASE 0x10

#define NETLINK_MSG_READY 0x10
#define	NETLINK_MSG_NOFREEZE_ME 0x16
#define NETLINK_MSG_GET_DEBUGGING 0x19
#define NETLINK_MSG_CLEANUP 0x24
#define NETLINK_MSG_NOFREEZE_ACK 0x27
#define NETLINK_MSG_IS_DEBUGGING 0x28

struct user_helper_data {
	int (*rcv_msg) (struct sk_buff *skb, struct nlmsghdr *nlh);
	void (* not_ready) (void);
	struct sock *nl;
	u32 sock_seq;
	pid_t pid;
	char *comm;
	char program[256];
	int pool_level;
	int pool_limit;
	struct sk_buff *emerg_skbs;
	int skb_size;
	int netlink_id;	
	char *name;
	struct user_helper_data *next;
	struct completion wait_for_process;
	int interface_version;
	int must_init;
};

#ifdef CONFIG_NET
int suspend_netlink_setup(struct user_helper_data *uhd);
void suspend_netlink_close(struct user_helper_data *uhd);
void suspend_send_netlink_message(struct user_helper_data *uhd,
		int type, void* params, size_t len);
#else
static inline int suspend_netlink_setup(struct user_helper_data *uhd)
{
	return 0;
}

static inline void suspend_netlink_close(struct user_helper_data *uhd) { };
static inline void suspend_send_netlink_message(struct user_helper_data *uhd,
		int type, void* params, size_t len) { };
#endif
