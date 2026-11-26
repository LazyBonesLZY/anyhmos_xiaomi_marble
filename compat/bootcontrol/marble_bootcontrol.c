/* SPDX-License-Identifier: MIT
 *
 * marble (Xiaomi SM7475 / POCO F5) boot_control probe, plus a strictly gated
 * mark_boot_successful() path.
 *
 * Why this file exists
 * --------------------
 * On marble the A/B slot bookkeeping lives in `misc` (/dev/block/sda11,
 * 2 MiB). ABL decrements `slot_info[active].tries_remaining` on every boot and
 * clears `successful_boot`. When the counter reaches zero with no successful
 * mark the slot becomes unbootable and the unit drops to fastboot, which the
 * user currently has to repair by hand with `fastboot set_active a`.
 *
 * The stock ROM marks success through vendor/lib64/libboot_control_qti.so,
 * which exports plain C entry points (see work/marble-bootcontrol-evidence.md).
 * That library is *not* present in the installed OHOS image (super was
 * replaced) and it needs a fstab with a `/misc` entry. This translation unit
 * therefore:
 *
 *   1. always implements a self-contained READ-ONLY probe that parses `misc`
 *      itself (no vendor blob, no write, no dependency beyond bionic libc),
 *      and optionally audits whether the vendor HAL and its symbols exist;
 *   2. implements the HAL-driven mark path ONLY when the build defines
 *      HMOS_ALLOW_MARK_BOOT_SUCCESS, and even then every write is preceded by
 *      hard gates: boot_id equality, bootconfig identity (hardware/sku/slot),
 *      and desktop-unlock evidence produced by this very boot.
 *
 * Without -DHMOS_ALLOW_MARK_BOOT_SUCCESS the binary is read-only by
 * construction: it never opens misc for writing and never calls a HAL entry
 * point. Nothing in this file fabricates a success state: the mark path prints
 * MARK_SUCCESS_SKIP with a reason and exits non-zero whenever a gate fails, and
 * it re-reads is_slot_marked_successful() before reporting success.
 *
 * Build (host, NDK):
 *   $NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android35-clang \
 *       -O2 -Wall -Wextra -fPIE -pie -o marble_bootcontrol \
 *       compat/bootcontrol/marble_bootcontrol.c
 * Run on device (bionic runtime staged in $D/runtime):
 *   LD_LIBRARY_PATH=$D/runtime/lib64 $D/runtime/bin/linker64 \
 *       $D/marble_bootcontrol probe
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define BOOTCONFIG_PATH "/proc/bootconfig"
#define BOOT_ID_PATH "/proc/sys/kernel/random/boot_id"
#define MISC_BY_NAME "/dev/block/by-name/misc"
#define MISC_BY_NUM "/dev/block/sda11"
#define HAL_SO_NAME "libboot_control_qti.so"
#define DEFAULT_HAL_DIR "/compat/bootcontrol-marble"
#define EVIDENCE_DIR_PRIMARY "/hmos-root/data/local/tmp"
#define EVIDENCE_DIR_FALLBACK "/data/local/tmp"

/* 64 KiB is far more than the 2048-byte bootloader_message block plus the
 * control block; reading a bounded prefix keeps the probe cheap. */
#define MISC_SCAN_BYTES (64 * 1024)
#define BOOTLOADER_CONTROL_MAGIC 0x42414342u /* "BCAB" */
#define BOOTLOADER_CONTROL_VERSION_MIN 2
#define BOOTLOADER_CONTROL_VERSION_MAX 3
#define BOOT_CTRL_MAX_SLOTS 4

/* Paths are overridable so the read-only parser can be exercised on a host or
 * against a captured image; the device defaults below are what the boot script
 * relies on. */
static char g_bootconfig[256] = BOOTCONFIG_PATH;
static char g_boot_id_file[256] = BOOT_ID_PATH;
static char g_misc_primary[256] = MISC_BY_NAME;
static char g_misc_fallback[256] = MISC_BY_NUM;
static char g_evidence_dirs[4][256];
static int g_evidence_dir_count;
#define EVIDENCE_PATH_MAX 1400

