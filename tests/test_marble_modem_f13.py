"""Offline unit & packaging verification for Batch F13 (MO SMS RIL route, rmnet0 net speed, GNSS filter, cutout)."""
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/packaging'))
import build_marble_h20_f13_tel_loc_cutout as pkg  # noqa: E402


class TestMarbleModemF13(unittest.TestCase):
    def test_host_ril_and_gnss_c11_suites(self):
        with tempfile.TemporaryDirectory(prefix='f13_host_') as td:
            ril_bin = Path(td) / 'ril_host_test'
            gnss_bin = Path(td) / 'gnss_host_test'
            subprocess.run(
                ['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-DHRIL_VENDOR_HOST_TEST',
                 '-O2', '-pthread', str(ROOT / 'compat/telephony/libril_vendor_qmi.c'), '-o', str(ril_bin)],
                check=True,
            )
            out1 = subprocess.check_output([str(ril_bin)], text=True)
            self.assertIn('RIL_VENDOR_QMI_HOST_TEST_OK=1', out1)

            subprocess.run(
                ['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-DGNSS_HDI_HOST_TEST',
                 '-O2', '-pthread', str(ROOT / 'compat/location/native_gnss_hdi_publisher.c'),
                 '-lm', '-o', str(gnss_bin)],
                check=True,
            )
            out2 = subprocess.check_output([str(gnss_bin)], text=True)
            self.assertIn('GNSS_HDI_HOST_TEST_OK=1', out2)

    def test_ims_core_disables_ohos_ims_sms_proxy_route(self):
        src = (ROOT / 'compat/telephony/hmos_ims_core_service.cpp').read_text(encoding='utf-8')
        self.assertIn('wire_svc.supportImsSms = 0;', src)

    def test_rmnet0_and_cutout_config(self):
        ril = (ROOT / 'compat/telephony/libril_vendor_qmi.c').read_text(encoding='utf-8')
        self.assertIn('#define MARBLE_DATA_IFNAME "rmnet0"', ril)
        f7 = (ROOT / 'devices/marble/runtime/telephony/start-modem-f7-marble.sh').read_text(encoding='utf-8')
        self.assertIn('--ipa-setup --ifname rmnet0 --mux-id 1 --mtu 1500', f7)
        xml = (ROOT / 'devices/marble/runtime/init/display_manager_config.xml').read_text(encoding='utf-8')
        self.assertIn('<cutoutArea>508 18 65 67</cutoutArea>', xml)
        self.assertIn('<defaultDisplayCutoutPath>M508 18 L573 18 v 67 h -65 Z</defaultDisplayCutoutPath>', xml)

    def test_f13_package_report_and_guards(self):
        self.assertEqual(len(pkg.GUARD_MEMBERS), 86)
        report_path = pkg.F13_BUILD / 'f13-build-report.json'
        if not report_path.is_file():
            self.skipTest('f13-build-report.json not generated yet')
        report = json.loads(report_path.read_text(encoding='utf-8'))
        self.assertEqual(report['batch'], 'F13')
        self.assertEqual(report['base']['sha256'], pkg.BASE_SHA)
        self.assertEqual(report['entries_before'], 600)
        self.assertEqual(report['entries_after'], 600)
        cand = Path(report['candidate']['path'])
        self.assertTrue(cand.is_file())


if __name__ == '__main__':
    unittest.main()
