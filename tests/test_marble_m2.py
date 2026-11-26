"""Offline unit & packaging verification for Batch M2 (IMS SMS + Stutter Fix + Camera V1_2/Torch + FastCharge + mi_thermald + SoftAP)."""
from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DONOR_FS = ROOT / 'work/donor-rom/ADY-AL10-207.0.0.111-SP10/fs'
sys.path.insert(0, str(ROOT / 'tools/packaging'))

import build_marble_phase4_images as phase4
import build_marble_h20_m2_cam_ims_softap_chg as m2


class TestMarbleM2(unittest.TestCase):
    def test_ims_sms_interface_in_sources(self):
        wire = (ROOT / 'compat/telephony/ims_core_wire.h').read_text(encoding='utf-8')
        self.assertIn('IMS_SMS_CMD_SEND_MESSAGE 6000u', wire)
        self.assertIn('IMS_SMS_CMD_REGISTER_CALLBACK 6500u', wire)
        self.assertIn('sizeof(ImsSendSmsResultWire) == 48', wire)
        self.assertIn('sizeof(ImsRadioResponseInfoWire) == 16', wire)
        svc = (ROOT / 'compat/telephony/hmos_ims_core_service.cpp').read_text(encoding='utf-8')
        self.assertIn('OHOS.Telephony.ImsSmsInterface', svc)
        self.assertIn('OHOS.Telephony.ImsSmsCallback', svc)
        self.assertIn('IMS_PROXY_TYPE_SMS', svc)
        self.assertIn('kQmiWmsRawSend', svc)

    def test_display_stutter_fix_in_sources(self):
        de = (ROOT / 'compat/display/displayengine_module.cpp').read_text(encoding='utf-8')
        self.assertIn('subType == 0xa1u', de)
        self.assertIn('WriteDispParamIfChanged', de)
        comp = (ROOT / 'compat/display/composer_vdi.cpp').read_text(encoding='utf-8')
        self.assertNotIn('drmModeAtomicCommit(drm, req, 0, nullptr)', comp)

    def test_hcb_17_hosts_hostapd(self):
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
        )
        hcb16 = phase4.build_phase4_hdf_hcb(**kwargs, hostapd_donor=None)
        hcb17 = phase4.build_phase4_hdf_hcb(**kwargs, hostapd_donor=DONOR_FS)
        self.assertNotIn(b'hostapd_host', hcb16)
        self.assertIn(b'hostapd_host', hcb17)
        self.assertIn(b'hostapd_interface_service', hcb17)
        self.assertIn(b'libhostapd_hdi_c_device.z.so', hcb17)

    def test_m2_patch_splices(self):
        sample_init = (
            b'{\n  "import": [\n'
            b'    "/compat/etc/init-m1-vibrator-marble.cfg",\n'
            b'    "/compat/etc/init-m1-touch-wakeup-marble.cfg"\n  ],\n'
            b'  "jobs": []\n}\n'
        )
        patched_init = m2.patch_init_debug(sample_init)
        doc = json.loads(patched_init.decode('utf-8'))
        self.assertIn('/compat/etc/init-m4-mi-thermal-marble.cfg', doc['import'])
        self.assertIn('/compat/etc/init-m4-softap-marble.cfg', doc['import'])

        sample_stage = (
            b'#!/busybox sh\n# === MARBLE_M1_VIB_DISP_STAGE_BEGIN ===\n'
            b'$B test -x "$R/compat/bin/init"\necho MARBLE_PHASE4_RAM_COMPAT_READY\n'
        )
        patched_stage = m2.patch_stage(sample_stage)
        self.assertIn(b'MARBLE_M2_CAM_THERMAL_SOFTAP_STAGE_BEGIN', patched_stage)
        self.assertIn(b'libmarble_camera_host_service_1.2.z.so', patched_stage)

        sample_postboot = (
            b'#!/busybox sh\nstart-vibrator-m1-marble.sh\n'
            b'say "U2_GNSS_MODE=hdf_location_host"\n'
        )
        patched_postboot = m2.patch_postboot(sample_postboot)
        self.assertIn(b'start-mi-thermal-marble.sh', patched_postboot)
        self.assertIn(b'start-softap-m4-marble.sh', patched_postboot)


if __name__ == '__main__':
    unittest.main()
