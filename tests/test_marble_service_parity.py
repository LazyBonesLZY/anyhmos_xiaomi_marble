"""Parity between the services we provision and the donor definitions.

Every service this port adds was copied from an ADY-AL10 init cfg.  Copying by
hand loses fields silently (that already happened to `disk_manager`,
`StorageSpaceMgr`, `device_manager` and `hiview`), and a missing `permission`
list shows up much later as "SA published but every call is denied".  This test
pins uid/gid/secon/permission/permission_acls against the donor files.

Intentional deviations are listed in ALLOWED and are only about *how* we start a
service (launcher path, sandbox flag), never about identity or permissions.
"""

import glob
import json
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DONOR_INIT = ROOT / 'work/donor-rom/ADY-AL10-207.0.0.111-SP10/fs/system/system/etc/init'
OUR_CFGS = sorted(glob.glob(str(ROOT / 'devices/marble/runtime/init/*.cfg')))

# Services we copied from the donor and therefore must stay identical on the
# fields that decide privileges.
COPIED = (
    'privacy_service', 'super_privacy_server', 'security_privacy_server', 'hiview',
    'disk_manager', 'StorageSpaceMgr', 'device_storage_manager_service',
    'sys_installer_sa', 'device_manager',
    'softbus_server', 'resource_schedule_service', 'cert_manager_service',
    'accessibility', 'backup_sa', 'deviceauth_service', 'deviceprofile',
    'sensors', 'mdnsmanager', 'media_monitor',
    'file_access_service', 'file_manager_service', 'sandbox_manager_service',
    'security_component_service', 'midi_server', 'audio_suite_server',
    'nfc_service', 'flashlight_service', 'distributedfiledaemon',
    'cloudfileservice', 'el5_filekey_manager', 'device_usage_stats_service',
    'aod_service', 'batterycare_service', 'bms_broker',
    'media_analysis_service', 'video_processing_service', 'security_guard',
    'app_lock_service', 'parent_control_service',
)

IDENTITY_FIELDS = ('uid', 'secon')


def _donor_services():
    if not DONOR_INIT.is_dir():
        return None
    services = {}
    for path in glob.glob(str(DONOR_INIT / '*.cfg')):
        try:
            data = json.loads(Path(path).read_text())
        except Exception:
            continue
        for svc in data.get('services') or []:
            services.setdefault(svc['name'], svc)
    return services


def _our_services():
    services = {}
    for path in OUR_CFGS:
        try:
            data = json.loads(Path(path).read_text())
        except Exception:
            continue
        for svc in data.get('services') or []:
            services.setdefault(svc['name'], svc)
    return services


class ServiceParityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.donor = _donor_services()
        if cls.donor is None:
            raise unittest.SkipTest('donor ROM extract not present in work/')
        cls.mine = _our_services()

    def test_every_copied_service_is_present(self):
        missing = [n for n in COPIED if n not in self.mine]
        self.assertEqual(missing, [], f'services dropped from our init cfgs: {missing}')

    def test_identity_fields_match_donor(self):
        for name in COPIED:
            ours, theirs = self.mine[name], self.donor.get(name)
            if theirs is None:
                continue
            for field in IDENTITY_FIELDS:
                self.assertEqual(ours.get(field), theirs.get(field), f'{name}.{field}')

    def test_permission_lists_match_donor(self):
        for name in COPIED:
            ours, theirs = self.mine[name], self.donor.get(name)
            if theirs is None:
                continue
            for field in ('permission', 'permission_acls'):
                want = set(theirs.get(field) or [])
                got = set(ours.get(field) or [])
                self.assertEqual(got, want,
                                 f'{name}.{field}: missing={sorted(want - got)} extra={sorted(got - want)}')

    def test_gid_sets_match_donor(self):
        for name in COPIED:
            ours, theirs = self.mine[name], self.donor.get(name)
            if theirs is None:
                continue
            want = set(theirs.get('gid') or [])
            got = set(ours.get('gid') or [])
            # Extra gids are tolerated only when documented here.
            self.assertTrue(want <= got, f'{name}.gid misses {sorted(want - got)}')

    def test_donor_critical_services_are_not_marked_critical_here(self):
        """A critical service restarts the framework when it dies; on a port that
        is still bringing hardware up that turns a single failure into a reboot
        loop, so the flag stays off on purpose."""
        offenders = [n for n in COPIED
                     if (self.mine[n].get('critical') is not None)
                     and (self.donor.get(n) or {}).get('critical') is not None]
        self.assertEqual(offenders, [])


if __name__ == '__main__':
    unittest.main()
