#!/usr/bin/env python3
"""Offline unit, ABI parity (240B vtable + UsbdEventHandle), HCB host-13, and packaging verification for Batch H4 (SM7475 Hardware USB Port HDI v2.0)."""
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

import build_marble_h20_h4_usb_port as pkg

USB_IMPL_CPP = ROOT / 'compat/usb/marble_usb_port_hdi_impl.cpp'
LLVM_READELF = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-readelf'
LLVM_NM = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-nm'


class MarbleBatchH4Tests(unittest.TestCase):
    def test_h4_usb_port_hdi_elf_vtable_abi_parity(self):
        hdi_so = pkg.H4_BUILD / 'libmarble_usb_port_interface_service_2.0.z.so'
        probe_bin = pkg.H4_BUILD / 'native_usb_port_hdi_probe'
        for p in (hdi_so, probe_bin):
            self.assertTrue(p.is_file(), f'Missing H4 artifact: {p}')

        dyn = subprocess.check_output([str(LLVM_READELF), '-d', str(hdi_so)], text=True)
        self.assertEqual(re.findall(r'\(SONAME\)\s+Library soname:\s+\[([^\]]+)\]', dyn),
                         ['libusb_port_interface_service_2.0.z.so'])
        needed = re.findall(r'\(NEEDED\)\s+Shared library:\s+\[([^\]]+)\]', dyn)
        self.assertEqual(needed, ['libutils.z.so', 'libc++.so', 'libc.so'])

        nm = subprocess.check_output([str(LLVM_NM), '-D', str(hdi_so)], text=True)
        self.assertIn(' UsbPortInterfaceImplGetInstance', nm)
        self.assertIn(' _ZN4OHOS3HDI3Usb4V2_011UsbPortImpl15UsbdEventHandleERKNS_4sptrIS3_EE', nm)

        syms = subprocess.check_output([str(LLVM_READELF), '-sW', str(hdi_so)], text=True)
        vtv_sizes = []
        for line in syms.splitlines():
            m = re.match(r'\s*\d+:\s+[0-9a-f]+\s+(\d+)\s+\S+\s+\S+\s+\S+\s+\S+\s+(_ZTVN4OHOS3HDI3Usb4V2_011UsbPortImplE)', line)
            if m:
                vtv_sizes.append(int(m.group(1)))
        self.assertTrue(vtv_sizes and all(sz == 240 for sz in vtv_sizes),
                        f'Unexpected UsbPortImpl vtable sizes: {vtv_sizes}')

        p_dyn = subprocess.check_output([str(LLVM_READELF), '-d', str(probe_bin)], text=True)
        p_needed = re.findall(r'\(NEEDED\)\s+Shared library:\s+\[([^\]]+)\]', p_dyn)
        self.assertIn('libusb_proxy_2.0.z.so', p_needed)

    def test_h4_runtime_configs_and_com9_safety_guards(self):
        cfg = json.loads(pkg.USB_H4_CFG.read_text(encoding='utf-8'))
        svcs = {s['name']: s for s in cfg['services']}
        self.assertIn('usb_host', svcs)
        self.assertEqual(svcs['usb_host']['path'], ['/vendor/bin/hdf_devhost', '-i', '13', '-n', 'usb_host'])
        self.assertEqual(svcs['usb_host']['uid'], 'usb_host')

        sh_raw = pkg.USB_H4_SCRIPT.read_bytes()
        self.assertNotIn(b'\r', sh_raw)
        subprocess.run(['sh', '-n'], input=sh_raw, check=True)

        impl_txt = USB_IMPL_CPP.read_text(encoding='utf-8')
        code_only = re.sub(r'/\*.*?\*/', '', impl_txt, flags=re.S)
        self.assertIn('/sys/class/typec', code_only)
        # Never write to sysfs Type-C role files or UDC (would drop USB/COM9!)
        self.assertNotIn('fopen(', code_only.replace('std::fopen(path.c_str(), "re")', ''))
        self.assertNotIn('/config/usb_gadget', code_only)
        self.assertNotIn('open(', code_only.replace('std::fopen(', '').replace('opendir(', ''))

    def test_h4_package_report_and_68_guards(self):
        self.assertEqual(len(pkg.GUARD_MEMBERS), 68)
        report_path = pkg.H4_BUILD / 'h4-build-report.json'
        if not report_path.is_file():
            self.skipTest('h4-build-report.json not generated yet')
        report = json.loads(report_path.read_text(encoding='utf-8'))
        cand = Path(report['candidate']['path'])
        if not cand.is_file():
            self.skipTest(f'Candidate image missing: {cand}')

        self.assertEqual(report['base']['sha256'], pkg.BASE_SHA)
        self.assertEqual(len(report['guard_members_verified']), 68)
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
            self.assertIn(b'usb_host', hcb)
            self.assertIn(b'libusb_port_driver.z.so', hcb)
            self.assertIn(b'usb_port_interface_service', hcb)
            init_doc = json.loads(got[pkg.INIT_DEBUG_MEMBER][1].decode('utf-8'))
            self.assertIn('/compat/etc/init-h4-usb-port-marble.cfg', init_doc['import'])
            stage_txt = got[pkg.STAGE_MEMBER][1].decode('utf-8')
            self.assertIn('MARBLE_H4_USB_PORT_HDI_STAGE_BEGIN', stage_txt)
            subprocess.run(['sh', '-n'], input=stage_txt.encode('utf-8'), check=True)
            pb_txt = got[pkg.POSTBOOT_MEMBER][1].decode('utf-8')
            self.assertIn('start-usb-port-h4-marble.sh', pb_txt)
            subprocess.run(['sh', '-n'], input=pb_txt.encode('utf-8'), check=True)
            manifest = archive.parse(got[r.MANIFEST][1])
            r.manifest_holds(got, manifest)


if __name__ == '__main__':
    unittest.main()
