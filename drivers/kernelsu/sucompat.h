#ifndef __KSU_H_SUCOMPAT
#define __KSU_H_SUCOMPAT

#include <linux/types.h>
#include <linux/fs.h>

extern bool ksu_su_compat_enabled;

void ksu_sucompat_init(void);
void ksu_sucompat_exit(void);

// Handler functions exported for hook_manager
int ksu_handle_faccessat(int *dfd, const char __user **filename_user, int *mode,
                         int *__unused_flags);
int ksu_handle_stat(int *dfd, const char __user **filename_user, int *flags);
int ksu_handle_execve_sucompat(const char __user **filename_user,
                               void *__never_use_argv, void *__never_use_envp,
                               int *__never_use_flags);

#ifdef CONFIG_KSU_SUSFS_SUS_SU
extern bool ksu_devpts_hook;
extern bool susfs_is_sus_su_hooks_enabled;
extern int susfs_sus_su_working_mode;

void ksu_susfs_enable_sus_su(void);
void ksu_susfs_disable_sus_su(void);
#endif

#endif // __KSU_H_SUCOMPAT