static void set_path(char *dst, size_t cap, const char *env, const char *fallback)
{
    const char *v = env ? getenv(env) : NULL;

    if (v && v[0])
        snprintf(dst, cap, "%s", v);
    else
        snprintf(dst, cap, "%s", fallback);
}

static void resolve_paths(void)
{
    const char *dirs = getenv("MARBLE_EVIDENCE_DIRS");
    const char *p;

    set_path(g_bootconfig, sizeof(g_bootconfig), "MARBLE_BOOTCONFIG", BOOTCONFIG_PATH);
    set_path(g_boot_id_file, sizeof(g_boot_id_file), "MARBLE_BOOT_ID_FILE", BOOT_ID_PATH);
    set_path(g_misc_primary, sizeof(g_misc_primary), "MARBLE_MISC_DEVICE", MISC_BY_NAME);
    set_path(g_misc_fallback, sizeof(g_misc_fallback), "MARBLE_MISC_DEVICE_FALLBACK",
             MISC_BY_NUM);

    g_evidence_dir_count = 0;
    if (dirs && dirs[0]) {
        while (*dirs && g_evidence_dir_count < (int)(sizeof(g_evidence_dirs) / 256)) {
            size_t len = strcspn(dirs, ":");
            if (len && len < 256) {
                memcpy(g_evidence_dirs[g_evidence_dir_count], dirs, len);
                g_evidence_dirs[g_evidence_dir_count][len] = '\0';
                g_evidence_dir_count++;
            }
            dirs += len;
            if (*dirs == ':') dirs++;
        }
    }
    if (!g_evidence_dir_count) {
        snprintf(g_evidence_dirs[0], 256, "%s", EVIDENCE_DIR_PRIMARY);
        snprintf(g_evidence_dirs[1], 256, "%s", EVIDENCE_DIR_FALLBACK);
        g_evidence_dir_count = 2;
    }
    (void)p;
}

/* ---- AOSP bootloader_control layout (packed, 32 bytes) ---------------- */
struct slot_metadata {
    uint8_t priority : 4;
    uint8_t tries_remaining : 3;
    uint8_t successful_boot : 1;
    uint8_t bootable : 1;
    uint8_t verity_corrupted : 1;
    uint8_t reserved : 6;
} __attribute__((packed));

struct bootloader_control {
    char slot_suffix[4];
    uint32_t magic;
    uint8_t version;
    uint8_t nb_slot : 3;
    uint8_t recovery_tries_remaining : 3;
    uint8_t control_reserved : 2;
    uint8_t reserved0[2];
    struct slot_metadata slot_info[BOOT_CTRL_MAX_SLOTS];
    uint8_t reserved1[8];
    uint32_t crc32_le;
} __attribute__((packed));

/* ---- bootconfig / identity ------------------------------------------- */
struct boot_identity {
    char hardware[64];
    char sku[64];
    char slot_suffix[8];
    char bootdevice[64];
};

