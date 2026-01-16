#ifndef KSU_SUSFS_H
#define KSU_SUSFS_H

#include <linux/types.h>
#include <linux/utsname.h>

#ifdef CONFIG_KSU_SUSFS_SPOOF_UNAME

struct st_susfs_uname {
	char release[__NEW_UTS_LEN + 1];
	char version[__NEW_UTS_LEN + 1];
};

void susfs_set_uname(void __user **user_info);
void susfs_spoof_uname(struct new_utsname *tmp);

#endif /* CONFIG_KSU_SUSFS_SPOOF_UNAME */

#endif /* KSU_SUSFS_H */
