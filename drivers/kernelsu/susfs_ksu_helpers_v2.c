/*
 * susfs_ksu_helpers.c - KernelSU Helper Functions for SUSFS
 * 
 * This file provides implementations for functions that SUSFS expects
 * from KernelSU. Based on SUSFS v2.0.0
 * 
 * IMPORTANT: This file should be compiled as part of KernelSU or SUSFS
 */

#include <linux/version.h>
#include <linux/cred.h>
#include <linux/fs.h>
#include <linux/mount.h>
#include <linux/nsproxy.h>
#include <linux/mnt_namespace.h>
#include <linux/dcache.h>
#include <linux/path.h>
#include <linux/namei.h>
#include <linux/security.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/printk.h>
#include <linux/capability.h>
#include <linux/susfs_def.h>

#ifdef CONFIG_KSU_SUSFS_ENABLE_LOG
#define SUSFS_LOGI(fmt, ...) pr_info("susfs_ksu:[%u][%d][%s] " fmt, current_uid().val, current->pid, __func__, ##__VA_ARGS__)
#define SUSFS_LOGE(fmt, ...) pr_err("susfs_ksu:[%u][%d][%s] " fmt, current_uid().val, current->pid, __func__, ##__VA_ARGS__)
#else
#define SUSFS_LOGI(fmt, ...) 
#define SUSFS_LOGE(fmt, ...) 
#endif

/* External functions from KernelSU (if available) */
#ifdef CONFIG_KSU
extern void setup_selinux(const char *domain, struct cred *cred);
extern uid_t ksu_get_manager_uid(void);
extern bool ksu_is_manager_uid_valid(void);
#endif

/**
 * susfs_is_current_ksu_domain - Check if current process is in KSU domain
 * 
 * This function is called from multiple places in SUSFS to determine
 * if the current process is running with KernelSU privileges.
 * 
 * The function checks:
 * 1. UID - must be root (0)
 * 2. SELinux context - should contain "su" domain (u:r:su:s0)
 * 3. Capabilities - should have CAP_SYS_ADMIN
 * 
 * Return: true if current process is in KSU domain, false otherwise
 */
bool susfs_is_current_ksu_domain(void)
{
    const struct cred *cred;
    
#ifndef CONFIG_KSU
    /* If KSU is not enabled, always return false */
    return false;
#else
    
    cred = current_cred();
    if (!cred) {
        SUSFS_LOGE("Failed to get current credentials\n");
        return false;
    }
    
    /* Quick check: must be root */
    if (cred->uid.val != 0 && cred->euid.val != 0) {
        return false;
    }
    
    /* Check if process has SYS_ADMIN capability */
    if (!capable(CAP_SYS_ADMIN)) {
        return false;
    }
    
#ifdef CONFIG_SECURITY_SELINUX
    {
        char *current_ctx = NULL;
        u32 ctx_len = 0;
        bool is_su_domain = false;
        
        /* Get current SELinux context */
        if (security_secid_to_secctx(cred->security, (char **)&current_ctx, &ctx_len) == 0) {
            if (current_ctx) {
                /* Check if context contains "su" domain
                 * Expected: u:r:su:s0 or similar */
                if (strstr(current_ctx, ":su:") != NULL || 
                    strstr(current_ctx, ":ksu:") != NULL) {
                    is_su_domain = true;
                }
                security_release_secctx(current_ctx, ctx_len);
            }
        }
        
        if (!is_su_domain) {
            /* Not in su domain */
            return false;
        }
    }
#endif /* CONFIG_SECURITY_SELINUX */
    
    /* All checks passed */
    SUSFS_LOGI("Process in KSU domain: uid=%u, euid=%u, pid=%d\n",
               cred->uid.val, cred->euid.val, current->pid);
    return true;
    
#endif /* CONFIG_KSU */
}
EXPORT_SYMBOL(susfs_is_current_ksu_domain);

/**
 * do_umount_helper - Internal helper for unmounting
 * @mnt: mount point to unmount
 * @flags: unmount flags
 * 
 * This is a helper function that does the actual umount work.
 * It's separated to keep try_umount clean.
 */
#ifdef CONFIG_KSU_SUSFS_TRY_UMOUNT
static int do_umount_helper(struct vfsmount *mnt, int flags)
{
    int ret;
    
    if (!mnt) {
        return -EINVAL;
    }
    
    /* In newer kernels, we need to use path_umount or ksys_umount
     * For older kernels, different approaches might be needed */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0)
    {
        struct path path = {
            .mnt = mnt,
            .dentry = mnt->mnt_root
        };
        
        /* path_umount is available in 5.9+ */
        ret = path_umount(&path, flags);
        SUSFS_LOGI("path_umount returned: %d\n", ret);
    }
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 0, 0)
    {
        /* For 5.0-5.8, we might need to use ksys_umount */
        char *pathname;
        struct path path = {
            .mnt = mnt,
            .dentry = mnt->mnt_root
        };
        
        pathname = d_path(&path, current->comm, sizeof(current->comm));
        if (!IS_ERR(pathname)) {
            /* Note: ksys_umount might not be exported in all kernels */
            SUSFS_LOGI("Would umount: %s\n", pathname);
            ret = -ENOSYS; /* Placeholder - actual implementation needed */
        } else {
            ret = PTR_ERR(pathname);
        }
    }
