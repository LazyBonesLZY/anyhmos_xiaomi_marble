/*
 * SPDX-License-Identifier: MIT
 *
 * Clean-room chroot launcher for Xiaomi marble (SM7475) stock mi_thermald.
 * Reuses the already-staged /compat/audio-runtime Bionic linker64 + core libs
 * while exposing marble's minimal vendor Bionic libs (/vendor/lib64: libcrypto.so,
 * libssl.so) and stock thermal profiles (/vendor/etc/thermal-*.conf) inside an
 * isolated chroot.
 */

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MARBLE_THERMAL_LINKER "/compat/audio-runtime/bin/linker64"
#define MARBLE_THERMAL_LD_PATH "/vendor/lib64:/compat/audio-runtime/lib64"
#define MARBLE_THERMAL_BIN_PRIMARY "/vendor/bin/mi_thermald"
#define MARBLE_THERMAL_BIN_FALLBACK "/bin/mi_thermald"

struct thermal_launch_cfg {
    bool check_only;
    const char *root_dir;
    const char *override_bin;
};

static bool path_is_safe_abs(const char *p)
{
    if (p == NULL || p[0] != '/') {
        return false;
    }
    const char *cur = p;
    while (*cur != '\0') {
        if (cur[0] == '/' && cur[1] == '.' && cur[2] == '.' &&
            (cur[3] == '/' || cur[3] == '\0')) {
            return false;
        }
        cur++;
    }
    return true;
}

static int join_chroot_path(char *out, size_t out_size, const char *root,
                            const char *rel_abs)
{
    size_t root_len;
    int n;

    if (out == NULL || out_size == 0 || root == NULL || !path_is_safe_abs(rel_abs)) {
        return -EINVAL;
    }
    root_len = strlen(root);
    if (root_len == 0) {
        return -EINVAL;
    }
    while (root_len > 1 && root[root_len - 1] == '/') {
        root_len--;
    }
    if (root_len == 1 && root[0] == '/') {
        n = snprintf(out, out_size, "%s", rel_abs);
    } else {
        n = snprintf(out, out_size, "%.*s%s", (int)root_len, root, rel_abs);
    }
    if (n < 0 || (size_t)n >= out_size) {
        return -ENAMETOOLONG;
    }
    return 0;
}

static int parse_args(int argc, char **argv, struct thermal_launch_cfg *cfg)
{
    int idx = 1;

    if (cfg == NULL) {
        return -EINVAL;
    }
    memset(cfg, 0, sizeof(*cfg));

    if (idx < argc && strcmp(argv[idx], "--check") == 0) {
        cfg->check_only = true;
        idx++;
    }
    if (argc - idx < 1 || argc - idx > 2) {
        return -EINVAL;
    }
    cfg->root_dir = argv[idx];
    if (cfg->root_dir == NULL || cfg->root_dir[0] == '\0') {
        return -EINVAL;
    }
    if (argc - idx == 2) {
        cfg->override_bin = argv[idx + 1];
        if (!path_is_safe_abs(cfg->override_bin)) {
            return -EINVAL;
        }
    }
    return 0;
}

static int resolve_target_bin(const char *root_dir, const char *override_bin,
                              const char **out_bin)
{
    char full[PATH_MAX];
    struct stat st;

    if (out_bin == NULL) {
        return -EINVAL;
    }
    if (override_bin != NULL) {
        if (join_chroot_path(full, sizeof(full), root_dir, override_bin) != 0) {
            return -EINVAL;
        }
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) {
            return -ENOENT;
        }
        *out_bin = override_bin;
        return 0;
    }

    if (join_chroot_path(full, sizeof(full), root_dir, MARBLE_THERMAL_BIN_PRIMARY) == 0 &&
        stat(full, &st) == 0 && S_ISREG(st.st_mode)) {
        *out_bin = MARBLE_THERMAL_BIN_PRIMARY;
        return 0;
    }
    if (join_chroot_path(full, sizeof(full), root_dir, MARBLE_THERMAL_BIN_FALLBACK) == 0 &&
        stat(full, &st) == 0 && S_ISREG(st.st_mode)) {
        *out_bin = MARBLE_THERMAL_BIN_FALLBACK;
        return 0;
    }
    return -ENOENT;
}

static int validate_chroot_tree(const char *root_dir, const char *override_bin,
                                const char **out_bin)
{
    struct stat st;
    char full[PATH_MAX];
    int rc;

    if (stat(root_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        return -ENOTDIR;
    }

    rc = join_chroot_path(full, sizeof(full), root_dir, MARBLE_THERMAL_LINKER);
    if (rc != 0) {
        return rc;
    }
    if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) {
        return -ENOENT;
    }

    return resolve_target_bin(root_dir, override_bin, out_bin);
}

#ifndef MARBLE_THERMAL_LAUNCHER_UNIT_TEST
int main(int argc, char **argv)
{
    struct thermal_launch_cfg cfg;
    const char *target_bin = NULL;
    int rc;

    if (parse_args(argc, argv, &cfg) != 0) {
        fprintf(stderr, "usage: native_thermal_launcher_marble [--check] ROOT [BIN]\n");
        return 2;
    }

    rc = validate_chroot_tree(cfg.root_dir, cfg.override_bin, &target_bin);
    if (rc != 0) {
        fprintf(stderr, "thermal preflight failed for root=%s: %s\n",
                cfg.root_dir, strerror(-rc));
        return 1;
    }

    if (cfg.check_only) {
        printf("THERMAL_LAUNCHER_CHECK_OK root=%s linker=%s bin=%s ld_path=%s\n",
               cfg.root_dir, MARBLE_THERMAL_LINKER, target_bin,
               MARBLE_THERMAL_LD_PATH);
        return 0;
    }

    if (chroot(cfg.root_dir) != 0 || chdir("/") != 0) {
        perror("thermal chroot");
        return 1;
    }
    if (setenv("LD_LIBRARY_PATH", MARBLE_THERMAL_LD_PATH, 1) != 0) {
        perror("thermal LD_LIBRARY_PATH");
        return 1;
    }

    execl(MARBLE_THERMAL_LINKER, "linker64", target_bin, (char *)NULL);
    perror("thermal exec");
    return 1;
}
#endif
