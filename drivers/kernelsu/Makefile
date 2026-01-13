# KernelSU objects
ksu_obj-y := ksuinit.o
ksu_obj-y += allowlist.o
ksu_obj-y += app_profile.o
ksu_obj-y += apk_sign.o
ksu_obj-y += sucompat.o
ksu_obj-y += throne_tracker.o
ksu_obj-y += setuid_hook.o
ksu_obj-y += kernel_compat.o
ksu_obj-y += kernel_umount.o
ksu_obj-y += supercalls.o
ksu_obj-y += feature.o
ksu_obj-y += ksud.o
ksu_obj-y += seccomp_cache.o
ksu_obj-y += file_wrapper.o
ksu_obj-y += su_mount_ns.o
ksu_obj-y += shim.o

ksu_obj-y += selinux/selinux.o
ksu_obj-y += selinux/sepolicy.o
ksu_obj-y += selinux/rules.o

ksu_obj-y += tiny_sulog.o
ksu_obj-$(CONFIG_KSU_MANUAL_SU) += manual_su.o

obj-$(CONFIG_KPM) += kpm/
kernelsu-objs := $(ksu_obj-y)
obj-$(CONFIG_KSU) += kernelsu.o

# Include paths
ccflags-y += -I$(srctree)/security/selinux
ccflags-y += -I$(srctree)/security/selinux/include
ccflags-y += -I$(objtree)/security/selinux
ccflags-y += -include $(srctree)/include/uapi/asm-generic/errno.h

# ------------------------------------------------------------------
# 🔒 KernelSU version (FIXED – no online fetch)
# ------------------------------------------------------------------
KSU_VERSION_API := 4.1.0
KSU_VERSION := 40106
KSU_VERSION_FULL := v$(KSU_VERSION_API)-sukisu-ultra-fixed@local

ccflags-y += -DKSU_VERSION=$(KSU_VERSION)
ccflags-y += -DKSU_VERSION_FULL=\"$(KSU_VERSION_FULL)\"

# ------------------------------------------------------------------
# Hook selection
# ------------------------------------------------------------------
ifeq ($(CONFIG_KSU_MANUAL_HOOK),y)
ccflags-y += -DCONFIG_KSU_MANUAL_HOOK
$(info -- KernelSU: MANUAL_HOOK enabled)
endif

ifeq ($(CONFIG_KSU_SYSCALL_HOOK),y)
ccflags-y += -DCONFIG_KSU_SYSCALL_HOOK
$(info -- KernelSU: SYSCALL_HOOK enabled)
endif

ifeq ($(CONFIG_KSU_NONE_HOOK),y)
ifeq ($(CONFIG_KSU_SUSFS),y)
ccflags-y += -DCONFIG_KSU_SUSFS
$(info -- KernelSU: SUSFS inline hook)
endif
endif

# ------------------------------------------------------------------
# Kernel type detection (informational only)
# ------------------------------------------------------------------
KERNEL_VERSION := $(VERSION).$(PATCHLEVEL)
KERNEL_TYPE := Non-GKI

ifneq ($(shell test \( $(VERSION) -ge 5 -a $(PATCHLEVEL) -ge 10 \) -o $(VERSION) -ge 6; echo $$?),0)
ifeq ($(VERSION)-$(PATCHLEVEL),5-4)
KERNEL_TYPE := GKI 1.0
endif
else
KERNEL_TYPE := GKI 2.0
endif

$(info -- KERNEL_VERSION: $(KERNEL_VERSION))
$(info -- KERNEL_TYPE: $(KERNEL_TYPE))

# ------------------------------------------------------------------
# Compatibility checks (safe for 4.14)
# ------------------------------------------------------------------
ifeq ($(shell grep -q "current_sid(void)" $(srctree)/security/selinux/include/objsec.h; echo $$?),0)
ccflags-y += -DKSU_COMPAT_HAS_CURRENT_SID
endif

ifeq ($(shell grep -q "struct selinux_state " $(srctree)/security/selinux/include/security.h; echo $$?),0)
ccflags-y += -DKSU_COMPAT_USE_SELINUX_STATE
endif

ifeq ($(shell grep -q "strncpy_from_user_nofault" $(srctree)/include/linux/uaccess.h; echo $$?),0)
ccflags-y += -DKSU_OPTIONAL_STRNCPY
endif

ifeq ($(shell grep -q "kernel_read" $(srctree)/fs/read_write.c; echo $$?),0)
ccflags-y += -DKSU_OPTIONAL_KERNEL_READ
endif

ifeq ($(shell grep -q "kernel_write" $(srctree)/fs/read_write.c; echo $$?),0)
ccflags-y += -DKSU_OPTIONAL_KERNEL_WRITE
endif

ifeq ($(shell grep -q "path_umount" $(srctree)/fs/namespace.c; echo $$?),0)
ccflags-y += -DKSU_HAS_PATH_UMOUNT
endif

ifeq ($(shell grep -q "path_mount" $(srctree)/fs/namespace.c; echo $$?),0)
ccflags-y += -DKSU_HAS_PATH_MOUNT
endif

ifeq ($(shell grep -q "atomic_t\s\+filter_count" $(srctree)/include/linux/seccomp.h; echo $$?),0)
ccflags-y += -DKSU_OPTIONAL_SECCOMP_FILTER_CNT
endif

ifeq ($(shell grep -q "struct proc_ops " $(srctree)/include/linux/proc_fs.h; echo $$?),0)
ccflags-y += -DKSU_COMPAT_HAS_PROC_OPS
endif

# ------------------------------------------------------------------
# Warnings (silence known false positives)
# ------------------------------------------------------------------
ccflags-y += -Wno-implicit-function-declaration
ccflags-y += -Wno-strict-prototypes
ccflags-y += -Wno-declaration-after-statement
ccflags-y += -Wno-unused-function
ccflags-y += -Wno-unused-variable
ccflags-y += -Wno-int-to-pointer-cast
