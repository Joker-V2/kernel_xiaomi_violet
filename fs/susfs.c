// SPDX-License-Identifier: GPL-2.0
#include <linux/kernel.h>
#include <linux/uaccess.h>
#include <linux/spinlock.h>
#include <linux/utsname.h>
#include <linux/string.h>

#include <linux/susfs.h>

#if defined(CONFIG_KSU_SUSFS) && defined(CONFIG_KSU_SUSFS_SPOOF_UNAME)

/*
 * NOTE:
 * - This file is intentionally minimal.
 * - NO global symbols except the required uname hook.
 * - Designed for NO-GKI kernels.
 */

static DEFINE_SPINLOCK(susfs_uname_lock);
static struct st_susfs_uname susfs_uname_data;

/* internal init */
static void susfs_uname_init(void)
{
	memset(&susfs_uname_data, 0, sizeof(susfs_uname_data));
}

/* userspace setter (called via ksu ioctl) */
void susfs_set_uname(void __user **user_info)
{
	struct st_susfs_uname info = {0};

	if (copy_from_user(&info,
		(struct st_susfs_uname __user *)*user_info,
		sizeof(info))) {
		return;
	}

	spin_lock(&susfs_uname_lock);

	if (!strcmp(info.release, "default"))
		strncpy(susfs_uname_data.release,
			utsname()->release,
			__NEW_UTS_LEN);
	else
		strncpy(susfs_uname_data.release,
			info.release,
			__NEW_UTS_LEN);

	if (!strcmp(info.version, "default"))
		strncpy(susfs_uname_data.version,
			utsname()->version,
			__NEW_UTS_LEN);
	else
		strncpy(susfs_uname_data.version,
			info.version,
			__NEW_UTS_LEN);

	spin_unlock(&susfs_uname_lock);
}

/*
 * ONLY exported symbol
 * Used by kernel/sys.c newuname hook
 */
void susfs_spoof_uname(struct new_utsname *tmp)
{
	if (unlikely(susfs_uname_data.release[0] == '\0'))
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

/* late init (must NOT be static because declared in header) */
int __init susfs_init(void)
{
	susfs_uname_init();
	return 0;
}
late_initcall(susfs_init);

#endif /* CONFIG_KSU_SUSFS && CONFIG_KSU_SUSFS_SPOOF_UNAME */

/* ===================== */
/* STUBS FOR KERNELSU    */
/* ===================== */

#ifndef CONFIG_KSU_SUSFS_SUS_PATH
void susfs_set_i_state_on_external_dir(void __user **u) {}
void susfs_add_sus_path(void __user **u) {}
void susfs_add_sus_path_loop(void __user **u) {}
void susfs_run_sus_path_loop(void) {}
#endif

#ifndef CONFIG_KSU_SUSFS_SUS_KSTAT
void susfs_add_sus_kstat(void __user **u) {}
void susfs_update_sus_kstat(void __user **u) {}
void susfs_sus_ino_for_generic_fillattr(unsigned long i, struct kstat *s) {}
void susfs_sus_ino_for_show_map_vma(unsigned long i, dev_t *d, unsigned long *o) {}
#endif

#ifndef CONFIG_KSU_SUSFS_SUS_MOUNT
void susfs_set_hide_sus_mnts_for_all_procs(void __user **u) {}
void susfs_reorder_mnt_id(struct mount *mnt) {}
#endif

#ifndef CONFIG_KSU_SUSFS_TRY_UMOUNT
void susfs_add_try_umount(void __user **u) {}
void susfs_try_umount(uid_t uid) {}
#endif

#ifndef CONFIG_KSU_SUSFS_ENABLE_LOG
void susfs_enable_log(void __user **u) {}
#endif

#ifndef CONFIG_KSU_SUSFS_SPOOF_CMDLINE_OR_BOOTCONFIG
void susfs_set_cmdline_or_bootconfig(void __user **u) {}
int susfs_spoof_cmdline_or_bootconfig(struct seq_file *m) { return 0; }
#endif

#ifndef CONFIG_KSU_SUSFS_OPEN_REDIRECT
void susfs_add_open_redirect(void __user **u) {}
struct filename* susfs_get_redirected_path(unsigned long ino) { return NULL; }
#endif

#ifndef CONFIG_KSU_SUSFS_SUS_MAP
void susfs_add_sus_map(void __user **u) {}
#endif

void susfs_set_avc_log_spoofing(void __user **u) {}
void susfs_get_enabled_features(void __user **u) {}
void susfs_show_variant(void __user **u) {}
void susfs_show_version(void __user **u) {}
