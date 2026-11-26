#!/usr/bin/env python3
"""Offline unit, ABI parity, HCB host-11, and packaging verification for Batch H2 (SM7475 Hardware Thermal HDI v1.1)."""
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

import build_marble_h20_h2_thermal as pkg

THERMAL_IMPL_CPP = ROOT / 'compat/thermal/marble_thermal_hdi_impl.cpp'
LLVM_READELF = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-readelf'
LLVM_NM = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-nm'


class MarbleBatchH2Tests(unittest.TestCase):
    def test_h2_thermal_hdi_elf_vtable_abi_parity(self):
        hdi_so = pkg.H2_BUILD / 'libmarble_thermal_interface_service_1.1.z.so'
        probe_bin = pkg.H2_BUILD / 'native_thermal_hdi_probe'
        for p in (hdi_so, probe_bin):
            self.assertTrue(p.is_file(), f'Missing H2 artifact: {p}')

        dyn = subprocess.check_output([str(LLVM_READELF), '-d', str(hdi_so)], text=True)
        self.assertEqual(re.findall(r'\(SONAME\)\s+Library soname:\s+\[([^\]]+)\]', dyn),
                         ['libthermal_interface_service_1.1.z.so'])
        needed = re.findall(r'\(NEEDED\)\s+Shared library:\s+\[([^\]]+)\]', dyn)
        self.assertEqual(needed, ['libutils.z.so', 'libc++.so', 'libc.so'])

        nm = subprocess.check_output([str(LLVM_NM), '-D', str(hdi_so)], text=True)
        self.assertIn(' ThermalInterfaceImplGetInstance', nm)
        self.assertIn(' marble_thermal_scan_summary', nm)

        syms = subprocess.check_output([str(LLVM_READELF), '-sW', str(hdi_so)], text=True)
        vtv_sizes = []
        for line in syms.splitlines():
            m = re.match(r'\s*\d+:\s+[0-9a-f]+\s+(\d+)\s+\S+\s+\S+\s+\S+\s+\S+\s+(_ZTVN4OHOS3HDI7Thermal4V1_120ThermalInterfaceImplE)', line)
            if m:
                vtv_sizes.append(int(m.group(1)))
        self.assertTrue(vtv_sizes and all(sz == 272 for sz in vtv_sizes),
                        f'Unexpected ThermalInterfaceImpl vtable sizes: {vtv_sizes}')

        p_dyn = subprocess.check_output([str(LLVM_READELF), '-d', str(probe_bin)], text=True)
        p_needed = re.findall(r'\(NEEDED\)\s+Shared library:\s+\[([^\]]+)\]', p_dyn)
        self.assertIn('libthermal_proxy_1.1.z.so', p_needed)

    def test_h2_runtime_configs_and_hcb_host11(self):
        cfg = json.loads(pkg.THERMAL_H2_CFG.read_text(encoding='utf-8'))
        svcs = {s['name']: s for s in cfg['services']}
        self.assertIn('thermal_host', svcs)
        self.assertEqual(svcs['thermal_host']['path'], ['/vendor/bin/hdf_devhost', '-i', '11', '-n', 'thermal_host'])
        self.assertEqual(svcs['thermal_host']['uid'], 'power_host')

        sh_raw = pkg.THERMAL_H2_SCRIPT.read_bytes()
        self.assertNotIn(b'\r', sh_raw)
        subprocess.run(['sh', '-n'], input=sh_raw, check=True)

        impl_txt = THERMAL_IMPL_CPP.read_text(encoding='utf-8')
        for zone_tok in ('"cpuss-"', '"gpuss-"', '"aoss-"', '"battery"', '"usb"', '"mdmss-"', '"camera-"', '"shell"', '"ambient"'):
            self.assertIn(zone_tok, impl_txt)

    def test_h2_package_report_and_60_guards(self):
        self.assertEqual(len(pkg.GUARD_MEMBERS), 60)
        report_path = pkg.H2_BUILD / 'h2-build-report.json'
        if not report_path.is_file():
            self.skipTest('h2-build-report.json not generated yet')
        report = json.loads(report_path.read_text(encoding='utf-8'))
        cand = Path(report['candidate']['path'])
        if not cand.is_file():
            self.skipTest(f'Candidate image missing: {cand}')

        self.assertEqual(report['base']['sha256'], pkg.BASE_SHA)
        self.assertEqual(len(report['guard_members_verified']), 60)
        self.assertEqual(report['entries_after'], report['entries_before'] + 4)

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
            self.assertIn(b'thermal_host', hcb)
            self.assertIn(b'libthermal_driver.z.so', hcb)
            self.assertIn(b'thermal_interface_service', hcb)
            init_doc = json.loads(got[pkg.INIT_DEBUG_MEMBER][1].decode('utf-8'))
            self.assertIn('/compat/etc/init-h2-thermal-marble.cfg', init_doc['import'])
            stage_txt = got[pkg.STAGE_MEMBER][1].decode('utf-8')
            self.assertIn('MARBLE_H2_THERMAL_HDI_STAGE_BEGIN', stage_txt)
            subprocess.run(['sh', '-n'], input=stage_txt.encode('utf-8'), check=True)
            pb_txt = got[pkg.POSTBOOT_MEMBER][1].decode('utf-8')
            self.assertIn('start-thermal-h2-marble.sh', pb_txt)
            subprocess.run(['sh', '-n'], input=pb_txt.encode('utf-8'), check=True)
            manifest = archive.parse(got[r.MANIFEST][1])
            r.manifest_holds(got, manifest)


if __name__ == '__main__':
    unittest.main()
