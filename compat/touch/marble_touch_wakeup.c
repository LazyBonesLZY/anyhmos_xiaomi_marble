// SPDX-License-Identifier: MIT
/*
 * Clean-room Double-Tap-to-Wake (DT2W) + Single-Tap-for-AOD uinput daemon
 * for Xiaomi marble (SM7475, Goodix 9916R + xiaomi_touch oneshot sensors).
 *
 * Watches POLLPRI sysfs notifications on:
 *   - /sys/class/touch/touch_dev/gesture_double_tap_state  -> KEY_WAKEUP (143)
 *   - /sys/class/touch/touch_dev/gesture_single_tap_state  -> evdev 765 (AOD single click)
 * and injects key events through /dev/uinput ("marble-touch-wakeup"), auto-creating
 * /dev/uinput (char 10:223) when ENOENT.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define MARBLE_TOUCH_DIR_DEFAULT "/sys/class/touch/touch_dev"
#define MARBLE_UINPUT_PATH_DEFAULT "/dev/uinput"
#define MARBLE_UINPUT_FALLBACK_PATH "/dev/misc/uinput"
#define MARBLE_UINPUT_MAJOR 10u
#define MARBLE_UINPUT_MINOR 223u
#define MARBLE_UINPUT_DEV_NAME "marble-touch-wakeup"
#define MARBLE_SYS_INPUT_DIR_DEFAULT "/sys/class/input"
#define MARBLE_DEV_INPUT_DIR_DEFAULT "/dev/input"
#define MARBLE_INPUT_UID 6696u
#define MARBLE_INPUT_GID 6696u
#define MARBLE_EVENT_NODE_RETRIES 10
#define MARBLE_EVENT_NODE_RETRY_MS 50
#define MARBLE_KEYCODE_AOD_SINGLE_CLICK 765u
#define MARBLE_DEBOUNCE_MS_DEFAULT 250u
#define MARBLE_RECHECK_INTERVAL_MS 10000

#ifndef MARBLE_TOUCH_WAKEUP_HOST_TEST
static volatile sig_atomic_t g_stop = 0;

static void handle_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}
#endif

static uint64_t monotonic_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int parse_sysfs_triggered(const char *buf, ssize_t len)
{
    if (!buf || len <= 0) {
        return 0;
    }
    ssize_t i = 0;
    while (i < len && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\r' || buf[i] == '\n')) {
        ++i;
    }
    if (i >= len || buf[i] != '1') {
        return 0;
    }
    ++i;
    if (i < len && buf[i] != '\0' && buf[i] != '\n' && buf[i] != '\r' &&
        buf[i] != ' ' && buf[i] != '\t') {
        return 0;
    }
    return 1;
}

static bool should_accept_gesture(uint64_t now_ns, uint64_t *last_emit_ns, uint64_t debounce_ns)
{
    if (!last_emit_ns) {
        return true;
    }
    if (*last_emit_ns == 0 || now_ns < *last_emit_ns || (now_ns - *last_emit_ns) >= debounce_ns) {
        *last_emit_ns = now_ns;
        return true;
    }
    return false;
}

static int parse_drm_dpms_is_on(const char *buf, ssize_t len)
{
    if (!buf || len <= 0) {
        return -1;
    }
    ssize_t i = 0;
    while (i < len && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\r' || buf[i] == '\n')) {
        ++i;
    }
    ssize_t rem = len - i;
    if (rem >= 2 && (buf[i] == 'O' || buf[i] == 'o') && (buf[i + 1] == 'n' || buf[i + 1] == 'N')) {
        if (rem == 2 || buf[i + 2] == '\n' || buf[i + 2] == '\r' ||
            buf[i + 2] == ' ' || buf[i + 2] == '\t' || buf[i + 2] == '\0') {
            return 1;
        }
    }
    if (rem >= 3 && (buf[i] == 'O' || buf[i] == 'o') &&
        (buf[i + 1] == 'f' || buf[i + 1] == 'F') &&
        (buf[i + 2] == 'f' || buf[i + 2] == 'F')) {
        return 0;
    }
    if (rem >= 7 && (strncmp(buf + i, "Standby", 7) == 0 || strncmp(buf + i, "Suspend", 7) == 0)) {
        return 0;
    }
    return -1;
}

static int parse_drm_enabled_is_on(const char *buf, ssize_t len)
{
    if (!buf || len <= 0) {
        return -1;
    }
    ssize_t i = 0;
    while (i < len && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\r' || buf[i] == '\n')) {
        ++i;
    }
    ssize_t rem = len - i;
    if (rem >= 7 && strncmp(buf + i, "enabled", 7) == 0) {
        return 1;
    }
    if (rem >= 8 && strncmp(buf + i, "disabled", 8) == 0) {
        return 0;
    }
    return -1;
}

static ssize_t read_small_file(const char *path, char *buf, size_t cap)
{
    if (!path || !buf || cap < 2) {
        return -1;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    ssize_t n;
    do {
        n = read(fd, buf, cap - 1);
    } while (n < 0 && errno == EINTR);
    int saved = errno;
    close(fd);
    errno = saved;
    if (n >= 0) {
        buf[n] = '\0';
    }
    return n;
}

static int write_small_file(const char *path, const char *text)
{
    if (!path || !text) {
        errno = EINVAL;
        return -1;
    }
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    size_t len = strlen(text);
    ssize_t n;
    do {
        n = write(fd, text, len);
    } while (n < 0 && errno == EINTR);
    int saved = errno;
    close(fd);
    errno = saved;
    return (n == (ssize_t)len) ? 0 : -1;
}

static int ensure_gesture_enabled(const char *touch_dir, const char *node_name, bool force_write)
{
    char path[256];
    int plen = snprintf(path, sizeof(path), "%s/%s", touch_dir, node_name);
    if (plen <= 0 || (size_t)plen >= sizeof(path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (!force_write) {
        char cur[32];
        ssize_t n = read_small_file(path, cur, sizeof(cur));
        if (n > 0 && parse_sysfs_triggered(cur, n)) {
            return 0;
        }
    }
    return write_small_file(path, "1\n");
}

#ifndef MARBLE_TOUCH_WAKEUP_HOST_TEST
static int query_display_dpms_on(void)
{
    static const char *const kDpmsPaths[] = {
        "/sys/class/drm/card0-DSI-1/dpms",
        "/sys/class/drm/card0-DSI-0/dpms",
    };
    static const char *const kEnabledPaths[] = {
        "/sys/class/drm/card0-DSI-1/enabled",
        "/sys/class/drm/card0-DSI-0/enabled",
    };
    char buf[64];
    for (size_t i = 0; i < sizeof(kDpmsPaths) / sizeof(kDpmsPaths[0]); ++i) {
        ssize_t n = read_small_file(kDpmsPaths[i], buf, sizeof(buf));
        if (n > 0) {
            int state = parse_drm_dpms_is_on(buf, n);
            if (state >= 0) {
                return state;
            }
        }
    }
    for (size_t i = 0; i < sizeof(kEnabledPaths) / sizeof(kEnabledPaths[0]); ++i) {
        ssize_t n = read_small_file(kEnabledPaths[i], buf, sizeof(buf));
        if (n > 0) {
            int state = parse_drm_enabled_is_on(buf, n);
            if (state >= 0) {
                return state;
            }
        }
    }
    return -1;
}
#endif

static int open_and_clear_state_node(const char *touch_dir, const char *node_name)
{
    char path[256];
    int plen = snprintf(path, sizeof(path), "%s/%s", touch_dir, node_name);
    if (plen <= 0 || (size_t)plen >= sizeof(path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    // Dummy pread from offset 0 clears the initial kernfs POLLPRI edge.
    char dummy[32];
    ssize_t n;
    do {
        n = pread(fd, dummy, sizeof(dummy) - 1, 0);
    } while (n < 0 && errno == EINTR);
    (void)n;
    return fd;
}

#ifndef MARBLE_TOUCH_WAKEUP_HOST_TEST
static int open_uinput_node(const char *uinput_path)
{
    int fd = open(uinput_path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd >= 0) {
        return fd;
    }
    if (errno == ENOENT) {
        if (mknod(uinput_path, S_IFCHR | 0600,
                  makedev(MARBLE_UINPUT_MAJOR, MARBLE_UINPUT_MINOR)) == 0 ||
            errno == EEXIST) {
            fprintf(stderr, "marble_touch_wakeup: ensured %s (char %u:%u)\n",
                    uinput_path, MARBLE_UINPUT_MAJOR, MARBLE_UINPUT_MINOR);
            fd = open(uinput_path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd >= 0) {
                return fd;
            }
        } else {
            fprintf(stderr, "marble_touch_wakeup: mknod(%s) failed errno=%d\n",
                    uinput_path, errno);
        }
    }
    if (strcmp(uinput_path, MARBLE_UINPUT_FALLBACK_PATH) != 0) {
        int alt = open(MARBLE_UINPUT_FALLBACK_PATH, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        if (alt >= 0) {
            return alt;
        }
    }
    return -1;
}

static int create_uinput_device(const char *uinput_path)
{
    int fd = open_uinput_node(uinput_path);
    if (fd < 0) {
        fprintf(stderr, "marble_touch_wakeup: open uinput(%s) failed errno=%d\n",
                uinput_path, errno);
        return -1;
    }
    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0 ||
        ioctl(fd, UI_SET_EVBIT, EV_SYN) < 0 ||
        ioctl(fd, UI_SET_KEYBIT, KEY_WAKEUP) < 0 ||
        ioctl(fd, UI_SET_KEYBIT, KEY_POWER) < 0 ||
        ioctl(fd, UI_SET_KEYBIT, MARBLE_KEYCODE_AOD_SINGLE_CLICK) < 0) {
        fprintf(stderr, "marble_touch_wakeup: UI_SET_*BIT failed errno=%d\n", errno);
        close(fd);
        return -1;
    }

    bool setup_ok = false;
#ifdef UI_DEV_SETUP
    struct uinput_setup usetup;
    memset(&usetup, 0, sizeof(usetup));
    usetup.id.bustype = BUS_VIRTUAL;
    usetup.id.vendor = 0x18d1;
    usetup.id.product = 0x7475;
    usetup.id.version = 1;
    snprintf(usetup.name, sizeof(usetup.name), "%s", MARBLE_UINPUT_DEV_NAME);
    if (ioctl(fd, UI_DEV_SETUP, &usetup) == 0) {
        setup_ok = true;
    }
#endif
    if (!setup_ok) {
        struct uinput_user_dev uudev;
        memset(&uudev, 0, sizeof(uudev));
        snprintf(uudev.name, sizeof(uudev.name), "%s", MARBLE_UINPUT_DEV_NAME);
        uudev.id.bustype = BUS_VIRTUAL;
        uudev.id.vendor = 0x18d1;
        uudev.id.product = 0x7475;
        uudev.id.version = 1;
        ssize_t written;
        do {
            written = write(fd, &uudev, sizeof(uudev));
        } while (written < 0 && errno == EINTR);
        if (written != (ssize_t)sizeof(uudev)) {
            fprintf(stderr, "marble_touch_wakeup: uinput_user_dev write failed errno=%d\n", errno);
            close(fd);
            return -1;
        }
    }

    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        fprintf(stderr, "marble_touch_wakeup: UI_DEV_CREATE failed errno=%d\n", errno);
        close(fd);
        return -1;
    }
    return fd;
}
#endif

static bool is_input_event_dir_name(const char *name)
{
    if (!name || strncmp(name, "event", 5) != 0 || name[5] == '\0') {
        return false;
    }
    for (const char *p = name + 5; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') {
            return false;
        }
    }
    return true;
}

static int parse_sysfs_dev_major_minor(const char *buf, ssize_t len,
                                       unsigned int *maj_out, unsigned int *min_out)
{
    if (!buf || len <= 0 || !maj_out || !min_out) {
        return -1;
    }
    ssize_t i = 0;
    while (i < len && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\r' || buf[i] == '\n')) {
        ++i;
    }
    if (i >= len || buf[i] < '0' || buf[i] > '9') {
        return -1;
    }
    unsigned long maj = 0;
    while (i < len && buf[i] >= '0' && buf[i] <= '9') {
        if (maj > (UINT_MAX - 9u) / 10u) {
            return -1;
        }
        maj = maj * 10u + (unsigned long)(buf[i] - '0');
        ++i;
    }
    if (i >= len || buf[i] != ':') {
        return -1;
    }
    ++i;
    if (i >= len || buf[i] < '0' || buf[i] > '9') {
        return -1;
    }
    unsigned long min = 0;
    while (i < len && buf[i] >= '0' && buf[i] <= '9') {
        if (min > (UINT_MAX - 9u) / 10u) {
            return -1;
        }
        min = min * 10u + (unsigned long)(buf[i] - '0');
        ++i;
    }
    while (i < len && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\r' || buf[i] == '\n' || buf[i] == '\0')) {
        ++i;
    }
    if (i != len) {
        return -1;
    }
    *maj_out = (unsigned int)maj;
    *min_out = (unsigned int)min;
    return 0;
}

static int find_input_event_in_sysfs(const char *sys_input_dir, const char *dev_name,
                                     char *event_name_out, size_t event_name_cap,
                                     unsigned int *maj_out, unsigned int *min_out)
{
    if (!sys_input_dir || !dev_name || !event_name_out || event_name_cap < 8 ||
        !maj_out || !min_out) {
        return -1;
    }
    DIR *dir = opendir(sys_input_dir);
    if (!dir) {
        return -1;
    }
    struct dirent *ent;
    int found = -1;
    while ((ent = readdir(dir)) != NULL) {
        if (!is_input_event_dir_name(ent->d_name)) {
            continue;
        }
        char path[512];
        int plen = snprintf(path, sizeof(path), "%s/%s/device/name", sys_input_dir, ent->d_name);
        if (plen <= 0 || (size_t)plen >= sizeof(path)) {
            continue;
        }
        char name_buf[128];
        ssize_t n = read_small_file(path, name_buf, sizeof(name_buf));
        if (n <= 0) {
            continue;
        }
        while (n > 0 && (name_buf[n - 1] == '\n' || name_buf[n - 1] == '\r')) {
            name_buf[--n] = '\0';
        }
        if (strcmp(name_buf, dev_name) != 0) {
            continue;
        }
        plen = snprintf(path, sizeof(path), "%s/%s/dev", sys_input_dir, ent->d_name);
        if (plen <= 0 || (size_t)plen >= sizeof(path)) {
            continue;
        }
        char dev_buf[64];
        ssize_t dn = read_small_file(path, dev_buf, sizeof(dev_buf));
        if (dn <= 0) {
            continue;
        }
        unsigned int maj = 0, min = 0;
        if (parse_sysfs_dev_major_minor(dev_buf, dn, &maj, &min) != 0) {
            continue;
        }
        int elen = snprintf(event_name_out, event_name_cap, "%s", ent->d_name);
        if (elen <= 0 || (size_t)elen >= event_name_cap) {
            continue;
        }
        *maj_out = maj;
        *min_out = min;
        found = 0;
        break;
    }
    closedir(dir);
    return found;
}

#ifndef MARBLE_TOUCH_WAKEUP_HOST_TEST
static int ensure_uinput_event_node(const char *dev_name)
{
    char event_name[64];
    unsigned int maj = 0;
    unsigned int min = 0;
    bool found = false;

    for (int attempt = 0; attempt < MARBLE_EVENT_NODE_RETRIES; ++attempt) {
        if (find_input_event_in_sysfs(MARBLE_SYS_INPUT_DIR_DEFAULT, dev_name,
                                      event_name, sizeof(event_name), &maj, &min) == 0) {
            found = true;
            break;
        }
        struct timespec req = {0, (long)MARBLE_EVENT_NODE_RETRY_MS * 1000000L};
        nanosleep(&req, NULL);
    }
    if (!found) {
        fprintf(stderr, "marble_touch_wakeup: sysfs event node for '%s' not found after %d retries\n",
                dev_name ? dev_name : "", MARBLE_EVENT_NODE_RETRIES);
        return -1;
    }

    if (mkdir(MARBLE_DEV_INPUT_DIR_DEFAULT, 0755) < 0 && errno != EEXIST) {
        fprintf(stderr, "marble_touch_wakeup: mkdir(%s) failed errno=%d\n",
                MARBLE_DEV_INPUT_DIR_DEFAULT, errno);
        return -1;
    }

    char node_path[128];
    int nlen = snprintf(node_path, sizeof(node_path), "%s/%s",
                        MARBLE_DEV_INPUT_DIR_DEFAULT, event_name);
    if (nlen <= 0 || (size_t)nlen >= sizeof(node_path)) {
        return -1;
    }

    if (mknod(node_path, S_IFCHR | 0660, makedev(maj, min)) < 0 && errno != EEXIST) {
        fprintf(stderr, "marble_touch_wakeup: mknod(%s, %u:%u) failed errno=%d\n",
                node_path, maj, min, errno);
        return -1;
    }
    if (chown(node_path, (uid_t)MARBLE_INPUT_UID, (gid_t)MARBLE_INPUT_GID) < 0) {
        fprintf(stderr, "marble_touch_wakeup: chown(%s, %u:%u) failed errno=%d\n",
                node_path, MARBLE_INPUT_UID, MARBLE_INPUT_GID, errno);
    }
    if (chmod(node_path, 0660) < 0) {
        fprintf(stderr, "marble_touch_wakeup: chmod(%s, 0660) failed errno=%d\n",
                node_path, errno);
    }
    fprintf(stderr, "marble_touch_wakeup: ensured %s (dev=%s char %u:%u owner=%u:%u mode=0660)\n",
            node_path, dev_name, maj, min, MARBLE_INPUT_UID, MARBLE_INPUT_GID);
    return 0;
}
#endif

static int write_input_event(int uinput_fd, uint16_t type, uint16_t code, int32_t value)
{
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.code = code;
    ev.value = value;
    ssize_t n;
    do {
        n = write(uinput_fd, &ev, sizeof(ev));
    } while (n < 0 && errno == EINTR);
    return (n == (ssize_t)sizeof(ev)) ? 0 : -1;
}

static int emit_key_tap(int uinput_fd, uint16_t keycode)
{
    if (write_input_event(uinput_fd, EV_KEY, keycode, 1) != 0) {
        return -1;
    }
    if (write_input_event(uinput_fd, EV_SYN, SYN_REPORT, 0) != 0) {
        return -1;
    }
    if (write_input_event(uinput_fd, EV_KEY, keycode, 0) != 0) {
        return -1;
    }
    if (write_input_event(uinput_fd, EV_SYN, SYN_REPORT, 0) != 0) {
        return -1;
    }
    return 0;
}

static int run_self_test(void)
{
    if (parse_sysfs_triggered("1", 1) != 1 ||
        parse_sysfs_triggered("1\n", 2) != 1 ||
        parse_sysfs_triggered(" 1\r\n", 4) != 1 ||
        parse_sysfs_triggered("0\n", 2) != 0 ||
        parse_sysfs_triggered("", 0) != 0 ||
        parse_sysfs_triggered("10\n", 3) != 0 ||
        parse_sysfs_triggered("2\n", 2) != 0) {
        fprintf(stderr, "MARBLE_TOUCH_WAKEUP_SELF_TEST_FAIL parser\n");
        return 1;
    }
    uint64_t last = 0;
    const uint64_t debounce = 250000000ULL;
    if (!should_accept_gesture(1000000000ULL, &last, debounce) ||
        should_accept_gesture(1100000000ULL, &last, debounce) ||
        should_accept_gesture(1249999999ULL, &last, debounce) ||
        !should_accept_gesture(1250000000ULL, &last, debounce) ||
        should_accept_gesture(1400000000ULL, &last, debounce) ||
        !should_accept_gesture(1500000000ULL, &last, debounce)) {
        fprintf(stderr, "MARBLE_TOUCH_WAKEUP_SELF_TEST_FAIL debounce\n");
        return 1;
    }
    if (parse_drm_dpms_is_on("On\n", 3) != 1 ||
        parse_drm_dpms_is_on("Off\n", 4) != 0 ||
        parse_drm_dpms_is_on("Suspend\n", 8) != 0 ||
        parse_drm_enabled_is_on("enabled\n", 8) != 1 ||
        parse_drm_enabled_is_on("disabled\n", 9) != 0) {
        fprintf(stderr, "MARBLE_TOUCH_WAKEUP_SELF_TEST_FAIL drm\n");
        return 1;
    }
    if (MARBLE_KEYCODE_AOD_SINGLE_CLICK > KEY_MAX) {
        fprintf(stderr, "MARBLE_TOUCH_WAKEUP_SELF_TEST_FAIL key_max\n");
        return 1;
    }
    unsigned int maj = 0, min = 0;
    if (parse_sysfs_dev_major_minor("13:74\n", 6, &maj, &min) != 0 || maj != 13u || min != 74u ||
        parse_sysfs_dev_major_minor(" 13:64 \r\n", 9, &maj, &min) != 0 || maj != 13u || min != 64u ||
        parse_sysfs_dev_major_minor("", 0, &maj, &min) == 0 ||
        parse_sysfs_dev_major_minor("13\n", 3, &maj, &min) == 0 ||
        parse_sysfs_dev_major_minor("13:\n", 4, &maj, &min) == 0 ||
        parse_sysfs_dev_major_minor(":74\n", 4, &maj, &min) == 0 ||
        parse_sysfs_dev_major_minor("13:74:1\n", 8, &maj, &min) == 0 ||
        parse_sysfs_dev_major_minor("abc:1\n", 6, &maj, &min) == 0 ||
        !is_input_event_dir_name("event0") ||
        !is_input_event_dir_name("event10") ||
        is_input_event_dir_name("event") ||
        is_input_event_dir_name("event10a") ||
        is_input_event_dir_name("mice")) {
        fprintf(stderr, "MARBLE_TOUCH_WAKEUP_SELF_TEST_FAIL dev_parser\n");
        return 1;
    }
    printf("MARBLE_TOUCH_WAKEUP_SELF_TEST_OK key_wakeup=%u key_power=%u key_aod=%u key_max=%u\n",
           (unsigned)KEY_WAKEUP, (unsigned)KEY_POWER,
           (unsigned)MARBLE_KEYCODE_AOD_SINGLE_CLICK, (unsigned)KEY_MAX);
    return 0;
}

#ifdef MARBLE_TOUCH_WAKEUP_HOST_TEST
int main(void)
{
    if (run_self_test() != 0) {
        return 1;
    }
    // Verify independent debounce timers and monotonic wrap safety.
    uint64_t dt_last = 0;
    uint64_t st_last = 0;
    const uint64_t debounce_ns = (uint64_t)MARBLE_DEBOUNCE_MS_DEFAULT * 1000000ULL;
    if (!should_accept_gesture(500000000ULL, &dt_last, debounce_ns) ||
        !should_accept_gesture(510000000ULL, &st_last, debounce_ns) ||
        should_accept_gesture(600000000ULL, &dt_last, debounce_ns) ||
        should_accept_gesture(610000000ULL, &st_last, debounce_ns) ||
        !should_accept_gesture(100000000ULL, &dt_last, debounce_ns)) {
        fprintf(stderr, "MARBLE_TOUCH_WAKEUP_HOST_TEST_FAIL independent_debounce\n");
        return 1;
    }

    // Verify sysfs enable & periodic re-enable on a temporary directory.
    char tmpl[] = "/tmp/marble_touch_wakeup_test_XXXXXX";
    char *dir = mkdtemp(tmpl);
    if (!dir) {
        perror("mkdtemp");
        return 1;
    }
    char dt_en[256], st_en[256], dt_st[256];
    snprintf(dt_en, sizeof(dt_en), "%s/gesture_double_tap_enabled", dir);
    snprintf(st_en, sizeof(st_en), "%s/gesture_single_tap_enabled", dir);
    snprintf(dt_st, sizeof(dt_st), "%s/gesture_double_tap_state", dir);

    int fd = open(dt_en, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (fd < 0 || write(fd, "0\n", 2) != 2) {
        fprintf(stderr, "MARBLE_TOUCH_WAKEUP_HOST_TEST_FAIL init_dt_en\n");
        if (fd >= 0) close(fd);
        return 1;
    }
    close(fd);
    fd = open(st_en, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (fd < 0 || write(fd, "0\n", 2) != 2) {
        fprintf(stderr, "MARBLE_TOUCH_WAKEUP_HOST_TEST_FAIL init_st_en\n");
        if (fd >= 0) close(fd);
        return 1;
    }
    close(fd);
    fd = open(dt_st, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (fd < 0 || write(fd, "1\n", 2) != 2) {
        fprintf(stderr, "MARBLE_TOUCH_WAKEUP_HOST_TEST_FAIL init_dt_st\n");
        if (fd >= 0) close(fd);
        return 1;
    }
    close(fd);

    if (ensure_gesture_enabled(dir, "gesture_double_tap_enabled", true) != 0 ||
        ensure_gesture_enabled(dir, "gesture_single_tap_enabled", true) != 0) {
        fprintf(stderr, "MARBLE_TOUCH_WAKEUP_HOST_TEST_FAIL enable\n");
        return 1;
    }
    char buf[32];
    if (read_small_file(dt_en, buf, sizeof(buf)) <= 0 || !parse_sysfs_triggered(buf, (ssize_t)strlen(buf)) ||
        read_small_file(st_en, buf, sizeof(buf)) <= 0 || !parse_sysfs_triggered(buf, (ssize_t)strlen(buf))) {
        fprintf(stderr, "MARBLE_TOUCH_WAKEUP_HOST_TEST_FAIL verify_enabled\n");
        return 1;
    }
    // Simulate suspend/resume driver reset back to 0 and verify periodic re-enable restores 1.
    if (write_small_file(dt_en, "0\n") != 0 ||
        ensure_gesture_enabled(dir, "gesture_double_tap_enabled", false) != 0 ||
        read_small_file(dt_en, buf, sizeof(buf)) <= 0 ||
        !parse_sysfs_triggered(buf, (ssize_t)strlen(buf))) {
        fprintf(stderr, "MARBLE_TOUCH_WAKEUP_HOST_TEST_FAIL reenable_after_reset\n");
        return 1;
    }
    int st_fd = open_and_clear_state_node(dir, "gesture_double_tap_state");
    if (st_fd < 0) {
        fprintf(stderr, "MARBLE_TOUCH_WAKEUP_HOST_TEST_FAIL open_state\n");
        return 1;
    }
    ssize_t sn = pread(st_fd, buf, sizeof(buf) - 1, 0);
    close(st_fd);
    if (sn <= 0 || !parse_sysfs_triggered(buf, sn)) {
        fprintf(stderr, "MARBLE_TOUCH_WAKEUP_HOST_TEST_FAIL pread_state\n");
        return 1;
    }

    unlink(dt_en);
    unlink(st_en);
    unlink(dt_st);
    rmdir(dir);

    // Verify monotonic clock and exact 4-event uinput key tap sequence via pipe.
    if (monotonic_ns() == 0) {
        fprintf(stderr, "MARBLE_TOUCH_WAKEUP_HOST_TEST_FAIL monotonic_ns\n");
        return 1;
    }
    int pfd[2];
    if (pipe(pfd) != 0) {
        perror("pipe");
        return 1;
    }
    if (emit_key_tap(pfd[1], KEY_WAKEUP) != 0 ||
        emit_key_tap(pfd[1], MARBLE_KEYCODE_AOD_SINGLE_CLICK) != 0) {
        fprintf(stderr, "MARBLE_TOUCH_WAKEUP_HOST_TEST_FAIL emit_key_tap\n");
        close(pfd[0]); close(pfd[1]);
        return 1;
    }
    struct input_event evs[8];
    ssize_t rn = read(pfd[0], evs, sizeof(evs));
    close(pfd[0]);
    close(pfd[1]);
    if (rn != (ssize_t)sizeof(evs) ||
        evs[0].type != EV_KEY || evs[0].code != KEY_WAKEUP || evs[0].value != 1 ||
        evs[1].type != EV_SYN || evs[1].code != SYN_REPORT || evs[1].value != 0 ||
        evs[2].type != EV_KEY || evs[2].code != KEY_WAKEUP || evs[2].value != 0 ||
        evs[3].type != EV_SYN || evs[3].code != SYN_REPORT || evs[3].value != 0 ||
        evs[4].type != EV_KEY || evs[4].code != MARBLE_KEYCODE_AOD_SINGLE_CLICK || evs[4].value != 1 ||
        evs[5].type != EV_SYN || evs[5].code != SYN_REPORT || evs[5].value != 0 ||
        evs[6].type != EV_KEY || evs[6].code != MARBLE_KEYCODE_AOD_SINGLE_CLICK || evs[6].value != 0 ||
        evs[7].type != EV_SYN || evs[7].code != SYN_REPORT || evs[7].value != 0) {
        fprintf(stderr, "MARBLE_TOUCH_WAKEUP_HOST_TEST_FAIL evdev_stream\n");
        return 1;
    }

    // Verify sysfs input event discovery for uinput device auto-mknod.
    char sys_tmpl[] = "/tmp/marble_sys_input_test_XXXXXX";
    char *sys_dir = mkdtemp(sys_tmpl);
    if (!sys_dir) {
        perror("mkdtemp");
        return 1;
    }
    char p1[320], p2[320], p3[320], p4[320], p5[320], p6[320];
    snprintf(p1, sizeof(p1), "%s/event0", sys_dir);
    snprintf(p2, sizeof(p2), "%s/event0/device", sys_dir);
    snprintf(p3, sizeof(p3), "%s/event10", sys_dir);
    snprintf(p4, sizeof(p4), "%s/event10/device", sys_dir);
    if (mkdir(p1, 0700) != 0 || mkdir(p2, 0700) != 0 ||
        mkdir(p3, 0700) != 0 || mkdir(p4, 0700) != 0) {
        fprintf(stderr, "MARBLE_TOUCH_WAKEUP_HOST_TEST_FAIL sysfs_mkdir\n");
        return 1;
    }
    snprintf(p5, sizeof(p5), "%s/event0/device/name", sys_dir);
    snprintf(p6, sizeof(p6), "%s/event0/dev", sys_dir);
    fd = open(p5, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (fd < 0 || write(fd, "goodix_ts\n", 10) != 10) { if (fd >= 0) close(fd); return 1; }
    close(fd);
    fd = open(p6, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (fd < 0 || write(fd, "13:64\n", 6) != 6) { if (fd >= 0) close(fd); return 1; }
    close(fd);

    char p7[320], p8[320];
    snprintf(p7, sizeof(p7), "%s/event10/device/name", sys_dir);
    snprintf(p8, sizeof(p8), "%s/event10/dev", sys_dir);
    fd = open(p7, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (fd < 0 || write(fd, MARBLE_UINPUT_DEV_NAME "\n", strlen(MARBLE_UINPUT_DEV_NAME) + 1) !=
                  (ssize_t)(strlen(MARBLE_UINPUT_DEV_NAME) + 1)) {
        if (fd >= 0) close(fd);
        return 1;
    }
    close(fd);
    fd = open(p8, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (fd < 0 || write(fd, "13:74\n", 6) != 6) { if (fd >= 0) close(fd); return 1; }
    close(fd);

    char found_ev[64];
    unsigned int found_maj = 0, found_min = 0;
    if (find_input_event_in_sysfs(sys_dir, MARBLE_UINPUT_DEV_NAME,
                                  found_ev, sizeof(found_ev), &found_maj, &found_min) != 0 ||
        strcmp(found_ev, "event10") != 0 || found_maj != 13u || found_min != 74u) {
        fprintf(stderr, "MARBLE_TOUCH_WAKEUP_HOST_TEST_FAIL find_uinput_event\n");
        return 1;
    }
    if (find_input_event_in_sysfs(sys_dir, "nonexistent-device",
                                  found_ev, sizeof(found_ev), &found_maj, &found_min) == 0) {
        fprintf(stderr, "MARBLE_TOUCH_WAKEUP_HOST_TEST_FAIL find_nonexistent\n");
        return 1;
    }
    unlink(p5); unlink(p6); unlink(p7); unlink(p8);
    rmdir(p2); rmdir(p1); rmdir(p4); rmdir(p3); rmdir(sys_dir);

    puts("MARBLE_TOUCH_WAKEUP_HOST_TEST_OK parser debounce drm sysfs_enable reenable pread evdev_stream sysfs_uinput_event");
    return 0;
}
#else
static void print_usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [--power-key] [--only-when-off] [--once] [--self-test]\n"
            "          [--touch-dir PATH] [--uinput PATH] [--debounce-ms MS]\n",
            prog);
}

int main(int argc, char **argv)
{
    const char *touch_dir = MARBLE_TOUCH_DIR_DEFAULT;
    const char *uinput_path = MARBLE_UINPUT_PATH_DEFAULT;
    bool use_power_key = false;
    bool only_when_off = false;
    bool run_once = false;
    uint32_t debounce_ms = MARBLE_DEBOUNCE_MS_DEFAULT;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--power-key") == 0) {
            use_power_key = true;
        } else if (strcmp(argv[i], "--only-when-off") == 0) {
            only_when_off = true;
        } else if (strcmp(argv[i], "--once") == 0) {
            run_once = true;
        } else if (strcmp(argv[i], "--self-test") == 0) {
            return run_self_test();
        } else if (strcmp(argv[i], "--touch-dir") == 0 && i + 1 < argc) {
            touch_dir = argv[++i];
        } else if (strcmp(argv[i], "--uinput") == 0 && i + 1 < argc) {
            uinput_path = argv[++i];
        } else if (strcmp(argv[i], "--debounce-ms") == 0 && i + 1 < argc) {
            unsigned long v = strtoul(argv[++i], NULL, 10);
            if (v > 5000) v = 5000;
            debounce_ms = (uint32_t)v;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "marble_touch_wakeup: unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 2;
        }
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    int uinput_fd = create_uinput_device(uinput_path);
    if (uinput_fd < 0) {
        return 1;
    }
    (void)ensure_uinput_event_node(MARBLE_UINPUT_DEV_NAME);

    int dt_en_rc = ensure_gesture_enabled(touch_dir, "gesture_double_tap_enabled", true);
    int st_en_rc = ensure_gesture_enabled(touch_dir, "gesture_single_tap_enabled", true);
    int dt_fd = open_and_clear_state_node(touch_dir, "gesture_double_tap_state");
    int st_fd = open_and_clear_state_node(touch_dir, "gesture_single_tap_state");

    fprintf(stderr,
            "marble_touch_wakeup: started dir=%s uinput=%s dt_en=%d st_en=%d dt_fd=%d st_fd=%d dt_key=%u only_when_off=%d\n",
            touch_dir, uinput_path, dt_en_rc, st_en_rc, dt_fd, st_fd,
            use_power_key ? (unsigned)KEY_POWER : (unsigned)KEY_WAKEUP,
            only_when_off ? 1 : 0);

    const uint64_t debounce_ns = (uint64_t)debounce_ms * 1000000ULL;
    uint64_t last_dt_ns = 0;
    uint64_t last_st_ns = 0;
    uint64_t last_recheck_ns = monotonic_ns();
    const uint16_t dt_keycode = use_power_key ? KEY_POWER : KEY_WAKEUP;

    while (!g_stop) {
        struct pollfd pfds[2];
        int pfd_kind[2];
        nfds_t nfds = 0;

        if (dt_fd >= 0) {
            pfds[nfds].fd = dt_fd;
            pfds[nfds].events = POLLPRI | POLLERR;
            pfds[nfds].revents = 0;
            pfd_kind[nfds] = 0;
            ++nfds;
        }
        if (st_fd >= 0) {
            pfds[nfds].fd = st_fd;
            pfds[nfds].events = POLLPRI | POLLERR;
            pfds[nfds].revents = 0;
            pfd_kind[nfds] = 1;
            ++nfds;
        }

        if (nfds == 0) {
            struct timespec req = {1, 0};
            nanosleep(&req, NULL);
        } else {
            int prc = poll(pfds, nfds, MARBLE_RECHECK_INTERVAL_MS);
            if (prc < 0) {
                if (errno == EINTR) {
                    continue;
                }
                fprintf(stderr, "marble_touch_wakeup: poll failed errno=%d\n", errno);
                struct timespec req = {0, 200000000L};
                nanosleep(&req, NULL);
            } else if (prc > 0) {
                for (nfds_t i = 0; i < nfds; ++i) {
                    if ((pfds[i].revents & (POLLPRI | POLLERR)) == 0) {
                        continue;
                    }
                    char buf[32];
                    ssize_t n;
                    do {
                        n = pread(pfds[i].fd, buf, sizeof(buf) - 1, 0);
                    } while (n < 0 && errno == EINTR);
                    if (n <= 0 || !parse_sysfs_triggered(buf, n)) {
                        continue;
                    }
                    uint64_t now_ns = monotonic_ns();
                    int dpms_on = query_display_dpms_on();
                    if (pfd_kind[i] == 0) {
                        if (only_when_off && dpms_on == 1) {
                            fprintf(stderr, "marble_touch_wakeup: double_tap skipped (dpms=On)\n");
                            continue;
                        }
                        if (!should_accept_gesture(now_ns, &last_dt_ns, debounce_ns)) {
                            continue;
                        }
                        int erc = emit_key_tap(uinput_fd, dt_keycode);
                        fprintf(stderr, "marble_touch_wakeup: double_tap key=%u dpms_on=%d rc=%d\n",
                                (unsigned)dt_keycode, dpms_on, erc);
                        if (run_once) {
                            g_stop = 1;
                            break;
                        }
                    } else {
                        if (!should_accept_gesture(now_ns, &last_st_ns, debounce_ns)) {
                            continue;
                        }
                        int erc = emit_key_tap(uinput_fd, MARBLE_KEYCODE_AOD_SINGLE_CLICK);
                        fprintf(stderr, "marble_touch_wakeup: single_tap key=%u dpms_on=%d rc=%d\n",
                                (unsigned)MARBLE_KEYCODE_AOD_SINGLE_CLICK, dpms_on, erc);
                        if (run_once) {
                            g_stop = 1;
                            break;
                        }
                    }
                }
            }
        }

        uint64_t now_ns = monotonic_ns();
        if (!g_stop && (now_ns < last_recheck_ns ||
                        (now_ns - last_recheck_ns) >= (uint64_t)MARBLE_RECHECK_INTERVAL_MS * 1000000ULL)) {
            last_recheck_ns = now_ns;
            (void)ensure_gesture_enabled(touch_dir, "gesture_double_tap_enabled", false);
            (void)ensure_gesture_enabled(touch_dir, "gesture_single_tap_enabled", false);
            if (dt_fd < 0) {
                dt_fd = open_and_clear_state_node(touch_dir, "gesture_double_tap_state");
            }
            if (st_fd < 0) {
                st_fd = open_and_clear_state_node(touch_dir, "gesture_single_tap_state");
            }
        }
    }

    if (dt_fd >= 0) close(dt_fd);
    if (st_fd >= 0) close(st_fd);
    (void)ioctl(uinput_fd, UI_DEV_DESTROY);
    close(uinput_fd);
    return 0;
}
#endif
