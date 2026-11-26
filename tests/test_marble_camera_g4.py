#!/usr/bin/env python3
"""Offline unit, all-4 Bayer/DCT-JPEG, ABI, and packaging verification for Batch G4 (All-4 Physical Optical Cameras)."""
from __future__ import annotations

import io
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

import build_marble_h20_g4_all_cameras as pkg

ISP_CAP_C = ROOT / 'compat/camera/marble_isp_capture.c'
HW_CLI_C = ROOT / 'compat/camera/marble_camera_hw_capture.c'
TABLES_H = ROOT / 'compat/camera/marble_sensor_tables.h'
UAPI_CAM = ROOT / 'work/marble-kernel-work/uapi-headers/include/camera'
LLVM_READELF = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-readelf'
LLVM_NM = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-nm'


class MarbleBatchG4Tests(unittest.TestCase):
    def test_g4_sensor_tables_and_host_selftest_with_bayer_orders(self):
        self.assertTrue(TABLES_H.is_file())
        tbl_txt = TABLES_H.read_text(encoding='utf-8')
        for sym in ('k_gc02m1_init_regs', 'k_s5k3p9_init_pre', 'k_s5k3p9_burst_6f12',
                    'k_s5k3p9_mode_2304x1728', 'k_ov64b40_init_regs', 'k_ov64b40_mode_1920x1080'):
            self.assertIn(sym, tbl_txt)

        with tempfile.TemporaryDirectory() as td:
            tdp = Path(td)
            host_bin = tdp / 'hw_capture_g4_host'
            jpg_out = tdp / 'selftest.jpg'
            subprocess.run(
                ['gcc', '-O2', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                 f'-I{UAPI_CAM}', f'-I{ROOT / "compat/camera"}',
                 str(ISP_CAP_C), str(HW_CLI_C), '-o', str(host_bin)],
                check=True,
            )
            res = subprocess.run(
                [str(host_bin), '--self-test', '--dump-jpeg', str(jpg_out)],
                check=True, capture_output=True, text=True,
            )
            self.assertIn('MARBLE_HW_CAPTURE_SELF_TEST_OK=1', res.stdout)
            data = jpg_out.read_bytes()
            self.assertTrue(data.startswith(b'\xff\xd8\xff\xe0') and data.endswith(b'\xff\xd9'))
            try:
                from PIL import Image
                im = Image.open(io.BytesIO(data))
                im.load()
                self.assertEqual(im.size, (64, 48))
            except ImportError:
                pass

    def test_g4_vdi_probes_and_cli_elf_abi_parity(self):
        vdi_so = pkg.G4_BUILD / 'libmarble_camera_vdi_impl.z.so'
        hw_bin = pkg.G4_BUILD / 'marble_camera_hw_capture'
        hdi_probe = pkg.G4_BUILD / 'native_camera_hdi_probe'
        fwk_probe = pkg.G4_BUILD / 'ohcamera_fwk_probe'
        for p in (vdi_so, hw_bin, hdi_probe, fwk_probe):
            self.assertTrue(p.is_file(), f'Missing G4 artifact: {p}')

        dyn = subprocess.check_output([str(LLVM_READELF), '-d', str(vdi_so)], text=True)
        self.assertEqual(re.findall(r'\(SONAME\)\s+Library soname:\s+\[([^\]]+)\]', dyn),
                         ['libmarble_camera_vdi_impl.z.so'])
        nm = subprocess.check_output([str(LLVM_NM), '-D', str(vdi_so)], text=True)
        for sym in (' hdfVdiDesc', ' marble_hw_session_open', ' marble_bayer_to_rgb24_resize_ex',
                    ' marble_hw_src_name', ' marble_encode_baseline_jpeg_rgb24'):
            self.assertIn(sym, nm)

        syms = subprocess.check_output([str(LLVM_READELF), '-sW', str(vdi_so)], text=True)
        sizes = {}
        for line in syms.splitlines():
            m = re.match(r'\s*\d+:\s+[0-9a-f]+\s+(\d+)\s+\S+\s+\S+\s+\S+\s+\S+\s+(_ZTV\S+)', line)
            if m:
                sizes[m.group(2)] = int(m.group(1))
        self.assertEqual([sz for n, sz in sizes.items() if 'MarbleCameraHostVdi' in n], [224])
        self.assertEqual([sz for n, sz in sizes.items() if 'MarbleCameraDeviceVdi' in n], [232])
        self.assertEqual([sz for n, sz in sizes.items() if 'MarbleStreamOperatorVdi' in n], [256])

        hw_dyn = subprocess.check_output([str(LLVM_READELF), '-d', str(hw_bin)], text=True)
        self.assertEqual(re.findall(r'\(NEEDED\)\s+Shared library:\s+\[([^\]]+)\]', hw_dyn), ['libc.so'])

        fwk_dyn = subprocess.check_output([str(LLVM_READELF), '-d', str(fwk_probe)], text=True)
        fwk_needed = re.findall(r'\(NEEDED\)\s+Shared library:\s+\[([^\]]+)\]', fwk_dyn)
        self.assertIn('libohcamera.so', fwk_needed)

    def test_g4_package_report_and_54_guards(self):
        self.assertEqual(len(pkg.GUARD_MEMBERS), 54)
        report_path = pkg.G4_BUILD / 'g4-build-report.json'
        if not report_path.is_file():
            self.skipTest('g4-build-report.json not generated yet')
        report = json.loads(report_path.read_text(encoding='utf-8'))
        cand = Path(report['candidate']['path'])
        if not cand.is_file():
            self.skipTest(f'Candidate image missing: {cand}')

        self.assertEqual(report['base']['sha256'], pkg.BASE_SHA)
        self.assertEqual(len(report['guard_members_verified']), 54)
        self.assertEqual(report['entries_after'], report['entries_before'] + 1)

        archive, r = pkg.load_oracle()
        img = r.read_original(cand)
        layout = r.layout(img)
        avb = r.verify_existing_avb(img, layout['payload_end'])
        self.assertIn('footer_masked', avb)

        with tempfile.TemporaryDirectory() as td:
            got, *_ = r.extract_entries(cand, img, layout, Path(td))
            for rel, meta in report['staged'].items():
                self.assertEqual(r.sha(got[pkg.PREFIX + rel][1]), meta['sha256'])
            stage_txt = got[pkg.STAGE_MEMBER][1].decode('utf-8')
            self.assertIn('ohcamera_fwk_probe', stage_txt)
            subprocess.run(['sh', '-n'], input=stage_txt.encode('utf-8'), check=True)
            manifest = archive.parse(got[r.MANIFEST][1])
            r.manifest_holds(got, manifest)


if __name__ == '__main__':
    unittest.main()
