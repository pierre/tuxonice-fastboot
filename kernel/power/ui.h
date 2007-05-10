/*
 * kernel/power/ui.h
 *
 * Copyright (C) 2004-2007 Nigel Cunningham (nigel at suspend2 net)
 */

enum {
	DONT_CLEAR_BAR,
	CLEAR_BAR
};

enum {
	/* Userspace -> Kernel */
	USERUI_MSG_ABORT = 0x11,
	USERUI_MSG_SET_STATE = 0x12,
	USERUI_MSG_GET_STATE = 0x13,
	USERUI_MSG_GET_DEBUG_STATE = 0x14,
	USERUI_MSG_SET_DEBUG_STATE = 0x15,
	USERUI_MSG_SPACE = 0x18,
	USERUI_MSG_GET_POWERDOWN_METHOD = 0x1A,
	USERUI_MSG_SET_POWERDOWN_METHOD = 0x1B,

	/* Kernel -> Userspace */
	USERUI_MSG_MESSAGE = 0x21,
	USERUI_MSG_PROGRESS = 0x22,
	USERUI_MSG_REDRAW = 0x25,

	USERUI_MSG_MAX,
};

struct userui_msg_params {
	unsigned long a, b, c, d;
	char text[255];
};

struct ui_ops {
	char (*wait_for_key) (int timeout);
	unsigned long (*update_status) (unsigned long value,
		unsigned long maximum, const char *fmt, ...);
	void (*prepare_status) (int clearbar, const char *fmt, ...);
	void (*cond_pause) (int pause, char *message);
	void (*abort)(int result_code, const char *fmt, ...);
	void (*prepare)(void);
	void (*cleanup)(void);
	void (*redraw)(void);
	void (*message)(unsigned long section, unsigned long level,
		int normally_logged, const char *fmt, ...);
};

extern struct ui_ops *s2_current_ui;

#define suspend_update_status(val, max, fmt, args...) \
 (s2_current_ui ? (s2_current_ui->update_status) (val, max, fmt, ##args) : max)

#define suspend_wait_for_keypress(timeout) \
 (s2_current_ui ? (s2_current_ui->wait_for_key) (timeout) : 0)

#define suspend_ui_redraw(void) \
	do { if (s2_current_ui) \
		(s2_current_ui->redraw)(); \
	} while(0)

#define suspend_prepare_console(void) \
	do { if (s2_current_ui) \
		(s2_current_ui->prepare)(); \
	} while(0)

#define suspend_cleanup_console(void) \
	do { if (s2_current_ui) \
		(s2_current_ui->cleanup)(); \
	} while(0)

#define abort_suspend(result, fmt, args...) \
	do { if (s2_current_ui) \
		(s2_current_ui->abort)(result, fmt, ##args); \
	     else { \
		set_result_state(SUSPEND_ABORTED); \
		set_result_state(result); \
	     } \
	} while(0)

#define suspend_cond_pause(pause, message) \
	do { if (s2_current_ui) \
		(s2_current_ui->cond_pause)(pause, message); \
	} while(0)

#define suspend_prepare_status(clear, fmt, args...) \
	do { if (s2_current_ui) \
		(s2_current_ui->prepare_status)(clear, fmt, ##args); \
	     else \
		printk(fmt, ##args); \
	} while(0)

extern int suspend_default_console_level;

#define suspend_message(sn, lev, log, fmt, a...) \
do { \
	if (s2_current_ui && (!sn || test_debug_state(sn))) \
		s2_current_ui->message(sn, lev, log, fmt, ##a); \
} while(0)

__exit void suspend_ui_cleanup(void);
extern int s2_ui_init(void);
extern void s2_ui_exit(void);
extern int s2_register_ui_ops(struct ui_ops *this_ui);
extern void s2_remove_ui_ops(struct ui_ops *this_ui);
