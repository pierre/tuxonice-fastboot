/*
 * kernel/power/tuxonice_power_off.h
 *
 * Copyright (C) 2006-2007 Nigel Cunningham (nigel at suspend2 net)
 *
 * This file is released under the GPLv2.
 *
 * Support for the powering down.
 */

int suspend_pm_state_finish(void);
void suspend2_power_down(void);
extern unsigned long suspend2_poweroff_method;
extern int suspend2_platform_prepare(void);
extern void suspend2_platform_finish(void);
int s2_poweroff_init(void);
void s2_poweroff_exit(void);
void suspend2_check_resleep(void);
