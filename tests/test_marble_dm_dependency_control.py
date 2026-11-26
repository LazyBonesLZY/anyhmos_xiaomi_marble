"""Offline role/lifecycle evidence, not proof of live SA or UI performance."""
import json
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]

class DeviceManagerDependencyTests(unittest.TestCase):
    def test_donor_softbus_and_dm_roles_are_not_interchangeable(self):
        init = ROOT/'work/donor-rom/ADY-AL10-207.0.0.111-SP10/fs/system/system/etc/init'
        if not init.is_dir(): self.skipTest('donor fixture absent')
        dm, = json.loads((init/'device_manager.cfg').read_text())['services']
        sb, = json.loads((init/'softbus_server_musl.cfg').read_text())['services']
        self.assertEqual(dm['uid'],'device_manager')
        self.assertEqual(dm['gid'],['device_manager','access_token'])
        self.assertEqual(dm['apl'],'system_basic')
        self.assertIn('ohos.permission.DISTRIBUTED_DATASYNC',dm['permission'])
        self.assertEqual(sb['uid'],'dsoftbus')
        self.assertNotEqual(dm['uid'],sb['uid'])
        self.assertEqual(sb['secon'],'u:r:softbus_server:s0')

    def test_separate_deployment_deltas_preserve_libraries(self):
        fixtures = [(ROOT/'work/marble-h9-softbus.d8zrrv6k/report.json',False),
                    (ROOT/'work/marble-dm-role.uc_k98s3/report.json',True)]
        for path, role in fixtures:
            if not path.is_file(): self.skipTest('deployment report fixture absent')
            r=json.loads(path.read_text())
            self.assertEqual(set(r['archive']['changed_raw_records']),{
                'hmos-compat-marble/MANIFEST.json',
                'hmos-compat-marble/graphics/init-platform-marble.cfg'})
            self.assertTrue(r['libraries_unchanged'])
            if role:
                self.assertEqual(set(r['dm_original_role_restored']),{
                    'gid','apl','permission','permission_acls'})
                self.assertTrue(r['softbus_unchanged'])
            else:
                self.assertTrue(r['dm_role_unchanged'])
