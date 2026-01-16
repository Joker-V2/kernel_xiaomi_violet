// SPDX-License-Identifier: GPL-2.0
#include <linux/kernel.h>
#include <linux/uaccess.h>
#include <linux/spinlock.h>
#include <linux/utsname.h>
#include <linux/string.h>

#include <linux/susfs.h>

#ifdef CONFIG_KSU_SUSFS_SPOOF_UNAME

static DEFINE_SPINLOCK(susfs_uname_lock);
static struct st_susfs_uname susfs_uname;
static bool susfs_uname_enabled;

/* userspace setter (KernelSU ioctl) */
void susfs_set_uname(void __user **user_info)
{
	struct st_susfs_uname info;

	if (!user_info)
		return;

	if (copy_from_user(&info,
		(struct st_susfs_uname __user *)*user_info,
		sizeof(info)))
		return;

	spin_lock(&susfs_uname_lock);

	memset(&susfs_uname, 0, sizeof(susfs_uname));

	strlcpy(susfs_uname.release,
		info.release[0] ? info.release : utsname()->release,
		sizeof(susfs_uname.release));

	strlcpy(susfs_uname.version,
		info.version[0] ? info.version : utsname()->version,
		sizeof(susfs_uname.version));

	susfs_uname_enabled = true;

	spin_unlock(&susfs_uname_lock);
}

/* syscall hook helper */
void susfs_spoof_uname(struct new_utsname *tmp)
{
	if (!susfs_uname_enabled)
		return;

	spin_lock(&susfs_uname_lock);

	strlcpy(tmp->release,
		susfs_uname.release,
		sizeof(tmp->release));

	strlcpy(tmp->version,
		susfs_uname.version,
		sizeof(tmp->version));

	spin_unlock(&susfs_uname_lock);
}

#endif /* CONFIG_KSU_SUSFS_SPOOF_UNAME */
