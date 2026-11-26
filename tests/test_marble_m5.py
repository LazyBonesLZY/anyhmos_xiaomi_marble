"""Offline unit & packaging verification for Batch M5 (PIN 96B + Vibrator + DT2W + Raise-to-Wake + SMS/HD Call + Camera + AOD/LiveView)."""
from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/packaging'))

import build_marble_h20_m5_fixes as m5


class TestMarbleM5(unittest.TestCase):
    def test_aod_and_touch_configs(self):
        aod_json = json.loads((ROOT / 'devices/marble/runtime/aod/aod-marble.json').read_text(encoding='utf-8'))
        sas = {sa['name']: sa for sa in aod_json['systemability']}
        self.assertIn(65840, sas)
        self.assertEqual(sas[65840]['libpath'], 'libaod_service.z.so')
        self.assertTrue(sas[65840]['run-on-create'])

        aod_cfg = json.loads((ROOT / 'devices/marble/runtime/aod/init-m5-aod-marble.cfg').read_text(encoding='utf-8'))
        svcs = {s['name']: s for s in aod_cfg['services']}
        self.assertIn('aod_service', svcs)
        self.assertEqual(svcs['aod_service']['uid'], 'aod')
        self.assertEqual(svcs['aod_service']['path'], ['/system/bin/sa_main', '/compat/etc/aod-marble.json'])

        touch_cfg = json.loads((ROOT / 'devices/marble/runtime/touch/init-m1-touch-wakeup-marble.cfg').read_text(encoding='utf-8'))
        tsvc = touch_cfg['services'][0]
        self.assertIn('--power-key', tsvc['path'])
        self.assertIn('--only-when-off', tsvc['path'])
        self.assertTrue({'MKNOD', 'CHOWN', 'FOWNER'}.issubset(set(tsvc['caps'])))

    def test_foundation_source_has_sa3204_and_m5_promotes_run_on_create(self):
        raw = (ROOT / 'devices/marble/runtime/init/foundation-marble.json').read_bytes()
        doc = json.loads(raw.decode('utf-8'))
        sa3204 = next((s for s in doc['systemability'] if s['name'] == 3204), None)
        self.assertIsNotNone(sa3204)
        self.assertEqual(sa3204['libpath'], 'libreminder.z.so')
        patched = json.loads(m5.patch_foundation_reminder(raw).decode('utf-8'))
        sa3204_patched = next(s for s in patched['systemability'] if s['name'] == 3204)
        self.assertTrue(sa3204_patched['run-on-create'])

    def test_m5_guard_members_preserve_no_tee_and_hcb(self):
        self.assertIn('hmos-compat-marble/graphics/libhmos_ram_no_tee.so', m5.GUARD_MEMBERS)
        self.assertIn('hmos-compat-marble/graphics/hdf_default_phase4.hcb', m5.GUARD_MEMBERS)

    def test_m5_patch_splices(self):
        sample_init = (
            b'{\n  "import": [\n'
            b'    "/compat/etc/init-m4-anco-marble.cfg",\n'
            b'    "/compat/etc/init-m4-motion-marble.cfg"\n  ],\n'
            b'  "jobs": []\n}\n'
        )
        patched_init = json.loads(m5.patch_init_debug(sample_init).decode('utf-8'))
        self.assertIn('/compat/etc/init-m5-aod-marble.cfg', patched_init['import'])

        sample_foundation = json.dumps({
            'process': 'foundation',
            'systemability': [{'name': 3203, 'libpath': 'libans.z.so', 'run-on-create': True}],
        }).encode('utf-8')
        patched_fnd = json.loads(m5.patch_foundation_reminder(sample_foundation).decode('utf-8'))
        sa3204 = next(s for s in patched_fnd['systemability'] if s['name'] == 3204)
        self.assertEqual(sa3204['libpath'], 'libreminder.z.so')
        self.assertTrue(sa3204['run-on-create'])

        sample_stage = (
            b'#!/busybox sh\n# === MARBLE_M4_MOTION_BRIGHT_STAGE_BEGIN ===\n'
            b'$B test -x "$R/compat/bin/init"\necho MARBLE_PHASE4_RAM_COMPAT_READY\n'
        )
        patched_stage = m5.patch_stage(sample_stage)
        self.assertIn(b'MARBLE_M5_AOD_STAGE_BEGIN', patched_stage)
        self.assertIn(b'init-m5-aod-marble.cfg', patched_stage)
        self.assertIn(b'aod-marble.json', patched_stage)

        sample_start = (
            b'#!/busybox sh\n'
            b'if $B test -f /graphics/start-sensors-marble.sh; then\n'
            b' echo ok\n'
            b'fi\n'
            + m5.START_INPUT_OLD + b'\n'
            b'pset marble.debug.start.ime true\n'
        )
        patched_start = m5.patch_start(sample_start)
        self.assertIn(b'MARBLE_M5_VIBRATOR_EARLY_BEGIN', patched_start)
        self.assertLess(
            patched_start.index(b'start-vibrator-m1-marble.sh'),
            patched_start.index(b'start-sensors-marble.sh'),
        )
        self.assertIn(b'MARBLE_M5_TOUCH_WAKEUP_EARLY_BEGIN', patched_start)
        self.assertIn(b'*haptics*|*vibrator*) $B chown 3034:3034', patched_start)
        self.assertIn(b'pset marble.debug.start.aod true', patched_start)
        self.assertIn(b'wait_sa 65840 10', patched_start)


if __name__ == '__main__':
    unittest.main()
