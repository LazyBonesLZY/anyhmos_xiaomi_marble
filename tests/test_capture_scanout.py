"""Run the real capture CLI against a local DRM ioctl/DMA-BUF fixture only.

Run with `python3 -m unittest discover -s tests -p test_capture_scanout.py`.
Requires Linux, a host C compiler with a linker supporting --wrap, and DRM/DMA-BUF UAPI headers.
"""

import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest


SOURCE = Path(__file__).resolve().parents[1] / 'compat/services/capture_scanout.c'
FIXTURE = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <drm/drm_fourcc.h>
#include <linux/dma-buf.h>

static int drm = -1, dma = -1, syncing;
static unsigned plane74_queries, connector32_queries, sync_start_calls;

__attribute__((destructor)) static void check_cleanup(void)
{
    assert(!syncing);
}
int __real_open(const char *, int, ...);
int __real_close(int);
ssize_t __real_write(int, const void *, size_t);
void *__real_mmap(void *, size_t, int, int, int, off_t);

static int scenario(const char *name)
{
    const char *value = getenv("CAPTURE_CASE");
    return value && !strcmp(value, name);
}

int __wrap_open(const char *path, int flags, ...)
{
    if (!strcmp(path, "/dev/dri/card0")) {
        assert(drm == -1);
        if ((flags & O_ACCMODE) != O_RDONLY)
            fputs("FIXTURE_WRITABLE_CARD\n", stderr);
        return drm = __real_open("/dev/null", O_RDONLY | O_CLOEXEC);
    }
    // Never touch any real device, even if the sampler attempts a different node.
    assert(strncmp(path, "/dev/", 5));
    assert((flags & (O_ACCMODE | O_CREAT | O_EXCL)) == (O_WRONLY | O_CREAT | O_EXCL));
    va_list ap;
    va_start(ap, flags);
    int mode = va_arg(ap, int);
    va_end(ap);
    assert(mode == 0600);
    return __real_open(path, flags, mode);
}

int __wrap_close(int fd)
{
    if (fd == dma)
        assert(!syncing); // An opened read-sync bracket must end, even on output errors.
    return __real_close(fd);
}

ssize_t __wrap_write(int fd, const void *buf, size_t size)
{
    assert(syncing && fd != dma && fd != drm);
    if (scenario("write-error")) {
        errno = EIO;
        return -1;
    }
    return __real_write(fd, buf, size);
}

void *__wrap_mmap(void *addr, size_t n, int prot, int flags, int fd, off_t offset)
{
    assert(fd == dma && prot == PROT_READ && flags == MAP_SHARED && offset == 0);
    assert(n <= (uint64_t)lseek(fd, 0, SEEK_END));
    return __real_mmap(addr, n, prot, flags, fd, offset);
}

// 5×3, 24-byte pitch, 4-byte offset. Nonzero alpha/padding is not RGB data.
enum { W = 5, H = 3, PITCH = 24, OFFSET = 4, END = OFFSET + PITCH * (H - 1) + W * 4 };
static int create_buffer(unsigned fb)
{
    unsigned char data[OFFSET + PITCH * H];
    size_t n = scenario("truncated") ? END - 1 : scenario("exact-end") ? END : sizeof(data);
    memset(data, 0xea, sizeof(data));
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            unsigned char *p = data + OFFSET + y * PITCH + x * 4;
            unsigned r = x + y + 1, g = 0, b = y + 20;
            if (scenario("step-misses-color") && (x != 1 || y != 0))
                r = b = 0;
            if (fb == 34) { // first enumerated plane: an independent HDMI framebuffer
                r = 44;
                b = 0;
            }
            p[0] = fb == 85 ? b : r; // FB 85 is ARGB8888; FB 86 is ABGR8888.
            p[1] = g;
            p[2] = fb == 85 ? r : b;
        }
    }
    int fd = memfd_create("capture-scanout-fixture", MFD_CLOEXEC);
    assert(fd >= 0 && pwrite(fd, data, n, 0) == (ssize_t)n);
    return fd;
}

