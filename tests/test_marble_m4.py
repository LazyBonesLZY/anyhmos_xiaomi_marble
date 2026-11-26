"""Offline unit & packaging verification for Batch M4 (Auto-Brightness ALS + Raise-to-Wake Motion HDI v1.1 + DroiTong ANCO)."""
from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DONOR_FS = ROOT / 'work/donor-rom/ADY-AL10-207.0.0.111-SP10/fs'
sys.path.insert(0, str(ROOT / 'tools/packaging'))

import build_marble_phase4_images as phase4
import build_marble_h20_m4_bright_motion_anco as m4


class TestMarbleM4(unittest.TestCase):
    def test_hcb_19_hosts_motion(self):
        kwargs = dict(
            wifi_chip_donor=DONOR_FS,
            wpa_on_demand_donor=DONOR_FS,
            a2dp_donor=DONOR_FS,
            ril_donor=DONOR_FS,
            location_host=True,
            displayengine=True,
            codec_image_host=True,
            codec_video_omx=True,
            codec_heif_hw=True,
            camera_host=True,
            nfc_host=True,
            thermal_host=True,
            power_host=True,
            usb_host=True,
            memorytracker_host=True,
            vibrator_host=True,
            hostapd_donor=DONOR_FS,
            pin_auth_host=True,
        )
        hcb18 = phase4.build_phase4_hdf_hcb(**kwargs, motion_host=False)
        hcb19 = phase4.build_phase4_hdf_hcb(**kwargs, motion_host=True)
        self.assertNotIn(b'motion_host', hcb18)
        self.assertIn(b'motion_host', hcb19)
        self.assertIn(b'motion_interface_service', hcb19)
        self.assertIn(b'libmotion_driver.z.so', hcb19)

    def test_msdp_sa2901_and_motion_host18(self):
        msdp = json.loads((ROOT / 'devices/marble/runtime/motion/msdp-marble.json').read_text(encoding='utf-8'))
        sas = {sa['name']: sa['libpath'] for sa in msdp['systemability']}
        self.assertEqual(sas.get(2901), 'libmotion_service.z.so')
        self.assertEqual(sas.get(2902), 'libdevicestatus_service.z.so')
        self.assertEqual(sas.get(2907), 'libuser_status_service.z.so')
        cfg = json.loads((ROOT / 'devices/marble/runtime/motion/init-m4-motion-marble.cfg').read_text(encoding='utf-8'))
        svcs = {s['name']: s for s in cfg['services']}
        self.assertEqual(svcs['motion_host']['path'], ['/vendor/bin/hdf_devhost', '-i', '18', '-n', 'motion_host'])

    def test_anco_updater_sa_and_broker(self):
        cfg = json.loads((ROOT / 'devices/marble/runtime/anco/init-m4-anco-marble.cfg').read_text(encoding='utf-8'))
        svcs = {s['name']: s for s in cfg['services']}
        self.assertIn('updater_sa', svcs)
        self.assertIn('broker', svcs)
        self.assertTrue(svcs['updater_sa'].get('ondemand'))
        self.assertNotIn('disabled', svcs['rgm_manager'])
        self.assertNotIn('disabled', svcs['rgm_engine_plugin'])
        self.assertNotIn('disabled', svcs['virt_device'])

    def test_m4_patch_splices(self):
        sample_init = (
            b'{\n  "import": [\n'
            b'    "/compat/etc/init-m3-pinauth-marble.cfg",\n'
            b'    "/compat/etc/init-m4-anco-marble.cfg"\n  ],\n'
            b'  "jobs": []\n}\n'
        )
        patched_init = m4.patch_init_debug(sample_init)
        doc = json.loads(patched_init.decode('utf-8'))
        self.assertIn('/compat/etc/init-m4-motion-marble.cfg', doc['import'])

        sample_platform = json.dumps({'services': [{'name': 'msdp', 'permission': ['ohos.permission.ACCELEROMETER']}]}).encode()
        patched_platform = json.loads(m4.patch_init_platform_msdp(sample_platform).decode())
        self.assertIn('ohos.permission.ACTIVITY_MOTION', patched_platform['services'][0]['permission'])
        self.assertIn('ohos.permission.GYROSCOPE', patched_platform['services'][0]['permission'])

        sample_stage = (
            b'#!/busybox sh\n# === MARBLE_M3_PINAUTH_ANCO_STAGE_BEGIN ===\n'
            b'$B test -x "$R/compat/bin/init"\necho MARBLE_PHASE4_RAM_COMPAT_READY\n'
        )
        patched_stage = m4.patch_stage(sample_stage)
        self.assertIn(b'MARBLE_M4_MOTION_BRIGHT_STAGE_BEGIN', patched_stage)
        self.assertIn(b'libmarble_motion_interface_service_1.1.z.so', patched_stage)

        sample_start = b'#!/busybox sh\npset marble.debug.start.uideps true\n'
        patched_start = m4.patch_start(sample_start)
        self.assertIn(b'marble.motion.ready true', patched_start)

        sample_postboot = b'#!/busybox sh\nstart-anco-m4-marble.sh\nsay "U2_GNSS_MODE=hdf_location_host"\n'
        patched_postboot = m4.patch_postboot(sample_postboot)
        self.assertIn(b'start-motion-m4-marble.sh', patched_postboot)


if __name__ == '__main__':
    unittest.main()
