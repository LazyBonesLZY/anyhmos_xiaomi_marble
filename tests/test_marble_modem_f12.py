#!/usr/bin/env python3
"""Offline unit & packaging verification for Batch F12 (SA 4014 IMS Core + MO Dial + Hangup + MO SMS + Status Bar Signal Icon)."""
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

import build_marble_h20_f12_tel_fixes as pkg

RIL_C = ROOT / 'compat/telephony/libril_vendor_qmi.c'
ABI_H = ROOT / 'compat/telephony/hril_vendor_abi.h'
IMS_WIRE_H = ROOT / 'compat/telephony/ims_core_wire.h'
IMS_CPP = ROOT / 'compat/telephony/hmos_ims_core_service.cpp'
INIT_F12 = ROOT / 'devices/marble/runtime/telephony/init-modem-f12-marble.cfg'
FLASH_SH = ROOT / 'work/marble-dual-20261123/flash-h20-f12-control.sh'
LLVM_READELF = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-readelf'
LLVM_NM = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-nm'


class MarbleBatchF12Tests(unittest.TestCase):
    def test_f12_host_ril_and_ims_wire_self_test(self):
        with tempfile.TemporaryDirectory() as td:
            ril_host = Path(td) / 'ril_host_f12'
            subprocess.run(
                ['gcc', '-O2', '-Wall', '-Wextra', '-Werror', '-std=c11', '-DHRIL_VENDOR_HOST_TEST',
                 str(RIL_C), '-lpthread', '-o', str(ril_host)],
                check=True,
            )
            res = subprocess.run([str(ril_host)], check=True, capture_output=True, text=True)
            self.assertIn('RIL_VENDOR_QMI_HOST_TEST_OK=1', res.stdout)

    def test_f12_abi_and_source_markers(self):
        abi = ABI_H.read_text(encoding='utf-8')
        for marker in (
            'void (*CallSupplement)(const ReqDataInfo *requestInfo, int32_t type);',
            'void (*GetCallFailReason)(const ReqDataInfo *requestInfo);',
            'offsetof(HRilCallReq, CallSupplement) == 12 * 8',
            'offsetof(HRilCallReq, GetCallFailReason) == 34 * 8',
            'sizeof(HRilCallReq) == 43 * 8',
        ):
            self.assertIn(marker, abi)

        ril = RIL_C.read_text(encoding='utf-8')
        for marker in (
            'build_voice_dial_tlvs',
            'parse_voice_call_end_reason',
            'transcode_cdma_pdu_to_gsm_submit',
            'TASK_CALL_SUPPLEMENT',
            'TASK_CALL_GET_FAIL_REASON',
            'g_hangup_pending',
            'g_call_watchdog_ticks',
            '0x56',
            'HNOTI_SIM_STATUS_UPDATED',
        ):
            self.assertIn(marker, ril)

        wire = IMS_WIRE_H.read_text(encoding='utf-8')
        for marker in (
            'MARBLE_IMS_CORE_SA_ID 4014',
            'sizeof(ImsServiceStatusWire) == 8',
            'sizeof(ImsRegistrationStatusWire) == 8',
            'parse_imsa_reg_status_tlvs',
            'parse_imsa_srv_status_tlvs',
        ):
            self.assertIn(marker, wire)

        ims = IMS_CPP.read_text(encoding='utf-8')
        for marker in (
            'u"OHOS.Telephony.ImsCoreServiceInterface"',
            'u"OHOS.Telephony.ImsCoreServiceCallback"',
            'IMS_CORE_CMD_GET_REG_STATUS',
            'IMS_CORE_CMD_REGISTER_CALLBACK',
            'IMS_CORE_CMD_GET_PROXY_OBJ',
            'WriteRawData',
        ):
            self.assertIn(marker, ims)

    def test_f12_init_cfg_and_patch_helpers(self):
        cfg = json.loads(INIT_F12.read_text(encoding='utf-8'))
        svcs = {s['name']: s for s in cfg['services']}
        jobs = {j['name']: j for j in cfg['jobs']}
        self.assertEqual(svcs['hmos_ims_core_service']['path'], ['/compat/bin/hmos_ims_core_service'])
        self.assertEqual(svcs['hmos_ims_core_service']['uid'], 'radio')
        self.assertEqual(svcs['hmos_ims_core_service']['secon'], 'u:r:telephony_sa:s0')
        self.assertEqual(jobs['param:marble-f12-pre-telephony']['condition'], 'marble.debug.start.pre_telephony=true')
        cmds = jobs['param:marble-f12-pre-telephony']['cmds']
        self.assertIn('start hmos_ims_core_service', cmds)
        self.assertIn('start telephony_sa', cmds)

        sample_init = b'{\n  "import": [\n    "/compat/etc/init-h7-nfc-service-marble.cfg"\n  ]\n}\n'
        patched_init = pkg.patch_init_debug_cfg(sample_init)
        self.assertIn(b'/compat/etc/init-modem-f12-marble.cfg', patched_init)

        sample_stage = b'#!/bin/sh\n# === MARBLE_F11_VOICE_AUDIO_STAGE_BEGIN ===\n' + pkg.STAGE_ANCHOR + b'\n'
        patched_stage = pkg.patch_stage(sample_stage)
        self.assertIn(b'MARBLE_F12_TEL_FIXES_STAGE_BEGIN', patched_stage)
        self.assertIn(b'hmos_ims_core_service', patched_stage)

        sample_start = (
            b'#!/bin/sh\n'
            + pkg.START_ANCHOR
            + b'\n# 7. Foundation\n'
            + b'if true; then\n'
            + pkg.START_POSTBOOT_ANCHOR
        )
        patched_start = pkg.patch_start(sample_start)
        self.assertIn(b'marble.debug.start.pre_telephony true', patched_start)
        self.assertIn(b'wait_sa 4010 15', patched_start)
        self.assertIn(b'wait_sa 4014 10', patched_start)
        self.assertIn(b'marble.debug.f12.tel_phase stop', patched_start)
        self.assertIn(b'marble.debug.f12.tel_phase start', patched_start)

    def test_f12_ohos_binaries(self):
        ril_so = pkg.F12_BUILD / 'libril_vendor_qmi.z.so'
        ims_bin = pkg.F12_BUILD / 'hmos_ims_core_service'
        self.assertTrue(ril_so.is_file(), f'Missing F12 RIL .so: {ril_so}')
        self.assertTrue(ims_bin.is_file(), f'Missing F12 IMS binary: {ims_bin}')

        dyn = subprocess.check_output([str(LLVM_READELF), '-d', str(ril_so)], text=True)
        needed = re.findall(r'\(NEEDED\)\s+Shared library:\s+\[([^\]]+)\]', dyn)
        soname = re.findall(r'\(SONAME\)\s+Library soname:\s+\[([^\]]+)\]', dyn)
        self.assertEqual(needed, ['libc.so'])
        self.assertEqual(soname, ['libril_vendor.z.so'])
        syms = subprocess.check_output([str(LLVM_NM), '-D', '--defined-only', str(ril_so)], text=True)
        self.assertIn('RilInitOps', syms)

        dyn_ims = subprocess.check_output([str(LLVM_READELF), '-d', str(ims_bin)], text=True)
        needed_ims = re.findall(r'\(NEEDED\)\s+Shared library:\s+\[([^\]]+)\]', dyn_ims)
        for lib in ('libipc_single.z.so', 'libutils.z.so', 'libc++.so', 'libc.so'):
            self.assertIn(lib, needed_ims)

    def test_f12_package_report_and_guards(self):
        self.assertEqual(len(pkg.GUARD_MEMBERS), 85)
        report_path = pkg.F12_BUILD / 'f12-build-report.json'
        if not report_path.is_file():
            self.skipTest('f12-build-report.json not generated yet')
        report = json.loads(report_path.read_text(encoding='utf-8'))
        cand = Path(report['candidate']['path'])
        if not cand.is_file():
            self.skipTest(f'Candidate image missing: {cand}')

        self.assertEqual(report['base']['sha256'], pkg.BASE_SHA)
        self.assertEqual(len(report['guard_members_verified']), 85)
        self.assertEqual(report['entries_after'], report['entries_before'] + 2)
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
            self.assertEqual(
                r.sha(got[pkg.IMS_CORE_MEMBER][1]),
                report['staged']['graphics/hmos_ims_core_service']['sha256'],
            )
            self.assertIn(b'init-modem-f12-marble.cfg', got[pkg.INIT_DEBUG_MEMBER][1])
            self.assertIn(b'MARBLE_F12_TEL_FIXES_STAGE_BEGIN', got[pkg.STAGE_MEMBER][1])
            self.assertIn(b'MARBLE_F12_PRE_TELEPHONY_BEGIN', got[pkg.START_MEMBER][1])
            manifest = archive.parse(got[r.MANIFEST][1])
            r.manifest_holds(got, manifest)

    def test_f12_flash_script_syntax(self):
        if not FLASH_SH.is_file():
            self.skipTest('flash-h20-f12-control.sh not created yet')
        subprocess.run(['bash', '-n', str(FLASH_SH)], check=True)
        txt = FLASH_SH.read_text(encoding='utf-8')
        self.assertIn('flash vendor_boot_a', txt)
        self.assertNotIn('flash boot_a', txt)
        self.assertIn('vendor_boot_h20_f11_voice_audio_00d7c1a6.img', txt)


if __name__ == '__main__':
    unittest.main()
