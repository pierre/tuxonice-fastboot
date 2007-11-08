/*
 * kernel/power/tuxonice_cluster.c
 *
 * Copyright (C) 2006-2007 Nigel Cunningham (nigel at tuxonice net)
 *
 * This file is released under the GPLv2.
 *
 * This file contains routines for cluster hibernation support.
 *
 * Based on ip autoconfiguration code in net/ipv4/ipconfig.c.
 *
 * How does it work?
 *
 * There is no 'master' node that tells everyone else what to do. All nodes
 * send messages to the broadcast address/port, maintain a list of peers
 * and figure out when to progress to the next step in hibernating or resuming.
 * This makes us more fault tolerant when it comes to nodes coming and going
 * (which may be more of an issue if we're hibernating when power supplies
 * are being unreliable).
 *
 * At boot time, we start a ktuxonice thread that handles communication with
 * other nodes. This node maintains a state machine that controls our progress
 * through hibernating and resuming, keeping us in step with other nodes. Nodes
 * are identified by their hw address.
 *
 * On startup, the node sends CLUSTER_HELLO on the configured interface's
 * broadcast address, port $toi_cluster_port (see below) and begins to listen
 * for other broadcast messages. CLUSTER_HELLO messages are repeated at
 * intervals of 5 minutes, with a random offset to spread traffic out.
 *
 * A hibernation cycle is initiated from any node via
 *
 * echo > /sys/power/tuxonice/do_hibernate
 *
 * and (possibily) the hibernate script. At each step of the process, the node
 * completes its work, and waits for all other nodes to signal completion of
 * their work (or timeout) before progressing to the next step.
 *
 * Request/state  Action before reply	Possible reply	Next state
 * REQ_HIBERNATE  capable, pre-script	ACK_HIBERNATE	NODE_PREP
 * 					NACK_HIBERNATE	INIT_0
 *
 * NODE_PREP	  prepare_image		ACK_PREP	IMAGE_WRITE
 *		 			NACK_PREP	INIT_0
 * 					ABORT		RUNNING
 *
 * IMAGE_WRITE	  write image		ACK_IO		power off
 * 					ABORT		POST_RESUME
 *
 * (Boot time)	  check for image	ACK_IMAGE	RESUME_PREP
 * 					(Note 1)
 * 					NACK_IMAGE	(Note 2)
 *
 * RESUME_PREP	  prepare read image	ACK_PREP	IMAGE_READ
 * 					NACK_PREP	(As NACK_IMAGE)
 *
 * IMAGE_READ	  read image		ACK_READ	POST_RESUME
 *
 * POST_RESUME	  thaw, post-script			RUNNING
 *
 * INIT_0	  init 0
 *
 * Other messages:
 *
 * - PING: Request for all other live nodes to send a PONG. Used at startup to
 *   announce presence, when a node is suspected dead and periodically, in case
 *   segments of the network are [un]plugged.
 *
 * - PONG: Response to a PING.
 *
 * - ABORT: Request to cancel writing an image.
 *
 * - BYE: Notification that this node is shutting down.
 *
 * Note 1: Repeated at 3s intervals until we continue to boot/resume, so that
 * nodes which are slower to start up can get state synchronised. If a node
 * starting up sees other nodes sending RESUME_PREP or IMAGE_READ, it may send
 * ACK_IMAGE and they will wait for it to catch up. If it sees ACK_READ, it
 * must invalidate its image (if any) and boot normally.
 *
 * Note 2: May occur when one node lost power or powered off while others
 * hibernated. This node waits for others to complete resuming (ACK_READ)
 * before completing its boot, so that it appears as a fail node restarting.
 *
 * If any node has an image, then it also has a list of nodes that hibernated
 * in synchronisation with it. The node will wait for other nodes to appear
 * or timeout before beginning its restoration.
 *
 * If a node has no image, it needs to wait, in case other nodes which do have
 * an image are going to resume, but are taking longer to announce their presence.
 * For this reason, the user can specify a timeout value and a number of nodes
 * detected before we just continue. (We might want to assume in a cluster of,
 * say, 15 nodes, if 8 others have booted without finding an image, the
 * remaining nodes will too. This might help in situations where some nodes are
 * much slower to boot, or more subject to hardware failures or such like).
 */

#include <linux/suspend.h>
#include <linux/module.h>
#include <linux/if.h>
#include <linux/rtnetlink.h>
#include <linux/netdevice.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <linux/in.h>
#include <linux/if_arp.h>
#include <net/ip.h>

#include "tuxonice.h"
#include "tuxonice_modules.h"
#include "tuxonice_sysfs.h"
#include "tuxonice_io.h"

#define MYNAME "TuxOnIce Clustering"

enum {
	OFFLINE,
	RUNNING,
	PREPARATION,
	DOING_IO
};

