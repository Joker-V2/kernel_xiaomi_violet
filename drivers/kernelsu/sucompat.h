int ksu_handle_faccessat(int *dfd, const char __user **filename_user,
                         int *mode, int *__unused_flags);

int ksu_handle_stat(int *dfd, const char __user **filename_user, int *flags);

int ksu_handle_execve_sucompat(const char __user **filename_user,
                               void *__never_use_argv,
                               void *__never_use_envp,
                               int *__never_use_flags);

void ksu_sucompat_init(void);
void ksu_sucompat_exit(void);

extern bool ksu_su_compat_enabled;
