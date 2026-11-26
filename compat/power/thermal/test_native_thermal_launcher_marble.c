/*
 * SPDX-License-Identifier: MIT
 *
 * Offline host unit tests for native_thermal_launcher_marble.c.
 */

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#define MARBLE_THERMAL_LAUNCHER_UNIT_TEST 1

#include "native_thermal_launcher_marble.c"

#include <assert.h>
#include <fcntl.h>
#include <sys/stat.h>

static void mkdir_p(const char *path)
{
    char buf[PATH_MAX];
    size_t len = strlen(path);
    assert(len < sizeof(buf));
    memcpy(buf, path, len + 1);
    for (size_t i = 1; i < len; i++) {
        if (buf[i] == '/') {
            buf[i] = '\0';
            (void)mkdir(buf, 0755);
            buf[i] = '/';
        }
    }
    (void)mkdir(buf, 0755);
}

static void touch_file(const char *path)
{
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0755);
    assert(fd >= 0);
    close(fd);
}

static void test_path_safety_and_join(void)
{
    char out[PATH_MAX];

    assert(path_is_safe_abs("/vendor/bin/mi_thermald"));
    assert(path_is_safe_abs("/bin/mi_thermald"));
    assert(!path_is_safe_abs(NULL));
    assert(!path_is_safe_abs("vendor/bin/mi_thermald"));
    assert(!path_is_safe_abs("/vendor/../bin/mi_thermald"));
    assert(!path_is_safe_abs("/.."));

    assert(join_chroot_path(out, sizeof(out), "/tmp/root/", "/vendor/bin/mi_thermald") == 0);
    assert(strcmp(out, "/tmp/root/vendor/bin/mi_thermald") == 0);

    assert(join_chroot_path(out, sizeof(out), "/", "/vendor/bin/mi_thermald") == 0);
    assert(strcmp(out, "/vendor/bin/mi_thermald") == 0);

    assert(join_chroot_path(out, 8, "/tmp/root", "/vendor/bin/mi_thermald") == -ENAMETOOLONG);
    assert(join_chroot_path(out, sizeof(out), "/tmp/root", "relative") == -EINVAL);
}

static void test_parse_args(void)
{
    struct thermal_launch_cfg cfg;

    char *argv_ok1[] = { "launcher", "/data/root" };
    assert(parse_args(2, argv_ok1, &cfg) == 0);
    assert(!cfg.check_only);
    assert(strcmp(cfg.root_dir, "/data/root") == 0);
    assert(cfg.override_bin == NULL);

    char *argv_ok2[] = { "launcher", "--check", "/data/root", "/vendor/bin/mi_thermald" };
    assert(parse_args(4, argv_ok2, &cfg) == 0);
    assert(cfg.check_only);
    assert(strcmp(cfg.root_dir, "/data/root") == 0);
    assert(strcmp(cfg.override_bin, "/vendor/bin/mi_thermald") == 0);

    char *argv_bad_missing[] = { "launcher" };
    assert(parse_args(1, argv_bad_missing, &cfg) == -EINVAL);

    char *argv_bad_extra[] = { "launcher", "/r", "/a", "/b" };
    assert(parse_args(4, argv_bad_extra, &cfg) == -EINVAL);

    char *argv_bad_traversal[] = { "launcher", "/r", "/vendor/../etc/passwd" };
    assert(parse_args(3, argv_bad_traversal, &cfg) == -EINVAL);
}

static void test_validate_chroot_tree(void)
{
    char tmpl[] = "/tmp/marble-thermal-test.XXXXXX";
    char *root = mkdtemp(tmpl);
    char p[PATH_MAX];
    const char *resolved = NULL;

    assert(root != NULL);

    /* Non-existent root fails with -ENOTDIR */
    assert(validate_chroot_tree("/tmp/nonexistent-marble-thermal-root-xyz", NULL, &resolved) == -ENOTDIR);

    /* Missing linker64 fails with -ENOENT */
    assert(validate_chroot_tree(root, NULL, &resolved) == -ENOENT);

    /* Add linker64, still missing binary */
    snprintf(p, sizeof(p), "%s/compat/audio-runtime/bin", root);
    mkdir_p(p);
    snprintf(p, sizeof(p), "%s/compat/audio-runtime/bin/linker64", root);
    touch_file(p);
    assert(validate_chroot_tree(root, NULL, &resolved) == -ENOENT);

    /* Fallback /bin/mi_thermald works when primary is absent */
    snprintf(p, sizeof(p), "%s/bin", root);
    mkdir_p(p);
    snprintf(p, sizeof(p), "%s/bin/mi_thermald", root);
    touch_file(p);
    assert(validate_chroot_tree(root, NULL, &resolved) == 0);
    assert(strcmp(resolved, MARBLE_THERMAL_BIN_FALLBACK) == 0);

    /* Primary /vendor/bin/mi_thermald wins when present */
    snprintf(p, sizeof(p), "%s/vendor/bin", root);
    mkdir_p(p);
    snprintf(p, sizeof(p), "%s/vendor/bin/mi_thermald", root);
    touch_file(p);
    assert(validate_chroot_tree(root, NULL, &resolved) == 0);
    assert(strcmp(resolved, MARBLE_THERMAL_BIN_PRIMARY) == 0);

    /* Explicit override bin works if present and fails if absent */
    assert(validate_chroot_tree(root, "/bin/mi_thermald", &resolved) == 0);
    assert(strcmp(resolved, "/bin/mi_thermald") == 0);
    assert(validate_chroot_tree(root, "/vendor/bin/missing", &resolved) == -ENOENT);

    /* Cleanup */
    snprintf(p, sizeof(p), "%s/vendor/bin/mi_thermald", root);
    unlink(p);
    snprintf(p, sizeof(p), "%s/vendor/bin", root);
    rmdir(p);
    snprintf(p, sizeof(p), "%s/vendor", root);
    rmdir(p);
    snprintf(p, sizeof(p), "%s/bin/mi_thermald", root);
    unlink(p);
    snprintf(p, sizeof(p), "%s/bin", root);
    rmdir(p);
    snprintf(p, sizeof(p), "%s/compat/audio-runtime/bin/linker64", root);
    unlink(p);
    snprintf(p, sizeof(p), "%s/compat/audio-runtime/bin", root);
    rmdir(p);
    snprintf(p, sizeof(p), "%s/compat/audio-runtime", root);
    rmdir(p);
    snprintf(p, sizeof(p), "%s/compat", root);
    rmdir(p);
    rmdir(root);
}

int main(void)
{
    test_path_safety_and_join();
    test_parse_args();
    test_validate_chroot_tree();
    printf("MARBLE_THERMAL_LAUNCHER_UNIT_TEST_PASS\n");
    return 0;
}
