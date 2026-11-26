"""Offline unit & packaging verification for Batch M1 (WDA QMAPv5 fix + Vibrator v2.0 + Color/AOD/DT2W)."""
from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DONOR_FS = ROOT / 'work/donor-rom/ADY-AL10-207.0.0.111-SP10/fs'
sys.path.insert(0, str(ROOT / 'tools/packaging'))

import build_marble_phase4_images as phase4
import build_marble_h20_m1_tel_vib_disp as m1


class TestMarbleM1(unittest.TestCase):
    def test_wda_race_fix_in_sources(self):
        probe = (ROOT / 'compat/telephony/marble_modem_probe.c').read_text(encoding='utf-8')
        self.assertIn('QMI_WDA_SET_DATA_FORMAT', probe)
        self.assertIn('Lock QMAPv5 format (llp=2, ul=5, dl=9) right after DPM_OPEN_PORT', probe)
        ril = (ROOT / 'compat/telephony/libril_vendor_qmi.c').read_text(encoding='utf-8')
        self.assertIn('ril_trace_op("imsdcm_wda"', ril)
        self.assertIn('ril_trace_op("pdp_act"', ril)
        self.assertIn('&ep_dpm, &ep_wda', ril)

    def test_sensors_sa3602_restored(self):
        doc = json.loads((ROOT / 'devices/marble/runtime/sensors/sensors-marble.json').read_text(encoding='utf-8'))
        sas = {sa['name']: sa for sa in doc['systemability']}
        self.assertIn(3601, sas)
        self.assertIn(3602, sas)
        self.assertEqual(sas[3602]['libpath'], 'libmiscdevice_service.z.so')
        self.assertEqual(sas[3602]['min_hdi_proxy_version'], ['libvibrator_proxy_2.0.z.so'])

    def test_hcb_vibrator_and_hostapd_nodes(self):
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
        )
        hcb15 = phase4.build_phase4_hdf_hcb(**kwargs, vibrator_host=False)
        hcb16 = phase4.build_phase4_hdf_hcb(**kwargs, vibrator_host=True)
        hcb17 = phase4.build_phase4_hdf_hcb(**kwargs, vibrator_host=True, hostapd_donor=DONOR_FS)
        self.assertNotIn(b'vibrator_host', hcb15)
        self.assertIn(b'vibrator_host', hcb16)
        self.assertIn(b'vibrator_interface_service', hcb16)
        self.assertNotIn(b'hostapd_host', hcb16)
        self.assertIn(b'hostapd_host', hcb17)
        self.assertIn(b'hostapd_interface_service', hcb17)

    def test_m1_patch_splices(self):
        sample_init = (
            b'{\n  "import": [\n'
            b'    "/compat/etc/init-h7-nfc-service-marble.cfg",\n'
            b'    "/compat/etc/init-modem-f12-marble.cfg"\n  ],\n'
            b'  "jobs": []\n}\n'
        )
        patched_init = m1.patch_init_debug(sample_init)
        doc = json.loads(patched_init.decode('utf-8'))
        self.assertIn('/compat/etc/init-m1-vibrator-marble.cfg', doc['import'])
        self.assertIn('/compat/etc/init-m1-touch-wakeup-marble.cfg', doc['import'])

        sample_stage = (
            b'#!/busybox sh\n# === MARBLE_F12_TEL_FIXES_STAGE_BEGIN ===\n'
            b'$B test -x "$R/compat/bin/init"\necho MARBLE_PHASE4_RAM_COMPAT_READY\n'
        )
        patched_stage = m1.patch_stage(sample_stage)
        self.assertIn(b'MARBLE_M1_VIB_DISP_STAGE_BEGIN', patched_stage)
        self.assertIn(b'/dev/uinput', patched_stage)

        sample_postboot = (
            b'#!/busybox sh\nstart-nfc-service-h7-marble.sh\n'
            b'say "U2_GNSS_MODE=hdf_location_host"\n'
        )
        patched_postboot = m1.patch_postboot(sample_postboot)
        self.assertIn(b'start-vibrator-m1-marble.sh', patched_postboot)
        self.assertIn(b'U2_TOUCH_WAKEUP_M1_TRIGGERED=1', patched_postboot)


if __name__ == '__main__':
    unittest.main()
