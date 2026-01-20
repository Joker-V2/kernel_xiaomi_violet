#include <linux/compiler_types.h>
#include <linux/preempt.h>
#include <linux/printk.h>
#include <linux/mm.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,0,0)
#include <linux/pgtable.h>
#else
#include <asm/pgtable.h>
#endif
#include <linux/uaccess.h>
#include <asm/current.h>
#include <linux/cred.h>
#include <linux/fs.h>
#include <linux/types.h>
#include <linux/version.h>
#include <linux/sched/task_stack.h>
#include <linux/ptrace.h>

#include "allowlist.h"
#include "feature.h"
#include "klog.h" // IWYU pragma: keep
#include "ksud.h"
#include "sucompat.h"
#include "app_profile.h"
#include "util.h"

extern void write_sulog(uint8_t sym);

#define SU_PATH "/system/bin/su"
#define SH_PATH "/system/bin/sh"

bool ksu_su_compat_enabled __read_mostly = true;

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

static void __user *userspace_stack_buffer(const void *d, size_t len)
{
    char __user *p = (void __user *)current_user_stack_pointer() - len;
    return copy_to_user(p, d, len) ? NULL : p;
}

static inline char __user *sh_user_path(void)
{
    static const char sh_path[] = "/system/bin/sh";
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
#ifndef CONFIG_KSU_KPROBES_HOOK
    if (!ksu_sucompat_non_kp) {
        return 0;
    }
#endif

#ifndef CONFIG_KSU_SUSFS_SUS_SU
    if (!ksu_is_allow_uid(current_uid().val)) {
        return 0;
    }
#endif

    char path[sizeof(SU_PATH) + 1] = {0};
    ksu_strncpy_from_user_nofault(path, *filename_user, sizeof(path));

    if (unlikely(!memcmp(path, SU_PATH, sizeof(SU_PATH)))) {
        pr_info("faccessat su->sh!\n");
        *filename_user = sh_user_path();
    }

    return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0) && defined(CONFIG_KSU_SUSFS_SUS_SU)
struct filename* susfs_ksu_handle_stat(int *dfd, const char __user **filename_user, int *flags) {
    struct filename *name = getname_flags(*filename_user, getname_statx_lookup_flags(*flags), NULL);

    if (unlikely(IS_ERR(name) || name->name == NULL)) {
        return name;
    }

    if (likely(memcmp(name->name, SU_PATH, sizeof(SU_PATH)))) {
        return name;
    }

    const char sh[] = SH_PATH;
    pr_info("vfs_fstatat su->sh!\n");
    memcpy((void *)name->name, sh, sizeof(sh));
    return name;
}
#endif

int ksu_handle_stat(int *dfd, const char __user **filename_user, int *flags)
{
#ifndef CONFIG_KSU_KPROBES_HOOK
    if (!ksu_sucompat_non_kp) {
        return 0;
    }
#endif

#ifndef CONFIG_KSU_SUSFS_SUS_SU
    if (!ksu_is_allow_uid(current_uid().val)) {
        return 0;
    }
#endif

    if (unlikely(!filename_user)) {
        return 0;
    }

    char path[sizeof(SU_PATH) + 1] = {0};
    ksu_strncpy_from_user_retry(path, *filename_user, sizeof(path));

    if (likely(memcmp(path, SU_PATH, sizeof(SU_PATH)))) {
        return 0;
    }

    pr_info("newfstatat su->sh!\n");
    *filename_user = sh_user_path();

    return 0;
}

int ksu_handle_execve_sucompat(int *fd, const char __user **filename_user,
                               void *__never_use_argv, void *__never_use_envp,
                               int *__never_use_flags)
{
#ifndef CONFIG_KSU_KPROBES_HOOK
    if (!ksu_sucompat_non_kp) {
        return 0;
    }
#endif

    if (unlikely(!filename_user))
        return 0;

    char path[sizeof(SU_PATH) + 1] = {0};
    ksu_strncpy_from_user_retry(path, *filename_user, sizeof(path));

    if (likely(memcmp(path, SU_PATH, sizeof(SU_PATH))))
        return 0;

    write_sulog('x');
    pr_info("sys_execve su found\n");
    *filename_user = ksud_user_path();

    escape_with_root_profile();

    return 0;
}

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
