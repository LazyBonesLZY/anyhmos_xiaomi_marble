#!/usr/bin/env python3
"""Offline unit, ABI/symbol parity, safety, and packaging verification for Batch H7 (HarmonyOS NFC SA 1140)."""
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

import build_marble_h20_h7_nfc_service as pkg

VENDOR_CPP = ROOT / 'compat/nfc/marble_nci_native_vendor.cpp'
PROBE_CPP = ROOT / 'compat/nfc/native_nfc_fwk_probe.cpp'
LLVM_READELF = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-readelf'
LLVM_NM = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-nm'


class MarbleBatchH7NfcServiceTests(unittest.TestCase):
    def test_h7_host_unit_selftest_and_safety(self):
        with tempfile.TemporaryDirectory() as td:
            host_bin = Path(td) / 'nci_vendor_host'
            subprocess.run(
                ['g++', '-O2', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                 '-DMARBLE_NCI_VENDOR_HOST_TEST', str(VENDOR_CPP), '-o', str(host_bin), '-ldl'],
                check=True,
            )
            r = subprocess.run([str(host_bin)], check=True, capture_output=True, text=True)
            self.assertIn('MARBLE_NCI_VENDOR_HOST_TEST_OK=1', r.stdout)
            self.assertIn('chip=sn110u', r.stdout)
            self.assertIn('lib=libnci_native_t001.z.so', r.stdout)

        for src_path in (VENDOR_CPP, PROBE_CPP):
            code = re.sub(r'/\*.*?\*/', '', src_path.read_text(encoding='utf-8'), flags=re.S)
            for forbidden in ('SetSelfTokenID', '/mnt/vendor/persist', 'libhmos_audio', 'wakeup_count'):
                self.assertNotIn(forbidden, code, f'Forbidden token {forbidden!r} in {src_path.name}')

        cfg = json.loads(pkg.NFC_H7_CFG.read_text(encoding='utf-8'))
        svcs = {s['name']: s for s in cfg.get('services', [])}
        self.assertIn('nfc_service', svcs)
        svc = svcs['nfc_service']
        self.assertEqual(svc['path'], ['/system/bin/sa_main', '/system/profile/nfc_service.json'])
        self.assertEqual(svc['uid'], 'nfc')
        self.assertEqual(svc['sandbox'], 0)
        self.assertIs(svc['saspawn'], False)
        self.assertIs(svc['ondemand'], True)

    def test_h7_elf_soname_vtable_and_donor_symbol_parity(self):
        vso = pkg.H7_BUILD / 'libmarble_nci_native_vendor.z.so'
        probe = pkg.H7_BUILD / 'native_nfc_fwk_probe'
        for p in (vso, probe):
            self.assertTrue(p.is_file(), f'Missing H7 artifact: {p}')

        dyn = subprocess.check_output([str(LLVM_READELF), '-d', str(vso)], text=True)
        self.assertEqual(re.findall(r'\(SONAME\)\s+Library soname:\s+\[([^\]]+)\]', dyn),
                         ['libnci_native_vendor.z.so'])
        needed = re.findall(r'\(NEEDED\)\s+Shared library:\s+\[([^\]]+)\]', dyn)
        self.assertEqual(sorted(needed), ['libc++.so', 'libc.so'])

        syms = subprocess.check_output([str(LLVM_READELF), '-sW', str(vso)], text=True)
        vtv = [
            int(m.group(1))
            for line in syms.splitlines()
            if (m := re.match(r'\s*\d+:\s+[0-9a-f]+\s+(\d+)\s+\S+\s+\S+\s+\S+\s+\S+\s+_ZTVN4OHOS3NFC3NCI19NciNativepProxyImplE$', line))
        ]
        self.assertTrue(vtv and all(sz == 56 for sz in vtv), f'Unexpected vtable size: {vtv}')

        def nci_syms(path: Path) -> set[str]:
            out = subprocess.check_output([str(LLVM_NM), '-D', '--defined-only', str(path)], text=True)
            return {
                line.split()[-1]
                for line in out.splitlines()
                if len(line.split()) >= 3
                and any(k in line.split()[-1] for k in ('NciNative', 'NciLibsLoader', 'NewInterface', 'DeleteInterface'))
            }
        self.assertEqual(sorted(nci_syms(pkg.DONOR_VENDOR_SO) - nci_syms(vso)), [])

        pdyn = subprocess.check_output([str(LLVM_READELF), '-d', str(probe)], text=True)
        pneeded = re.findall(r'\(NEEDED\)\s+Shared library:\s+\[([^\]]+)\]', pdyn)
        self.assertIn('libnfc_inner_kits_controller.z.so', pneeded)
        self.assertIn('libnfc_inner_kits_common.z.so', pneeded)

    def test_h7_package_report_and_78_guards(self):
        self.assertEqual(len(pkg.GUARD_MEMBERS), 78)
        report_path = pkg.H7_BUILD / 'h7-build-report.json'
        if not report_path.is_file():
            self.skipTest('h7-build-report.json not generated yet')
        report = json.loads(report_path.read_text(encoding='utf-8'))
        cand = Path(report['candidate']['path'])
        if not cand.is_file():
            self.skipTest(f'Candidate image missing: {cand}')

        self.assertEqual(report['base']['sha256'], pkg.BASE_SHA)
        self.assertEqual(len(report['guard_members_verified']), 78)
        self.assertEqual(report['entries_after'], report['entries_before'] + 6)

        archive, r = pkg.load_oracle()
        img = r.read_original(cand)
        layout = r.layout(img)
        avb = r.verify_existing_avb(img, layout['payload_end'])
        self.assertIn('footer_masked', avb)

        with tempfile.TemporaryDirectory() as td:
            got, *_ = r.extract_entries(cand, img, layout, Path(td))
            for rel, meta in report['staged'].items():
                self.assertEqual(r.sha(got[pkg.PREFIX + rel][1]), meta['sha256'])
            init_doc = json.loads(got[pkg.INIT_DEBUG_MEMBER][1].decode('utf-8'))
            self.assertIn('/compat/etc/init-h7-nfc-service-marble.cfg', init_doc.get('import', []))
            stage_txt = got[pkg.STAGE_MEMBER][1].decode('utf-8')
            self.assertIn('MARBLE_H7_NFC_SERVICE_STAGE_BEGIN', stage_txt)
            self.assertIn('libnci_native_vendor.z.so', stage_txt)
            subprocess.run(['sh', '-n'], input=stage_txt.encode('utf-8'), check=True)
            pb_txt = got[pkg.POSTBOOT_MEMBER][1].decode('utf-8')
            self.assertIn('start-nfc-service-h7-marble.sh', pb_txt)
            subprocess.run(['sh', '-n'], input=pb_txt.encode('utf-8'), check=True)
            manifest = archive.parse(got[r.MANIFEST][1])
            r.manifest_holds(got, manifest)


if __name__ == '__main__':
    unittest.main()
