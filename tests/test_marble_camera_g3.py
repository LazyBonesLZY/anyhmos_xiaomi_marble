#!/usr/bin/env python3
"""Offline unit, DSP/DCT-JPEG, ABI, and packaging verification for Batch G3 (SM7475 Real Silicon Camera Capture)."""
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

import build_marble_h20_g3_camera_capture as pkg

ISP_CAP_C = ROOT / 'compat/camera/marble_isp_capture.c'
HW_CLI_C = ROOT / 'compat/camera/marble_camera_hw_capture.c'
UAPI_CAM = ROOT / 'work/marble-kernel-work/uapi-headers/include/camera'
LLVM_READELF = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-readelf'
LLVM_NM = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-nm'


class MarbleBatchG3Tests(unittest.TestCase):
    def test_g3_dsp_raw10_demosaic_and_dct_jpeg_roundtrip(self):
        with tempfile.TemporaryDirectory() as td:
            tdp = Path(td)
            host_bin = tdp / 'hw_capture_host'
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
            # Must contain DQT, SOF0, DHT, SOS markers for real Baseline DCT JPEG
            for marker in (b'\xff\xdb', b'\xff\xc0', b'\xff\xc4', b'\xff\xda'):
                self.assertIn(marker, data)
            try:
                from PIL import Image
                im = Image.open(io.BytesIO(data))
                im.load()
                self.assertEqual(im.size, (64, 48))
                tl, tr, bl = im.getpixel((4, 4)), im.getpixel((48, 4)), im.getpixel((4, 36))
                self.assertGreater(tl[0], tl[1] + 80)
                self.assertGreater(tr[1], tr[0] + 80)
                self.assertGreater(bl[2], bl[0] + 80)
            except ImportError:
                pass

    def test_g3_vdi_and_cli_elf_abi_parity(self):
        vdi_so = pkg.G3_BUILD / 'libmarble_camera_vdi_impl.z.so'
        hw_bin = pkg.G3_BUILD / 'marble_camera_hw_capture'
        probe_bin = pkg.G3_BUILD / 'native_camera_hdi_probe'
        for p in (vdi_so, hw_bin, probe_bin):
            self.assertTrue(p.is_file(), f'Missing G3 artifact: {p}')

        # VDI SONAME, vtable sizes (224/232/256), hdfVdiDesc + real HW capture symbols
        dyn = subprocess.check_output([str(LLVM_READELF), '-d', str(vdi_so)], text=True)
        self.assertEqual(re.findall(r'\(SONAME\)\s+Library soname:\s+\[([^\]]+)\]', dyn),
                         ['libmarble_camera_vdi_impl.z.so'])
        nm = subprocess.check_output([str(LLVM_NM), '-D', str(vdi_so)], text=True)
        for sym in (' hdfVdiDesc', ' marble_hw_session_open', ' marble_hw_session_grab_rgb24',
                    ' marble_encode_baseline_jpeg_rgb24'):
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

        # Standalone capture CLI has minimal libc.so-only NEEDED
        hw_dyn = subprocess.check_output([str(LLVM_READELF), '-d', str(hw_bin)], text=True)
        self.assertEqual(re.findall(r'\(NEEDED\)\s+Shared library:\s+\[([^\]]+)\]', hw_dyn), ['libc.so'])

    def test_g3_package_report_and_54_guards(self):
        self.assertEqual(len(pkg.GUARD_MEMBERS), 54)
        report_path = pkg.G3_BUILD / 'g3-build-report.json'
        if not report_path.is_file():
            self.skipTest('g3-build-report.json not generated yet')
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
            self.assertIn('marble_camera_hw_capture', stage_txt)
            subprocess.run(['sh', '-n'], input=stage_txt.encode('utf-8'), check=True)
            manifest = archive.parse(got[r.MANIFEST][1])
            r.manifest_holds(got, manifest)


if __name__ == '__main__':
    unittest.main()
