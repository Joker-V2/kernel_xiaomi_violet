// SPDX-License-Identifier: GPL-2.0
#include <linux/kernel.h>
#include <linux/uaccess.h>
#include <linux/spinlock.h>
#include <linux/utsname.h>
#include <linux/uts.h>
#include <linux/string.h>

#include <linux/susfs.h>

#if defined(CONFIG_KSU_SUSFS) && defined(CONFIG_KSU_SUSFS_SPOOF_UNAME)

/*
 * REAL uname implementation
 * - modifies init_uts_ns
 * - no syscall hook
 * - no fake tmp struct
 */

static DEFINE_SPINLOCK(susfs_uname_lock);

/* called from userspace via ksu ioctl */
void susfs_set_uname(void __user **user_info)
{
	struct st_susfs_uname info;

	if (copy_from_user(&info,
		(struct st_susfs_uname __user *)*user_info,
		sizeof(info))) {
		return;
	}

	spin_lock(&susfs_uname_lock);

	if (info.release[0])
		strlcpy(init_uts_ns.name.release,
		        info.release,
		        __NEW_UTS_LEN);

	if (info.version[0])
		strlcpy(init_uts_ns.name.version,
		        info.version,
		        __NEW_UTS_LEN);

	spin_unlock(&susfs_uname_lock);
}

#endif /* CONFIG_KSU_SUSFS && CONFIG_KSU_SUSFS_SPOOF_UNAME */
