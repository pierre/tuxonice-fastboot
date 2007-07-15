
ifeq ($(CONFIG_PM_DEBUG),y)
EXTRA_CFLAGS	+=	-DDEBUG
endif

obj-y				:= main.o process.o console.o
obj-$(CONFIG_PM_LEGACY)		+= pm.o
obj-$(CONFIG_SOFTWARE_SUSPEND)	+= snapshot.o

tuxonice_core-objs := tuxonice_modules.o tuxonice_sysfs.o tuxonice_highlevel.o \
		tuxonice_io.o tuxonice_pagedir.o tuxonice_prepare_image.o \
		tuxonice_extent.o tuxonice_pageflags.o tuxonice_ui.o \
		tuxonice_power_off.o tuxonice_atomic_copy.o

obj-$(CONFIG_TOI)		+= tuxonice_builtin.o

ifdef CONFIG_TOI_CHECKSUM
tuxonice_core-objs		+= tuxonice_checksum.o
endif

ifdef CONFIG_NET
tuxonice_core-objs		+= tuxonice_storage.o tuxonice_netlink.o
endif

obj-$(CONFIG_TOI_CORE)		+= tuxonice_core.o
obj-$(CONFIG_TOI_CRYPTO)	+= tuxonice_compress.o

obj-$(CONFIG_TOI_SWAP)		+= tuxonice_block_io.o tuxonice_swap.o
obj-$(CONFIG_TOI_FILE)		+= tuxonice_block_io.o tuxonice_file.o
obj-$(CONFIG_TOI_CLUSTER)	+= tuxonice_cluster.o

obj-$(CONFIG_TOI_USERUI)	+= tuxonice_userui.o

obj-$(CONFIG_SOFTWARE_SUSPEND)	+= swsusp.o disk.o swap.o user.o

obj-$(CONFIG_MAGIC_SYSRQ)	+= poweroff.o
