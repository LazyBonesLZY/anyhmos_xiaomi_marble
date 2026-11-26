#!/usr/bin/env python3
"""Offline unit tests and artifact checks for Batch U2.1 (Native HDF location_host + LPM GNSS, surface_post fastpath, Venus VPU)."""
import importlib.util
import json
import re
import subprocess
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / 'tools/packaging'
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import build_marble_phase4_images as p4

U2_BUILD = ROOT / 'work/marble-u2-build'
DONOR_FS = ROOT / 'work/donor-rom/ADY-AL10-207.0.0.111-SP10/fs'
VENUS_SH = ROOT / 'devices/marble/runtime/codec/start-venus-marble.sh'
POSTBOOT_SH = ROOT / 'devices/marble/runtime/telephony/start-u2-postboot-marble.sh'
LOC_CFG = ROOT / 'devices/marble/runtime/telephony/init-u2-location-marble.cfg'
PACKAGER = TOOLS / 'build_marble_h20_u2_loc_media.py'
BASE_U1_IMG = Path('/mnt/e/marble-tmp/vendor_boot_h20_u1_ux_security_7689087f.img')

EXPECTED_VPU_FW = (
    'vpu20_4v.b00',
    'vpu20_4v.b01',
    'vpu20_4v.b02',
    'vpu20_4v.b03',
    'vpu20_4v.b04',
    'vpu20_4v.b05',
    'vpu20_4v.b06',
    'vpu20_4v.b07',
    'vpu20_4v.b08',
    'vpu20_4v.b09',
    'vpu20_4v.b18',
    'vpu20_4v.b19',
    'vpu20_4v.mbn',
    'vpu20_4v.mdt',
)


