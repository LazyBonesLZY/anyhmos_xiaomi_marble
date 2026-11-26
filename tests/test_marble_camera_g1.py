#!/usr/bin/env python3
"""Offline unit & packaging verification for Batch G1 (SM7475 Camera kernel stack + LX7 ICP FW + 4-sensor CCI probe)."""
from __future__ import annotations

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

import build_marble_h20_g1_camera_hw as pkg

PROBE_C = ROOT / 'compat/camera/marble_camera_probe.c'
UAPI_DIR = ROOT / 'work/marble-kernel-work/uapi-headers/include/camera'
LLVM_READELF = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-readelf'


class MarbleBatchG1Tests(unittest.TestCase):
    def test_g1_host_camera_probe_self_test(self):
        with tempfile.TemporaryDirectory() as td:
            probe_host = Path(td) / 'marble_camera_probe_host'
            subprocess.run(
                ['gcc', '-O2', '-Wall', '-Wextra', '-Werror', f'-I{UAPI_DIR}',
                 str(PROBE_C), '-o', str(probe_host)],
                check=True,
            )
            res = subprocess.run([str(probe_host), '--self-test'], check=True, capture_output=True, text=True)
            self.assertIn('MARBLE_CAMERA_PROBE_SELF_TEST_OK=1', res.stdout)

    def test_g1_verified_four_sensor_table(self):
        src = PROBE_C.read_text(encoding='utf-8')
        for marker in (
            '"ov64b40_wide"', '0x20, 0x300a', '0x5664',
            '"s5k3p9_front"', '0x20, 0x0000', '0x3109',
            '"imx355_ultra"', '0x34, 0x0016', '0x0355',
            '"gc02m1_macro"', '0x6e, 0x00f0', '0x02e0',
        ):
            self.assertIn(marker, src)

    def test_g1_ohos_probe_elf_and_script(self):
        probe_bin = pkg.G1_BUILD / 'marble_camera_probe'
        self.assertTrue(probe_bin.is_file(), f'Missing G1 OHOS camera probe: {probe_bin}')
        dyn = subprocess.check_output([str(LLVM_READELF), '-d', str(probe_bin)], text=True)
        needed = re.findall(r'\(NEEDED\)\s+Shared library:\s+\[([^\]]+)\]', dyn)
        self.assertEqual(needed, ['libc.so'])

        script_raw = pkg.CAM_SCRIPT.read_bytes()
        self.assertNotIn(b'\r', script_raw)
        subprocess.run(['sh', '-n'], input=script_raw, check=True)
        script_txt = script_raw.decode('utf-8')
        for marker in (
            'qcom_ipc_lite.ko', 'synx-driver.ko', 'leds-qti-flash.ko',
            'camcc-waipio.ko', 'camera.ko', 'CAMERA_ICP.mdt',
            'marble_camera_probe --all', 'CAMERA_G1_DONE=1', '*,camera,*',
        ):
            self.assertIn(marker, script_txt)

    def test_g1_icp_fw_tarball(self):
        self.assertTrue(pkg.ICP_FW_TAR.is_file(), f'Missing ICP FW tarball: {pkg.ICP_FW_TAR}')
        with tarfile.open(pkg.ICP_FW_TAR, mode='r:gz') as tf:
            names = set(tf.getnames())
        self.assertEqual(len(names), 23)
        self.assertIn('CAMERA_ICP.mdt', names)
        self.assertIn('CAMERA_ICP.b00', names)
        self.assertIn('CAMERA_ICP.b08', names)
        self.assertIn('CAMERA_ICP.b21', names)
        self.assertNotIn('CAMERA_ICP.elf', names)

    def test_g1_package_report_and_guards(self):
        self.assertEqual(len(pkg.GUARD_MEMBERS), 47)
        report_path = pkg.G1_BUILD / 'g1-build-report.json'
        if not report_path.is_file():
            self.skipTest('g1-build-report.json not generated yet')
        report = json.loads(report_path.read_text(encoding='utf-8'))
        cand = Path(report['candidate']['path'])
        if not cand.is_file():
            self.skipTest(f'Candidate image missing: {cand}')

        self.assertEqual(report['base']['sha256'], pkg.BASE_SHA)
        self.assertEqual(len(report['guard_members_verified']), 47)
        self.assertEqual(report['entries_after'], report['entries_before'] + 3)
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
            self.assertIn('MARBLE_G1_CAMERA_HW_STAGE_BEGIN', stage_txt)
            post_txt = got[pkg.POSTBOOT_MEMBER][1].decode('utf-8')
            self.assertIn('start-camera-g1-marble.sh', post_txt)
            manifest = archive.parse(got[r.MANIFEST][1])
            r.manifest_holds(got, manifest)


if __name__ == '__main__':
    unittest.main()
