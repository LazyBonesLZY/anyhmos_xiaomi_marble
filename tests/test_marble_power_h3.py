#!/usr/bin/env python3
"""Offline unit, ABI parity (360B vtable), HCB host-12, and packaging verification for Batch H3 (SM7475 Hardware Power HDI v1.3)."""
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

import build_marble_h20_h3_power as pkg

POWER_IMPL_CPP = ROOT / 'compat/power/marble_power_hdi_impl.cpp'
LLVM_READELF = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-readelf'
LLVM_NM = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-nm'


class MarbleBatchH3Tests(unittest.TestCase):
    def test_h3_power_hdi_elf_vtable_abi_parity(self):
        hdi_so = pkg.H3_BUILD / 'libmarble_power_interface_service_1.3.z.so'
        probe_bin = pkg.H3_BUILD / 'native_power_hdi_probe'
        for p in (hdi_so, probe_bin):
            self.assertTrue(p.is_file(), f'Missing H3 artifact: {p}')

        dyn = subprocess.check_output([str(LLVM_READELF), '-d', str(hdi_so)], text=True)
        self.assertEqual(re.findall(r'\(SONAME\)\s+Library soname:\s+\[([^\]]+)\]', dyn),
                         ['libpower_interface_service_1.3.z.so'])
        needed = re.findall(r'\(NEEDED\)\s+Shared library:\s+\[([^\]]+)\]', dyn)
        self.assertEqual(needed, ['libutils.z.so', 'libc++.so', 'libc.so'])

        nm = subprocess.check_output([str(LLVM_NM), '-D', str(hdi_so)], text=True)
        self.assertIn(' PowerInterfaceImplGetInstance', nm)

        syms = subprocess.check_output([str(LLVM_READELF), '-sW', str(hdi_so)], text=True)
        vtv_sizes = []
        for line in syms.splitlines():
            m = re.match(r'\s*\d+:\s+[0-9a-f]+\s+(\d+)\s+\S+\s+\S+\s+\S+\s+\S+\s+(_ZTVN4OHOS3HDI5Power4V1_318PowerInterfaceImplE)', line)
            if m:
                vtv_sizes.append(int(m.group(1)))
        self.assertTrue(vtv_sizes and all(sz == 360 for sz in vtv_sizes),
                        f'Unexpected PowerInterfaceImpl vtable sizes: {vtv_sizes}')

        p_dyn = subprocess.check_output([str(LLVM_READELF), '-d', str(probe_bin)], text=True)
        p_needed = re.findall(r'\(NEEDED\)\s+Shared library:\s+\[([^\]]+)\]', p_dyn)
        self.assertIn('libpower_proxy_1.3.z.so', p_needed)
        self.assertIn('libpower_proxy_1.2.z.so', p_needed)

    def test_h3_runtime_configs_and_safety_guards(self):
        cfg = json.loads(pkg.POWER_H3_CFG.read_text(encoding='utf-8'))
        svcs = {s['name']: s for s in cfg['services']}
        self.assertIn('power_host', svcs)
        self.assertEqual(svcs['power_host']['path'], ['/vendor/bin/hdf_devhost', '-i', '12', '-n', 'power_host'])
        self.assertEqual(svcs['power_host']['uid'], 'power_host')

        sh_raw = pkg.POWER_H3_SCRIPT.read_bytes()
        self.assertNotIn(b'\r', sh_raw)
        subprocess.run(['sh', '-n'], input=sh_raw, check=True)

        impl_txt = POWER_IMPL_CPP.read_text(encoding='utf-8')
        self.assertIn('/sys/power/wake_lock', impl_txt)
        self.assertIn('/sys/power/wake_unlock', impl_txt)
        # Never open blocking /sys/power/wakeup_count
        self.assertNotIn('open("/sys/power/wakeup_count"', impl_txt)
        self.assertNotIn('readSmallFile("/sys/power/wakeup_count"', impl_txt)

    def test_h3_package_report_and_64_guards(self):
        self.assertEqual(len(pkg.GUARD_MEMBERS), 64)
        report_path = pkg.H3_BUILD / 'h3-build-report.json'
        if not report_path.is_file():
            self.skipTest('h3-build-report.json not generated yet')
        report = json.loads(report_path.read_text(encoding='utf-8'))
        cand = Path(report['candidate']['path'])
        if not cand.is_file():
            self.skipTest(f'Candidate image missing: {cand}')

        self.assertEqual(report['base']['sha256'], pkg.BASE_SHA)
        self.assertEqual(len(report['guard_members_verified']), 64)
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
            self.assertIn(b'power_host', hcb)
            self.assertIn(b'libpower_driver.z.so', hcb)
            self.assertIn(b'power_interface_service', hcb)
            init_doc = json.loads(got[pkg.INIT_DEBUG_MEMBER][1].decode('utf-8'))
            self.assertIn('/compat/etc/init-h3-power-marble.cfg', init_doc['import'])
            stage_txt = got[pkg.STAGE_MEMBER][1].decode('utf-8')
            self.assertIn('MARBLE_H3_POWER_HDI_STAGE_BEGIN', stage_txt)
            subprocess.run(['sh', '-n'], input=stage_txt.encode('utf-8'), check=True)
            pb_txt = got[pkg.POSTBOOT_MEMBER][1].decode('utf-8')
            self.assertIn('start-power-h3-marble.sh', pb_txt)
            subprocess.run(['sh', '-n'], input=pb_txt.encode('utf-8'), check=True)
            manifest = archive.parse(got[r.MANIFEST][1])
            r.manifest_holds(got, manifest)


if __name__ == '__main__':
    unittest.main()