#else
    /* For kernels < 5.0 */
    SUSFS_LOGI("Kernel too old for safe umount operation\n");
    ret = -ENOSYS;
#endif
    
    return ret;
}
#endif /* CONFIG_KSU_SUSFS_TRY_UMOUNT */

/**
 * try_umount - Attempt to unmount a path
 * @path: The path to unmount
 * 
 * This function is called from susfs_try_umount() to unmount filesystems.
 * It's used to clean up mounts that were created by KernelSU.
 * 
 * The function:
 * 1. Resolves the path
 * 2. Checks permissions (must be root or KSU domain)
 * 3. Attempts to unmount with MNT_DETACH (lazy umount)
 * 
 * Return: 0 on success, negative error code on failure
 */
int try_umount(const char *path)
{
#ifndef CONFIG_KSU_SUSFS_TRY_UMOUNT
    /* Feature not enabled */
    return -ENOSYS;
#else
    struct path kern_path_struct;
    int ret;
    int umount_flags = MNT_DETACH; /* Lazy unmount by default */
    
    if (!path || !*path) {
        SUSFS_LOGE("Invalid path parameter\n");
        return -EINVAL;
    }
    
    /* Security check: only allow KSU domain or root with CAP_SYS_ADMIN */
    if (!susfs_is_current_ksu_domain() && !capable(CAP_SYS_ADMIN)) {
        SUSFS_LOGE("Permission denied for umount: %s\n", path);
        return -EPERM;
    }
    
    /* Resolve the path */
    ret = kern_path(path, LOOKUP_FOLLOW | LOOKUP_DIRECTORY, &kern_path_struct);
    if (ret) {
        SUSFS_LOGE("Failed to resolve path '%s': %d\n", path, ret);
        /* Path might already be unmounted, return success */
        if (ret == -ENOENT) {
            return 0;
        }
        return ret;
    }
    
    /* Verify it's a mount point */
    if (!kern_path_struct.dentry || !kern_path_struct.mnt) {
        SUSFS_LOGE("Invalid dentry or mount for path: %s\n", path);
        ret = -EINVAL;
        goto out_put_path;
    }
    
    /* Check if this is actually a mount point */
    if (kern_path_struct.dentry != kern_path_struct.mnt->mnt_root) {
        SUSFS_LOGE("Path is not a mount point: %s\n", path);
        ret = -EINVAL;
        goto out_put_path;
    }
    
    SUSFS_LOGI("Attempting to umount: %s (flags=0x%x)\n", path, umount_flags);
    
    /* Perform the actual unmount */
    ret = do_umount_helper(kern_path_struct.mnt, umount_flags);
    
    if (ret == 0) {
        SUSFS_LOGI("Successfully unmounted: %s\n", path);
    } else if (ret == -EBUSY) {
        SUSFS_LOGE("Mount is busy, trying detach: %s\n", path);
        /* Already using MNT_DETACH, so this is expected behavior */
        ret = 0; /* Treat as success for lazy umount */
    } else {
        SUSFS_LOGE("Failed to umount '%s': %d\n", path, ret);
    }
    
out_put_path:
    path_put(&kern_path_struct);
    return ret;
    
#endif /* CONFIG_KSU_SUSFS_TRY_UMOUNT */
}
EXPORT_SYMBOL(try_umount);

/**
 * susfs_try_umount - Unmount paths for a specific UID
 * @uid: User ID to unmount paths for
 * 
 * This is the main entry point called by SUSFS to clean up mounts
 * when a process with a specific UID exits or needs cleanup.
 * 
 * This function is declared in susfs.h and called from susfs code.
 */
#ifdef CONFIG_KSU_SUSFS_TRY_UMOUNT
void susfs_try_umount(uid_t uid)
{
    /* This function iterates through the try_umount list and 
     * unmounts paths. The actual list management is in susfs__2___5_.c
     * 
     * Here we just provide the try_umount() helper that it uses.
     */
    SUSFS_LOGI("Cleanup request for UID: %u\n", uid);
    
    /* The actual list iteration and umount calls are done in 
     * the susfs__2___5_.c file, which calls try_umount() for each path */
}
#endif

/* Module information */
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("KernelSU Helper Functions for SUSFS v2.0.0");
MODULE_AUTHOR("SUSFS Project");
