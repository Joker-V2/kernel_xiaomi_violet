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
