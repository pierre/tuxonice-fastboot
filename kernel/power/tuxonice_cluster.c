/*
 * kernel/power/tuxonice_cluster.c
 *
 * Copyright (C) 2006-2007 Nigel Cunningham (nigel at suspend2 net)
 *
 * This file is released under the GPLv2.
 *
 * This file contains routines for cluster hibernation support.
 *
 */

#include <linux/suspend.h>
#include <linux/module.h>

#include "tuxonice.h"
#include "tuxonice_modules.h"
#include "tuxonice_sysfs.h"
#include "tuxonice_io.h"

static char suspend_cluster_master[63] = CONFIG_TOI_DEFAULT_CLUSTER_MASTER;

static struct suspend_module_ops suspend_cluster_ops;

/* suspend_cluster_print_debug_stats
 *
 * Description:	Print information to be recorded for debugging purposes into a
 * 		buffer.
 * Arguments:	buffer: Pointer to a buffer into which the debug info will be
 * 			printed.
 * 		size:	Size of the buffer.
 * Returns:	Number of characters written to the buffer.
 */
static int suspend_cluster_print_debug_stats(char *buffer, int size)
{
	int len;
	
	if (strlen(suspend_cluster_master))
		len = snprintf_used(buffer, size, "- Cluster master is '%s'.\n",
				suspend_cluster_master);
	else
		len = snprintf_used(buffer, size, "- Cluster support is disabled.\n");
	return len;
}

/* cluster_memory_needed
 *
 * Description:	Tell the caller how much memory we need to operate during
 * 		suspend/resume.
 * Returns:	Unsigned long. Maximum number of bytes of memory required for
 * 		operation.
 */
static int suspend_cluster_memory_needed(void)
{
	return 0;
}

static int suspend_cluster_storage_needed(void)
{
	return 1 + strlen(suspend_cluster_master);
}
	
/* suspend_cluster_save_config_info
 *
 * Description:	Save informaton needed when reloading the image at resume time.
 * Arguments:	Buffer:		Pointer to a buffer of size PAGE_SIZE.
 * Returns:	Number of bytes used for saving our data.
 */
static int suspend_cluster_save_config_info(char *buffer)
{
	strcpy(buffer, suspend_cluster_master);
	return strlen(suspend_cluster_master + 1);
}

/* suspend_cluster_load_config_info
 *
 * Description:	Reload information needed for declustering the image at 
 * 		resume time.
 * Arguments:	Buffer:		Pointer to the start of the data.
 *		Size:		Number of bytes that were saved.
 */
static void suspend_cluster_load_config_info(char *buffer, int size)
{
	strncpy(suspend_cluster_master, buffer, size);
	return;
}

/*
 * data for our sysfs entries.
 */
static struct suspend_sysfs_data sysfs_params[] = {
	{
		SUSPEND2_ATTR("master", SYSFS_RW),
		SYSFS_STRING(suspend_cluster_master, 63, SYSFS_SM_NOT_NEEDED)
	},

	{
		SUSPEND2_ATTR("enabled", SYSFS_RW),
		SYSFS_INT(&suspend_cluster_ops.enabled, 0, 1)
	}
};

/*
 * Ops structure.
 */

static struct suspend_module_ops suspend_cluster_ops = {
	.type			= FILTER_MODULE,
	.name			= "Cluster",
	.directory		= "cluster",
	.module			= THIS_MODULE,
	.memory_needed 		= suspend_cluster_memory_needed,
	.print_debug_info	= suspend_cluster_print_debug_stats,
	.save_config_info	= suspend_cluster_save_config_info,
	.load_config_info	= suspend_cluster_load_config_info,
	.storage_needed		= suspend_cluster_storage_needed,
	
	.sysfs_data		= sysfs_params,
	.num_sysfs_entries	= sizeof(sysfs_params) / sizeof(struct suspend_sysfs_data),
};

/* ---- Registration ---- */

#ifdef MODULE
#warning Module set.
#define INIT static __init
#define EXIT static __exit
#else
#define INIT
#define EXIT
#endif

INIT int s2_cluster_init(void)
{
	int temp = suspend_register_module(&suspend_cluster_ops);

	if (!strlen(suspend_cluster_master))
		suspend_cluster_ops.enabled = 0;
	return temp;	
}

EXIT void s2_cluster_exit(void)
{
	suspend_unregister_module(&suspend_cluster_ops);
}

#ifdef MODULE
MODULE_LICENSE("GPL");
module_init(s2_cluster_init);
module_exit(s2_cluster_exit);
MODULE_AUTHOR("Nigel Cunningham");
MODULE_DESCRIPTION("Cluster Support for Suspend2");
#endif