int __wrap_ioctl(int fd, unsigned long req, ...)
{
    va_list ap;
    va_start(ap, req);
    void *p = va_arg(ap, void *);
    va_end(ap);
    if (req == DMA_BUF_IOCTL_SYNC) {
        struct dma_buf_sync *s = p;
        assert(fd == dma);
        if (s->flags == (DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ)) {
            assert(!syncing);
            ++sync_start_calls;
            if (scenario("sync-start-error") ||
                (scenario("sync-start-retry") && sync_start_calls <= 2)) {
                errno = scenario("sync-start-error") ? EIO : sync_start_calls == 1 ? EINTR : EAGAIN;
                return -1;
            }
            syncing = 1;
        } else {
            assert(s->flags == (DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ) && syncing);
            syncing = 0;
            if (scenario("sync-end-error")) {
                errno = EIO;
                return -1;
            }
        }
        return 0;
    }
    assert(fd == drm);
    switch (req) {
    case DRM_IOCTL_SET_CLIENT_CAP: {
        struct drm_set_client_cap *c = p;
        assert(c->value == 1 && (c->capability == DRM_CLIENT_CAP_UNIVERSAL_PLANES ||
                                c->capability == DRM_CLIENT_CAP_ATOMIC));
        if (c->capability == DRM_CLIENT_CAP_ATOMIC && scenario("legacy")) {
            errno = EOPNOTSUPP;
            return -1;
        }
        return 0;
    }
    case DRM_IOCTL_MODE_GETRESOURCES: {
        struct drm_mode_card_res *r = p;
        static const uint32_t ids[] = {31, 33, 34, 32};
        if (r->count_connectors >= 4)
            memcpy((void *)(uintptr_t)r->connector_id_ptr, ids, sizeof(ids));
        r->count_connectors = 4;
        return 0;
    }
    case DRM_IOCTL_MODE_GETCONNECTOR: {
        struct drm_mode_get_connector *c = p;
        assert(c->connector_id >= 31 && c->connector_id <= 34);
        assert(c->count_modes && c->modes_ptr); // count_modes=0 forces probing on this kernel.
        c->connector_type = c->connector_id == 31 ? DRM_MODE_CONNECTOR_HDMIA : DRM_MODE_CONNECTOR_DSI;
        c->connector_type_id = c->connector_id == 31 ? 1 : c->connector_id - 31;
        c->connection = c->connector_id == 33 && !scenario("ambiguous") ? 2 : 1;
        if (c->connector_id == 32) {
            ++connector32_queries;
            if (scenario("dsi-disconnected") ||
                (scenario("route-change") && connector32_queries > 1))
                c->connection = 2;
        }
        c->encoder_id = c->connector_id + 10;
        return 0;
    }
    case DRM_IOCTL_MODE_GETENCODER: {
        struct drm_mode_get_encoder *e = p;
        assert(e->encoder_id >= 41 && e->encoder_id <= 44);
        e->crtc_id = e->encoder_id - 30;
        e->possible_crtcs = 0xf; // Not the current route.
        return 0;
    }
    case DRM_IOCTL_MODE_GETCRTC: {
        struct drm_mode_crtc *c = p;
        assert(c->crtc_id >= 11 && c->crtc_id <= 14);
        c->mode_valid = 1; // CRTC 14 has a retained mode but atomic ACTIVE=0.
        c->mode.hdisplay = c->crtc_id == 11 ? 1920 : 1080;
        c->mode.vdisplay = c->crtc_id == 11 ? 1080 : 2400;
        return 0;
    }
    case DRM_IOCTL_MODE_OBJ_GETPROPERTIES: {
        struct drm_mode_obj_get_properties *o = p;
        uint32_t prop;
        uint64_t value;
        if (o->obj_type == DRM_MODE_OBJECT_CRTC) {
            prop = 101;
            value = o->obj_id != 14;
            if (o->obj_id == 12 && scenario("active-unreadable")) {
                errno = EACCES;
                return -1;
            }
        } else {
            assert(o->obj_type == DRM_MODE_OBJECT_PLANE);
            prop = 102;
            value = o->obj_id == 72 ? 2 : o->obj_id == 73 ? 0 : 1;
        }
        if (o->count_props) {
            *(uint32_t *)(uintptr_t)o->props_ptr = prop;
            *(uint64_t *)(uintptr_t)o->prop_values_ptr = value;
        }
        o->count_props = 1;
        return 0;
    }
    case DRM_IOCTL_MODE_GETPROPERTY: {
        struct drm_mode_get_property *g = p;
        assert(g->prop_id == 101 || g->prop_id == 102);
        strcpy(g->name, g->prop_id == 101 ? "ACTIVE" : "type");
        return 0;
    }
    case DRM_IOCTL_MODE_GETPLANERESOURCES: {
        struct drm_mode_get_plane_res *r = p;
        // Stale/unrelated, disconnected, cursor, overlay then primary.
        static const uint32_t ids[] = {69, 90, 71, 72, 73, 74, 75};
        unsigned count = sizeof(ids) / sizeof(*ids);
        if (r->count_planes >= count)
            memcpy((void *)(uintptr_t)r->plane_id_ptr, ids, sizeof(ids));
        r->count_planes = count;
        return 0;
    }
    case DRM_IOCTL_MODE_GETPLANE: {
        struct drm_mode_get_plane *g = p;
        switch (g->plane_id) {
        case 69: g->fb_id = 34; g->crtc_id = 11; break;
        case 90: g->fb_id = 82; g->crtc_id = 14; break;
        case 71: g->fb_id = 83; g->crtc_id = 13; break;
        case 72: g->fb_id = 84; g->crtc_id = 12; break;
        case 73: g->fb_id = scenario("no-content") ? 0 : 85; g->crtc_id = 12; break;
        case 74: g->fb_id = scenario("no-content") || scenario("no-primary") ? 0 : 86;
                 g->crtc_id = 12;
                 if (++plane74_queries > 1 && scenario("flip"))
                     g->fb_id = 87;
                 break;
        case 75: g->fb_id = 90; g->crtc_id = 0; break;
        default: errno = ENOENT; return -1;
        }
        return 0;
    }
    case DRM_IOCTL_MODE_GETFB2: {
        struct drm_mode_fb_cmd2 *f = p;
        f->handles[0] = scenario("withheld-handle") ? 0 : f->fb_id;
        f->width = W;
        f->height = H;
        f->pitches[0] = scenario("short-pitch") ? 16 : PITCH;
        f->offsets[0] = OFFSET;
        f->flags = DRM_MODE_FB_MODIFIERS;
        f->pixel_format = f->fb_id == 85 ? DRM_FORMAT_ARGB8888 : DRM_FORMAT_ABGR8888;
        f->modifier[0] = scenario("nonlinear") ? DRM_FORMAT_MOD_INVALID : DRM_FORMAT_MOD_LINEAR;
        return 0;
    }
    case DRM_IOCTL_PRIME_HANDLE_TO_FD: {
        struct drm_prime_handle *h = p;
        assert(h->flags & DRM_CLOEXEC);
        if (h->flags & DRM_RDWR)
            fputs("FIXTURE_WRITABLE_PRIME\n", stderr);
        h->fd = dma = create_buffer(h->handle);
        return 0;
    }
    default:
        fprintf(stderr, "unexpected DRM ioctl %#lx\n", req);
        abort(); // ioctl allowlist: no modeset, atomic commit, master grab or buffer writes.
    }
}
'''


class CaptureScanoutTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shlex.split(os.environ.get('CC', 'cc'))
        if not sys.platform.startswith('linux') or not cc or not shutil.which(cc[0]):
            raise unittest.SkipTest('Linux and a host C compiler are required')
        headers = subprocess.run(
            [*cc, '-x', 'c', '-fsyntax-only', '-'],
            input='#include <drm/drm.h>\n#include <drm/drm_mode.h>\n'
                  '#include <drm/drm_fourcc.h>\n#include <linux/dma-buf.h>\n',
            text=True, capture_output=True, check=False, timeout=20,
        )
        if headers.returncode:
            raise unittest.SkipTest('Linux DRM/DMA-BUF UAPI headers not found')
        temp = tempfile.TemporaryDirectory(prefix='capture-scanout-test-')
        cls.addClassCleanup(temp.cleanup)
        cls.directory = Path(temp.name)
        shim = cls.directory / 'fixture.c'
        shim.write_text(FIXTURE, encoding='utf-8')
        cls.binary = cls.directory / 'capture_scanout'
        subprocess.run(
            [*cc, '-std=c11', '-Wall', '-Wextra', '-Werror', str(SOURCE), str(shim),
             '-o', str(cls.binary), '-Wl,--wrap=open,--wrap=ioctl,--wrap=close,--wrap=mmap,--wrap=write'],
            check=True, timeout=30,
        )

    def invoke(self, *args, case='default'):
        return subprocess.run(
            [str(self.binary), *map(str, args)], capture_output=True, check=False,
            cwd=self.directory, umask=0o077, env={**os.environ, 'CAPTURE_CASE': case}, timeout=5,
        )

    def assert_success(self, result):
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(b'SCANOUT_CAPTURE_OK', result.stderr)
        self.assertNotIn(b'FIXTURE_WRITABLE_', result.stderr)

    def assert_no_ppm(self, result, marker):
        self.assertGreater(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, b'')
        self.assertIn(marker, result.stderr)
        self.assertNotIn(b'SCANOUT_CAPTURE_OK', result.stderr)

    def test_connected_oled_primary_beats_first_bound_plane(self):
        out = self.directory / 'legacy-call.ppm'
        result = self.invoke(out, 2)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(b'plane=74 fb=86', result.stderr)
        self.assert_success(result)
        self.assertIn(b'crtc=12 connector=32', result.stderr)
        self.assertIn(b'plane_type=primary', result.stderr)
        self.assertEqual(result.stdout, b'')
        self.assertEqual(out.read_bytes(), b'P6\n3 2\n255\n' + bytes([
            1, 0, 20, 3, 0, 20, 5, 0, 20, 3, 0, 22, 5, 0, 22, 7, 0, 22,
        ]))
        self.assertEqual(out.stat().st_mode & 0o777, 0o600)
        self.assertIn(b'output=3x2 sampled_pixels=6 nonzero_pixels=6 nonzero_bytes=12', result.stderr)

    def test_explicit_overlay_and_stdout_rgb_order(self):
        result = self.invoke('--plane', 73, '--crtc', 12, '-', 2, case='exact-end')
        self.assert_success(result)
        self.assertIn(b'plane=73 fb=85', result.stderr)
        self.assertIn(b'plane_type=overlay', result.stderr)
        self.assertEqual(result.stdout, b'P6\n3 2\n255\n' + bytes([
            1, 0, 20, 3, 0, 20, 5, 0, 20, 3, 0, 22, 5, 0, 22, 7, 0, 22,
        ]))

    def test_overlay_fallback_does_not_pick_cursor(self):
        result = self.invoke('-', case='no-primary')
        self.assert_success(result)
        self.assertIn(b'plane=73 fb=85', result.stderr)
        self.assertTrue(result.stdout.startswith(b'P6\n5 3\n255\n'))
        self.assert_no_ppm(self.invoke('-', case='no-content'), b'no non-cursor plane')
        self.assert_success(self.invoke('--plane', 72, '-', case='no-content'))

    def test_inactive_disconnected_and_mismatched_explicit_targets(self):
        for args, marker in [
            (('--crtc', 14), b'no connected active CRTC'),
            (('--plane', 71), b'no connected active CRTC'),
            (('--plane', 75), b'not bound to a CRTC and FB'),
            (('--crtc', 11, '--plane', 74), b'expected CRTC'),
        ]:
            with self.subTest(args=args):
                self.assert_no_ppm(self.invoke(*args, '-'), marker)
        self.assert_no_ppm(self.invoke('-', case='active-unreadable'), b'CRTC ACTIVE')

    def test_fallback_and_ambiguity_are_not_silent_oled_claims(self):
        for args, case in [((), 'dsi-disconnected'), (('--crtc', 11), 'default'),
                           (('--crtc', 11), 'legacy')]:
            with self.subTest(args=args, case=case):
                result = self.invoke(*args, '-', case=case)
                self.assert_success(result)
                self.assertIn(b'plane=69 fb=34', result.stderr)
                self.assertIn(b'crtc=11 connector=31 connector_type=11', result.stderr)
                if case == 'legacy':
                    self.assertIn(b'crtc_state=legacy_mode_valid', result.stderr)
        self.assert_no_ppm(self.invoke('-', case='ambiguous'), b'ambiguous connected active CRTCs')
        result = self.invoke('--crtc', 12, '-', case='ambiguous')
        self.assert_success(result)
        self.assertIn(b'crtc=12 connector=32', result.stderr)

    def test_zero_sample_is_not_full_fb_claim(self):
        wide = self.invoke('-', 2, case='step-misses-color')
        full = self.invoke('-', 1, case='step-misses-color')
        self.assert_success(wide)
        self.assert_success(full)
        self.assertEqual(wide.stdout, b'P6\n3 2\n255\n' + bytes(18))
        self.assertIn(b'output=3x2 sampled_pixels=6 nonzero_pixels=0 nonzero_bytes=0', wide.stderr)
        self.assertIn(b'output=5x3 sampled_pixels=15 nonzero_pixels=1 nonzero_bytes=2', full.stderr)
        self.assertIn(b'scope=single_plane_fb_rgb', wide.stderr)
        self.assertIn(b'composited=0 atomic_snapshot=0 panel_visibility=unverified', wide.stderr)

    def test_buffer_validation_and_selection_change_fail_before_output(self):
        for case, marker in [
            ('withheld-handle', b'GETFB2 supplied no GEM handle'),
            ('nonlinear', b'unsupported FB layout'),
            ('short-pitch', b'unsupported FB layout'),
            ('truncated', b'FB exceeds DMA-BUF size'),
            ('route-change', b'display route changed'),
            ('flip', b'plane binding changed before sampling'),
            ('sync-start-error', b'sync start'),
        ]:
            with self.subTest(case=case):
                self.assert_no_ppm(self.invoke('-', case=case), marker)

    def test_sync_retries_transient_failures(self):
        result = self.invoke('-', 2, case='sync-start-retry')
        self.assert_success(result)
        self.assertIn(b'output=3x2 sampled_pixels=6', result.stderr)

    def test_exclusive_file_and_io_failure(self):
        out = self.directory / 'existing.ppm'
        out.write_bytes(b'preserve')
        self.assert_no_ppm(self.invoke(out), b'output:')
        self.assertEqual(out.read_bytes(), b'preserve')
        self.assert_no_ppm(self.invoke('-', case='write-error'), b'write PPM')
        result = self.invoke('-', case='sync-end-error')
        self.assertGreater(result.returncode, 0, result.stderr)
        self.assertIn(b'sync end', result.stderr)
        self.assertNotIn(b'SCANOUT_CAPTURE_OK', result.stderr)

    def test_malformed_fields_do_not_emit_ppm(self):
        for args in [('-', '0'), ('-', '17'), ('-', '4294967297'), ('-', '2junk'),
                     ('--plane', '-1', '-'), ('--crtc', '0', '-'), ('--plane', '74')]:
            with self.subTest(args=args):
                result = self.invoke(*args)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertIn(b'usage:', result.stderr)
                self.assertEqual(result.stdout, b'')


if __name__ == '__main__':
    unittest.main()
