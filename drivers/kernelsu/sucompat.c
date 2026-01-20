#include <linux/compiler_types.h>
#include <linux/preempt.h>
#include <linux/printk.h>
#include <linux/mm.h>
#include <linux/pgtable.h>
#include <linux/uaccess.h>
#include <asm/current.h>
#include <linux/cred.h>
#include <linux/fs.h>
#include <linux/types.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 11, 0)
#include <linux/sched/task_stack.h>
#else
#include <linux/sched.h>
#endif

#ifdef CONFIG_KSU_SUSFS_SUS_SU
#include <linux/susfs_def.h>
#endif

#include "allowlist.h"
#include "arch.h"
#include "klog.h" // IWYU pragma: keep
#include "ksud.h"
#include "sucompat.h"

extern void escape_to_root(void);

#define SU_PATH "/system/bin/su"
#define SH_PATH "/system/bin/sh"

#ifndef CONFIG_KSU_KPROBES_HOOK
static bool ksu_sucompat_non_kp __read_mostly = true;
#endif

static const char sh_path[] = SH_PATH;
static const char ksud_path[] = KSUD_PATH;
static const char su[] = SU_PATH;

static inline void __user *userspace_stack_buffer(const void *d, size_t len)
{
    char __user *p = (void __user *)current_user_stack_pointer() - len;
    return copy_to_user(p, d, len) ? NULL : p;
}

static inline char __user *sh_user_path(void)
{
    return userspace_stack_buffer(sh_path, sizeof(sh_path));
}

static inline char __user *ksud_user_path(void)
{
    return userspace_stack_buffer(ksud_path, sizeof(ksud_path));
}

int ksu_handle_faccessat(int *dfd, const char __user **filename_user,
                         int *mode, int *__unused_flags)
{
#ifndef CONFIG_KSU_KPROBES_HOOK
    if (!ksu_sucompat_non_kp)
        return 0;
#endif

#ifndef CONFIG_KSU_SUSFS_SUS_SU
    if (!ksu_is_allow_uid(current_uid().val))
        return 0;
#endif

#ifdef CONFIG_KSU_SUSFS_SUS_SU
    char path[sizeof(su) + 1] = {0};
#else
    char path[sizeof(su) + 1];
    memset(path, 0, sizeof(path));
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 11, 0)
    strncpy_from_user_nofault(path, *filename_user, sizeof(path));
#else
    strncpy_from_user_nofault(path, *filename_user, sizeof(path));
#endif

    if (!memcmp(path, su, sizeof(su))) {
        pr_info("faccessat su->sh!\n");
        *filename_user = sh_user_path();
    }

    return 0;
}

int ksu_handle_stat(int *dfd, const char __user **filename_user, int *flags)
{
#ifndef CONFIG_KSU_KPROBES_HOOK
    if (!ksu_sucompat_non_kp)
        return 0;
#endif

#ifndef CONFIG_KSU_SUSFS_SUS_SU
    if (!ksu_is_allow_uid(current_uid().val))
        return 0;
#endif

    if (!filename_user)
        return 0;

#ifdef CONFIG_KSU_SUSFS_SUS_SU
    char path[sizeof(su) + 1] = {0};
#else
    char path[sizeof(su) + 1];
    memset(path, 0, sizeof(path));
#endif

    strncpy_from_user_nofault(path, *filename_user, sizeof(path));

    if (!memcmp(path, su, sizeof(su))) {
        pr_info("newfstatat su->sh!\n");
        *filename_user = sh_user_path();
    }

    return 0;
}

int ksu_handle_execve_sucompat(int *fd, const char __user **filename_user,
                               void *__never_use_argv, void *__never_use_envp,
                               int *__never_use_flags)
{
#ifdef CONFIG_KSU_SUSFS_SUS_SU
    char path[sizeof(su) + 1] = {0};
#else
    char path[sizeof(su) + 1];
    memset(path, 0, sizeof(path));
#endif

#ifndef CONFIG_KSU_KPROBES_HOOK
    if (!ksu_sucompat_non_kp)
        return 0;
#endif

    if (!filename_user)
        return 0;

    strncpy_from_user_nofault(path, *filename_user, sizeof(path));

    if (memcmp(path, su, sizeof(su)))
        return 0;

    if (!ksu_is_allow_uid(current_uid().val))
        return 0;

    pr_info("sys_execve su found\n");
    *filename_user = ksud_user_path();

    escape_to_root();

    return 0;
}

int ksu_handle_devpts(struct inode *inode)
{
#ifndef CONFIG_KSU_KPROBES_HOOK
    if (!ksu_sucompat_non_kp)
        return 0;
#endif

    if (!current->mm)
        return 0;

    uid_t uid = current_uid().val;
    if (uid % 100000 < 10000)
        return 0;

    if (!ksu_is_allow_uid(uid))
        return 0;

#ifdef CONFIG_KSU_SUSFS_SUS_SU
    if (ksu_devpts_hook) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 1, 0)
        struct inode_security_struct *sec = selinux_inode(inode);
#else
        struct inode_security_struct *sec = (struct inode_security_struct *)inode->i_security;
#endif
        if (sec)
            sec->sid = ksu_devpts_sid;
    }
#endif

    return 0;
}

#ifndef CONFIG_KSU_KPROBES_HOOK
void ksu_sucompat_init()
{
    ksu_sucompat_non_kp = true;
    pr_info("ksu_sucompat_init: manual hooks enabled\n");
}

void ksu_sucompat_exit()
{
    ksu_sucompat_non_kp = false;
    pr_info("ksu_sucompat_exit: manual hooks disabled\n");
}
#endif

#ifdef CONFIG_KSU_SUSFS_SUS_SU
bool ksu_devpts_hook = false;
bool susfs_is_sus_su_hooks_enabled __read_mostly = false;
int susfs_sus_su_working_mode = 0;

static bool ksu_is_su_kps_enabled(void)
{
    return false; // manual hook: always false
}

void ksu_susfs_disable_sus_su(void)
{
    susfs_is_sus_su_hooks_enabled = false;
    ksu_devpts_hook = false;
    susfs_sus_su_working_mode = 0;
}

void ksu_susfs_enable_sus_su(void)
{
    susfs_is_sus_su_hooks_enabled = true;
    ksu_devpts_hook = true;
    susfs_sus_su_working_mode = 1;
}
#endif
