/*
 * kernel/power/tuxonice_cluster.h
 *
 * Copyright (C) 2006-2007 Nigel Cunningham (nigel at suspend2 net)
 * Copyright (C) 2006 Red Hat, inc.
 *
 * This file is released under the GPLv2.
 */

#ifdef CONFIG_TOI_CLUSTER
extern int toi_cluster_init(void);
extern void toi_cluster_exit(void);
#else
static inline int toi_cluster_init(void) { return 0; }
static inline void toi_cluster_exit(void) { }
#endif

