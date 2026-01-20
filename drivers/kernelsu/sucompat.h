#ifndef __KSU_H_SUCOMPAT
#define __KSU_H_SUCOMPAT

#include <linux/types.h>
#include <linux/fs.h>  // struct inode

extern bool ksu_su_compat_enabled; // For SUSFS mode

void ksu_sucompat_init(void);
void ksu_sucompat_exit(void);

/* Handler functions for manual syscall hooks */
int ksu_handle_faccessat(int *dfd, const char __user **filename_user,
                         int *mode, int *__unused_flags);
int ksu_handle_stat(int *dfd, const char __user **filename_user,
                    int *flags);

/* execve handlers: Manual hooks */
int ksu_handle_execve_sucompat(int *fd, const char __user **filename_user,
                               void *__never_use_argv,
                               void *__never_use_envp,
                               int *__never_use_flags);

/* execveat handler (for newer kernels or special manual hook) */
int ksu_handle_execveat_sucompat(int *fd, struct filename **filename_ptr,
                                 void *__never_use_argv,
                                 void *__never_use_envp,
                                 int *__never_use_flags);

/* Handler for devpts inode (pts_unix98_lookup) */
int ksu_handle_devpts(struct inode *inode);

#endif // __KSU_H_SUCOMPAT
