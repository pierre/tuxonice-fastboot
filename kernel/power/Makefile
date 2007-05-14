
ifeq ($(CONFIG_PM_DEBUG),y)
EXTRA_CFLAGS	+=	-DDEBUG
endif

obj-y				:= main.o process.o console.o
obj-$(CONFIG_PM_LEGACY)		+= pm.o
obj-$(CONFIG_SUSPEND_SHARED)	+= snapshot.o

suspend_core-objs := modules.o sysfs.o suspend.o \
		io.o pagedir.o prepare_image.o \
		extent.o pageflags.o ui.o \
		power_off.o atomic_copy.o

obj-$(CONFIG_SUSPEND2)		+= suspend2_builtin.o

ifdef CONFIG_SUSPEND2_CHECKSUM
suspend_core-objs	+= checksum.o
endif

ifdef CONFIG_NET
suspend_core-objs	+= storage.o netlink.o
endif

obj-$(CONFIG_SUSPEND2_CORE)	+= suspend_core.o
obj-$(CONFIG_SUSPEND2_CRYPTO)	+= suspend_compress.o

obj-$(CONFIG_SUSPEND2_SWAP)	+= suspend_block_io.o suspend_swap.o
obj-$(CONFIG_SUSPEND2_FILE)	+= suspend_block_io.o suspend_file.o
obj-$(CONFIG_SUSPEND2_CLUSTER)	+= cluster.o

obj-$(CONFIG_SUSPEND2_USERUI)	+= suspend_userui.o

obj-$(CONFIG_SOFTWARE_SUSPEND)	+= swsusp.o disk.o swap.o user.o

obj-$(CONFIG_MAGIC_SYSRQ)	+= poweroff.o
