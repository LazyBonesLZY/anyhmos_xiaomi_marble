/* SPDX-License-Identifier: MIT
 * Brightness persistence for marble.
 *
 * The kernel panel driver comes up at a fixed level every boot and the OHOS
 * display power manager re-applies its own default, so the slider value the
 * user picked last session is lost.  This helper keeps the value on userdata
 * and re-applies it once the display stack is up.
 *
 *   brightness_persist apply   # sysfs <- saved value (no-op when unset)
 *   brightness_persist watch   # poll sysfs, persist every change
 *   brightness_persist get     # print the saved value
 *
 * The file lives under /data/service/el1/public/display (root-owned) so it
 * survives reboots but stays inside the normal per-user data tree.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SYSFS_BRIGHTNESS "/sys/class/backlight/panel0-backlight/brightness"
#define SYSFS_MAX "/sys/class/backlight/panel0-backlight/max_brightness"
#define STORE_DIR "/data/service/el1/public/display"
#define STORE_FILE STORE_DIR "/brightness_last"
#define POLL_SECONDS 2

static volatile sig_atomic_t stopping;

static void on_signal(int sig)
{
    (void)sig;
    stopping = 1;
}

static int read_int_file(const char *path, long *out)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    char buf[32];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    char *end = NULL;
    long v = strtol(buf, &end, 10);
    if (end == buf) return -1;
    *out = v;
    return 0;
}

static long saved_value(long fallback)
{
    long v;
    if (read_int_file(STORE_FILE, &v) != 0) return fallback;
    return v;
}

static int write_int_file(const char *path, long value)
{
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%ld\n", value);
    if (n <= 0) return -1;
    int fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return -1;
    ssize_t w = write(fd, buf, (size_t)n);
    close(fd);
    return w == n ? 0 : -1;
}

static int ensure_store_dir(void)
{
    if (mkdir(STORE_DIR, 0755) == 0 || errno == EEXIST) return 0;
    return -1;
}

static int cmd_apply(void)
{
    long max = 0;
    if (read_int_file(SYSFS_MAX, &max) != 0 || max <= 0) {
        printf("BRIGHTNESS_PERSIST_APPLY=no-sysfs\n");
        return 0;
    }
    long want = saved_value(-1);
    if (want < 0) {
        printf("BRIGHTNESS_PERSIST_APPLY=no-saved max=%ld\n", max);
        return 0;
    }
    long current = 0;
    (void)read_int_file(SYSFS_BRIGHTNESS, &current);
    if (want > max) want = max;
    /* The panel driver clamps silently; verify the read-back like the rest of
     * this port does instead of trusting the write() return value. */
    if (write_int_file(SYSFS_BRIGHTNESS, want) != 0) {
        printf("BRIGHTNESS_PERSIST_APPLY=write-fail errno=%d\n", errno);
        return 1;
    }
    long observed = -1;
    (void)read_int_file(SYSFS_BRIGHTNESS, &observed);
    printf("BRIGHTNESS_PERSIST_APPLY=%s want=%ld observed=%ld previous=%ld max=%ld\n",
           observed == want ? "ok" : "mismatch", want, observed, current, max);
    return observed == want ? 0 : 1;
}

static int cmd_watch(void)
{
    if (ensure_store_dir() != 0) {
        printf("BRIGHTNESS_PERSIST_WATCH=fail mkdir errno=%d\n", errno);
        return 1;
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    (void)sigaction(SIGTERM, &sa, NULL);
    (void)sigaction(SIGINT, &sa, NULL);
    long last = -1;
    (void)read_int_file(SYSFS_BRIGHTNESS, &last);
    printf("BRIGHTNESS_PERSIST_WATCH=ready start=%ld\n", last);
    int applied = 0;
    while (!stopping) {
        long now = -1;
        if (read_int_file(SYSFS_BRIGHTNESS, &now) != 0) {
            sleep(POLL_SECONDS);
            continue;
        }
        if (!applied && now != last) {
            /* First change after boot usually comes from the display power
             * manager re-applying its default; do not persist that value over
             * the user's choice, just adopt it as the baseline. */
            applied = 1;
            last = now;
            printf("BRIGHTNESS_PERSIST_WATCH=baseline value=%ld\n", now);
            continue;
        }
        if (now != last) {
            if (write_int_file(STORE_FILE, now) == 0) {
                printf("BRIGHTNESS_PERSIST_SAVED value=%ld\n", now);
            } else {
                printf("BRIGHTNESS_PERSIST_SAVE_FAIL errno=%d\n", errno);
            }
            last = now;
            applied = 1;
        }
        sleep(POLL_SECONDS);
    }
    printf("BRIGHTNESS_PERSIST_WATCH=stop\n");
    return 0;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "get";
    if (!strcmp(mode, "apply")) return cmd_apply();
    if (!strcmp(mode, "watch")) return cmd_watch();
    if (!strcmp(mode, "get")) {
        long v = saved_value(-1);
        if (v < 0) {
            printf("BRIGHTNESS_PERSIST_STORED=none\n");
            return 0;
        }
        printf("BRIGHTNESS_PERSIST_STORED=%ld\n", v);
        return 0;
    }
    printf("usage: %s apply|watch|get\n", argv[0]);
    return 2;
}
