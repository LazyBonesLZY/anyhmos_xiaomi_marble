#!/usr/bin/env python3
"""Offline unit, host DYNAMIC_HANDLE EOS test, ELF hygiene, and packaging verification for Batch H6."""
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

import build_marble_h20_h6_venc_eos as pkg

LLVM_READELF = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-readelf'


class MarbleBatchH6Tests(unittest.TestCase):
    def test_h6_omx_host_dynamic_handle_eos_self_test(self):
        with tempfile.TemporaryDirectory(prefix='h6_omx_host_') as td:
            exe = Path(td) / 'omx_venus_host_test'
            subprocess.run(
                ['cc', '-std=c11', '-DOMX_VENUS_HOST_TEST', '-O2', '-Wall', '-Wextra', '-Werror',
                 '-pthread', str(ROOT / 'compat/codec/omx_core_venus.c'),
                 str(ROOT / 'compat/codec/venus_v4l2.c'), '-o', str(exe)],
                check=True,
            )
            res = subprocess.run([str(exe)], check=True, capture_output=True, text=True, timeout=10)
            self.assertIn('OMX_VENUS_HOST_TEST_OK', res.stdout)

    def test_h6_elf_contracts(self):
        omx_so = pkg.H6_BUILD / 'libOMX_Core.z.so'
        probe_bin = pkg.H6_BUILD / 'native_venc_probe'
        for p in (omx_so, probe_bin):
            self.assertTrue(p.is_file(), f'Missing H6 artifact: {p}')

        dyn = subprocess.check_output([str(LLVM_READELF), '-d', str(omx_so)], text=True)
        self.assertEqual(re.findall(r'\(SONAME\)\s+Library soname:\s+\[([^\]]+)\]', dyn), ['libOMX_Core.z.so'])
        self.assertEqual(re.findall(r'\(NEEDED\)\s+Shared library:\s+\[([^\]]+)\]', dyn), ['libc.so'])

        p_dyn = subprocess.check_output([str(LLVM_READELF), '-d', str(probe_bin)], text=True)
        p_needed = re.findall(r'\(NEEDED\)\s+Shared library:\s+\[([^\]]+)\]', p_dyn)
        for req in ('libnative_media_venc.so', 'libnative_media_vdec.so', 'libnative_media_codecbase.so'):
            self.assertIn(req, p_needed)

    def test_h6_package_report_and_78_guards(self):
        self.assertEqual(len(pkg.GUARD_MEMBERS), 78)
        report_path = pkg.H6_BUILD / 'h6-build-report.json'
        if not report_path.is_file():
            self.skipTest('h6-build-report.json not generated yet')
        report = json.loads(report_path.read_text(encoding='utf-8'))
        cand = Path(report['candidate']['path'])
        if not cand.is_file():
            self.skipTest(f'Candidate image missing: {cand}')

        self.assertEqual(report['base']['sha256'], pkg.BASE_SHA)
        self.assertEqual(len(report['guard_members_verified']), 78)
        self.assertEqual(report['entries_after'], report['entries_before'] + 1)

        archive, r = pkg.load_oracle()
        img = r.read_original(cand)
        layout = r.layout(img)
        avb = r.verify_existing_avb(img, layout['payload_end'])
        self.assertIn('footer_masked', avb)

        with tempfile.TemporaryDirectory() as td:
            got, *_ = r.extract_entries(cand, img, layout, Path(td))
            for rel, meta in report['staged'].items():
                self.assertEqual(r.sha(got[pkg.PREFIX + rel][1]), meta['sha256'])
            stage_txt = got[pkg.STAGE_MEMBER][1].decode('utf-8')
            self.assertIn('MARBLE_H6_VENC_PROBE_STAGE_BEGIN', stage_txt)
            subprocess.run(['sh', '-n'], input=stage_txt.encode('utf-8'), check=True)
            manifest = archive.parse(got[r.MANIFEST][1])
            r.manifest_holds(got, manifest)


if __name__ == '__main__':
    unittest.main()
