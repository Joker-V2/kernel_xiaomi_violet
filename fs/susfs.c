// SPDX-License-Identifier: GPL-2.0
/*
 * SUSFS minimal implementation
 * Feature: uname spoofing only
 * Target: NO-GKI kernels
 */

#include <linux/kernel.h>
#include <linux/uaccess.h>
#include <linux/spinlock.h>
#include <linux/utsname.h>
#include <linux/string.h>
#include <linux/init.h>

#include <linux/susfs.h>

#if defined(CONFIG_KSU_SUSFS) && defined(CONFIG_KSU_SUSFS_SPOOF_UNAME)

/* ===================================================== */
/* =============== INTERNAL STATE ====================== */
/* ===================================================== */

static DEFINE_SPINLOCK(susfs_uname_lock);
static struct st_susfs_uname susfs_uname_data;

/* ===================================================== */
/* =============== INTERNAL INIT ======================= */
/* ===================================================== */

static void susfs_uname_internal_init(void)
{
	memset(&susfs_uname_data, 0, sizeof(susfs_uname_data));
}

/* ===================================================== */
/* =============== USERSPACE API ======================= */
/* ===================================================== */
/*
 * Called from KernelSU ioctl
 * CMD_SUSFS_SET_UNAME
 */
void susfs_set_uname(void __user **user_info)
{
	struct st_susfs_uname info;

	if (!user_info || !*user_info)
		return;

	if (copy_from_user(&info,
		(struct st_susfs_uname __user *)*user_info,
		sizeof(info)))
		return;

	spin_lock(&susfs_uname_lock);

	/* release */
	if (!strcmp(info.release, "default")) {
		strncpy(susfs_uname_data.release,
			utsname()->release,
			__NEW_UTS_LEN);
	} else {
		strncpy(susfs_uname_data.release,
			info.release,
			__NEW_UTS_LEN);
	}

	/* version */
	if (!strcmp(info.version, "default")) {
		strncpy(susfs_uname_data.version,
			utsname()->version,
			__NEW_UTS_LEN);
	} else {
		strncpy(susfs_uname_data.version,
			info.version,
			__NEW_UTS_LEN);
	}

	spin_unlock(&susfs_uname_lock);
}

/* ===================================================== */
/* =============== KERNEL HOOK ========================= */
/* ===================================================== */
/*
 * Called from kernel/sys.c → newuname()
 * This is the REAL kernel-level hook
 */
void susfs_spoof_uname(struct new_utsname *tmp)
{
	if (!tmp)
		return;

	/* not configured yet */
	if (susfs_uname_data.release[0] == '\0')
		return;

	spin_lock(&susfs_uname_lock);

	strncpy(tmp->release,
		susfs_uname_data.release,
		__NEW_UTS_LEN);

	strncpy(tmp->version,
		susfs_uname_data.version,
		__NEW_UTS_LEN);

	spin_unlock(&susfs_uname_lock);
}

/* ===================================================== */
/* =============== MODULE INIT ========================== */
/* ===================================================== */

int __init susfs_init(void)
{
	susfs_uname_internal_init();
	pr_info("susfs: uname spoofing initialized\n");
	return 0;
}

late_initcall(susfs_init);

#endif /* CONFIG_KSU_SUSFS && CONFIG_KSU_SUSFS_SPOOF_UNAME */

/* ===================================================== */
/* =============== STUBS (required by KernelSU) ======== */
/* ===================================================== */

void susfs_set_avc_log_spoofing(void __user **u) {}
void susfs_get_enabled_features(void __user **u) {}
void susfs_show_variant(void __user **u) {}
void susfs_show_version(void __user **u) {}
