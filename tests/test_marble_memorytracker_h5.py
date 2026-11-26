#!/usr/bin/env python3
"""Offline unit, ABI parity (208B vtable), HCB host-14, and packaging verification for Batch H5 (SM7475 Hardware MemoryTracker HDI v1.0)."""
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

import build_marble_h20_h5_memorytracker as pkg

MT_IMPL_CPP = ROOT / 'compat/memorytracker/marble_memorytracker_hdi_impl.cpp'
LLVM_READELF = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-readelf'
LLVM_NM = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-nm'


class MarbleBatchH5Tests(unittest.TestCase):
    def test_h5_memorytracker_hdi_elf_vtable_abi_parity(self):
        hdi_so = pkg.H5_BUILD / 'libmarble_memory_tracker_interface_service_1.0.z.so'
        probe_bin = pkg.H5_BUILD / 'native_memorytracker_hdi_probe'
        for p in (hdi_so, probe_bin):
            self.assertTrue(p.is_file(), f'Missing H5 artifact: {p}')

        dyn = subprocess.check_output([str(LLVM_READELF), '-d', str(hdi_so)], text=True)
        self.assertEqual(re.findall(r'\(SONAME\)\s+Library soname:\s+\[([^\]]+)\]', dyn),
                         ['libmemory_tracker_interface_service_1.0.z.so'])
        needed = re.findall(r'\(NEEDED\)\s+Shared library:\s+\[([^\]]+)\]', dyn)
        self.assertEqual(needed, ['libutils.z.so', 'libc++.so', 'libc.so'])

        nm = subprocess.check_output([str(LLVM_NM), '-D', str(hdi_so)], text=True)
        self.assertIn(' MemoryTrackerInterfaceImplGetInstance', nm)

        syms = subprocess.check_output([str(LLVM_READELF), '-sW', str(hdi_so)], text=True)
        vtv_sizes = []
        for line in syms.splitlines():
            m = re.match(r'\s*\d+:\s+[0-9a-f]+\s+(\d+)\s+\S+\s+\S+\s+\S+\s+\S+\s+(_ZTVN4OHOS3HDI13Memorytracker4V1_026MemoryTrackerInterfaceImplE)', line)
            if m:
                vtv_sizes.append(int(m.group(1)))
        self.assertTrue(vtv_sizes and all(sz == 208 for sz in vtv_sizes),
                        f'Unexpected MemoryTrackerInterfaceImpl vtable sizes: {vtv_sizes}')

        p_dyn = subprocess.check_output([str(LLVM_READELF), '-d', str(probe_bin)], text=True)
        p_needed = re.findall(r'\(NEEDED\)\s+Shared library:\s+\[([^\]]+)\]', p_dyn)
        self.assertIn('libmemorytracker_proxy_1.0.z.so', p_needed)

    def test_h5_runtime_configs_and_readonly_safety(self):
        cfg = json.loads(pkg.MT_H5_CFG.read_text(encoding='utf-8'))
        svcs = {s['name']: s for s in cfg['services']}
        self.assertIn('memorytracker_host', svcs)
        self.assertEqual(svcs['memorytracker_host']['path'], ['/vendor/bin/hdf_devhost', '-i', '14', '-n', 'memorytracker_host'])
        self.assertEqual(svcs['memorytracker_host']['uid'], 'memmgr')

        sh_raw = pkg.MT_H5_SCRIPT.read_bytes()
        self.assertNotIn(b'\r', sh_raw)
        subprocess.run(['sh', '-n'], input=sh_raw, check=True)

        code_only = re.sub(r'/\*.*?\*/', '', MT_IMPL_CPP.read_text(encoding='utf-8'), flags=re.S)
        self.assertIn('/sys/kernel/dmabuf/buffers', code_only)
        self.assertNotIn('"w', code_only)
        self.assertNotIn('O_WRONLY', code_only)
        self.assertNotIn('O_RDWR', code_only)

    def test_h5_package_report_and_72_guards(self):
        self.assertEqual(len(pkg.GUARD_MEMBERS), 72)
        report_path = pkg.H5_BUILD / 'h5-build-report.json'
        if not report_path.is_file():
            self.skipTest('h5-build-report.json not generated yet')
        report = json.loads(report_path.read_text(encoding='utf-8'))
        cand = Path(report['candidate']['path'])
        if not cand.is_file():
            self.skipTest(f'Candidate image missing: {cand}')

        self.assertEqual(report['base']['sha256'], pkg.BASE_SHA)
        self.assertEqual(len(report['guard_members_verified']), 72)
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
            self.assertIn(b'memorytracker_host', hcb)
            self.assertIn(b'libmemorytracker_driver.z.so', hcb)
            self.assertIn(b'memory_tracker_interface_service', hcb)
            init_doc = json.loads(got[pkg.INIT_DEBUG_MEMBER][1].decode('utf-8'))
            self.assertIn('/compat/etc/init-h5-memorytracker-marble.cfg', init_doc['import'])
            stage_txt = got[pkg.STAGE_MEMBER][1].decode('utf-8')
            self.assertIn('MARBLE_H5_MEMORYTRACKER_HDI_STAGE_BEGIN', stage_txt)
            subprocess.run(['sh', '-n'], input=stage_txt.encode('utf-8'), check=True)
            pb_txt = got[pkg.POSTBOOT_MEMBER][1].decode('utf-8')
            self.assertIn('start-memorytracker-h5-marble.sh', pb_txt)
            subprocess.run(['sh', '-n'], input=pb_txt.encode('utf-8'), check=True)
            manifest = archive.parse(got[r.MANIFEST][1])
            r.manifest_holds(got, manifest)


if __name__ == '__main__':
    unittest.main()
