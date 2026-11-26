#!/usr/bin/env python3
"""Offline unit & packaging verification for Batch F10 (5G/5A + IMSDCM VoLTE/VoNR/IMS-SMS + WMS SMS + VOICE Call + SA 4005/4006)."""
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

import build_marble_h20_f10_5g_ims_call_sms as pkg

RIL_C = ROOT / 'compat/telephony/libril_vendor_qmi.c'
ABI_H = ROOT / 'compat/telephony/hril_vendor_abi.h'
TEL_F2 = ROOT / 'devices/marble/runtime/telephony/telephony-marble.json'
TEL_F3 = ROOT / 'devices/marble/runtime/telephony/telephony-f3-marble.json'
TEL_F10 = ROOT / 'devices/marble/runtime/telephony/telephony-f10-marble.json'
FLASH_SH = ROOT / 'work/marble-dual-20261123/flash-h20-f10-control.sh'
LLVM_READELF = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-readelf'
LLVM_NM = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-nm'


class MarbleBatchF10Tests(unittest.TestCase):
    def test_f10_host_ril_self_test(self):
        with tempfile.TemporaryDirectory() as td:
            ril_host = Path(td) / 'ril_host_f10'
            subprocess.run(
                ['gcc', '-O2', '-Wall', '-Wextra', '-Werror', '-std=c11', '-DHRIL_VENDOR_HOST_TEST',
                 str(RIL_C), '-lpthread', '-o', str(ril_host)],
                check=True,
            )
            res = subprocess.run([str(ril_host)], check=True, capture_output=True, text=True)
            self.assertIn('RIL_VENDOR_QMI_HOST_TEST_OK=1', res.stdout)

    def test_f10_abi_and_qmi_markers(self):
        abi = ABI_H.read_text(encoding='utf-8')
        for marker in (
            'HNOTI_CALL_STATE_UPDATED = 1001,',
            'HNOTI_SMS_NEW_SMS = 1101,',
            'HNOTI_NETWORK_PHY_CHNL_CFG_UPDATED = 1404,',
            'sizeof(HRilPhyChannelConfig) == 48',
            'sizeof(HRilSmsResponse) == 24',
            'sizeof(HRilDial) == 16',
            'sizeof(HRilCallDtmfInfo) == 32',
            'sizeof(HRilCallInfo) == 72',
        ):
            self.assertIn(marker, abi)

        ril = RIL_C.read_text(encoding='utf-8')
        for marker in (
            'SVC_IMSDCM 0x302u',
            'QMI_WMS_BIND_SUBSCRIPTION 0x004cu',
            'QMI_VOICE_BIND_SUBSCRIPTION 0x0044u',
            'QMI_IMSDCM_START_CONN 0x0020u',
            'qmi_wms_rpc_slot_locked',
            'qmi_voice_rpc_slot_locked',
            'parse_smsp_record',
            'parse_wms_event_report_ind',
            'parse_voice_call_list',
            'build_imsdcm_start_resp',
            'build_imsdcm_conn_ind',
            'v_SetNrOptionMode',
            'v_SendSmsAck',
        ):
            self.assertIn(marker, ril)

    def test_f10_telephony_json_preserves_f2_f3(self):
        f10 = json.loads(TEL_F10.read_text(encoding='utf-8'))
        by_name = {s['name']: s for s in f10['systemability']}
        self.assertEqual(set(by_name), {4010, 4007, 4006, 4008, 66250})
        self.assertEqual(by_name[4006]['libpath'], 'libtel_cellular_call.z.so')
        self.assertEqual(by_name[4006]['depend'], [4010])
        for prior in (TEL_F2, TEL_F3):
            doc = json.loads(prior.read_text(encoding='utf-8'))
            self.assertNotIn(4006, {s['name'] for s in doc['systemability']})

    def test_f10_stage_and_foundation_patch_helpers(self):
        sample_stage = (
            b'#!/bin/sh\n'
            + pkg.OPT_SA_OLD + b'\n'
            + b'# === MARBLE_H7_NFC_SERVICE_STAGE_BEGIN ===\n'
            + pkg.STAGE_ANCHOR + b'\n'
        )
        patched_stage = pkg.patch_stage(sample_stage)
        self.assertIn(pkg.OPT_SA_NEW, patched_stage)
        self.assertNotIn(b"'4005 ", patched_stage)
        self.assertIn(b'const.telephony.efl_forbidden=false', patched_stage)
        self.assertIn(b'const.telephony.show_efl_mobile_type=true', patched_stage)
        self.assertIn(b'$B cat "$f10_dst.f10tmp" > "$f10_dst"', patched_stage)

        sample_fc = json.dumps({
            'process': 'foundation',
            'systemability': [{'name': i, 'libpath': f'lib{i}.so'} for i in range(1, 12)],
        }).encode('utf-8')
        patched_fc = json.loads(pkg.patch_foundation_json(sample_fc).decode('utf-8'))
        self.assertEqual(patched_fc['systemability'][-1]['name'], 4005)
        self.assertEqual(patched_fc['systemability'][-1]['libpath'], 'libtel_call_manager.z.so')

    def test_f10_ohos_so_exports(self):
        ril_so = pkg.F10_BUILD / 'libril_vendor_qmi.z.so'
        self.assertTrue(ril_so.is_file(), f'Missing F10 OHOS RIL .so: {ril_so}')
        dyn = subprocess.check_output([str(LLVM_READELF), '-d', str(ril_so)], text=True)
        needed = re.findall(r'\(NEEDED\)\s+Shared library:\s+\[([^\]]+)\]', dyn)
        soname = re.findall(r'\(SONAME\)\s+Library soname:\s+\[([^\]]+)\]', dyn)
        self.assertEqual(needed, ['libc.so'])
        self.assertEqual(soname, ['libril_vendor.z.so'])
        syms = subprocess.check_output([str(LLVM_NM), '-D', '--defined-only', str(ril_so)], text=True)
        self.assertIn('RilInitOps', syms)

    def test_f10_package_report_and_guards(self):
        self.assertEqual(len(pkg.GUARD_MEMBERS), 84)
        report_path = pkg.F10_BUILD / 'f10-build-report.json'
        if not report_path.is_file():
            self.skipTest('f10-build-report.json not generated yet')
        report = json.loads(report_path.read_text(encoding='utf-8'))
        cand = Path(report['candidate']['path'])
        if not cand.is_file():
            self.skipTest(f'Candidate image missing: {cand}')

        self.assertEqual(report['base']['sha256'], pkg.BASE_SHA)
        self.assertEqual(len(report['guard_members_verified']), 84)
        self.assertEqual(report['entries_before'], report['entries_after'])
        archive, r = pkg.load_oracle()
        img = r.read_original(cand)
        layout = r.layout(img)
        avb = r.verify_existing_avb(img, layout['payload_end'])
        self.assertIn('footer_masked', avb)
        with tempfile.TemporaryDirectory() as td:
            got, *_ = r.extract_entries(cand, img, layout, Path(td))
            self.assertEqual(
                r.sha(got[pkg.RIL_MEMBER][1]),
                report['staged']['graphics/libril_vendor_qmi.z.so']['sha256'],
            )
            fc = json.loads(got[pkg.FOUNDATION_MEMBER][1].decode('utf-8'))
            self.assertTrue(any(s.get('name') == 4005 for s in fc['systemability']))
            tel = json.loads(got[pkg.TELEPHONY_MEMBER][1].decode('utf-8'))
            self.assertTrue(any(s.get('name') == 4006 for s in tel['systemability']))
            st = got[pkg.STAGE_MEMBER][1]
            self.assertIn(b'MARBLE_F10_5G_IMS_CALL_SMS_STAGE_BEGIN', st)
            self.assertNotIn(b"'4005 ", st)
            manifest = archive.parse(got[r.MANIFEST][1])
            r.manifest_holds(got, manifest)

    def test_f10_flash_script_syntax(self):
        if not FLASH_SH.is_file():
            self.skipTest('flash-h20-f10-control.sh not created yet')
        subprocess.run(['bash', '-n', str(FLASH_SH)], check=True)
        txt = FLASH_SH.read_text(encoding='utf-8')
        self.assertIn('flash vendor_boot_a', txt)
        self.assertNotIn('flash boot_a', txt)


if __name__ == '__main__':
    unittest.main()
