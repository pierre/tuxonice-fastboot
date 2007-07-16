/*
 * kernel/power/tuxonice_checksum.h
 *
 * Copyright (C) 2006-2007 Nigel Cunningham (nigel at suspend2 net)
 * Copyright (C) 2006 Red Hat, inc.
 *
 * This file is released under the GPLv2.
 *
 * This file contains data checksum routines for TuxOnIce,
 * using cryptoapi. They are used to locate any modifications
 * made to pageset 2 while we're saving it.
 */

#if defined(CONFIG_TOI_CHECKSUM)
extern int toi_checksum_init(void);
extern void toi_checksum_exit(void);
void calculate_check_checksums(int check);
int allocate_checksum_pages(void);
void free_checksum_pages(void);
#else
static inline int toi_checksum_init(void) { return 0; }
static inline void toi_checksum_exit(void) { }
static inline void calculate_check_checksums(int check) { };
static inline int allocate_checksum_pages(void) { return 0; };
static inline void free_checksum_pages(void) { };
#endif

