#ifndef KSU_SUSFS_H
#define KSU_SUSFS_H

#include <linux/types.h>
#include <linux/utsname.h>

/*
 * Minimal SUSFS header
 * Only what uname spoofing really needs
 * Everything else intentionally removed
 */

/* ===== CONFIG ===== */
#ifdef CONFIG_KSU_SUSFS_SPOOF_UNAME

struct st_susfs_uname {
	char release[__NEW_UTS_LEN + 1];
	char version[__NEW_UTS_LEN + 1];
	int  err;
};

/* userspace setter (KernelSU ioctl) */
void susfs_set_uname(void __user **user_info);

/* kernel hook (used in kernel/sys.c) */
void susfs_spoof_uname(struct new_utsname *tmp);

#endif /* CONFIG_KSU_SUSFS_SPOOF_UNAME */

/* ===== CORE INIT ===== */
/* must be global (used by late_initcall) */
int susfs_init(void);

/* ===== STUBS (keep linker happy) ===== */
void susfs_set_avc_log_spoofing(void __user **u);
void susfs_get_enabled_features(void __user **u);
void susfs_show_variant(void __user **u);
void susfs_show_version(void __user **u);

#endif /* KSU_SUSFS_H */
