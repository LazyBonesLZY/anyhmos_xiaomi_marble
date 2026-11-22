/* SPDX-License-Identifier: MIT
 * Minimal AArch64 PID1 entry for Xiaomi marble (boot.img generic ramdisk).
 * No libc, mounts, services, or userdata mutations in this stub.
 * Links the Phase 2 probe and IPC/dmabuf artifacts from vendor_boot's
 * /hmos-compat-marble/ directory into / and execs hmos-probe as PID 1.
 */
#define P "/hmos-compat-marble/"

static long call3(long number, long a, long b, long c)
{
    register long x8 __asm__("x8") = number;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
    return x0;
}

static long link_file(const char *target, const char *name)
{
    register long x8 __asm__("x8") = 37; /* linkat */
    register long x0 __asm__("x0") = -100;
    register long x1 __asm__("x1") = (long)target;
    register long x2 __asm__("x2") = -100;
    register long x3 __asm__("x3") = (long)name;
    register long x4 __asm__("x4") = 0;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4) : "memory");
    return x0;
}

static long access_exists(const char *path)
{
    register long x8 __asm__("x8") = 48; /* faccessat */
    register long x0 __asm__("x0") = -100;
    register long x1 __asm__("x1") = (long)path;
    register long x2 __asm__("x2") = 0; /* F_OK */
    register long x3 __asm__("x3") = 0;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3) : "memory");
    return x0;
}

static const char *const required_links[][2] = {
    {P "hmos-ipc.ko", "/hmos-ipc.ko"},
    {P "hmos-dmabuf-sync.ko", "/hmos-dmabuf-sync.ko"},
    {P "token_contract", "/token_contract"},
    {P "binder_nested", "/binder_nested"},
    {P "binder_oneway_pid", "/binder_oneway_pid"},
};

static const char *const optional_links[][2] = {
    {P "busybox", "/busybox"},
    {P "init.android.original", "/init.android.original"},
    {P "hmos-init-debug.cfg", "/hmos-init-debug.cfg"},
    {P "hmos-native-hilog", "/hmos-native-hilog"},
    {P "hmos-hdf-graphics.hcb", "/hmos-hdf-graphics.hcb"},
};

static const char *const optional_symlinks[][2] = {
    {P "graphics", "/graphics"},
    {P "stock-gpu-firmware", "/stock-gpu-firmware"},
};

__attribute__((noreturn)) void entry(void)
{
    int failed = call3(172, 0, 0, 0) != 1; /* getpid */
    for (unsigned i = 0; !failed && i < sizeof(required_links) / sizeof(required_links[0]); ++i) {
        if (link_file(required_links[i][0], required_links[i][1]) < 0)
            failed = 1;
    }
    for (unsigned i = 0; !failed && i < sizeof(optional_links) / sizeof(optional_links[0]); ++i) {
        (void)link_file(optional_links[i][0], optional_links[i][1]);
    }
    for (unsigned i = 0; !failed && i < sizeof(optional_symlinks) / sizeof(optional_symlinks[0]); ++i) {
        if (access_exists(optional_symlinks[i][0]) == 0)
            (void)call3(36, (long)optional_symlinks[i][0], -100, (long)optional_symlinks[i][1]); /* symlinkat */
    }
    char *const args[] = {P "hmos-probe", 0};
    char *const env[] = {0};
    if (!failed)
        call3(221, (long)args[0], (long)args, (long)env); /* execve */
    const char error[] = "MARBLE_BOOT_ENTRY_FAILED: retain PID1 for recovery\n";
    call3(64, 2, (long)error, sizeof(error) - 1);
    const long delay[] = {1, 0};
    for (;;)
        call3(101, (long)delay, 0, 0); /* nanosleep */
}

__asm__(".global _start\n_start:\nmov x29, xzr\nmov x30, xzr\nb entry\n");
