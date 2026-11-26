"""Keep timer and scheduler identities separate before live service integration."""
import json
from pathlib import Path
import unittest
ROOT=Path(__file__).resolve().parents[1]
class ResourcePreflightTests(unittest.TestCase):
    def test_original_profile_is_not_a_single_timer_service(self):
        p=ROOT/'work/donor-rom/ADY-AL10-207.0.0.111-SP10/fs/system/system/profile/resource_schedule_service.json'
        if not p.exists():self.skipTest('donor profile absent')
        ss=json.loads(p.read_text())['systemability']
        byid={x['name']:x for x in ss}
        self.assertEqual(byid[1903]['libpath'],'libbgtaskmgr_service.z.so')
        self.assertEqual(byid[1901]['libpath'],'libresschedsvc.z.so')
        self.assertEqual(byid[1906]['libpath'],'libsocperf_server.z.so')
        self.assertEqual(len([x for x in ss if x['run-on-create']]),14)
        self.assertNotIn(3702,byid)

    def test_minimal_trial_does_not_enable_cpu_freezer_operations(self):
        p=ROOT/'work/marble-resource-trial.h0x6oymo/report.json'
        if not p.is_file():self.skipTest('resource trial fixture absent')
        r=json.loads(p.read_text())
        self.assertEqual(r['selected_sas'],[1901,1903])
        self.assertFalse(r['cpu_freezer_changes'])
        self.assertTrue(r['libraries_unchanged'])
        self.assertTrue(r['original_role_preserved_except_trial_profile_path'])
        self.assertTrue(all(c.startswith('mkdir /data/service/el1/public/') for c in r['mkdirs']))
        self.assertEqual(len(r['changed_existing_records']),3)

    def test_lifecycle_capture_records_one_uid_without_duplication(self):
        p=ROOT/'work/marble-dual-20261123/resource-lifecycle-check.json'
        if not p.is_file():self.skipTest('live lifecycle capture absent')
        r=json.loads(p.read_text())
        self.assertEqual(r['before_pid'],r['after_exit_record_pid'])
        self.assertNotEqual(r['before_pid'],r['reopened_record_pid'])
        self.assertEqual(r['unit_count'],1)
        self.assertEqual(r['resourceNumber'],4)
        self.assertTrue(r['same_resource_parameters'])
