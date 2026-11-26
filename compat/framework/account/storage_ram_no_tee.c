/* SPDX-License-Identifier: MIT
 * RAM-data boot trial shim: report unavailable HUKS HDI immediately and provide
 * real ext4 /data per-UID/GID usage + Q_GETNEXTQUOTA when kernel quotactl fails.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/quota.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef PRJQUOTA
#define PRJQUOTA 2
#endif
#ifndef Q_GETNEXTQUOTA
#define Q_GETNEXTQUOTA 0x800009
#endif

struct hmos_nextdqblk {
    uint64_t dqb_bhardlimit;
    uint64_t dqb_bsoftlimit;
    uint64_t dqb_curspace;
    uint64_t dqb_ihardlimit;
    uint64_t dqb_isoftlimit;
    uint64_t dqb_curinodes;
    uint64_t dqb_btime;
    uint64_t dqb_itime;
    uint32_t dqb_valid;
    uint32_t dqb_id;
};

_Static_assert(offsetof(struct dqblk, dqb_curspace) == 16,
               "dqb_curspace must sit at offset 16 to match storage_daemon");
_Static_assert(offsetof(struct hmos_nextdqblk, dqb_curspace) == 16,
               "if_nextdqblk dqb_curspace offset must match ABI (16 bytes)");
_Static_assert(offsetof(struct hmos_nextdqblk, dqb_curinodes) == 40,
               "if_nextdqblk dqb_curinodes offset must match ABI (40 bytes)");
_Static_assert(offsetof(struct hmos_nextdqblk, dqb_id) == 68,
               "if_nextdqblk dqb_id offset must match ABI (68 bytes)");
_Static_assert(sizeof(struct hmos_nextdqblk) == 72,
               "if_nextdqblk size must match ABI (72 bytes)");

struct IHuks;
struct IHuks *IHuksGetInstance(const char *name, bool direct)
{
    if (name && direct && !strcmp(name, "hdi_service")) return NULL;
    typedef struct IHuks *(*Get)(const char *, bool);
    Get get = (Get)dlsym(RTLD_NEXT, "IHuksGetInstance");
    return get ? get(name, direct) : NULL;
}

typedef struct {
    uint32_t id;
    uint64_t bytes;
    uint64_t inodes;
    uint8_t used;
} IdQuotaSlot;

typedef struct {
    IdQuotaSlot *slots;
    size_t cap;
    size_t count;
} IdQuotaMap;

static pthread_mutex_t g_quota_lock = PTHREAD_MUTEX_INITIALIZER;
static IdQuotaMap g_uid_map = {NULL, 0, 0};
static IdQuotaMap g_gid_map = {NULL, 0, 0};
static uint32_t *g_sorted_uids = NULL;
static size_t g_sorted_uid_count = 0;
static int64_t g_last_scan_ns = 0;
static char g_cached_env_roots[512] = "";

static int64_t monotonic_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

static size_t hash_id(uint32_t id)
{
    uint32_t x = id * 2654435761U;
    return (size_t)(x ^ (x >> 16));
}

static bool map_grow(IdQuotaMap *map, size_t new_cap)
{
    IdQuotaSlot *next = calloc(new_cap, sizeof(*next));
    if (!next) return false;
    size_t mask = new_cap - 1;
    for (size_t i = 0; i < map->cap; ++i) {
        if (!map->slots[i].used) continue;
        size_t idx = hash_id(map->slots[i].id) & mask;
        while (next[idx].used) idx = (idx + 1) & mask;
        next[idx] = map->slots[i];
    }
    free(map->slots);
    map->slots = next;
    map->cap = new_cap;
    return true;
}

static void map_add(IdQuotaMap *map, uint32_t id, uint64_t bytes, uint64_t inodes)
{
    if (map->cap == 0 || (map->count + 1) * 10 >= map->cap * 7) {
        size_t next_cap = map->cap ? map->cap * 2 : 512;
        if (!map_grow(map, next_cap)) return;
    }
    size_t mask = map->cap - 1;
    size_t idx = hash_id(id) & mask;
    while (map->slots[idx].used) {
        if (map->slots[idx].id == id) {
            map->slots[idx].bytes += bytes;
            map->slots[idx].inodes += inodes;
            return;
        }
        idx = (idx + 1) & mask;
    }
    map->slots[idx].used = 1;
    map->slots[idx].id = id;
    map->slots[idx].bytes = bytes;
    map->slots[idx].inodes = inodes;
    map->count++;
}

static void map_lookup(const IdQuotaMap *map, uint32_t id, uint64_t *bytes, uint64_t *inodes)
{
    *bytes = 0;
    *inodes = 0;
    if (!map->slots || map->cap == 0) return;
    size_t mask = map->cap - 1;
    size_t idx = hash_id(id) & mask;
    while (map->slots[idx].used) {
        if (map->slots[idx].id == id) {
            *bytes = map->slots[idx].bytes;
            *inodes = map->slots[idx].inodes;
            return;
        }
        idx = (idx + 1) & mask;
    }
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t ua = *(const uint32_t *)a;
    uint32_t ub = *(const uint32_t *)b;
    return (ua > ub) - (ua < ub);
}

static void scan_dir_fd(int fd, int depth, IdQuotaMap *uid_map, IdQuotaMap *gid_map)
{
    if (depth > 32) {
        close(fd);
        return;
    }
    DIR *dir = fdopendir(fd);
    if (!dir) {
        close(fd);
        return;
    }
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        const char *name = de->d_name;
        if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')))
            continue;
        struct stat st;
        if (fstatat(dirfd(dir), name, &st, AT_SYMLINK_NOFOLLOW) != 0)
            continue;
        uint64_t bytes = st.st_blocks > 0 ? (uint64_t)st.st_blocks * 512ULL : 0ULL;
        map_add(uid_map, (uint32_t)st.st_uid, bytes, 1ULL);
        map_add(gid_map, (uint32_t)st.st_gid, bytes, 1ULL);
        if (S_ISDIR(st.st_mode)) {
            int child = openat(dirfd(dir), name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (child >= 0) scan_dir_fd(child, depth + 1, uid_map, gid_map);
        }
    }
    closedir(dir);
}

static void scan_root_path(const char *root, IdQuotaMap *uid_map, IdQuotaMap *gid_map)
{
    if (!root || !root[0]) return;
    int fd = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd >= 0) scan_dir_fd(fd, 0, uid_map, gid_map);
}

static void refresh_quota_snapshot_locked(const char *env_roots)
{
    static const char *const kDefaultRoots[] = {
        "/data/app",
        "/data/service",
        "/data/storage",
        "/data/local",
    };
    IdQuotaMap fresh_uid = {NULL, 0, 0};
    IdQuotaMap fresh_gid = {NULL, 0, 0};
    if (env_roots && env_roots[0]) {
        char buf[512];
        strncpy(buf, env_roots, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        char *save = NULL;
        for (char *tok = strtok_r(buf, ":", &save); tok; tok = strtok_r(NULL, ":", &save))
            scan_root_path(tok, &fresh_uid, &fresh_gid);
    } else {
        for (size_t i = 0; i < sizeof(kDefaultRoots) / sizeof(kDefaultRoots[0]); ++i)
            scan_root_path(kDefaultRoots[i], &fresh_uid, &fresh_gid);
    }

    uint32_t *sorted = NULL;
    size_t sorted_count = 0;
    if (fresh_uid.count > 0) {
        sorted = calloc(fresh_uid.count, sizeof(*sorted));
        if (sorted) {
            for (size_t i = 0; i < fresh_uid.cap && sorted_count < fresh_uid.count; ++i) {
                if (fresh_uid.slots[i].used) sorted[sorted_count++] = fresh_uid.slots[i].id;
            }
            if (sorted_count > 1) qsort(sorted, sorted_count, sizeof(*sorted), cmp_u32);
        }
    }

    free(g_uid_map.slots);
    free(g_gid_map.slots);
    free(g_sorted_uids);
    g_uid_map = fresh_uid;
    g_gid_map = fresh_gid;
    g_sorted_uids = sorted;
    g_sorted_uid_count = sorted_count;
    g_last_scan_ns = monotonic_ns();
    if (env_roots && env_roots[0]) {
        strncpy(g_cached_env_roots, env_roots, sizeof(g_cached_env_roots) - 1);
        g_cached_env_roots[sizeof(g_cached_env_roots) - 1] = '\0';
    } else {
        g_cached_env_roots[0] = '\0';
    }
}

static bool is_userdata_special(const char *special)
{
    return special && strstr(special, "userdata") != NULL;
}

static bool is_handled_quota_cmd(int cmd)
{
    return cmd == (int)QCMD(Q_GETQUOTA, USRQUOTA) ||
           cmd == (int)QCMD(Q_GETQUOTA, GRPQUOTA) ||
           cmd == (int)QCMD(Q_GETQUOTA, PRJQUOTA) ||
           cmd == (int)QCMD(Q_GETNEXTQUOTA, USRQUOTA);
}

int quotactl(int cmd, const char *special, int id, char *addr)
{
    typedef int (*QuotactlFn)(int, const char *, int, char *);
    static QuotactlFn real_fn = NULL;
    if (!real_fn) {
        real_fn = (QuotactlFn)dlsym(RTLD_NEXT, "quotactl");
    }

    const char *env_roots = getenv("HMOS_QUOTA_SCAN_ROOT");
    if (is_handled_quota_cmd(cmd) && (is_userdata_special(special) || (env_roots && env_roots[0]))) {
        if (!addr) {
            errno = EFAULT;
            return -1;
        }
        if (id < 0) {
            errno = EINVAL;
            return -1;
        }
        if (real_fn && !(env_roots && env_roots[0])) {
            int saved = errno;
            int rc = real_fn(cmd, special, id, addr);
            if (rc == 0) return 0;
            int err = errno;
            if (err != ESRCH && err != ENOSYS && err != ENODEV && err != ENXIO && err != ENOENT && err != EPERM) {
                errno = err;
                return -1;
            }
            errno = saved;
        }

        const char *want_env = env_roots ? env_roots : "";
        pthread_mutex_lock(&g_quota_lock);
        int64_t now = monotonic_ns();
        const int64_t kTtlNs = 30LL * 1000000000LL;
        if (g_last_scan_ns == 0 || (now - g_last_scan_ns) > kTtlNs ||
            strcmp(g_cached_env_roots, want_env) != 0) {
            refresh_quota_snapshot_locked(env_roots);
        }

        uint64_t bytes = 0, inodes = 0;
        if (cmd == (int)QCMD(Q_GETNEXTQUOTA, USRQUOTA)) {
            size_t lo = 0, hi = g_sorted_uid_count;
            uint32_t target = (uint32_t)id;
            while (lo < hi) {
                size_t mid = lo + (hi - lo) / 2;
                if (g_sorted_uids[mid] < target) lo = mid + 1;
                else hi = mid;
            }
            if (lo >= g_sorted_uid_count) {
                pthread_mutex_unlock(&g_quota_lock);
                errno = ENOENT;
                return -1;
            }
            uint32_t next_uid = g_sorted_uids[lo];
            map_lookup(&g_uid_map, next_uid, &bytes, &inodes);
            pthread_mutex_unlock(&g_quota_lock);

            struct hmos_nextdqblk ndq;
            memset(&ndq, 0, sizeof(ndq));
            ndq.dqb_curspace = bytes;
            ndq.dqb_curinodes = inodes;
            ndq.dqb_valid = QIF_ALL;
            ndq.dqb_id = next_uid;
            memcpy(addr, &ndq, sizeof(ndq));
            return 0;
        }

        if (cmd == (int)QCMD(Q_GETQUOTA, USRQUOTA)) {
            map_lookup(&g_uid_map, (uint32_t)id, &bytes, &inodes);
        } else if (cmd == (int)QCMD(Q_GETQUOTA, GRPQUOTA)) {
            map_lookup(&g_gid_map, (uint32_t)id, &bytes, &inodes);
        }
        pthread_mutex_unlock(&g_quota_lock);

        struct dqblk dq;
        memset(&dq, 0, sizeof(dq));
        dq.dqb_curspace = bytes;
        dq.dqb_curinodes = inodes;
        dq.dqb_valid = QIF_ALL;
        memcpy(addr, &dq, sizeof(dq));
        return 0;
    }

    if (real_fn) return real_fn(cmd, special, id, addr);
    errno = ENOSYS;
    return -1;
}
