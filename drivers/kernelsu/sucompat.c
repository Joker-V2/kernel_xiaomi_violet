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

#include "allowlist.h"
#include "feature.h"
#include "klog.h" // IWYU pragma: keep
#include "ksud.h"
#include "sucompat.h"
#include "app_profile.h"
#include "util.h"

extern void write_sulog(uint8_t sym);
extern void escape_with_root_profile(void);

#define SU_PATH "/system/bin/su"
#define SH_PATH "/system/bin/sh"

bool ksu_su_compat_enabled __read_mostly = true;

#ifndef strncpy_from_user_nofault
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 11, 0)
#define strncpy_from_user_nofault(dst, src, len) strncpy_from_user(dst, src, len)
#define strncpy_from_user_retry(dst, src, len) strncpy_from_user(dst, src, len)
#endif
#endif

static int su_compat_feature_get(u64 *value)
{
    *value = ksu_su_compat_enabled ? 1 : 0;
    return 0;
}

static int su_compat_feature_set(u64 value)
{
    bool enable = value != 0;
    ksu_su_compat_enabled = enable;
    pr_info("su_compat: set to %d\n", enable);
    return 0;
}

static const struct ksu_feature_handler su_compat_handler = {
    .feature_id = KSU_FEATURE_SU_COMPAT,
    .name = "su_compat",
    .get_handler = su_compat_feature_get,
    .set_handler = su_compat_feature_set,
};

static inline void __user *userspace_stack_buffer(const void *d, size_t len)
{
    char __user *p = (void __user *)current_user_stack_pointer() - len;
    return copy_to_user(p, d, len) ? NULL : p;
}

static inline char __user *sh_user_path(void)
{
    static const char sh_path[] = SH_PATH;
    return userspace_stack_buffer(sh_path, sizeof(sh_path));
}

static inline char __user *ksud_user_path(void)
{
    static const char ksud_path[] = KSUD_PATH;
    return userspace_stack_buffer(ksud_path, sizeof(ksud_path));
}

int ksu_handle_faccessat(int *dfd, const char __user **filename_user,
                         int *mode, int *__unused_flags)
{
    const char su[] = SU_PATH;

    if (!ksu_is_allow_uid_for_current(current_uid().val))
        return 0;

    char path[sizeof(su) + 1];
    memset(path, 0, sizeof(path));
    strncpy_from_user_nofault(path, *filename_user, sizeof(path));

    if (unlikely(!memcmp(path, su, sizeof(su)))) {
        write_sulog('a');
        pr_info("faccessat su->sh!\n");
        *filename_user = sh_user_path();
    }
    return 0;
}

int ksu_handle_stat(int *dfd, const char __user **filename_user, int *flags)
{
    const char su[] = SU_PATH;

    if (!ksu_is_allow_uid_for_current(current_uid().val))
        return 0;

    if (unlikely(!filename_user))
        return 0;

    char path[sizeof(su) + 1];
    memset(path, 0, sizeof(path));
    strncpy_from_user_nofault(path, *filename_user, sizeof(path));

    if (unlikely(!memcmp(path, su, sizeof(su)))) {
        write_sulog('s');
        pr_info("newfstatat su->sh!\n");
        *filename_user = sh_user_path();
    }
    return 0;
}

int ksu_handle_execve_sucompat(const char __user **filename_user,
                               void *__never_use_argv, void *__never_use_envp,
                               int *__never_use_flags)
{
    const char su[] = SU_PATH;
    const char __user *fn;
    char path[sizeof(su) + 1];
    long ret;
    unsigned long addr;

    if (unlikely(!filename_user))
        return 0;

    if (!ksu_is_allow_uid_for_current(current_uid().val))
        return 0;

    addr = untagged_addr((unsigned long)*filename_user);
    fn = (const char __user *)addr;
    memset(path, 0, sizeof(path));
    ret = strncpy_from_user_nofault(path, fn, sizeof(path));

    if (ret < 0) {
        ret = strncpy_from_user_retry(path, fn, sizeof(path));
    }

    if (ret < 0) {
        pr_warn("Access filename when execve failed: %ld", ret);
        return 0;
    }

    if (likely(memcmp(path, su, sizeof(su))))
        return 0;

    write_sulog('x');
    pr_info("sys_execve su found\n");
    *filename_user = ksud_user_path();

    escape_with_root_profile();
    return 0;
}

#ifdef CONFIG_KSU_SUSFS_SUS_SU
bool ksu_devpts_hook = false;
bool susfs_is_sus_su_hooks_enabled __read_mostly = false;
int susfs_sus_su_working_mode = 0;

static bool ksu_is_su_kps_enabled(void);
void ksu_susfs_disable_sus_su(void);
void ksu_susfs_enable_sus_su(void);
#endif

void ksu_sucompat_init()
{
    if (ksu_register_feature_handler(&su_compat_handler)) {
        pr_err("Failed to register su_compat feature handler\n");
    }
}

void ksu_sucompat_exit()
{
    ksu_unregister_feature_handler(KSU_FEATURE_SU_COMPAT);
}

#ifdef CONFIG_KSU_SUSFS_SUS_SU
static bool ksu_is_su_kps_enabled(void) {
    return susfs_is_sus_su_hooks_enabled; // أو استخدام المصفوفة القديمة من kprobes إذا موجودة
}

void ksu_susfs_disable_sus_su(void) {
    susfs_is_sus_su_hooks_enabled = false;
    ksu_devpts_hook = false;
    susfs_sus_su_working_mode = 0;
    if (!ksu_is_su_kps_enabled()) {
        ksu_sucompat_init();
        ksu_su_compat_enabled = true;
    }
}

void ksu_susfs_enable_sus_su(void) {
    if (ksu_is_su_kps_enabled()) {
        ksu_sucompat_exit();
        ksu_su_compat_enabled = false;
    }
    susfs_is_sus_su_hooks_enabled = true;
    ksu_devpts_hook = true;
    susfs_sus_su_working_mode = 1;
}
#endif
