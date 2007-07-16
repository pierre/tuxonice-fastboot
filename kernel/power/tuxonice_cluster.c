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

static char toi_cluster_master[63] = CONFIG_TOI_DEFAULT_CLUSTER_MASTER;

static struct toi_module_ops toi_cluster_ops;

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
	
	if (strlen(toi_cluster_master))
		len = snprintf_used(buffer, size, "- Cluster master is '%s'.\n",
				toi_cluster_master);
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
	return 1 + strlen(toi_cluster_master);
}
	
/* toi_cluster_save_config_info
 *
 * Description:	Save informaton needed when reloading the image at resume time.
 * Arguments:	Buffer:		Pointer to a buffer of size PAGE_SIZE.
 * Returns:	Number of bytes used for saving our data.
 */
static int toi_cluster_save_config_info(char *buffer)
{
	strcpy(buffer, toi_cluster_master);
	return strlen(toi_cluster_master + 1);
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
	strncpy(toi_cluster_master, buffer, size);
	return;
}

/*
 * data for our sysfs entries.
 */
static struct toi_sysfs_data sysfs_params[] = {
	{
		TOI_ATTR("master", SYSFS_RW),
		SYSFS_STRING(toi_cluster_master, 63, SYSFS_SM_NOT_NEEDED)
	},

	{
		TOI_ATTR("enabled", SYSFS_RW),
		SYSFS_INT(&toi_cluster_ops.enabled, 0, 1)
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
#warning Module set.
#define INIT static __init
#define EXIT static __exit
#else
#define INIT
#define EXIT
#endif

INIT int toi_cluster_init(void)
{
	int temp = toi_register_module(&toi_cluster_ops);

	if (!strlen(toi_cluster_master))
		toi_cluster_ops.enabled = 0;
	return temp;	
}

EXIT void toi_cluster_exit(void)
{
	toi_unregister_module(&toi_cluster_ops);
}

#ifdef MODULE
MODULE_LICENSE("GPL");
module_init(toi_cluster_init);
module_exit(toi_cluster_exit);
MODULE_AUTHOR("Nigel Cunningham");
MODULE_DESCRIPTION("Cluster Support for TuxOnIce");
#endif