static void copy_field(char *dst, size_t cap, const char *src)
{
    size_t n = strnlen(src, cap - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int take_quoted(const char *line, const char *prefix, char *dst, size_t cap)
{
    size_t plen = strlen(prefix);
    char value[128];
    int got;

    if (strncmp(line, prefix, plen)) return 0;
    got = sscanf(line + plen, "\"%127[^\"]\"", value);
    if (got != 1) return 0;
    copy_field(dst, cap, value);
    return 1;
}

static int parse_bootconfig_line(const char *line, struct boot_identity *id)
{
    int seen = 0;

    seen += take_quoted(line, "androidboot.hardware = ", id->hardware,
                        sizeof(id->hardware));
    seen += take_quoted(line, "androidboot.hardware.sku = ", id->sku,
                        sizeof(id->sku));
    seen += take_quoted(line, "androidboot.slot_suffix = ", id->slot_suffix,
                        sizeof(id->slot_suffix));
    seen += take_quoted(line, "androidboot.bootdevice = ", id->bootdevice,
                        sizeof(id->bootdevice));
    return seen;
}

static int read_boot_identity(struct boot_identity *id)
{
    FILE *f;
    char line[512];
    int seen = 0;

    memset(id, 0, sizeof(*id));
    f = fopen(g_bootconfig, "re");
    if (!f) {
        printf("BOOTCONFIG unavailable errno=%d\n", errno);
        return -1;
    }
    while (fgets(line, sizeof(line), f)) {
        if (parse_bootconfig_line(line, id)) seen++;
    }
    fclose(f);
    if (!id->hardware[0] || !id->sku[0] || !id->slot_suffix[0]) return -1;
    printf("BOOTCONFIG hardware=%s sku=%s slot=%s bootdevice=%s keys=%d\n",
           id->hardware, id->sku, id->slot_suffix,
           id->bootdevice[0] ? id->bootdevice : "?", seen);
    return 0;
}

static int read_boot_id(char *out, size_t cap)
{
    FILE *f = fopen(g_boot_id_file, "re");
    size_t n;

    if (!f) return -1;
    if (!fgets(out, (int)cap, f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    out[strcspn(out, "\r\n")] = '\0';
    n = strlen(out);
    if (n != 36) return -1;
    return 0;
}

/* ---- crc32 (zlib compatible) ----------------------------------------- */
static uint32_t crc32_table[256];
static int crc32_table_ready;

static void crc32_init(void)
{
    uint32_t i, j, c;

    if (crc32_table_ready) return;
    for (i = 0; i < 256; i++) {
        c = i;
        for (j = 0; j < 8; j++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc32_table[i] = c;
    }
    crc32_table_ready = 1;
}

static uint32_t crc32_le(uint32_t crc, const uint8_t *buf, size_t len)
{
    size_t i;

    crc32_init();
    crc ^= 0xFFFFFFFFu;
    for (i = 0; i < len; i++) crc = crc32_table[(crc ^ buf[i]) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

/* ---- read-only misc probe -------------------------------------------- */
struct misc_report {
    char device[128];
    int have_device;
    size_t found_offset;
    int have_control;
    struct bootloader_control control;
    uint32_t crc_expected;
    uint32_t crc_computed;
    int crc_valid;
    int current_slot;
    int retry_low;
    unsigned retry_low_tries;
};

/* Locate the bootloader_control block. ABL stores it after the 2048-byte
 * bootloader_message, but the exact offset has differed across Qualcomm
 * baselines, so the magic is located instead of assumed. */
static int locate_control(const uint8_t *buf, size_t len, size_t *offset,
                          struct bootloader_control *out)
{
    size_t i;

    for (i = 0; i + sizeof(struct bootloader_control) <= len; i += 4) {
        struct bootloader_control ctl;
        memcpy(&ctl, buf + i, sizeof(ctl));
        if (ctl.magic != BOOTLOADER_CONTROL_MAGIC) continue;
        if (ctl.version < BOOTLOADER_CONTROL_VERSION_MIN ||
            ctl.version > BOOTLOADER_CONTROL_VERSION_MAX) continue;
        if (ctl.nb_slot < 1 || ctl.nb_slot > BOOT_CTRL_MAX_SLOTS) continue;
        memcpy(out, &ctl, sizeof(ctl));
        *offset = i;
        return 0;
    }
    return -1;
}

static int slot_index_from_suffix(const char *suffix)
{
    if (!suffix[0]) return -1;
    if (suffix[0] != '_') return -1;
    if (suffix[1] < 'a' || suffix[1] > 'd') return -1;
    return suffix[1] - 'a';
}

static int probe_misc(struct misc_report *rep, const struct boot_identity *id)
{
    static uint8_t buf[MISC_SCAN_BYTES];
    const char *candidates[] = { g_misc_primary, g_misc_fallback };
    size_t i;
    ssize_t got;
    int fd = -1;

    memset(rep, 0, sizeof(*rep));
    rep->current_slot = slot_index_from_suffix(id->slot_suffix);

    for (i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        /* /dev/block/by-name/misc is a symlink to /dev/block/sda11, so the
         * open must follow it; the descriptor is read-only regardless. */
        fd = open(candidates[i], O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            copy_field(rep->device, sizeof(rep->device), candidates[i]);
            rep->have_device = 1;
            break;
        }
    }
    if (fd < 0) {
        printf("MISC_DEVICE_UNAVAILABLE tried=%s,%s errno=%d\n",
               g_misc_primary, g_misc_fallback, errno);
        return -1;
    }
    printf("MISC_DEVICE path=%s read_only=1\n", rep->device);

    got = 0;
    while ((size_t)got < sizeof(buf)) {
        ssize_t n = read(fd, buf + got, sizeof(buf) - (size_t)got);
        if (n <= 0) break;
        got += n;
    }
    close(fd);
    if (got <= 0) {
        printf("MISC_READ_FAILED bytes=%ld errno=%d\n", (long)got, errno);
        return -1;
    }
    printf("MISC_READ bytes=%ld\n", (long)got);

    if (locate_control(buf, (size_t)got, &rep->found_offset, &rep->control)) {
        printf("MISC_CONTROL_NOT_FOUND scanned=%ld magic=0x%08x\n", (long)got,
               BOOTLOADER_CONTROL_MAGIC);
        return -1;
    }
    rep->have_control = 1;
    rep->crc_expected = rep->control.crc32_le;
    rep->crc_computed = crc32_le(0, (const uint8_t *)&rep->control,
                                offsetof(struct bootloader_control, crc32_le));
    rep->crc_valid = (rep->crc_expected == rep->crc_computed);

    printf("MISC_CONTROL offset=%zu version=%u nb_slot=%u slot_suffix=%.4s "
           "recovery_tries=%u crc_expected=0x%08x crc_computed=0x%08x crc_valid=%d\n",
           rep->found_offset, (unsigned)rep->control.version,
           (unsigned)rep->control.nb_slot, rep->control.slot_suffix,
           (unsigned)rep->control.recovery_tries_remaining, rep->crc_expected,
           rep->crc_computed, rep->crc_valid);

    for (i = 0; i < (size_t)rep->control.nb_slot; i++) {
        const struct slot_metadata *m = &rep->control.slot_info[i];
        printf("MISC_SLOT index=%zu suffix=_%c priority=%u tries_remaining=%u "
               "successful=%u bootable=%u verity_corrupted=%u\n",
               i, (char)('a' + i), (unsigned)m->priority,
               (unsigned)m->tries_remaining, (unsigned)m->successful_boot,
               (unsigned)m->bootable, (unsigned)m->verity_corrupted);
    }
    printf("MISC_CURRENT_SLOT index=%d\n", rep->current_slot);

    if (rep->current_slot >= 0 && rep->current_slot < (int)rep->control.nb_slot) {
        const struct slot_metadata *cur = &rep->control.slot_info[rep->current_slot];
        rep->retry_low = cur->tries_remaining <= 1 || cur->successful_boot == 0;
        rep->retry_low_tries = cur->tries_remaining;
        printf("MISC_ACTIVE_SLOT_STATE tries_remaining=%u successful=%u bootable=%u\n",
               (unsigned)cur->tries_remaining, (unsigned)cur->successful_boot,
               (unsigned)cur->bootable);
        if (rep->retry_low)
            printf("MISC_RETRY_RISK active=%d tries_remaining=%u successful=%u "
                   "advice=%s\n",
                   rep->current_slot, (unsigned)cur->tries_remaining,
                   (unsigned)cur->successful_boot,
                   "manual_set_active_a_when_dropped_to_fastboot");
    }
    if (!rep->crc_valid)
        printf("MISC_CRC_MISMATCH action=do_not_trust_parsed_fields\n");
    return 0;
}

/* ---- vendor HAL audit (no entry point is called) --------------------- */
struct hal_info {
    void *handle;
    char path[192];
    int mark_symbol;
    int init_symbol;
    int current_slot_symbol;
    int marked_symbol;
    int bootable_symbol;
};

static void *resolve_any(void *handle, const char *const *names, int *found)
{
    size_t i;
    void *p;

    for (i = 0; names[i]; i++) {
        p = dlsym(handle, names[i]);
        if (p) {
            *found = 1;
            return p;
        }
    }
    *found = 0;
    return NULL;
}

static int audit_hal(struct hal_info *hal, const char *dir)
{
    static const char *const mark_names[] = { "_Z20mark_boot_successfulv",
                                              "mark_boot_successful", NULL };
    static const char *const init_names[] = { "_Z16bootcontrol_initv",
                                              "bootcontrol_init", NULL };
    static const char *const cur_names[] = { "_Z16get_current_slotv",
                                             "get_current_slot", NULL };
    static const char *const marked_names[] = { "_Z25is_slot_marked_successfulj",
                                                "is_slot_marked_successful", NULL };
    static const char *const bootable_names[] = { "_Z16is_slot_bootablej",
                                                  "is_slot_bootable", NULL };

    memset(hal, 0, sizeof(*hal));
    snprintf(hal->path, sizeof(hal->path), "%s/%s", dir, HAL_SO_NAME);
    if (access(hal->path, R_OK)) {
        printf("HAL_PRESENT=0 reason=so_missing path=%s\n", hal->path);
        return -1;
    }
    hal->handle = dlopen(hal->path, RTLD_NOW | RTLD_LOCAL);
    if (!hal->handle) {
        const char *err = dlerror();
        printf("HAL_PRESENT=0 reason=dlopen_failed path=%s error=%s\n", hal->path,
               err ? err : "?");
        return -1;
    }
    resolve_any(hal->handle, mark_names, &hal->mark_symbol);
    resolve_any(hal->handle, init_names, &hal->init_symbol);
    resolve_any(hal->handle, cur_names, &hal->current_slot_symbol);
    resolve_any(hal->handle, marked_names, &hal->marked_symbol);
    resolve_any(hal->handle, bootable_names, &hal->bootable_symbol);
    printf("HAL_PRESENT=1 path=%s entry_points_called=0\n", hal->path);
    printf("HAL_SYMBOL mark_boot_successful=%d bootcontrol_init=%d "
           "get_current_slot=%d is_slot_marked_successful=%d is_slot_bootable=%d\n",
           hal->mark_symbol, hal->init_symbol, hal->current_slot_symbol,
           hal->marked_symbol, hal->bootable_symbol);
    if (!hal->mark_symbol || !hal->init_symbol)
        printf("HAL_INCOMPLETE reason=missing_required_symbol\n");
    return 0;
}

/* ---- desktop unlock evidence ----------------------------------------- */
struct evidence {
    int boot_id_seen;
    int unlock_seen;
    int snapshot_seen;
    int done_seen;
    unsigned long snapshot_bytes;
    char file[256];
};

static int line_has_screenlock_verified(const char *line)
{
    /* native_screenlock_request prints "SCREENLOCK_VERIFIED after=0 ...";
     * the orchestrator may wrap it as "UNLOCK_OUT=...". Require after=0 and a
     * plausible callback/elapsed tail so partially written lines are rejected. */
    const char *p = strstr(line, "SCREENLOCK_VERIFIED after=0 ");
    if (p) {
        const char *tail = p + strlen("SCREENLOCK_VERIFIED after=0 ");
        if (!strncmp(tail, "already_unlocked=1", 18)) return 1;
        if (!strncmp(tail, "callback=", 9)) {
            char *end = NULL;
            long v = strtol(tail + 9, &end, 10);
            (void)v;
            return end && end != tail + 9;
        }
        return 0;
    }
    return 0;
}

static int scan_evidence_file(const char *path, const char *boot_id,
                              struct evidence *ev)
{
    FILE *f = fopen(path, "re");
    char line[1024];
    int any = 0;

    if (!f) return 0;
    while (fgets(line, sizeof(line), f)) {
        char needle[96];

        snprintf(needle, sizeof(needle), "BEGIN boot_id=%s", boot_id);
        if (strstr(line, needle)) {
            ev->boot_id_seen = 1;
            any = 1;
        }
        if (line_has_screenlock_verified(line)) {
            ev->unlock_seen = 1;
            any = 1;
        }
        if (strstr(line, "SNAPSHOT_FILE_READY bytes=")) {
            const char *p = strstr(line, "SNAPSHOT_FILE_READY bytes=") +
                            sizeof("SNAPSHOT_FILE_READY bytes=") - 1;
            unsigned long n = strtoul(p, NULL, 10);
            if (n >= 65536) {
                ev->snapshot_seen = 1;
                ev->snapshot_bytes = n;
                any = 1;
            }
        }
        if (strstr(line, "PHASE4_ORCHESTRATOR_DONE")) {
            ev->done_seen = 1;
            any = 1;
        }
    }
    fclose(f);
    if (any) copy_field(ev->file, sizeof(ev->file), path);
    return any;
}

static int gather_evidence(const char *boot_id, struct evidence *ev)
{
    int d;

    memset(ev, 0, sizeof(*ev));
    for (d = 0; d < g_evidence_dir_count; d++) {
        const char *dir_name = g_evidence_dirs[d];
        DIR *dir = opendir(dir_name);
        struct dirent *ent;

        if (!dir) continue;
        while ((ent = readdir(dir))) {
            char path[EVIDENCE_PATH_MAX];
            if (!strstr(ent->d_name, "autostart")) continue;
            snprintf(path, sizeof(path), "%s/%s", dir_name, ent->d_name);
            scan_evidence_file(path, boot_id, ev);
        }
        closedir(dir);
    }
    printf("EVIDENCE boot_id=%d unlock=%d snapshot=%d done=%d snapshot_bytes=%lu "
           "file=%s\n",
           ev->boot_id_seen, ev->unlock_seen, ev->snapshot_seen, ev->done_seen,
           ev->snapshot_bytes, ev->file[0] ? ev->file : "none");
    return (ev->boot_id_seen && ev->unlock_seen && ev->snapshot_seen && ev->done_seen)
               ? 0
               : -1;
}

static int evidence_complete(const struct evidence *ev)
{
    return ev->boot_id_seen && ev->unlock_seen && ev->snapshot_seen && ev->done_seen;
}

/* ---- identity gates --------------------------------------------------- */
static int identity_ok(const struct boot_identity *id)
{
    if (strcmp(id->hardware, "qcom")) {
        printf("GATE hardware mismatch expected=qcom got=%s\n", id->hardware);
        return -1;
    }
    if (strcmp(id->sku, "marble")) {
        printf("GATE sku mismatch expected=marble got=%s\n", id->sku);
        return -1;
    }
    if (strcmp(id->slot_suffix, "_a")) {
        printf("GATE slot mismatch expected=_a got=%s\n", id->slot_suffix);
        return -1;
    }
    return 0;
}

/* ---- mark path (compiled only on explicit opt-in) -------------------- */
#ifdef HMOS_ALLOW_MARK_BOOT_SUCCESS
struct hal_api {
    void *handle;
    int (*init)(void);
    int (*mark)(void);
    unsigned (*current_slot)(void);
    int (*marked)(unsigned);
};

static int load_hal_api(struct hal_api *api, const char *dir)
{
    char path[192];

    memset(api, 0, sizeof(*api));
    snprintf(path, sizeof(path), "%s/%s", dir, HAL_SO_NAME);
    api->handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!api->handle) {
        const char *err = dlerror();
        printf("MARK_SUCCESS_SKIP reason=dlopen_failed path=%s error=%s\n", path,
               err ? err : "unknown");
        return -1;
    }
    *(void **)(&api->init) = dlsym(api->handle, "_Z16bootcontrol_initv");
    *(void **)(&api->mark) = dlsym(api->handle, "_Z20mark_boot_successfulv");
    *(void **)(&api->current_slot) = dlsym(api->handle, "_Z16get_current_slotv");
    *(void **)(&api->marked) = dlsym(api->handle, "_Z25is_slot_marked_successfulj");
    if (!api->init || !api->mark || !api->current_slot || !api->marked) {
        printf("MARK_SUCCESS_SKIP reason=hal_symbols_missing\n");
        return -1;
    }
    return 0;
}

static int run_mark(const char *requested_boot_id, const char *hal_dir)
{
    char boot_id[64];
    struct boot_identity id;
    struct evidence ev;
    struct misc_report rep;
    struct hal_api api;
    unsigned slot;
    int rc;

    if (!requested_boot_id || strlen(requested_boot_id) != 36) {
        printf("MARK_SUCCESS_SKIP reason=bad_boot_id_argument\n");
        return 40;
    }
    if (read_boot_id(boot_id, sizeof(boot_id))) {
        printf("MARK_SUCCESS_SKIP reason=boot_id_unreadable\n");
        return 41;
    }
    printf("BOOT_ID=%s\n", boot_id);
    if (strcmp(requested_boot_id, boot_id)) {
        printf("MARK_SUCCESS_SKIP reason=boot_id_mismatch requested=%s current=%s\n",
               requested_boot_id, boot_id);
        return 42;
    }
    if (read_boot_identity(&id)) {
        printf("MARK_SUCCESS_SKIP reason=bootconfig_incomplete\n");
        return 43;
    }
    if (identity_ok(&id)) {
        printf("MARK_SUCCESS_SKIP reason=bootconfig_identity_mismatch\n");
        return 44;
    }
    if (probe_misc(&rep, &id)) {
        printf("MARK_SUCCESS_SKIP reason=misc_unreadable\n");
        return 45;
    }
    if (!rep.crc_valid) {
        printf("MARK_SUCCESS_SKIP reason=misc_crc_invalid\n");
        return 46;
    }
    if (gather_evidence(boot_id, &ev) || !evidence_complete(&ev)) {
        printf("MARK_SUCCESS_SKIP reason=desktop_evidence_incomplete\n");
        return 47;
    }
    if (rep.current_slot < 0 ||
        rep.current_slot >= (int)rep.control.nb_slot) {
        printf("MARK_SUCCESS_SKIP reason=current_slot_unknown\n");
        return 48;
    }
    if (rep.control.slot_info[rep.current_slot].bootable != 1) {
        printf("MARK_SUCCESS_SKIP reason=slot_not_bootable\n");
        return 49;
    }
    if (load_hal_api(&api, hal_dir)) return 50;

    slot = api.current_slot();
    printf("MARK_HAL_SLOT current=%u\n", slot);
    if ((int)slot != rep.current_slot) {
        printf("MARK_SUCCESS_SKIP reason=hal_and_misc_slot_disagree hal=%u misc=%d\n",
               slot, rep.current_slot);
        return 51;
    }
    if (api.marked(slot) == 1) {
        printf("MARK_ALREADY_SUCCESSFUL slot=%u write=0\n", slot);
        return 0;
    }
    /* Last gate: all evidence verified. bootcontrol_init() may refresh the
     * virtual-A/B message in misc, then mark_boot_successful() writes misc. */
    rc = api.init();
    printf("MARK_HAL_INIT rc=%d\n", rc);
    if (api.marked(slot) != 1) {
        rc = api.mark();
        printf("MARK_HAL_CALL rc=%d\n", rc);
    }
    if (api.marked(slot) != 1) {
        printf("MARK_SUCCESS_SKIP reason=hal_verify_failed slot=%u\n", slot);
        return 52;
    }
    printf("MARK_SUCCESS_OK slot=%u boot_id=%s verified=1\n", slot, boot_id);
    return 0;
}
#endif /* HMOS_ALLOW_MARK_BOOT_SUCCESS */

/* ---- CLI ------------------------------------------------------------- */
static void usage(void)
{
    printf("usage: marble_bootcontrol [probe [--hal-audit] | mark <boot_id>]\n");
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "probe";
    const char *hal_dir = getenv("MARBLE_BOOTCONTROL_DIR");
    struct boot_identity id;
    char boot_id[64];
    int hal_audit = 0;
    int i;

    if (!hal_dir || !hal_dir[0]) hal_dir = DEFAULT_HAL_DIR;
    setvbuf(stdout, NULL, _IOLBF, 0);
    resolve_paths();
    printf("MARBLE_BOOTCONTROL version=1 pid=%d\n", (int)getpid());

    if (!strcmp(mode, "-h") || !strcmp(mode, "--help")) {
        usage();
        return 0;
    }
    for (i = 2; i < argc; i++)
        if (!strcmp(argv[i], "--hal-audit")) hal_audit = 1;

    if (read_boot_id(boot_id, sizeof(boot_id))) {
        printf("BOOT_ID unavailable\n");
    } else {
        printf("BOOT_ID=%s\n", boot_id);
    }
    if (read_boot_identity(&id)) printf("BOOTCONFIG_INCOMPLETE\n");

    if (!strcmp(mode, "probe")) {
        struct misc_report rep;
        struct hal_info hal;
        struct evidence ev;
        int have_boot_id = 0;
        int probe_rc;
        int identity_rc;
        int evidence_ok = 0;

        memset(&rep, 0, sizeof(rep));
        memset(&ev, 0, sizeof(ev));
        probe_rc = probe_misc(&rep, &id);
        if (probe_rc == 0 && rep.have_device)
            printf("MISC_PROBE_OK read_only=1\n");
        identity_rc = identity_ok(&id);
        printf("GATE identity=%d\n", identity_rc == 0 ? 1 : 0);
        have_boot_id = boot_id[0] != '\0';
        if (have_boot_id) {
            gather_evidence(boot_id, &ev);
            evidence_ok = evidence_complete(&ev);
        }
        printf("GATE evidence=%d\n", evidence_ok);
        printf("GATE misc_crc=%d\n", rep.crc_valid);
        printf("GATES_ALL_PASS=%d\n",
               (probe_rc == 0 && identity_rc == 0 && rep.crc_valid && have_boot_id &&
                evidence_ok)
                   ? 1
                   : 0);
        if (hal_audit || !access(DEFAULT_HAL_DIR "/" HAL_SO_NAME, R_OK))
            audit_hal(&hal, hal_dir);
        else
            printf("HAL_PRESENT=0 reason=not_staged path=%s/%s\n", hal_dir,
                   HAL_SO_NAME);
        printf("MARBLE_BOOTCONTROL_READONLY=1\n");
#ifdef HMOS_ALLOW_MARK_BOOT_SUCCESS
        printf("MARK_PATH_COMPILED=1\n");
#else
        printf("MARK_PATH_COMPILED=0 reason=read_only_build\n");
        printf("MARK_SUCCESS_SKIP reason=mark_path_not_compiled\n");
#endif
        return 0;
    }

    if (!strcmp(mode, "mark")) {
#ifdef HMOS_ALLOW_MARK_BOOT_SUCCESS
        return run_mark(argc > 2 ? argv[2] : NULL, hal_dir);
#else
        printf("MARK_SUCCESS_SKIP reason=mark_path_not_compiled boot_id=%s\n",
               argc > 2 && argv[2] ? argv[2] : "");
        printf("MARBLE_BOOTCONTROL_READONLY=1 write_attempted=0\n");
        return 0;
#endif
    }

    printf("MARK_SUCCESS_SKIP reason=unknown_mode mode=%s\n", mode);
    usage();
    return 30;
}