class TestBatchU2(unittest.TestCase):
    def test_gnss_publisher_host_and_elf(self):
        gnss_c = ROOT / 'compat/location/native_gnss_hdi_publisher.c'
        src = gnss_c.read_text(encoding='utf-8')
        self.assertIn('QMI_LOC_INJECT_UTC_TIME', src)
        self.assertIn('build_loc_inject_utc_time_tlv', src)
        self.assertIn('GNSS_HDI_LOC_DISPATCH', src)

        with tempfile.TemporaryDirectory(prefix='u2_gnss_') as tmp:
            tp = Path(tmp)
            b1 = tp / 'gnss_host'
            subprocess.run(
                ['gcc', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread', '-DGNSS_HDI_HOST_TEST',
                 str(gnss_c), '-o', str(b1), '-lm'],
                check=True,
            )
            r1 = subprocess.run([str(b1)], check=True, capture_output=True, text=True)
            self.assertIn('GNSS_HDI_HOST_TEST_OK=1', r1.stdout)

            hdf_inc = ROOT / 'work/upstream/drivers_hdf_core/interfaces/inner_api'
            if hdf_inc.is_dir():
                b2 = tp / 'gnss_host_hdr'
                subprocess.run(
                    ['gcc', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread', '-DGNSS_HDI_HOST_TEST',
                     f'-I{hdf_inc}/ipc', f'-I{hdf_inc}/utils', f'-I{hdf_inc}/core', f'-I{hdf_inc}/osal/uhdf',
                     str(gnss_c), '-o', str(b2), '-lm'],
                    check=True,
                )
                r2 = subprocess.run([str(b2)], check=True, capture_output=True, text=True)
                self.assertIn('GNSS_HDI_HOST_TEST_OK=1', r2.stdout)

        gnss_bin = U2_BUILD / 'native_gnss_hdi_publisher'
        self.assertTrue(gnss_bin.is_file(), f'Missing OHOS gnss binary: {gnss_bin}')
        gnss_drv = U2_BUILD / 'libmarble_gnss_hdi_driver.z.so'
        self.assertTrue(gnss_drv.is_file(), f'Missing OHOS gnss HDF driver so: {gnss_drv}')
        readelf_tool = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-readelf'
        if readelf_tool.is_file():
            dyn = subprocess.check_output([str(readelf_tool), '-d', str(gnss_bin)], text=True)
            needed = re.findall(r'\(NEEDED\)\s+Shared library:\s+\[([^\]]+)\]', dyn)
            self.assertEqual(needed, ['libc.so'])
            interp = subprocess.check_output([str(readelf_tool), '-l', str(gnss_bin)], text=True)
            self.assertIn('/lib/ld-musl-aarch64.so.1', interp)

            drv_dyn = subprocess.check_output([str(readelf_tool), '-d', str(gnss_drv)], text=True)
            drv_needed = re.findall(r'\(NEEDED\)\s+Shared library:\s+\[([^\]]+)\]', drv_dyn)
            self.assertEqual(drv_needed, ['libc.so'])
            self.assertIn('[libmarble_gnss_hdi_driver.z.so]', drv_dyn)
            drv_syms = subprocess.check_output([str(readelf_tool), '-Ws', '--wide', str(gnss_drv)], text=True)
            self.assertRegex(drv_syms, r'\bOBJECT\s+GLOBAL\s+DEFAULT\s+\d+\s+driverDesc\b')

    def test_eight_host_hcb_and_location_init_cfg(self):
        cfg = json.loads(LOC_CFG.read_text(encoding='utf-8'))
        svcs = {s['name']: s for s in cfg['services']}
        self.assertEqual(set(svcs), {'location_host'})
        loc = svcs['location_host']
        self.assertIs(loc['ondemand'], True)
        self.assertEqual(loc['path'], ['/vendor/bin/hdf_devhost', '-i', '7', '-n', 'location_host'])
        self.assertEqual(loc['uid'], 'location_host')
        self.assertEqual(loc['gid'], ['location_host', 'system', 'gps', 'radio'])
        self.assertEqual(loc['sandbox'], 0)
        self.assertEqual(loc['disabled'], 1)
        self.assertIn('mkdir /data/local/tmp/gnss-hdi 0775 location_host gps', cfg['jobs'][0]['cmds'])

        if not DONOR_FS.is_dir():
            self.skipTest(f'Missing donor fs: {DONOR_FS}')
        h7 = p4.build_phase4_hdf_hcb(
            wifi_chip_donor=DONOR_FS,
            wpa_on_demand_donor=DONOR_FS,
            a2dp_donor=DONOR_FS,
            ril_donor=DONOR_FS,
        )
        h8 = p4.build_phase4_hdf_hcb(
            wifi_chip_donor=DONOR_FS,
            wpa_on_demand_donor=DONOR_FS,
            a2dp_donor=DONOR_FS,
            ril_donor=DONOR_FS,
            location_host=True,
        )
        self.assertGreater(len(h8), len(h7))
        for tok in (
            b'location_host\x00',
            b'libmarble_gnss_hdi_driver.z.so\x00',
            b'gnss_interface_service\x00',
            b'\x02preload\x00\x10\x02',
        ):
            self.assertIn(tok, h8)
        self.assertGreater(h8.index(b'location_host\x00'), h8.index(b'riladapter_host\x00'))

    def test_surface_post_fastpath_and_symbols(self):
        sp_src = (ROOT / 'compat/services/surface_post.cpp').read_text(encoding='utf-8')
        self.assertIn('VerboseDiagEnabled()', sp_src)
        self.assertIn('hmos-render-verbose', sp_src)
        reg_idx = sp_src.index('HmosRegisterPostV2(')
        lambda_idx = sp_src.index('[post, owner, ownerInline](', reg_idx)
        self.assertIn('hmos-gpu-owner-cleanup', sp_src[reg_idx:lambda_idx])
        self.assertNotIn('hmos-gpu-owner-cleanup', sp_src[lambda_idx:])

        so_path = U2_BUILD / 'libhmos_surface_diagnostic.so'
        self.assertTrue(so_path.is_file(), f'Missing rebuilt surface diagnostic so: {so_path}')
        nm_tool = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-nm'
        if nm_tool.is_file():
            syms = subprocess.check_output([str(nm_tool), '-D', '--defined-only', str(so_path)], text=True)
            for sym in (
                'HmosObserveBufferImport',
                'HmosResolveBufferImport',
                '_ZN4OHOS5Rosen17NativeBufferUtils13DeleteVkImageEPv',
                '_ZN4OHOS5Rosen17NativeBufferUtils27IsYcbcrModelOrRangeNotEqualEP15OH_NativeBuffer29VkSamplerYcbcrModelConversion19VkSamplerYcbcrRange',
                '_ZN4OHOS5Rosen17NativeBufferUtils34MakeBackendTextureFromNativeBufferEP18NativeWindowBufferiib',
                '_ZN4OHOS5Rosen7Drawing10GPUContext16RegisterPostFuncERKNSt3__h8functionIFvRKNS4_IFvvEEEEEE',
                '_ZN4OHOS5Rosen7Drawing5Image16BuildFromTextureERNS1_10GPUContextERKNS1_11TextureInfoENS1_13TextureOriginENS1_12BitmapFormatERKNSt3__h10shared_ptrINS1_10ColorSpaceEEEPFvPvESG_b',
                '_ZN4OHOS5Rosen7Drawing5ImageD0Ev',
                '_ZN4OHOS5Rosen7Drawing5ImageD1Ev',
                '_ZN4OHOS5Rosen7Drawing7Surface22MakeFromBackendTextureEPNS1_10GPUContextERKNS1_11TextureInfoENS1_13TextureOriginEiNS1_9ColorTypeENSt3__h10shared_ptrINS1_10ColorSpaceEEEPFvPvESE_',
            ):
                self.assertIn(sym, syms)

    def test_vpu_tarball_and_scripts(self):
        tb = U2_BUILD / 'vpu20_4v-fw.tar.gz'
        self.assertTrue(tb.is_file(), f'Missing VPU firmware tarball: {tb}')
        with tarfile.open(tb, 'r:gz') as tf:
            names = {n.lstrip('./') for n in tf.getnames() if n not in ('.', './')}
            self.assertEqual(names, set(EXPECTED_VPU_FW))

        for sh_path in (VENUS_SH, POSTBOOT_SH):
            txt = sh_path.read_text(encoding='utf-8')
            self.assertTrue(txt.startswith('#!/busybox sh\n'))
            subprocess.run(['sh', '-n', str(sh_path)], check=True)

        post_txt = POSTBOOT_SH.read_text(encoding='utf-8')
        self.assertNotIn('--check', post_txt)
        self.assertIn('start-venus-marble.sh', post_txt)
        self.assertIn('start-modem-f3-marble.sh --tftp', post_txt)
        self.assertIn('start-modem-f1-marble.sh', post_txt)
        self.assertIn('--set-mode lpm', post_txt)
        self.assertIn('op_mode=1(low_power)', post_txt)
        self.assertIn('U2_GNSS_MODE=hdf_location_host', post_txt)
        self.assertNotIn('--set-mode online', post_txt)
        self.assertNotIn('marble.debug.start.gnss', post_txt)
        self.assertNotIn('pidof native_gnss_hdi_publisher', post_txt)

    def test_packager_audio_safety_and_image_roundtrip(self):
        # Strict nighttime speaker gain guard.
        self.assertIn('0.03f', (ROOT / 'compat/audio/audio_vdi.c').read_text(encoding='utf-8'))
        self.assertIn('0.03f', (ROOT / 'compat/audio/pal_server.c').read_text(encoding='utf-8'))

        spec = importlib.util.spec_from_file_location('build_u2', PACKAGER)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)

        sample_init = b'{\n  "import": [\n    "/compat/etc/init-u1-services-marble.cfg"\n  ],\n  "services": []\n}\n'
        p_init = mod.patch_init_debug(sample_init)
        doc = json.loads(p_init.decode('utf-8'))
        self.assertIn('/compat/etc/init-u2-location-marble.cfg', doc['import'])
        self.assertLess(len(p_init), 32768)
        with self.assertRaises(ValueError):
            mod.patch_init_debug(p_init)

        sample_stage = b'#!/bin/sh\nset -eu\n' + mod.STAGE_ANCHOR + b'\n'
        p_stage = mod.patch_stage(sample_stage)
        self.assertNotIn(b'\r', p_stage)
        for req in (
            b'MARBLE_U2_STAGE_READY',
            b'vpu20_4v-fw.tar.gz',
            b'libmarble_gnss_hdi_driver.z.so',
            b'init-u2-location-marble.cfg',
            b'marble_location.para',
            b'const.location.gps.support=true',
            b'const.location.geocode_service_name=com.huawei.hmos.location',
        ):
            self.assertIn(req, p_stage)
        with self.assertRaises(ValueError):
            mod.patch_stage(p_stage)

        sample_start = b'#!/bin/sh\n' + mod.START_END_ANCHOR + b'\n'
        p_start = mod.patch_start_phase4(sample_start)
        self.assertIn(b'U2_POSTBOOT_LAUNCHED=1', p_start)
        self.assertNotIn(b'marble.debug.start.gnss', p_start)
        self.assertNotIn(b'U2_GNSS_EARLY_REQUESTED', p_start)
        with self.assertRaises(ValueError):
            mod.patch_start_phase4(p_start)

        report_path = U2_BUILD / 'u2-build-report.json'
        if report_path.is_file() and BASE_U1_IMG.is_file():
            rep = json.loads(report_path.read_text(encoding='utf-8'))
            self.assertEqual(rep['base']['sha256'], mod.BASE_SHA)
            self.assertEqual(len(rep['guard_members_verified']), len(mod.GUARD_MEMBERS))
            cand_path = Path(rep['candidate']['path'])
            self.assertTrue(cand_path.is_file())

            oracle_spec = importlib.util.spec_from_file_location('arc_u2', mod.ORACLE)
            arc = importlib.util.module_from_spec(oracle_spec)
            oracle_spec.loader.exec_module(arc)
            r = arc.load_tools()
            img_bytes = r.read_original(cand_path)
            lay = r.layout(img_bytes)
            with tempfile.TemporaryDirectory(prefix='u2_inspect_') as tmp:
                got, _, _, _, _ = r.extract_entries(cand_path, img_bytes, lay, Path(tmp))
                init_doc = json.loads(got[mod.INIT_DEBUG_MEMBER][1].decode('utf-8'))
                self.assertIn('/compat/etc/init-u2-location-marble.cfg', init_doc['import'])

                hcb_bytes = got[mod.HCB_MEMBER][1]
                self.assertIn(b'location_host\x00', hcb_bytes)
                self.assertIn(b'gnss_interface_service\x00', hcb_bytes)
                self.assertIn(b'libmarble_gnss_hdi_driver.z.so\x00', hcb_bytes)

                stage_txt = got[mod.STAGE_MEMBER][1].decode('utf-8')
                subprocess.run(['sh', '-n'], input=stage_txt.encode('utf-8'), check=True)
                self.assertIn('MARBLE_U2_STAGE_READY', stage_txt)
                self.assertIn('marble_location.para', stage_txt)

                sp4_txt = got[mod.START_PHASE4_MEMBER][1].decode('utf-8')
                subprocess.run(['sh', '-n'], input=sp4_txt.encode('utf-8'), check=True)
                self.assertIn('U2_POSTBOOT_LAUNCHED=1', sp4_txt)
                self.assertNotIn('marble.debug.start.gnss', sp4_txt)

                for req in (
                    'hmos-compat-marble/graphics/native_gnss_hdi_publisher',
                    'hmos-compat-marble/graphics/libmarble_gnss_hdi_driver.z.so',
                    'hmos-compat-marble/graphics/init-u2-location-marble.cfg',
                    'hmos-compat-marble/graphics/libhmos_surface_diagnostic.so',
                    'hmos-compat-marble/graphics/vpu20_4v-fw.tar.gz',
                    'hmos-compat-marble/graphics/start-venus-marble.sh',
                    'hmos-compat-marble/graphics/start-u2-postboot-marble.sh',
                ):
                    self.assertIn(req, got, f'Missing {req} in U2 image')


if __name__ == '__main__':
    unittest.main()