enum {
	MSG_PING,
	MSG_PONG,
	MSG_ABORT,
	MSG_BYE,
	MSG_REQ_HIBERNATE,
	MSG_HIBERNATE_ACK,
	MSG_HIBERNATE_NACK,
	MSG_PREP_ACK,
	MSG_PREP_NACK,
	MSG_IO_ACK,
	MSG_IO_NACK
};

struct cluster_member {
	char *ip;
	int state;
	unsigned long *last_message;
};

static struct cluster_member *member_list;

/* Key used to allow multiple clusters on the same lan */
static char toi_cluster_key[32] = CONFIG_TOI_DEFAULT_CLUSTER_KEY;
static char pre_hibernate_script[255] = CONFIG_TOI_DEFAULT_CLUSTER_PRE_HIBERNATE;
static char post_hibernate_script[255] = CONFIG_TOI_DEFAULT_CLUSTER_POST_HIBERNATE;

static int toi_cluster_state = RUNNING;

static char toi_cluster_iface[IFNAMSIZ] = CONFIG_TOI_DEFAULT_CLUSTER_INTERFACE;
#define toi_cluster_port_send 3501
#define toi_cluster_port_recv 3502
static DEFINE_SPINLOCK(toi_recv_lock);

static struct net_device *net_dev;
static struct toi_module_ops toi_cluster_ops;

static int toi_recv(struct sk_buff *skb, struct net_device *dev,
		struct packet_type *pt, struct net_device *orig_dev);

static struct packet_type toi_cluster_packet_type __initdata = {
	.type =	__constant_htons(ETH_P_IP),
	.func =	toi_recv,
};

struct toi_pkt {		/* BOOTP packet format */
	struct iphdr iph;	/* IP header */
	struct udphdr udph;	/* UDP header */
	u8 htype;		/* HW address type */
	u8 hlen;		/* HW address length */
	__be32 xid;		/* Transaction ID */
	__be16 secs;		/* Seconds since we started */
	__be16 flags;		/* Just what it says */
	u8 hw_addr[16];		/* Sender's HW address */
	u8 message;		/* Message */
};

/*
 *  Process received TOI packet.
 */
static int toi_recv(struct sk_buff *skb, struct net_device *dev,
		struct packet_type *pt, struct net_device *orig_dev)
{
	struct toi_pkt *b;
	struct iphdr *h;
	int len;

	/* Perform verifications before taking the lock.  */
	if (skb->pkt_type == PACKET_OTHERHOST)
		goto drop;

	if (dev != net_dev)
		goto drop;

	if ((skb = skb_share_check(skb, GFP_ATOMIC)) == NULL)
		return NET_RX_DROP;

	if (!pskb_may_pull(skb,
			   sizeof(struct iphdr) +
			   sizeof(struct udphdr)))
		goto drop;

	b = (struct toi_pkt *)skb_network_header(skb);
	h = &b->iph;

	if (h->ihl != 5 || h->version != 4 || h->protocol != IPPROTO_UDP)
		goto drop;

	/* Fragments are not supported */
	if (h->frag_off & htons(IP_OFFSET | IP_MF)) {
		if (net_ratelimit())
			printk(KERN_ERR "TuxOnIce: Ignoring fragmented "
			       "cluster message.\n");
		goto drop;
	}

	if (skb->len < ntohs(h->tot_len))
		goto drop;

	if (ip_fast_csum((char *) h, h->ihl))
		goto drop;

	if (b->udph.source != htons(toi_cluster_port_send) ||
	    b->udph.dest != htons(toi_cluster_port_recv))
		goto drop;

	if (ntohs(h->tot_len) < ntohs(b->udph.len) + sizeof(struct iphdr))
		goto drop;

	len = ntohs(b->udph.len) - sizeof(struct udphdr);

	/* Ok the front looks good, make sure we can get at the rest.  */
	if (!pskb_may_pull(skb, skb->len))
		goto drop;

	b = (struct toi_pkt *)skb_network_header(skb);
	h = &b->iph;

	/* One message at a time, please. */
	spin_lock(&toi_recv_lock);

	switch (b->message) {
		case MSG_PING:
			break;
		case MSG_PONG:
			break;
		case MSG_ABORT:
			break;
		case MSG_BYE:
			break;
		case MSG_REQ_HIBERNATE:
			break;
		case MSG_HIBERNATE_ACK:
			break;
		case MSG_HIBERNATE_NACK:
			break;
		case MSG_PREP_ACK:
			break;
		case MSG_PREP_NACK:
			break;
		case MSG_IO_ACK:
			break;
		case MSG_IO_NACK:
			break;
		default:
			if (net_ratelimit())
				printk(KERN_ERR "Unrecognised TuxOnIce cluster"
					" message %d from %s.\n",
					b->message, b->hw_addr);
	};

drop_unlock:
	spin_unlock(&toi_recv_lock);

drop:
	/* Throw the packet out. */
	kfree_skb(skb);

