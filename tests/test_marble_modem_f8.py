#!/usr/bin/env python3
"""Offline unit & packaging verification for Batch F8 (OHOS RadioTech LTE=9/NR=12 + HRilDataReq V1_3/V1_4 + China carrier cust)."""
from __future__ import annotations

import io
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

import build_marble_h20_f8_ohos_cellular as pkg

RIL_C = ROOT / 'compat/telephony/libril_vendor_qmi.c'
ABI_H = ROOT / 'compat/telephony/hril_vendor_abi.h'
LLVM_READELF = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-readelf'
LLVM_NM = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-nm'


class MarbleBatchF8Tests(unittest.TestCase):
    def test_f8_host_ril_self_test(self):
        with tempfile.TemporaryDirectory() as td:
            ril_host = Path(td) / 'ril_host_f8'
            subprocess.run(
                ['gcc', '-O2', '-Wall', '-Wextra', '-Werror', '-std=c11', '-DHRIL_VENDOR_HOST_TEST',
                 str(RIL_C), '-lpthread', '-o', str(ril_host)],
                check=True,
            )
            res = subprocess.run([str(ril_host)], check=True, capture_output=True, text=True)
            self.assertIn('RIL_VENDOR_QMI_HOST_TEST_OK=1', res.stdout)

    def test_f8_ohos_radiotech_and_datareq_abi(self):
        abi = ABI_H.read_text(encoding='utf-8')
        for marker in (
            'HRIL_TECH_GSM = 1,',
            'HRIL_TECH_CDMA1X = 2,',
            'HRIL_TECH_WCDMA = 3,',
            'HRIL_TECH_TD_SCDMA = 6,',
            'HRIL_TECH_EVDO = 7,',
            'HRIL_TECH_LTE = 9,',
            'HRIL_TECH_NR = 12,',
            'HRilLinkBandwidthInfo',
            'sizeof(HRilLinkBandwidthInfo) == 36',
            'offsetof(HRilDataReq, GetLinkBandwidthInfo) == 4 * 8',
            'offsetof(HRilDataReq, SendDataPerformanceMode) == 7 * 8',
            'offsetof(HRilDataReq, SendDataSleepMode) == 8 * 8',
            'offsetof(HRilDataReq, ActivatePdpContextWithApnTypes) == 12 * 8',
            'offsetof(HRilDataReq, ActivatePdpContextWithApnTypesforSlice) == 18 * 8',
        ):
            self.assertIn(marker, abi)

        ril = RIL_C.read_text(encoding='utf-8')
        for marker in (
            'TASK_DATA_GET_LINK_BW',
            'TASK_DATA_SET_PERF_MODE',
            'TASK_DATA_SET_SLEEP_MODE',
            'is_secondary_non_internet_apn',
            '.ActivatePdpContextWithApnTypes = v_ActivatePdpContext',
            '.ActivatePdpContextWithApnTypesforSlice = v_ActivatePdpContext',
        ):
            self.assertIn(marker, ril)

    def test_f8_ohos_so_and_carrier_bundle(self):
        ril_so = pkg.F8_BUILD / 'libril_vendor_qmi.z.so'
        self.assertTrue(ril_so.is_file(), f'Missing F8 OHOS RIL .so: {ril_so}')
        dyn = subprocess.check_output([str(LLVM_READELF), '-d', str(ril_so)], text=True)
        needed = re.findall(r'\(NEEDED\)\s+Shared library:\s+\[([^\]]+)\]', dyn)
        soname = re.findall(r'\(SONAME\)\s+Library soname:\s+\[([^\]]+)\]', dyn)
        self.assertEqual(needed, ['libc.so'])
        self.assertEqual(soname, ['libril_vendor.z.so'])
        syms = subprocess.check_output([str(LLVM_NM), '-D', '--defined-only', str(ril_so)], text=True)
        self.assertIn('RilInitOps', syms)

        self.assertTrue(pkg.CARRIER_TAR.is_file(), f'Missing carrier bundle: {pkg.CARRIER_TAR}')
        with tarfile.open(pkg.CARRIER_TAR, 'r:gz') as tf:
            names = set(tf.getnames())
            self.assertIn('46011/etc/telephony/pdp_profile.json', names)
            self.assertIn('46000/etc/telephony/pdp_profile.json', names)
            self.assertIn('46001/etc/telephony/pdp_profile.json', names)
            self.assertIn('46015/etc/telephony/pdp_profile.json', names)
            self.assertIn('rule/etc/telephony/OpkeyInfo.json', names)
            self.assertIn('network/etc/telephony/ecc_data.json', names)
            ct = json.loads(tf.extractfile('46011/etc/telephony/pdp_profile.json').read().decode('utf-8'))
            apns = [x.get('apn') for x in ct['operator_infos']]
            self.assertIn('ctnet', apns)

    def test_f8_package_report_and_guards(self):
        self.assertEqual(len(pkg.GUARD_MEMBERS), 46)
        report_path = pkg.F8_BUILD / 'f8-build-report.json'
        if not report_path.is_file():
            self.skipTest('f8-build-report.json not generated yet')
        report = json.loads(report_path.read_text(encoding='utf-8'))
        cand = Path(report['candidate']['path'])
        if not cand.is_file():
            self.skipTest(f'Candidate image missing: {cand}')

        self.assertEqual(report['base']['sha256'], pkg.BASE_SHA)
        self.assertEqual(len(report['guard_members_verified']), 46)
        archive, r = pkg.load_oracle()
        img = r.read_original(cand)
        layout = r.layout(img)
        avb = r.verify_existing_avb(img, layout['payload_end'])
        self.assertIn('footer_masked', avb)
        with tempfile.TemporaryDirectory() as td:
            got, *_ = r.extract_entries(cand, img, layout, Path(td))
            self.assertIn(pkg.PREFIX + 'graphics/carrier-cn-marble.tar.gz', got)
            stage_bytes = got[pkg.STAGE_MEMBER][1]
            self.assertIn(b'MARBLE_F8_OHOS_CELLULAR_STAGE_BEGIN', stage_bytes)
            self.assertIn(b'MARBLE_F7_CELLULAR_DATA_STAGE_BEGIN', stage_bytes)
            subprocess.run(['sh', '-n'], input=stage_bytes, check=True)
            manifest = archive.parse(got[r.MANIFEST][1])
            r.manifest_holds(got, manifest)


if __name__ == '__main__':
    unittest.main()
