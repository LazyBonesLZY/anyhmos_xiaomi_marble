#!/usr/bin/env python3
"""Offline unit tests and artifact checks for Batch U3.1 (codec_host + JPEG VDI, DisplayEngine V1.1 + SA 65962, Venus probe)."""
import importlib.util
import json
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / 'tools/packaging'
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import build_marble_phase4_images as p4

U3_BUILD = ROOT / 'work/marble-u3-build'
DONOR_FS = ROOT / 'work/donor-rom/ADY-AL10-207.0.0.111-SP10/fs'
CODEC_CFG = ROOT / 'devices/marble/runtime/codec/init-u3-codec-display-marble.cfg'
PACKAGER = TOOLS / 'build_marble_h20_u3_media_display.py'
BASE_U2_IMG = Path('/mnt/e/marble-tmp/vendor_boot_h20_u2_loc_media_974b7b75.img')
LLVM_BIN = ROOT / 'work/ohos-sdk/installed/native/llvm/bin'


class TestBatchU3MediaDisplay(unittest.TestCase):
    def test_jpeg_vdi_host_and_elf(self):
        vdi_c = ROOT / 'compat/codec/jpeg_vdi_impl.c'
        src = vdi_c.read_text(encoding='utf-8')
        self.assertIn('GetCodecJpegHwi', src)
        self.assertIn('DMA_HEAP_IOCTL_ALLOC', src)
        self.assertIn('DMA_BUF_IOCTL_SYNC', src)

        # Verify kJpegNaturalOrder table is a valid 64-entry permutation of 0..63.
        m = re.search(r'kJpegNaturalOrder\[64\]\s*=\s*\{([^}]+)\}', src)
        self.assertIsNotNone(m)
        vals = [int(tok) for tok in re.findall(r'\d+', m.group(1))]
        self.assertEqual(len(vals), 64)
        self.assertEqual(sorted(vals), list(range(64)))

        with tempfile.TemporaryDirectory(prefix='u3_jpeg_') as tmp:
            exe = Path(tmp) / 'jpeg_vdi_host_test'
            subprocess.run(
                ['gcc', '-O2', '-Wall', '-Wextra', '-Werror', '-std=gnu11',
                 '-DJPEG_VDI_HOST_TEST', str(vdi_c), '-ldl', '-lpthread', '-o', str(exe)],
                check=True,
            )
            res = subprocess.run([str(exe)], check=True, capture_output=True, text=True)
            self.assertIn('JPEG_VDI_HOST_TEST_OK', res.stdout)

        vdi_so = U3_BUILD / 'libjpeg_vdi_impl.z.so'
        probe_bin = U3_BUILD / 'native_venus_v4l2_probe'
        self.assertTrue(vdi_so.is_file(), f'Missing compiled VDI: {vdi_so}')
        self.assertTrue(probe_bin.is_file(), f'Missing compiled Venus probe: {probe_bin}')

        readelf = LLVM_BIN / 'llvm-readelf'
        nm = LLVM_BIN / 'llvm-nm'
        if readelf.is_file() and nm.is_file():
            dyn = subprocess.check_output([str(readelf), '-d', str(vdi_so)], text=True)
            needed = re.findall(r'\(NEEDED\)\s+Shared library:\s+\[([^\]]+)\]', dyn)
            self.assertEqual(needed, ['libc.so'])
            self.assertIn('[libjpeg_vdi_impl.z.so]', dyn)

            syms = subprocess.check_output([str(nm), '-D', '--defined-only', str(vdi_so)], text=True)
            self.assertRegex(syms, r'\bT\s+GetCodecJpegHwi\b')

            p_dyn = subprocess.check_output([str(readelf), '-d', str(probe_bin)], text=True)
            p_needed = re.findall(r'\(NEEDED\)\s+Shared library:\s+\[([^\]]+)\]', p_dyn)
            self.assertEqual(p_needed, ['libc.so'])
            p_interp = subprocess.check_output([str(readelf), '-l', str(probe_bin)], text=True)
            self.assertIn('/lib/ld-musl-aarch64.so.1', p_interp)

    def test_displayengine_module_elf_and_donor_abi(self):
        de_cpp = ROOT / 'compat/display/displayengine_module.cpp'
        de_src = de_cpp.read_text(encoding='utf-8')
        self.assertIn('DisplayEngineWrapperOH', de_src)

        de_so = U3_BUILD / 'libdisplayengine_module.so'
        self.assertTrue(de_so.is_file(), f'Missing compiled DisplayEngine module: {de_so}')
        readelf = LLVM_BIN / 'llvm-readelf'
        nm = LLVM_BIN / 'llvm-nm'
        if readelf.is_file() and nm.is_file():
            dyn = subprocess.check_output([str(readelf), '-d', str(de_so)], text=True)
            needed = re.findall(r'\(NEEDED\)\s+Shared library:\s+\[([^\]]+)\]', dyn)
            self.assertEqual(needed, ['libc++.so', 'libc.so'])
            self.assertIn('[libdisplayengine_module.so]', dyn)

            syms = subprocess.check_output([str(nm), '-D', '--defined-only', str(de_so)], text=True)
            defined = {line.split()[-1] for line in syms.splitlines() if line.strip() and line.split()[1] in ('T', 'W')}

            donor_wrapper = DONOR_FS / 'vendor/lib64/libdisplay_engine_wrapper_service_1.1.z.so'
            if donor_wrapper.is_file():
                w_syms = subprocess.check_output([str(readelf), '-Ws', '--wide', str(donor_wrapper)], text=True)
                und = re.findall(r'\bUND\s+(_ZN22DisplayEngineWrapperOH\S+)', w_syms)
                self.assertEqual(len(und), 6)
                for sym in und:
                    self.assertIn(sym, defined, f'Missing donor DisplayEngineWrapperOH export: {sym}')

    def test_nine_host_hcb_and_u3_init_cfg(self):
        cfg = json.loads(CODEC_CFG.read_text(encoding='utf-8'))
        jobs = {j['name']: j for j in cfg['jobs']}
        self.assertIn('post-fs-data', jobs)
        self.assertIn('param:marble-u3-media', jobs)
        self.assertEqual(jobs['param:marble-u3-media']['condition'], 'marble.debug.start.u3_media=true')
        self.assertIn('start codec_host', jobs['param:marble-u3-media']['cmds'])
        self.assertIn('start display_engine_service', jobs['param:marble-u3-media']['cmds'])

        svcs = {s['name']: s for s in cfg['services']}
        self.assertEqual(set(svcs), {'codec_host', 'display_engine_service'})
        self.assertEqual(svcs['codec_host']['path'], ['/vendor/bin/hdf_devhost', '-i', '8', '-n', 'codec_host'])
        self.assertEqual(svcs['codec_host']['uid'], 'codec_host')
        self.assertEqual(svcs['codec_host']['gid'], ['codec_host', 'vcodec', 'dev_dma_heap', 'graphics'])
        self.assertEqual(svcs['codec_host']['sandbox'], 0)
        self.assertEqual(svcs['codec_host']['disabled'], 1)

        de = svcs['display_engine_service']
        self.assertEqual(de['path'], ['/system/bin/sa_main', '/system/profile/display_engine_service.json'])
        self.assertEqual(de['uid'], 'display_engine_service')
        self.assertEqual(de['gid'], ['servicectrl', 'system', 'graphics', 'dfs'])
        self.assertEqual(de['apl'], 'system_basic')
        self.assertEqual(de['secon'], 'u:r:display_engine_service:s0')
        self.assertEqual(len(de['permission']), 6)
        self.assertEqual(de['sandbox'], 0)
        self.assertEqual(de['disabled'], 1)

        if not DONOR_FS.is_dir():
            self.skipTest(f'Missing donor fs: {DONOR_FS}')
        h8 = p4.build_phase4_hdf_hcb(
            wifi_chip_donor=DONOR_FS,
            wpa_on_demand_donor=DONOR_FS,
            a2dp_donor=DONOR_FS,
            ril_donor=DONOR_FS,
            location_host=True,
        )
        h9 = p4.build_phase4_hdf_hcb(
            wifi_chip_donor=DONOR_FS,
            wpa_on_demand_donor=DONOR_FS,
            a2dp_donor=DONOR_FS,
            ril_donor=DONOR_FS,
            location_host=True,
            displayengine=True,
            codec_image_host=True,
        )
        self.assertGreater(len(h9), len(h8))
        for tok in (
            b'displayengine_device\x00',
            b'libdisplayengine_driver.z.so\x00',
            b'display_engine_wrapper_service\x00',
            b'codec_host\x00',
            b'libcodec_image_driver.z.so\x00',
            b'codec_image_service\x00',
            b'image_codec_config\x00',
            b'image_codec_capabilities\x00',
            b'supportPixelFmts\x00',
        ):
            self.assertIn(tok, h9)
        self.assertGreater(h9.index(b'codec_host\x00'), h9.index(b'location_host\x00'))
        self.assertGreater(h9.index(b'display_engine_wrapper_service\x00'), h9.index(b'display_composer_service\x00'))
        self.assertLess(h9.index(b'display_engine_wrapper_service\x00'), h9.index(b'user_auth_interface_service\x00'))

    def test_packager_audio_safety_and_image_roundtrip(self):
        # Strict nighttime speaker gain guard.
        self.assertIn('0.03f', (ROOT / 'compat/audio/audio_vdi.c').read_text(encoding='utf-8'))
        self.assertIn('0.03f', (ROOT / 'compat/audio/pal_server.c').read_text(encoding='utf-8'))

        spec = importlib.util.spec_from_file_location('build_u3', PACKAGER)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        self.assertEqual(len(mod.GUARD_MEMBERS), 18)

        sample_init = b'{\n  "import": [\n    "/compat/etc/init-u2-location-marble.cfg"\n  ],\n  "services": []\n}\n'
        p_init = mod.patch_init_debug(sample_init)
        doc = json.loads(p_init.decode('utf-8'))
        self.assertIn('/compat/etc/init-u3-codec-display-marble.cfg', doc['import'])
        self.assertLess(len(p_init), 32768)
        with self.assertRaises(ValueError):
            mod.patch_init_debug(p_init)

        sample_stage = (
            b'#!/bin/sh\nset -eu\n'
            b"printf '%s\\n' '4005 65936 65952 65962 66006 66048 70634' > \"$R/compat/etc/sceneboard-optional-sa.list\"\n"
            + mod.STAGE_ANCHOR + b'\n'
        )
        p_stage = mod.patch_stage(sample_stage)
        self.assertNotIn(b'\r', p_stage)
        self.assertNotIn(b'65962', p_stage)
        self.assertNotIn(b'bind_once', p_stage)
        for req in (
            b'MARBLE_U3_STAGE_READY',
            b'u3_bind()',
            b'libjpeg_vdi_impl.z.so',
            b'libdisplayengine_module.so',
            b'native_venus_v4l2_probe',
            b'init-u3-codec-display-marble.cfg',
            b'persist.multimedia.image.hardwaredecode.enabled=true',
            b'u3_bind "$R/compat/lib64/libjpeg_vdi_impl.z.so" "$R/vendor/lib64/libjpeg_vdi_impl.z.so"',
        ):
            self.assertIn(req, p_stage)
        with self.assertRaises(ValueError):
            mod.patch_stage(p_stage)

        sample_start = b'#!/bin/sh\n' + mod.START_SERVICES_ANCHOR + b'\n echo ok\ndone\n'
        p_start = mod.patch_start_phase4(sample_start)
        self.assertNotIn(b'skipped ', p_start)
        self.assertIn(b'marble.debug.start.u3_media', p_start)
        self.assertIn(b'marble.debug.skip.u3_media', p_start)
        with self.assertRaises(ValueError):
            mod.patch_start_phase4(p_start)

        report_path = U3_BUILD / 'h20-u3-media-display-report.json'
        if report_path.is_file() and BASE_U2_IMG.is_file():
            rep = json.loads(report_path.read_text(encoding='utf-8'))
            self.assertEqual(rep['base']['sha256'], mod.BASE_SHA)
            self.assertEqual(len(rep['guard_members_verified']), 18)
            cand_path = Path(rep['candidate']['path'])
            self.assertTrue(cand_path.is_file())

            _, r = mod.load_oracle()
            img_bytes = r.read_original(cand_path)
            lay = r.layout(img_bytes)
            with tempfile.TemporaryDirectory(prefix='u3_inspect_') as tmp:
                got, *_ = r.extract_entries(cand_path, img_bytes, lay, Path(tmp))
                init_doc = json.loads(got[mod.INIT_DEBUG_MEMBER][1].decode('utf-8'))
                self.assertIn('/compat/etc/init-u3-codec-display-marble.cfg', init_doc['import'])

                hcb_bytes = got[mod.HCB_MEMBER][1]
                self.assertIn(b'display_engine_wrapper_service\x00', hcb_bytes)
                self.assertIn(b'codec_host\x00', hcb_bytes)
                self.assertIn(b'codec_image_service\x00', hcb_bytes)

                stage_txt = got[mod.STAGE_MEMBER][1].decode('utf-8')
                subprocess.run(['sh', '-n'], input=stage_txt.encode('utf-8'), check=True)
                self.assertIn('MARBLE_U3_STAGE_READY', stage_txt)
                u3_stage_block = stage_txt[stage_txt.index('MARBLE_U3_MEDIA_DISPLAY_STAGE_BEGIN'):stage_txt.index('MARBLE_U3_MEDIA_DISPLAY_STAGE_END')]
                self.assertNotIn('bind_once', u3_stage_block)
                self.assertIn('u3_bind()', u3_stage_block)
                opt_line = [ln for ln in stage_txt.splitlines() if 'sceneboard-optional-sa.list' in ln][0]
                self.assertNotIn('65962', opt_line)

                sp4_txt = got[mod.START_PHASE4_MEMBER][1].decode('utf-8')
                subprocess.run(['sh', '-n'], input=sp4_txt.encode('utf-8'), check=True)
                self.assertIn('marble.debug.start.u3_media', sp4_txt)
                u3_start_block = sp4_txt[sp4_txt.index('MARBLE_U3_MEDIA_DISPLAY_START_BEGIN'):sp4_txt.index('MARBLE_U3_MEDIA_DISPLAY_START_END')]
                self.assertNotIn('skipped ', u3_start_block)

                for req in (
                    'hmos-compat-marble/graphics/libjpeg_vdi_impl.z.so',
                    'hmos-compat-marble/graphics/libdisplayengine_module.so',
                    'hmos-compat-marble/graphics/native_venus_v4l2_probe',
                    'hmos-compat-marble/graphics/init-u3-codec-display-marble.cfg',
                ):
                    self.assertIn(req, got, f'Missing {req} in U3.1 image')


if __name__ == '__main__':
    unittest.main()