	return 0;
}

/*
 *  Send cluster message to single interface.
 */
static void toi_send_if(int message)
{
	struct sk_buff *skb;
	struct toi_pkt *b;
	int hh_len = LL_RESERVED_SPACE(net_dev);
	struct iphdr *h;

	/* Allocate packet */
	skb = alloc_skb(sizeof(struct toi_pkt) + hh_len + 15, GFP_KERNEL);
	if (!skb)
		return;
	skb_reserve(skb, hh_len);
	b = (struct toi_pkt *) skb_put(skb, sizeof(struct toi_pkt));
	memset(b, 0, sizeof(struct toi_pkt));

	/* Construct IP header */
	skb_reset_network_header(skb);
	h = ip_hdr(skb);
	h->version = 4;
	h->ihl = 5;
	h->tot_len = htons(sizeof(struct toi_pkt));
	h->frag_off = htons(IP_DF);
	h->ttl = 64;
	h->protocol = IPPROTO_UDP;
	h->daddr = htonl(INADDR_BROADCAST);
	h->check = ip_fast_csum((unsigned char *) h, h->ihl);

	/* Construct UDP header */
	b->udph.source = htons(toi_cluster_port_send);
	b->udph.dest = htons(toi_cluster_port_recv);
	b->udph.len = htons(sizeof(struct toi_pkt) - sizeof(struct iphdr));
	/* UDP checksum not calculated -- explicitly allowed in BOOTP RFC */

	/* Construct DHCP/BOOTP header */
	b->message = message;
	if (net_dev->type < 256) /* check for false types */
		b->htype = net_dev->type;
	else if (net_dev->type == ARPHRD_IEEE802_TR) /* fix for token ring */
		b->htype = ARPHRD_IEEE802;
	else if (net_dev->type == ARPHRD_FDDI)
		b->htype = ARPHRD_ETHER;
	else {
		printk("Unknown ARP type 0x%04x for device %s\n",
				net_dev->type, net_dev->name);
		b->htype = net_dev->type; /* can cause undefined behavior */
	}
	b->hlen = net_dev->addr_len;
	memcpy(b->hw_addr, net_dev->dev_addr, net_dev->addr_len);
	b->secs = htons(3); /* 3 seconds */

	/* Chain packet down the line... */
	skb->dev = net_dev;
	skb->protocol = htons(ETH_P_IP);
	if ((net_dev->hard_header &&
	     net_dev->hard_header(skb, net_dev, ntohs(skb->protocol),
		     net_dev->broadcast, net_dev->dev_addr, skb->len) < 0) ||
			dev_queue_xmit(skb) < 0)
		printk("E");
}

/* toi_cluster_print_debug_stats
 *
 * Description:	Print information to be recorded for debugging purposes into a
 * 		buffer.
 * Arguments:	buffer: Pointer to a buffer into which the debug info will be
 * 			printed.
 * 		size:	Size of the buffer.
 * Returns:	Number of characters written to the buffer.
 */
static int toi_cluster_print_debug_stats(char *buffer, int size)
{
	int len;
	
	if (strlen(toi_cluster_iface))
		len = snprintf_used(buffer, size, "- Cluster interface is '%s'.\n",
				toi_cluster_iface);
	else
		len = snprintf_used(buffer, size, "- Cluster support is disabled.\n");
	return len;
}

/* cluster_memory_needed
 *
 * Description:	Tell the caller how much memory we need to operate during
 * 		hibernate/resume.
 * Returns:	Unsigned long. Maximum number of bytes of memory required for
 * 		operation.
 */
static int toi_cluster_memory_needed(void)
{
	return 0;
}

static int toi_cluster_storage_needed(void)
{
	return 1 + strlen(toi_cluster_iface);
}
	
/* toi_cluster_save_config_info
 *
 * Description:	Save informaton needed when reloading the image at resume time.
 * Arguments:	Buffer:		Pointer to a buffer of size PAGE_SIZE.
 * Returns:	Number of bytes used for saving our data.
 */
static int toi_cluster_save_config_info(char *buffer)
{
	strcpy(buffer, toi_cluster_iface);
	return strlen(toi_cluster_iface + 1);
}

/* toi_cluster_load_config_info
 *
 * Description:	Reload information needed for declustering the image at 
 * 		resume time.
 * Arguments:	Buffer:		Pointer to the start of the data.
 *		Size:		Number of bytes that were saved.
 */
static void toi_cluster_load_config_info(char *buffer, int size)
{
	strncpy(toi_cluster_iface, buffer, size);
	return;
}

