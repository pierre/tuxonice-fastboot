/*
 * kernel/power/tuxonice_atomic_copy.h
 *
 * Copyright 2007 Nigel Cunningham (nigel at suspend2 net)
 *
 * Distributed under GPLv2.
 *
 * Routines for doing the atomic save/restore.
 */

enum {
	ATOMIC_ALL_STEPS,
	ATOMIC_STEP_IRQS,
	ATOMIC_STEP_CPU_HOTPLUG,
	ATOMIC_STEP_DEVICE_RESUME,
	ATOMIC_STEP_RESUME_CONSOLE,
	ATOMIC_STEP_RESTORE_CONSOLE
};

int suspend2_go_atomic(pm_message_t state, int suspend_time);
void suspend2_end_atomic(int stage, int suspend_time);
