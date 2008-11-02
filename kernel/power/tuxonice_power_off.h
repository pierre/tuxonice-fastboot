/*
 * kernel/power/tuxonice_power_off.h
 *
 * Copyright (C) 2006-2007 Nigel Cunningham (nigel at tuxonice net)
 *
 * This file is released under the GPLv2.
 *
 * Support for the powering down.
 */

int toi_pm_state_finish(void);
void toi_power_down(void);
extern unsigned long toi_poweroff_method;
extern int toi_platform_prepare(void);
int toi_poweroff_init(void);
void toi_poweroff_exit(void);
void toi_check_resleep(void);

extern int platform_begin(int platform_mode);
extern int platform_pre_snapshot(int platform_mode);
extern int platform_leave(int platform_mode);
extern int platform_end(int platform_mode);
extern int platform_finish(int platform_mode);
extern int platform_pre_restore(int platform_mode);
extern int platform_restore_cleanup(int platform_mode);

#define platform_test() (toi_poweroff_method == 4)
#define toi_platform_begin() platform_begin(platform_test())
#define toi_platform_pre_snapshot() platform_pre_snapshot(platform_test())
#define toi_platform_leave() platform_leave(platform_test())
#define toi_platform_end() platform_end(platform_test())
#define toi_platform_finish() platform_finish(platform_test())
#define toi_platform_pre_restore() platform_pre_restore(platform_test())
#define toi_platform_restore_cleanup() platform_restore_cleanup(platform_test())
#define toi_platform_recover() platform_recover(platform_test())
