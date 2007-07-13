/*
 * kernel/power/tuxonice_storage.h
 *
 * Copyright (C) 2005-2007 Nigel Cunningham (nigel at suspend2 net)
 *
 * This file is released under the GPLv2.
 */

#ifdef CONFIG_NET
int suspend_prepare_usm(void);
void suspend_cleanup_usm(void);

int suspend_activate_storage(int force);
int suspend_deactivate_storage(int force);
extern int s2_usm_init(void);
extern void s2_usm_exit(void);
#else
static inline int s2_usm_init(void) { return 0; }
static inline void s2_usm_exit(void) { }

static inline int suspend_activate_storage(int force)
{
	return 0;
}

static inline int suspend_deactivate_storage(int force)
{
	return 0;
}

static inline int suspend_prepare_usm(void) { return 0; }
static inline void suspend_cleanup_usm(void) { }
#endif

enum {
	USM_MSG_BASE = 0x10,

	/* Kernel -> Userspace */
	USM_MSG_CONNECT = 0x30,
	USM_MSG_DISCONNECT = 0x31,
	USM_MSG_SUCCESS = 0x40,
	USM_MSG_FAILED = 0x41,

	USM_MSG_MAX,
};

#ifdef CONFIG_NET
extern __init int suspend_usm_init(void);
extern __exit void suspend_usm_cleanup(void);
#else
#define suspend_usm_init() do { } while(0)
#define suspend_usm_cleanup() do { } while(0)
#endif
