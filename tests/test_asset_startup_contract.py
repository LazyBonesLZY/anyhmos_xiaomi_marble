"""Preserve H19/H20's real, explicitly gated Asset role in Phase 4 sources."""
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import unittest

from tools.packaging.build_marble_phase2_images import parse_cpio_newc

ROOT = Path(__file__).resolve().parents[1]
PLATFORM = ROOT / 'devices/marble/runtime/init/init-platform-marble.cfg'
DONOR = ROOT / 'work/donor-rom/ADY-AL10-207.0.0.111-SP10/fs/system/system'
H20 = ROOT / 'work/marble-h19-yuv-native.1re8uo3y/vendor_boot_yuv_native_c687adec.img'
GATE = 'marble.debug.start.asset_control=true'


def asset_entries(document):
    service, = (s for s in document['services'] if s['name'] == 'asset_service')
    job, = (j for j in document['jobs'] if 'start asset_service' in j.get('cmds', []))
    return service, job


class AssetStartupContractTests(unittest.TestCase):
    def test_phase4_sources_preserve_the_explicit_start_and_native_role(self):
        service, job = asset_entries(json.loads(PLATFORM.read_bytes()))
        self.assertEqual(service['path'], ['/system/bin/sa_main', '/system/profile/asset_service.json'])
        self.assertEqual((service['uid'], service['gid'], service['secon']),
                         ('asset_service', ['asset_service'], 'u:r:asset_service:s0'))
        self.assertIs(service['ondemand'], True)
        self.assertEqual((service['disabled'], service['start-mode']), (1, 'condition'))
        self.assertEqual(job, {
            'name': 'param:marble-asset-control', 'condition': GATE,
            'cmds': [
                'mkdir /data/service/el1/public/asset_service 0711 asset_service asset_service',
                'mkdir /data/service/el1/public/asset_clone 0711 asset_service asset_service',
                'start asset_service',
            ],
        })
        boot = ROOT / 'devices/marble/runtime/boot/start-phase4-marble.sh'
        self.assertNotIn('marble.debug.start.asset_control', boot.read_text())

    def test_donor_permissions_and_profile_are_retained(self):
        path = DONOR / 'etc/init/asset_service.cfg'
        if not path.is_file():
            self.skipTest('donor Asset init fixture absent')
        donor = json.loads(path.read_bytes())
        role, = donor['services']
        dirs, = donor['jobs']
        service, job = asset_entries(json.loads(PLATFORM.read_bytes()))
        self.assertEqual(service, {**role, 'disabled': 1, 'once': 1, 'sandbox': 0, 'saspawn': False})
        self.assertEqual(job['cmds'], dirs['cmds'] + ['start asset_service'])
        sa, = json.loads((DONOR / 'profile/asset_service.json').read_bytes())['systemability']
        self.assertEqual(sa['name'], 8100)
        self.assertIs(sa['run-on-create'], False)

    @unittest.skipUnless(shutil.which('lz4'), 'lz4 absent')
    def test_phase4_asset_entries_match_the_pinned_h20_image(self):
        if not H20.is_file():
            self.skipTest('local H20 image absent')
        image = H20.read_bytes()
        self.assertEqual(hashlib.sha256(image).hexdigest(),
                         'c687adec08b1b917fcbdab6fcc96caebc038aaabca43503dc1def0ceb9953079')
        self.assertEqual(struct.unpack_from('<8sII', image), (b'VNDRBOOT', 4, 4096))
        size, = struct.unpack_from('<I', image, 24)
        raw = subprocess.check_output(['lz4', '-d', '-c'], input=image[4096:4096+size], timeout=12)
        payload = parse_cpio_newc(raw)['hmos-compat-marble/graphics/init-platform-marble.cfg'][1]
        self.assertEqual(asset_entries(json.loads(PLATFORM.read_bytes())), asset_entries(json.loads(payload)))

    def test_h19_candidate_changed_no_library(self):
        path = ROOT / 'work/marble-asset-control.022b22cy/report.json'
        if not path.is_file():
            self.skipTest('local H19 report absent')
        report = json.loads(path.read_bytes())
        self.assertTrue(report['libraries_unchanged'])
        self.assertEqual(set(report['archive']['changed_raw_records']), {
            'hmos-compat-marble/MANIFEST.json',
            'hmos-compat-marble/graphics/init-platform-marble.cfg',
        })
