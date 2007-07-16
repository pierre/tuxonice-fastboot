/*
 * kernel/power/tuxonice_power_off.h
 *
 * Copyright (C) 2006-2007 Nigel Cunningham (nigel at suspend2 net)
 *
 * This file is released under the GPLv2.
 *
 * Support for the powering down.
 */

int toi_pm_state_finish(void);
void toi_power_down(void);
extern unsigned long toi_poweroff_method;
extern int toi_platform_prepare(void);
extern void toi_platform_finish(void);
int toi_poweroff_init(void);
void toi_poweroff_exit(void);
void toi_check_resleep(void);
