#ifndef __KSU_H_SUCOMPAT
#define __KSU_H_SUCOMPAT

#include <linux/types.h>
#include <linux/fs.h>
#include <linux/cred.h>
#include <linux/kernel.h>
#include <linux/uaccess.h>
#include <linux/sched.h>
#include <linux/kprobes.h>

extern bool ksu_su_compat_enabled;

// Init/exit
void ksu_sucompat_init(void);
void ksu_sucompat_exit(void);

// Handler functions exported for hook_manager
int ksu_handle_faccessat(int *dfd, const char __user **filename_user, int *mode,
                         int *__unused_flags);

int ksu_handle_stat(int *dfd, const char __user **filename_user, int *flags);

int ksu_handle_execveat_sucompat(int *fd, struct filename **filename_ptr,
                                 void *__never_use_argv,
                                 void *__never_use_envp,
                                 int *__never_use_flags);

int ksu_handle_execve_sucompat(int *fd, const char __user **filename_user,
                               void *__never_use_argv,
                               void *__never_use_envp,
                               int *__never_use_flags);

int ksu_handle_devpts(struct inode *inode);

#endif // __KSU_H_SUCOMPAT
