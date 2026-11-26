"""Offline unit & packaging verification for Batch M3 (Venus HW Decode + Lockscreen PIN + DroiTong ANCO)."""
from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DONOR_FS = ROOT / 'work/donor-rom/ADY-AL10-207.0.0.111-SP10/fs'
sys.path.insert(0, str(ROOT / 'tools/packaging'))

import build_marble_phase4_images as phase4
import build_marble_h20_m3_codec_pin_anco as m3


class TestMarbleM3(unittest.TestCase):
    def test_hcb_18_hosts_pin_auth(self):
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
        )
        hcb17 = phase4.build_phase4_hdf_hcb(**kwargs, pin_auth_host=False)
        hcb18 = phase4.build_phase4_hdf_hcb(**kwargs, pin_auth_host=True)
        self.assertNotIn(b'pin_auth_host', hcb17)
        self.assertIn(b'pin_auth_host', hcb18)
        self.assertIn(b'pin_auth_interface_service', hcb18)
        self.assertIn(b'libpin_auth_driver.z.so', hcb18)

    def test_useriam_sa941_and_host17(self):
        doc = json.loads((ROOT / 'devices/marble/runtime/pinauth/useriam-core-marble.json').read_text(encoding='utf-8'))
        sas = {sa['name']: sa['libpath'] for sa in doc['systemability']}
        self.assertEqual(sas.get(941), 'libpinauthservice.z.so')
        self.assertEqual(sas.get(901), 'libuserauthservice.z.so')
        cfg = json.loads((ROOT / 'devices/marble/runtime/pinauth/init-m3-pinauth-marble.cfg').read_text(encoding='utf-8'))
        svcs = {s['name']: s for s in cfg['services']}
        self.assertEqual(svcs['pin_auth_host']['path'], ['/vendor/bin/hdf_devhost', '-i', '17', '-n', 'pin_auth_host'])
        wire = (ROOT / 'compat/telephony/ims_core_wire.h').read_text(encoding='utf-8')
        self.assertIn('#define IMS_PROXY_TYPE_CALL 0', wire)
        self.assertIn('#define IMS_PROXY_TYPE_SMS 1', wire)

    def test_m3_patch_splices(self):
        sample_init = (
            b'{\n  "import": [\n'
            b'    "/compat/etc/init-m4-mi-thermal-marble.cfg",\n'
            b'    "/compat/etc/init-m4-softap-marble.cfg"\n  ],\n'
            b'  "jobs": []\n}\n'
        )
        patched_init = m3.patch_init_debug(sample_init)
        doc = json.loads(patched_init.decode('utf-8'))
        self.assertIn('/compat/etc/init-m3-pinauth-marble.cfg', doc['import'])
        self.assertIn('/compat/etc/init-m4-anco-marble.cfg', doc['import'])

        sample_stage = (
            b'#!/busybox sh\n# === MARBLE_M2_CAM_THERMAL_SOFTAP_STAGE_BEGIN ===\n'
            b'$B test -x "$R/compat/bin/init"\necho MARBLE_PHASE4_RAM_COMPAT_READY\n'
        )
        patched_stage = m3.patch_stage(sample_stage)
        self.assertIn(b'MARBLE_M3_PINAUTH_ANCO_STAGE_BEGIN', patched_stage)
        self.assertIn(b'libpin_auth_interface_service_3.0.z.so', patched_stage)

        sample_start = (
            b'#!/busybox sh\nstage=services\n'
            b'for gate in storage kvdata useriam account appspawn theme wallpaper ui diagnostics deviceproviders b1; do\n'
            b' pset "marble.debug.start.$gate" true\ndone\n'
        )
        patched_start = m3.patch_start(sample_start)
        self.assertIn(b'marble.pinauth.ready true', patched_start)

        sample_postboot = (
            b'#!/busybox sh\nstart-softap-m4-marble.sh\n'
            b'say "U2_GNSS_MODE=hdf_location_host"\n'
        )
        patched_postboot = m3.patch_postboot(sample_postboot)
        self.assertIn(b'start-anco-m4-marble.sh', patched_postboot)


if __name__ == '__main__':
    unittest.main()
