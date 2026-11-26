#!/usr/bin/env python3
"""Offline unit & packaging verification for Batch F7 (Cellular UI fixes + QMI WDA/WDS + IPA rmnet_data0)."""
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

import build_marble_h20_f7_cellular_data as pkg

F7_SH = ROOT / 'devices/marble/runtime/telephony/start-modem-f7-marble.sh'
POSTBOOT_SH = ROOT / 'devices/marble/runtime/telephony/start-u2-postboot-marble.sh'
PROBE_C = ROOT / 'compat/telephony/marble_modem_probe.c'
RIL_C = ROOT / 'compat/telephony/libril_vendor_qmi.c'
ABI_H = ROOT / 'compat/telephony/hril_vendor_abi.h'
LLVM_READELF = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-readelf'
LLVM_NM = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-nm'


class MarbleBatchF7Tests(unittest.TestCase):
    def test_host_self_tests(self):
        with tempfile.TemporaryDirectory() as td:
            tp = Path(td)
            probe_host = tp / 'probe_host'
            ril_host = tp / 'ril_host'
            subprocess.run(
                ['gcc', '-O2', '-Wall', '-Wextra', '-Werror', '-std=c11',
                 str(PROBE_C), '-o', str(probe_host)],
                check=True,
            )
            p_res = subprocess.run([str(probe_host), '--self-test'], check=True, capture_output=True, text=True)
            self.assertIn('MODEM_PROBE_SELF_TEST_OK=1', p_res.stdout)

            subprocess.run(
                ['gcc', '-O2', '-Wall', '-Wextra', '-Werror', '-std=c11', '-DHRIL_VENDOR_HOST_TEST',
                 str(RIL_C), '-lpthread', '-o', str(ril_host)],
                check=True,
            )
            r_res = subprocess.run([str(ril_host)], check=True, capture_output=True, text=True)
            self.assertIn('RIL_VENDOR_QMI_HOST_TEST_OK=1', r_res.stdout)

    def test_f7_abi_and_sources(self):
        abi_txt = ABI_H.read_text(encoding='utf-8')
        for marker in (
            'HNOTI_NETWORK_CS_REG_STATUS_UPDATED = 1401',
            'HNOTI_NETWORK_SIGNAL_STRENGTH_UPDATED = 1402',
            'HNOTI_NETWORK_TIME_UPDATED = 1403',
            'HNOTI_NETWORK_PS_REG_STATUS_UPDATED = 1406',
            'HNOTI_MODEM_RADIO_STATE_UPDATED = 1501',
            'HNOTI_MODEM_VOICE_TECH_UPDATED = 1502',
            'HRilDataInfo',
            'HRilDataCallResponse',
            'SendSimMatchedOperatorInfo',
            'SetLinkBandwidthReportingRule',
            'SetDataProfileInfo',
        ):
            self.assertIn(marker, abi_txt)

        ril_txt = RIL_C.read_text(encoding='utf-8')
        for marker in (
            'SVC_DPM',
            'QMI_DPM_OPEN_PORT',
            'g_slot_radio_state',
            'QMI_WDS_START_NETWORK_INTERFACE',
            'QMI_WDS_GET_CURRENT_SETTINGS',
            'QMI_WDA_SET_DATA_FORMAT',
            'rssi_changed_significantly',
            'parse_wds_current_settings',
            'HNOTI_MODEM_RADIO_STATE_UPDATED',
            'HNOTI_DATA_PDP_CONTEXT_LIST_UPDATED',
            'MARBLE_DATA_IFNAME',
            'build_wda_set_data_fmt_tlvs(qreq, 2u, 5u, 9u',
        ):
            self.assertIn(marker, ril_txt)

        probe_txt = PROBE_C.read_text(encoding='utf-8')
        for marker in (
            'SVC_DPM',
            'QMI_DPM_OPEN_PORT',
            'MARBLE_RMNET_IOCTL_EXTENDED_V2',
            'RMNET_PRIV_FLAGS_INGRESS_MAP_CKSUMV5',
            'MARBLE_RMNET_IOCTL_SET_LLP_IP',
            'MARBLE_RMNET_IOCTL_SET_INGRESS_FMT',
            'MARBLE_RMNET_IOCTL_SET_EGRESS_FMT',
            'MARBLE_RMNET_IOCTL_ADD_MUX_CHANNEL',
            'build_rmnet_newlink_msg',
            'build_wda_set_data_fmt_tlvs(req, 2u, 5u, 9u',
            '--ipa-setup',
            '--dpm-open',
            '--configure-net',
        ):
            self.assertIn(marker, probe_txt)

    def test_f7_ohos_so_and_static_probe(self):
        probe_bin = pkg.F7_BUILD / 'marble_modem_probe'
        ril_so = pkg.F7_BUILD / 'libril_vendor_qmi.z.so'
        self.assertTrue(probe_bin.is_file(), f'Missing F7 static probe: {probe_bin}')
        self.assertTrue(ril_so.is_file(), f'Missing F7 OHOS RIL .so: {ril_so}')

        dyn = subprocess.check_output([str(LLVM_READELF), '-d', str(ril_so)], text=True)
        needed = re.findall(r'\(NEEDED\)\s+Shared library:\s+\[([^\]]+)\]', dyn)
        soname = re.findall(r'\(SONAME\)\s+Library soname:\s+\[([^\]]+)\]', dyn)
        self.assertEqual(needed, ['libc.so'])
        self.assertEqual(soname, ['libril_vendor.z.so'])

        syms = subprocess.check_output([str(LLVM_NM), '-D', '--defined-only', str(ril_so)], text=True)
        self.assertIn('RilInitOps', syms)

        finfo = subprocess.check_output(['file', str(probe_bin)], text=True)
        self.assertIn('ARM aarch64', finfo)
        self.assertIn('statically linked', finfo)
        self.assertNotIn(b'ARM64 Bionic', probe_bin.read_bytes())

    def test_f7_script_and_postboot_contracts(self):
        self.assertTrue(F7_SH.is_file())
        self.assertTrue(POSTBOOT_SH.is_file())
        subprocess.run(['sh', '-n', str(F7_SH)], check=True)
        subprocess.run(['sh', '-n', str(POSTBOOT_SH)], check=True)

        f7_txt = F7_SH.read_text(encoding='utf-8')
        self.assertNotIn('unbind', f7_txt)
        self.assertNotIn('/bind', f7_txt)
        self.assertIn('--ipa-setup --ifname rmnet0 --mux-id 1 --mtu 1500', f7_txt)
        self.assertIn('--dpm-open --ep-type 4 --iface-id 1', f7_txt)
        self.assertIn('wait_qmi_ipa_init', f7_txt)
        self.assertIn('F7_IPA_DATA_READY=1', f7_txt)
        for flag in ('--ipa', '--ipa-data', '--online', '--telephony', '--all', '--status'):
            self.assertIn(flag, f7_txt)
        self.assertLess(f7_txt.index('do_ipa_data ||'), f7_txt.index('--set-mode online'))

        post_txt = POSTBOOT_SH.read_text(encoding='utf-8')
        self.assertIn('start-modem-f7-marble.sh --all', post_txt)
        self.assertIn('start-modem-f6-marble.sh --all', post_txt)
        self.assertLess(post_txt.index('start-modem-f7-marble.sh --all'), post_txt.index('start-modem-f6-marble.sh --all'))

    def test_f7_package_report_and_guards(self):
        self.assertEqual(len(pkg.GUARD_MEMBERS), 43)
        report_path = pkg.F7_BUILD / 'f7-build-report.json'
        if not report_path.is_file():
            self.skipTest('f7-build-report.json not generated yet')
        report = json.loads(report_path.read_text(encoding='utf-8'))
        cand = Path(report['candidate']['path'])
        if not cand.is_file():
            self.skipTest(f'Candidate image missing: {cand}')

        self.assertEqual(report['base']['sha256'], pkg.BASE_SHA)
        self.assertEqual(len(report['guard_members_verified']), 43)
        archive, r = pkg.load_oracle()
        img = r.read_original(cand)
        layout = r.layout(img)
        avb = r.verify_existing_avb(img, layout['payload_end'])
        self.assertIn('footer_masked', avb)
        with tempfile.TemporaryDirectory() as td:
            got, *_ = r.extract_entries(cand, img, layout, Path(td))
            self.assertIn(pkg.PREFIX + 'graphics/start-modem-f7-marble.sh', got)
            stage_bytes = got[pkg.STAGE_MEMBER][1]
            self.assertIn(b'MARBLE_F7_CELLULAR_DATA_STAGE_BEGIN', stage_bytes)
            subprocess.run(['sh', '-n'], input=stage_bytes, check=True)
            manifest = archive.parse(got[r.MANIFEST][1])
            r.manifest_holds(got, manifest)


if __name__ == '__main__':
    unittest.main()
