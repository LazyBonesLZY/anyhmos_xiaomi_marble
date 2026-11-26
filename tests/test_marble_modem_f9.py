#!/usr/bin/env python3
"""Offline unit & packaging verification for Batch F9 (SetRadioProtocol + per-slot modemId + CurrentCellInfoVendor LTE/NR)."""
from __future__ import annotations

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

import build_marble_h20_f9_sim_cell as pkg

RIL_C = ROOT / 'compat/telephony/libril_vendor_qmi.c'
ABI_H = ROOT / 'compat/telephony/hril_vendor_abi.h'
LLVM_READELF = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-readelf'
LLVM_NM = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-nm'


class MarbleBatchF9Tests(unittest.TestCase):
    def test_f9_host_ril_self_test(self):
        with tempfile.TemporaryDirectory() as td:
            ril_host = Path(td) / 'ril_host_f9'
            subprocess.run(
                ['gcc', '-O2', '-Wall', '-Wextra', '-Werror', '-std=c11', '-DHRIL_VENDOR_HOST_TEST',
                 str(RIL_C), '-lpthread', '-o', str(ril_host)],
                check=True,
            )
            res = subprocess.run([str(ril_host)], check=True, capture_output=True, text=True)
            self.assertIn('RIL_VENDOR_QMI_HOST_TEST_OK=1', res.stdout)

    def test_f9_radio_protocol_and_cell_info_abi(self):
        abi = ABI_H.read_text(encoding='utf-8')
        for marker in (
            'HNOTI_SIM_RADIO_PROTOCOL_UPDATED = 1208,',
            'HRIL_RADIO_PROTOCOL_STATUS_SUCCESS = 1,',
            'HRIL_CELL_RAT_LTE = 5,',
            'HRIL_CELL_RAT_NR = 6,',
            'sizeof(HRilCurrentCellInfoVendor) == 56',
            'offsetof(HRilSimReq, SetRadioProtocol) == 19 * 8',
        ):
            self.assertIn(marker, abi)

        ril = RIL_C.read_text(encoding='utf-8')
        for marker in (
            'QMI_NAS_GET_CELL_LOCATION_INFO 0x0043u',
            'TASK_SIM_SET_PROTOCOL',
            '.SetRadioProtocol = v_SetRadioProtocol',
            'parse_nas_cell_location_info',
            'build_current_cell_info',
            'HNOTI_SIM_RADIO_PROTOCOL_UPDATED',
        ):
            self.assertIn(marker, ril)

    def test_f9_ohos_so_exports(self):
        ril_so = pkg.F9_BUILD / 'libril_vendor_qmi.z.so'
        self.assertTrue(ril_so.is_file(), f'Missing F9 OHOS RIL .so: {ril_so}')
        dyn = subprocess.check_output([str(LLVM_READELF), '-d', str(ril_so)], text=True)
        needed = re.findall(r'\(NEEDED\)\s+Shared library:\s+\[([^\]]+)\]', dyn)
        soname = re.findall(r'\(SONAME\)\s+Library soname:\s+\[([^\]]+)\]', dyn)
        self.assertEqual(needed, ['libc.so'])
        self.assertEqual(soname, ['libril_vendor.z.so'])
        syms = subprocess.check_output([str(LLVM_NM), '-D', '--defined-only', str(ril_so)], text=True)
        self.assertIn('RilInitOps', syms)

    def test_f9_package_report_and_guards(self):
        self.assertEqual(len(pkg.GUARD_MEMBERS), 48)
        report_path = pkg.F9_BUILD / 'f9-build-report.json'
        if not report_path.is_file():
            self.skipTest('f9-build-report.json not generated yet')
        report = json.loads(report_path.read_text(encoding='utf-8'))
        cand = Path(report['candidate']['path'])
        if not cand.is_file():
            self.skipTest(f'Candidate image missing: {cand}')

        self.assertEqual(report['base']['sha256'], pkg.BASE_SHA)
        self.assertEqual(len(report['guard_members_verified']), 48)
        self.assertEqual(report['entries_before'], report['entries_after'])
        archive, r = pkg.load_oracle()
        img = r.read_original(cand)
        layout = r.layout(img)
        avb = r.verify_existing_avb(img, layout['payload_end'])
        self.assertIn('footer_masked', avb)
        with tempfile.TemporaryDirectory() as td:
            got, *_ = r.extract_entries(cand, img, layout, Path(td))
            self.assertEqual(
                r.sha(got[pkg.PREFIX + 'graphics/libril_vendor_qmi.z.so'][1]),
                report['staged']['graphics/libril_vendor_qmi.z.so']['sha256'],
            )
            manifest = archive.parse(got[r.MANIFEST][1])
            r.manifest_holds(got, manifest)


if __name__ == '__main__':
    unittest.main()
