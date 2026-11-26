#!/usr/bin/env python3
"""Host and OHOS ABI verification tests for storage_ram_no_tee.c quotactl shim."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]
SHIM_SRC = REPO / 'compat' / 'framework' / 'account' / 'storage_ram_no_tee.c'
OHOS_QUOTA_H = REPO / 'work' / 'ohos-sdk' / 'installed' / 'native' / 'sysroot' / 'usr' / 'include' / 'sys' / 'quota.h'
OHOS_LINUX_QUOTA_H = REPO / 'work' / 'ohos-sdk' / 'installed' / 'native' / 'sysroot' / 'usr' / 'include' / 'linux' / 'quota.h'
OHOS_SO = REPO / 'work' / 'marble-b2-build' / 'libhmos_ram_no_tee.so'
LLVM_NM = REPO / 'work' / 'ohos-sdk' / 'installed' / 'native' / 'llvm' / 'bin' / 'llvm-nm'

TEST_HARNESS_C = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/quota.h>
#include <sys/stat.h>
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

_Static_assert(offsetof(struct dqblk, dqb_curspace) == 16, "host dqblk dqb_curspace offset");
_Static_assert(offsetof(struct hmos_nextdqblk, dqb_id) == 68, "host if_nextdqblk dqb_id offset");
_Static_assert(sizeof(struct hmos_nextdqblk) == 72, "host if_nextdqblk size");

struct IHuks;
extern struct IHuks *IHuksGetInstance(const char *, bool);

static void write_file_bytes(const char *path, size_t nbytes, uint64_t *blocks_out, uint64_t *inodes_out)
{
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    assert(fd >= 0);
    char buf[4096];
    memset(buf, 0x5a, sizeof(buf));
    size_t rem = nbytes;
    while (rem > 0) {
        size_t chunk = rem > sizeof(buf) ? sizeof(buf) : rem;
        ssize_t w = write(fd, buf, chunk);
        assert(w == (ssize_t)chunk);
        rem -= (size_t)w;
    }
    fsync(fd);
    close(fd);
    struct stat st;
    assert(lstat(path, &st) == 0);
    *blocks_out += (uint64_t)st.st_blocks * 512ULL;
    *inodes_out += 1;
}

static void add_dir_blocks(const char *path, uint64_t *blocks_out, uint64_t *inodes_out)
{
    struct stat st;
    assert(lstat(path, &st) == 0);
    *blocks_out += (uint64_t)st.st_blocks * 512ULL;
    *inodes_out += 1;
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    const char *root1 = argv[1];
    const char *root2 = argv[2];
    int uid = (int)getuid();
    int gid = (int)getgid();

    assert(IHuksGetInstance("hdi_service", true) == NULL);

    char sub1[512], f1[512], f2[512], f3[512], f_late[512];
    assert(snprintf(sub1, sizeof(sub1), "%s/sub", root1) < (int)sizeof(sub1));
    assert(mkdir(sub1, 0755) == 0);
    assert(snprintf(f1, sizeof(f1), "%s/a.bin", root1) < (int)sizeof(f1));
    assert(snprintf(f2, sizeof(f2), "%s/b.bin", sub1) < (int)sizeof(f2));
    assert(snprintf(f3, sizeof(f3), "%s/c.bin", root2) < (int)sizeof(f3));
    assert(snprintf(f_late, sizeof(f_late), "%s/late.bin", root2) < (int)sizeof(f_late));

    uint64_t expected_bytes = 0, expected_inodes = 0;
    add_dir_blocks(sub1, &expected_bytes, &expected_inodes);
    write_file_bytes(f1, 4096, &expected_bytes, &expected_inodes);
    write_file_bytes(f2, 16384, &expected_bytes, &expected_inodes);
    write_file_bytes(f3, 8192, &expected_bytes, &expected_inodes);

    char roots[1024];
    assert(snprintf(roots, sizeof(roots), "%s:%s", root1, root2) < (int)sizeof(roots));
    assert(setenv("HMOS_QUOTA_SCAN_ROOT", roots, 1) == 0);

    /* USRQUOTA (0x80000700) */
    struct dqblk dq;
    memset(&dq, 0xff, sizeof(dq));
    int rc = quotactl((int)QCMD(Q_GETQUOTA, USRQUOTA), "/dev/block/by-name/userdata", uid, (char *)&dq);
    assert(rc == 0);
    assert(dq.dqb_curspace == expected_bytes);
    assert(dq.dqb_curspace >= 4096 + 16384 + 8192);
    assert(dq.dqb_curinodes == expected_inodes);

    /* Alternate special device path returns identical cached usage. */
    memset(&dq, 0, sizeof(dq));
    rc = quotactl(0x80000700, "/dev/block/userdata", uid, (char *)&dq);
    assert(rc == 0);
    assert(dq.dqb_curspace == expected_bytes);

    /* GRPQUOTA (0x80000701) and PRJQUOTA (0x80000702) */
    memset(&dq, 0xff, sizeof(dq));
    rc = quotactl(0x80000701, "/dev/block/by-name/userdata", gid, (char *)&dq);
    assert(rc == 0);
    assert(dq.dqb_curspace == expected_bytes);
    assert(dq.dqb_curinodes == expected_inodes);

    memset(&dq, 0xff, sizeof(dq));
    rc = quotactl((int)QCMD(Q_GETQUOTA, PRJQUOTA), "/dev/block/by-name/userdata", 42, (char *)&dq);
    assert(rc == 0);
    assert(dq.dqb_curspace == 0 && dq.dqb_curinodes == 0);

    /* Q_GETNEXTQUOTA (0x80000900) iteration loop */
    int cursor = 0;
    int seen_uid = 0;
    uint32_t prev_id = 0;
    int iters = 0;
    for (;;) {
        struct hmos_nextdqblk ndq;
        memset(&ndq, 0xff, sizeof(ndq));
        rc = quotactl((int)QCMD(Q_GETNEXTQUOTA, USRQUOTA), "/dev/block/by-name/userdata", cursor, (char *)&ndq);
        if (rc != 0) {
            assert(errno == ENOENT);
            break;
        }
        assert(iters == 0 || ndq.dqb_id > prev_id);
        prev_id = ndq.dqb_id;
        if (ndq.dqb_id == (uint32_t)uid) {
            seen_uid++;
            assert(ndq.dqb_curspace == expected_bytes);
            assert(ndq.dqb_curinodes == expected_inodes);
        }
        cursor = (int)(ndq.dqb_id + 1U);
        iters++;
        assert(iters < 64);
    }
    assert(seen_uid == 1);

    /* Empty-tree Q_GETNEXTQUOTA immediately returns ENOENT. */
    char empty_root[512];
    assert(snprintf(empty_root, sizeof(empty_root), "%s/empty", root2) < (int)sizeof(empty_root));
    assert(mkdir(empty_root, 0755) == 0);
    assert(setenv("HMOS_QUOTA_SCAN_ROOT", empty_root, 1) == 0);
    struct hmos_nextdqblk empty_ndq;
    errno = 0;
    assert(quotactl(0x80000900, "/dev/block/by-name/userdata", 0, (char *)&empty_ndq) == -1 && errno == ENOENT);

    /* Restore roots and verify 30s monotonic TTL cache behavior. */
    assert(setenv("HMOS_QUOTA_SCAN_ROOT", roots, 1) == 0);
    memset(&dq, 0, sizeof(dq));
    assert(quotactl(0x80000700, "/dev/block/by-name/userdata", uid, (char *)&dq) == 0);
    assert(dq.dqb_curspace == expected_bytes);

    uint64_t dummy_b = 0, dummy_i = 0;
    write_file_bytes(f_late, 12288, &dummy_b, &dummy_i);
    memset(&dq, 0, sizeof(dq));
    assert(quotactl(0x80000700, "/dev/block/by-name/userdata", uid, (char *)&dq) == 0);
    assert(dq.dqb_curspace == expected_bytes);

    /* Updating HMOS_QUOTA_SCAN_ROOT forces a fresh snapshot including f_late. */
    char roots_refresh[1024];
    assert(snprintf(roots_refresh, sizeof(roots_refresh), "%s:%s/", root1, root2) < (int)sizeof(roots_refresh));
    assert(setenv("HMOS_QUOTA_SCAN_ROOT", roots_refresh, 1) == 0);
    memset(&dq, 0, sizeof(dq));
    assert(quotactl(0x80000700, "/dev/block/by-name/userdata", uid, (char *)&dq) == 0);
    assert(dq.dqb_curspace == expected_bytes + dummy_b);

    /* Unused UID succeeds with 0 usage instead of ESRCH. */
    memset(&dq, 0xff, sizeof(dq));
    assert(quotactl(0x80000700, "/dev/block/by-name/userdata", uid + 12345, (char *)&dq) == 0);
    assert(dq.dqb_curspace == 0 && dq.dqb_curinodes == 0);

    /* Error edge cases */
    errno = 0;
    assert(quotactl(0x80000700, "/dev/block/by-name/userdata", uid, NULL) == -1 && errno == EFAULT);
    errno = 0;
    assert(quotactl(0x80000900, "/dev/block/by-name/userdata", uid, NULL) == -1 && errno == EFAULT);
    errno = 0;
    assert(quotactl(0x80000700, "/dev/block/by-name/userdata", -1, (char *)&dq) == -1 && errno == EINVAL);

    printf("STORAGE_QUOTA_SHIM_TEST_PASS uid=%d gid=%d bytes=%llu inodes=%llu late=%llu\n",
           uid, gid, (unsigned long long)expected_bytes, (unsigned long long)expected_inodes,
           (unsigned long long)dummy_b);
    return 0;
}
'''


class StorageQuotaShimTests(unittest.TestCase):
    def test_ohos_shim_exports_and_quota_header(self) -> None:
        self.assertTrue(OHOS_SO.is_file(), f'Build storage-no-tee first: {OHOS_SO}')
        syms = subprocess.check_output([str(LLVM_NM), '-D', str(OHOS_SO)], text=True)
        self.assertIn(' T IHuksGetInstance', syms)
        self.assertIn(' T quotactl', syms)
        quota_h = OHOS_QUOTA_H.read_text(encoding='utf-8')
        linux_quota_h = OHOS_LINUX_QUOTA_H.read_text(encoding='utf-8')
        self.assertIn('#define Q_GETQUOTA 0x800007', quota_h)
        self.assertIn('uint64_t dqb_curspace;', quota_h)
        self.assertIn('#define PRJQUOTA 2', linux_quota_h)
        self.assertIn('#define Q_GETNEXTQUOTA 0x800009', linux_quota_h)

    def test_quotactl_uid_gid_prj_next_and_ttl_cache(self) -> None:
        with tempfile.TemporaryDirectory(prefix='quota-shim-', dir='/mnt/e/marble-tmp') as tmp:
            tmp_path = Path(tmp)
            shim_so = tmp_path / 'libquota_test.so'
            harness_c = tmp_path / 'harness.c'
            harness_bin = tmp_path / 'harness'
            root1 = tmp_path / 'r1'
            root2 = tmp_path / 'r2'
            root1.mkdir()
            root2.mkdir()
            harness_c.write_text(TEST_HARNESS_C, encoding='utf-8')

            subprocess.run([
                'gcc', '-O2', '-Wall', '-Wextra', '-shared', '-fPIC',
                str(SHIM_SRC), '-ldl', '-lpthread', '-o', str(shim_so),
            ], check=True)
            subprocess.run([
                'gcc', '-O2', '-Wall', '-Wextra',
                str(harness_c), str(shim_so), '-ldl',
                '-Wl,-rpath,' + str(tmp_path),
                '-o', str(harness_bin),
            ], check=True)
            env = dict(os.environ)
            env['LD_PRELOAD'] = str(shim_so)
            out = subprocess.check_output(
                [str(harness_bin), str(root1), str(root2)],
                env=env,
                text=True,
            )
            self.assertIn('STORAGE_QUOTA_SHIM_TEST_PASS', out)


if __name__ == '__main__':
    unittest.main()