static void cluster_startup(void)
{
	int num_machines_with_image = 0;

	/* Find out whether an image exists here. Send ACK_IMAGE or NACK_IMAGE
	 * as appropriate.
	 *
	 * If we don't have an image:
	 * - Wait until someone else says they have one, or conditions are met
	 *   for continuing to boot (n machines or t seconds).
	 * - If anyone has an image, wait for them to resume before continuing
	 *   to boot.
	 *
	 * If we have an image:
	 * - Wait until conditions are met before continuing to resume (n
	 *   machines or t seconds). Send RESUME_PREP and freeze processes.
	 *   NACK_PREP if freezing fails (shouldn't) and follow logic for
	 *   us having no image above. On success, wait for [N]ACK_PREP from
	 *   other machines. Read image (including atomic restore) until done.
	 *   Wait for ACK_READ from others (should never fail). Thaw processes
	 *   and do post-resume. (The section after the atomic restore is done
	 *   via the code for hibernating).
	 */
}

/* toi_cluster_open_iface
 *
 * Description:	Prepare to use an interface.
 */

static int toi_cluster_open_iface(void)
{
	struct net_device *dev;

	rtnl_lock();

	for_each_netdev(dev) {
		if (dev == &loopback_dev ||
		    strcmp(dev->name, toi_cluster_iface))
			continue;

		net_dev = dev;
		break;
	}

	if (!net_dev) {
		printk(KERN_ERR MYNAME ": Device %s not found.\n",
				toi_cluster_iface);
		return -ENODEV;
	}

	rtnl_unlock();

	dev_add_pack(&toi_cluster_packet_type);
	toi_cluster_state = RUNNING;

	/* Send our Hello message */
	cluster_startup();
	return 0;
}

/* toi_cluster_close_iface
 *
 * Description: Stop using an interface.
 */

static int toi_cluster_close_iface(void)
{
	dev_remove_pack(&toi_cluster_packet_type);
	toi_cluster_state = OFFLINE;
	return 0;
}

/*
 * data for our sysfs entries.
 */
static struct toi_sysfs_data sysfs_params[] = {
	{
		TOI_ATTR("master", SYSFS_RW),
		SYSFS_STRING(toi_cluster_iface, IFNAMSIZ, 0)
	},

	{
		TOI_ATTR("enabled", SYSFS_RW),
		SYSFS_INT(&toi_cluster_ops.enabled, 0, 1, 0)
	},

	{
		TOI_ATTR("cluster_name", SYSFS_RW),
		SYSFS_STRING(toi_cluster_key, 32, 0)
	},

	{
		TOI_ATTR("pre-hibernate-script", SYSFS_RW),
		SYSFS_STRING(pre_hibernate_script, 256, 0)
	},

	{
		TOI_ATTR("post-hibernate-script", SYSFS_RW),
		SYSFS_STRING(post_hibernate_script, 256, 0)
	}
};

/*
 * Ops structure.
 */

static struct toi_module_ops toi_cluster_ops = {
	.type			= FILTER_MODULE,
	.name			= "Cluster",
	.directory		= "cluster",
	.module			= THIS_MODULE,
	.memory_needed 		= toi_cluster_memory_needed,
	.print_debug_info	= toi_cluster_print_debug_stats,
	.save_config_info	= toi_cluster_save_config_info,
	.load_config_info	= toi_cluster_load_config_info,
	.storage_needed		= toi_cluster_storage_needed,
	
	.sysfs_data		= sysfs_params,
	.num_sysfs_entries	= sizeof(sysfs_params) / sizeof(struct toi_sysfs_data),
};

/* ---- Registration ---- */

#ifdef MODULE
#define INIT static __init
#define EXIT static __exit
#else
#define INIT
#define EXIT
#endif

INIT int toi_cluster_init(void)
{
	int temp = toi_register_module(&toi_cluster_ops);

	toi_cluster_ops.enabled = (strlen(toi_cluster_iface) > 0);

	if (toi_cluster_ops.enabled)
		toi_cluster_open_iface();

	return temp;	
}

EXIT void toi_cluster_exit(void)
{
	toi_unregister_module(&toi_cluster_ops);
}

static int __init toi_cluster_iface_setup(char *iface)
{
	toi_cluster_ops.enabled = (*iface &&
			strcmp(iface, "off"));

	if (toi_cluster_ops.enabled)
		strncpy(toi_cluster_iface, iface, strlen(iface));
}

__setup("toi_cluster=", toi_cluster_iface_setup);

#ifdef MODULE
MODULE_LICENSE("GPL");
module_init(toi_cluster_init);
module_exit(toi_cluster_exit);
MODULE_AUTHOR("Nigel Cunningham");
MODULE_DESCRIPTION("Cluster Support for TuxOnIce");
#endif
