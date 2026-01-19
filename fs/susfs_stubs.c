// SPDX-License-Identifier: GPL-2.0
#include <linux/types.h>
#include <linux/uidgid.h>

/*
 * These are stub implementations to satisfy linker
 * when SUSFS / KernelSU features are partially enabled.
 */

/* ---------- KernelSU ---------- */
bool ksu_handle_sys_reboot(int cmd)
{
	return false;
}

/* ---------- SUSFS domains ---------- */
bool susfs_is_current_ksu_domain(void)
{
	return false;
}

bool susfs_is_current_zygote_domain(void)
{
	return false;
}

/* ---------- SUSFS umount ---------- */
#ifdef CONFIG_KSU_SUSFS_SUS_MOUNT
void try_umount(const char *mnt, bool check_mnt, int flags, uid_t uid)
{
	/* stub */
}

void susfs_try_umount_all(void)
{
	/* stub */
}
#endif
