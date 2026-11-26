#!/usr/bin/env python3
"""Offline unit, ABI/vtable, HCB, and packaging verification for Batch H1 (SM7475 Hardware NFC)."""
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

import build_marble_h20_h1_nfc as pkg

NFC_HW_C = ROOT / 'compat/nfc/marble_nfc_hw_probe.c'
NFC_HDI_CPP = ROOT / 'compat/nfc/marble_nfc_hdi_impl.cpp'
LLVM_READELF = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-readelf'
LLVM_NM = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-nm'


class MarbleBatchH1NfcTests(unittest.TestCase):
    def test_h1_host_unit_selftests(self):
        with tempfile.TemporaryDirectory() as td:
            tdp = Path(td)
            hw_host = tdp / 'nfc_hw_host'
            hdi_host = tdp / 'nfc_hdi_host'
            subprocess.run(
                ['gcc', '-O2', '-std=gnu11', '-Wall', '-Wextra', '-Werror', str(NFC_HW_C), '-o', str(hw_host)],
                check=True,
            )
            r1 = subprocess.run([str(hw_host), '--self-test'], check=True, capture_output=True, text=True)
            self.assertIn('MARBLE_NFC_HW_PROBE_SELF_TEST_OK=1', r1.stdout)

            subprocess.run(
                ['g++', '-O2', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                 '-DMARBLE_NFC_HDI_HOST_TEST', str(NFC_HDI_CPP), '-o', str(hdi_host)],
                check=True,
            )
            r2 = subprocess.run([str(hdi_host)], check=True, capture_output=True, text=True)
            self.assertIn('MARBLE_NFC_HDI_HOST_TEST_OK=1', r2.stdout)

    def test_h1_elf_soname_vtable_and_exports(self):
        hdi_so = pkg.H1_BUILD / 'libmarble_nfc_interface_service_1.1.z.so'
        hw_bin = pkg.H1_BUILD / 'marble_nfc_hw_probe'
        hdi_bin = pkg.H1_BUILD / 'native_nfc_hdi_probe'
        for p in (hdi_so, hw_bin, hdi_bin):
            self.assertTrue(p.is_file(), f'Missing H1 artifact: {p}')

        dyn = subprocess.check_output([str(LLVM_READELF), '-d', str(hdi_so)], text=True)
        self.assertEqual(re.findall(r'\(SONAME\)\s+Library soname:\s+\[([^\]]+)\]', dyn),
                         ['libnfc_interface_service_1.1.z.so'])
        needed = re.findall(r'\(NEEDED\)\s+Shared library:\s+\[([^\]]+)\]', dyn)
        self.assertEqual(sorted(needed), ['libc++.so', 'libc.so', 'libutils.z.so'])

        nm = subprocess.check_output([str(LLVM_NM), '-D', str(hdi_so)], text=True)
        self.assertIn(' NfcInterfaceImplGetInstance', nm)

        syms = subprocess.check_output([str(LLVM_READELF), '-sW', str(hdi_so)], text=True)
        vtv = [int(m.group(1)) for line in syms.splitlines()
               if (m := re.match(r'\s*\d+:\s+[0-9a-f]+\s+(\d+)\s+\S+\s+\S+\s+\S+\s+\S+\s+_ZTVN4OHOS3HDI3Nfc4V1_17NfcImplE$', line))]
        self.assertTrue(vtv and all(sz == 296 for sz in vtv), f'Unexpected NfcImpl vtable sizes: {vtv}')

        hw_dyn = subprocess.check_output([str(LLVM_READELF), '-d', str(hw_bin)], text=True)
        self.assertEqual(re.findall(r'\(NEEDED\)\s+Shared library:\s+\[([^\]]+)\]', hw_dyn), ['libc.so'])

        probe_dyn = subprocess.check_output([str(LLVM_READELF), '-d', str(hdi_bin)], text=True)
        pneeded = re.findall(r'\(NEEDED\)\s+Shared library:\s+\[([^\]]+)\]', probe_dyn)
        self.assertIn('libnfc_proxy_1.1.z.so', pneeded)
        self.assertIn('libnfc_proxy_1.0.z.so', pneeded)

    def test_h1_package_report_hcb_and_55_guards(self):
        self.assertEqual(len(pkg.GUARD_MEMBERS), 55)
        report_path = pkg.H1_BUILD / 'h1-build-report.json'
        if not report_path.is_file():
            self.skipTest('h1-build-report.json not generated yet')
        report = json.loads(report_path.read_text(encoding='utf-8'))
        cand = Path(report['candidate']['path'])
        if not cand.is_file():
            self.skipTest(f'Candidate image missing: {cand}')

        self.assertEqual(report['base']['sha256'], pkg.BASE_SHA)
        self.assertEqual(len(report['guard_members_verified']), 55)
        self.assertEqual(report['entries_after'], report['entries_before'] + 5)

        archive, r = pkg.load_oracle()
        img = r.read_original(cand)
        layout = r.layout(img)
        avb = r.verify_existing_avb(img, layout['payload_end'])
        self.assertIn('footer_masked', avb)

        with tempfile.TemporaryDirectory() as td:
            got, *_ = r.extract_entries(cand, img, layout, Path(td))
            for rel, meta in report['staged'].items():
                self.assertEqual(r.sha(got[pkg.PREFIX + rel][1]), meta['sha256'])
            hcb = got[pkg.HCB_MEMBER][1]
            self.assertIn(b'nfc_host', hcb)
            self.assertIn(b'libnfc_hdi_driver.z.so', hcb)
            self.assertIn(b'nfc_interface_service', hcb)
            init_doc = json.loads(got[pkg.INIT_DEBUG_MEMBER][1].decode('utf-8'))
            self.assertIn('/compat/etc/init-h1-nfc-marble.cfg', init_doc.get('import', []))
            stage_txt = got[pkg.STAGE_MEMBER][1].decode('utf-8')
            self.assertIn('MARBLE_H1_NFC_HDI_STAGE_BEGIN', stage_txt)
            subprocess.run(['sh', '-n'], input=stage_txt.encode('utf-8'), check=True)
            pb_txt = got[pkg.POSTBOOT_MEMBER][1].decode('utf-8')
            self.assertIn('start-nfc-h1-marble.sh', pb_txt)
            subprocess.run(['sh', '-n'], input=pb_txt.encode('utf-8'), check=True)
            manifest = archive.parse(got[r.MANIFEST][1])
            r.manifest_holds(got, manifest)


if __name__ == '__main__':
    unittest.main()
