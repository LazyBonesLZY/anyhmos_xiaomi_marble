#!/usr/bin/env python3
"""Offline unit, ABI, and packaging verification for Batch G2 (OHOS Camera HDI + SA 3008)."""
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

import build_marble_h20_g2_camera_hdi as pkg
import build_marble_phase4_images as p4

VDI_CPP = ROOT / 'compat/camera/marble_camera_vdi_impl.cpp'
PROBE_HDI_CPP = ROOT / 'compat/camera/native_camera_hdi_probe.cpp'
LLVM_READELF = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-readelf'
LLVM_NM = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-nm'
DONOR_USB_VDI = pkg.DONOR_FS / 'vendor/lib64/libcamera_external_usb_camera.so'


class MarbleBatchG2Tests(unittest.TestCase):
    def test_g2_vdi_and_probe_host_self_tests(self):
        with tempfile.TemporaryDirectory() as td:
            vdi_host = Path(td) / 'vdi_host_test'
            probe_host = Path(td) / 'probe_hdi_host_test'
            subprocess.run(
                ['g++', '-O2', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                 '-DMARBLE_CAMERA_VDI_HOST_TEST', str(VDI_CPP), '-o', str(vdi_host)],
                check=True,
            )
            res = subprocess.run([str(vdi_host)], check=True, capture_output=True, text=True)
            self.assertIn('MARBLE_CAMERA_VDI_HOST_TEST_OK=1', res.stdout)
            self.assertIn('sensors=4', res.stdout)

            subprocess.run(
                ['g++', '-O2', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                 '-DMARBLE_CAMERA_HDI_PROBE_HOST_TEST', str(PROBE_HDI_CPP), '-o', str(probe_host)],
                check=True,
            )
            res2 = subprocess.run([str(probe_host)], check=True, capture_output=True, text=True)
            self.assertIn('MARBLE_CAMERA_HDI_PROBE_SELF_TEST_OK=1', res2.stdout)

    def test_g2_vdi_elf_and_vtable_abi_parity(self):
        vdi_so = pkg.G2_BUILD / 'libmarble_camera_vdi_impl.z.so'
        self.assertTrue(vdi_so.is_file(), f'Missing VDI shared library: {vdi_so}')
        dyn = subprocess.check_output([str(LLVM_READELF), '-d', str(vdi_so)], text=True)
        soname = re.findall(r'\(SONAME\)\s+Library soname:\s+\[([^\]]+)\]', dyn)
        self.assertEqual(soname, ['libmarble_camera_vdi_impl.z.so'])
        nm = subprocess.check_output([str(LLVM_NM), '-D', str(vdi_so)], text=True)
        self.assertIn(' hdfVdiDesc', nm)

        # Verify vtable sizes match donor CameraHostVdiImpl (224), CameraDeviceVdiImpl (232),
        # StreamOperatorVdiImpl (256) with OHOS_PLATFORM RefBase::CanPromote slot present.
        syms = subprocess.check_output([str(LLVM_READELF), '-sW', str(vdi_so)], text=True)
        sizes = {}
        for line in syms.splitlines():
            m = re.match(r'\s*\d+:\s+[0-9a-f]+\s+(\d+)\s+\S+\s+\S+\s+\S+\s+\S+\s+(_ZTV\S+)', line)
            if m:
                sizes[m.group(2)] = int(m.group(1))
        host_sz = [sz for n, sz in sizes.items() if 'MarbleCameraHostVdi' in n]
        dev_sz = [sz for n, sz in sizes.items() if 'MarbleCameraDeviceVdi' in n]
        op_sz = [sz for n, sz in sizes.items() if 'MarbleStreamOperatorVdi' in n]
        self.assertEqual(host_sz, [224])
        self.assertEqual(dev_sz, [232])
        self.assertEqual(op_sz, [256])

    def test_g2_hcb_builders_and_cfg_script(self):
        hcb10 = p4.build_phase4_hdf_hcb(
            wifi_chip_donor=pkg.DONOR_FS,
            wpa_on_demand_donor=pkg.DONOR_FS,
            a2dp_donor=pkg.DONOR_FS,
            ril_donor=pkg.DONOR_FS,
            location_host=True,
            displayengine=True,
            codec_image_host=True,
            codec_video_omx=True,
            codec_heif_hw=True,
            camera_host=True,
        )
        self.assertIn(b'camera_host\x00', hcb10)
        self.assertIn(b'libcamera_host_service_1.0.z.so\x00', hcb10)
        self.assertIn(b'camera_service\x00', hcb10)

        cam_hcb = p4.build_marble_camera_host_config_hcb()
        self.assertTrue(cam_hcb.startswith(b'\x0a\xa0\x0a\xa0'))
        self.assertIn(b'vdiLibList\x00', cam_hcb)
        self.assertIn(b'libcamera_external_usb_camera.so\x00', cam_hcb)

        cfg = json.loads(pkg.CAM_G2_CFG.read_text(encoding='utf-8'))
        svcs = {s['name']: s for s in cfg['services']}
        self.assertEqual(svcs['camera_host']['path'], ['/vendor/bin/hdf_devhost', '-i', '9', '-n', 'camera_host'])
        self.assertNotIn('ondemand', svcs['camera_host'])
        self.assertEqual(svcs['camera_service']['path'], ['/system/bin/sa_main', '/system/profile/camera_service.json'])

        script_raw = pkg.CAM_G2_SCRIPT.read_bytes()
        self.assertNotIn(b'\r', script_raw)
        subprocess.run(['sh', '-n'], input=script_raw, check=True)
        txt = script_raw.decode('utf-8')
        for marker in ('chown 0:3090', 'marble.camera.ready true', 'CAMERA_G2_DONE=1', '*,camera_hdi,*'):
            self.assertIn(marker, txt)

    def test_g2_package_report_and_guards(self):
        self.assertEqual(len(pkg.GUARD_MEMBERS), 47)
        report_path = pkg.G2_BUILD / 'g2-build-report.json'
        if not report_path.is_file():
            self.skipTest('g2-build-report.json not generated yet')
        report = json.loads(report_path.read_text(encoding='utf-8'))
        cand = Path(report['candidate']['path'])
        if not cand.is_file():
            self.skipTest(f'Candidate image missing: {cand}')

        self.assertEqual(report['base']['sha256'], pkg.BASE_SHA)
        self.assertEqual(len(report['guard_members_verified']), 47)
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
            stage_txt = got[pkg.STAGE_MEMBER][1].decode('utf-8')
            self.assertIn('MARBLE_G2_CAMERA_HDI_STAGE_BEGIN', stage_txt)
            self.assertIn('g2_bind() {', stage_txt)
            self.assertNotIn('bind_once "$R/compat/lib64/libmarble_camera_vdi_impl.z.so"', stage_txt)
            self.assertNotIn(' 3008 ', stage_txt)
            init_cfg = json.loads(got[pkg.INIT_DEBUG_MEMBER][1].decode('utf-8'))
            self.assertIn('/compat/etc/init-g2-camera-marble.cfg', init_cfg['import'])
            post_txt = got[pkg.POSTBOOT_MEMBER][1].decode('utf-8')
            self.assertIn('start-camera-g2-marble.sh', post_txt)
            self.assertIn(b'camera_host\x00', got[pkg.HCB_MEMBER][1])
            manifest = archive.parse(got[r.MANIFEST][1])
            r.manifest_holds(got, manifest)


if __name__ == '__main__':
    unittest.main()
