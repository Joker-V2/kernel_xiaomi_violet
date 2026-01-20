#include <linux/dcache.h>
#include <linux/security.h>
#include <asm/current.h>
#include <linux/cred.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/kprobes.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 11, 0)
#include <linux/sched/task_stack.h>
#else
#include <linux/sched.h>
#endif

#include "allowlist.h"
#include "arch.h"
#include "klog.h" // IWYU pragma: keep
#include "ksud.h"
#include "sucompat.h"

#ifdef CONFIG_KSU_SUSFS_SUS_SU
#include <linux/susfs_def.h>
#endif

#define SU_PATH "/system/bin/su"
#define SH_PATH "/system/bin/sh"

#ifndef CONFIG_KSU_KPROBES_HOOK
static bool ksu_sucompat_non_kp __read_mostly = true;
#endif

extern void escape_with_root_profile(void);

/* write data below user stack pointer */
static void __user *userspace_stack_buffer(const void *d, size_t len)
{
	char __user *p = (void __user *)current_user_stack_pointer() - len;
	return copy_to_user(p, d, len) ? NULL : p;
}

static char __user *sh_user_path(void)
{
	static const char sh_path[] = SH_PATH;
	return userspace_stack_buffer(sh_path, sizeof(sh_path));
}

static char __user *ksud_user_path(void)
{
	static const char ksud_path[] = KSUD_PATH;
	return userspace_stack_buffer(ksud_path, sizeof(ksud_path));
}

int ksu_handle_faccessat(int *dfd, const char __user **filename_user,
			 int *mode, int *__unused_flags)
{
	const char su[] = SU_PATH;
	char path[sizeof(su) + 1] = {0};

#ifndef CONFIG_KSU_KPROBES_HOOK
	if (!ksu_sucompat_non_kp)
		return 0;
#endif

	if (!ksu_is_allow_uid(current_uid().val))
		return 0;

	if (strncpy_from_user(path, *filename_user, sizeof(path)) < 0)
		return 0;

	if (!memcmp(path, su, sizeof(su))) {
		pr_info("faccessat su -> sh\n");
		*filename_user = sh_user_path();
	}

	return 0;
}

int ksu_handle_stat(int *dfd, const char __user **filename_user, int *flags)
{
	const char su[] = SU_PATH;
	char path[sizeof(su) + 1] = {0};

#ifndef CONFIG_KSU_KPROBES_HOOK
	if (!ksu_sucompat_non_kp)
		return 0;
#endif

	if (!ksu_is_allow_uid(current_uid().val))
		return 0;

	if (!filename_user)
		return 0;

	if (strncpy_from_user(path, *filename_user, sizeof(path)) < 0)
		return 0;

	if (!memcmp(path, su, sizeof(su))) {
		pr_info("newfstatat su -> sh\n");
		*filename_user = sh_user_path();
	}

	return 0;
}

int ksu_handle_execve_sucompat(int *fd, const char __user **filename_user,
			       void *__unused_argv, void *__unused_envp,
			       int *__unused_flags)
{
	const char su[] = SU_PATH;
	char path[sizeof(su) + 1] = {0};

#ifndef CONFIG_KSU_KPROBES_HOOK
	if (!ksu_sucompat_non_kp)
		return 0;
#endif

	if (!filename_user)
		return 0;

	if (strncpy_from_user(path, *filename_user, sizeof(path)) < 0)
		return 0;

	if (memcmp(path, su, sizeof(su)))
		return 0;

	if (!ksu_is_allow_uid(current_uid().val))
		return 0;

	pr_info("execve su -> ksud\n");
	*filename_user = ksud_user_path();

	escape_with_root_profile();

	return 0;
}

#ifdef CONFIG_KSU_KPROBES_HOOK

static int execve_handler_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct pt_regs *r = PT_REAL_REGS(regs);
	const char __user **filename_user =
		(const char **)&PT_REGS_PARM1(r);

	return ksu_handle_execve_sucompat(AT_FDCWD, filename_user,
					 NULL, NULL, NULL);
}

static int faccessat_handler_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct pt_regs *r = PT_REAL_REGS(regs);
	int *dfd = (int *)&PT_REGS_PARM1(r);
	const char __user **filename_user =
		(const char **)&PT_REGS_PARM2(r);
	int *mode = (int *)&PT_REGS_PARM3(r);

	return ksu_handle_faccessat(dfd, filename_user, mode, NULL);
}

static int newfstatat_handler_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct pt_regs *r = PT_REAL_REGS(regs);
	int *dfd = (int *)&PT_REGS_PARM1(r);
	const char __user **filename_user =
		(const char **)&PT_REGS_PARM2(r);
	int *flags = (int *)&PT_REGS_SYSCALL_PARM4(r);

	return ksu_handle_stat(dfd, filename_user, flags);
}

static struct kprobe kp_execve = {
	.symbol_name = SYS_EXECVE_SYMBOL,
	.pre_handler = execve_handler_pre,
};

static struct kprobe kp_faccessat = {
	.symbol_name = SYS_FACCESSAT_SYMBOL,
	.pre_handler = faccessat_handler_pre,
};

static struct kprobe kp_newfstatat = {
	.symbol_name = SYS_NEWFSTATAT_SYMBOL,
	.pre_handler = newfstatat_handler_pre,
};

#endif

void ksu_sucompat_init(void)
{
#ifdef CONFIG_KSU_KPROBES_HOOK
	register_kprobe(&kp_execve);
	register_kprobe(&kp_faccessat);
	register_kprobe(&kp_newfstatat);
	pr_info("sucompat: kprobes enabled\n");
#else
	ksu_sucompat_non_kp = true;
	pr_info("sucompat: syscall hooks enabled\n");
#endif
}

void ksu_sucompat_exit(void)
{
#ifdef CONFIG_KSU_KPROBES_HOOK
	unregister_kprobe(&kp_execve);
	unregister_kprobe(&kp_faccessat);
	unregister_kprobe(&kp_newfstatat);
#else
	ksu_sucompat_non_kp = false;
#endif
}
