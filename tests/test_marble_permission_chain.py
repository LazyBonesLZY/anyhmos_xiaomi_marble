"""Permission and capability chains that the port depends on.

Three chains kept coming up while bringing features up, and each one fails in a
way that looks unrelated to its cause:

* installing a HAP needs a caller that holds INSTALL_BUNDLE, and the error the
  user sees is only `9568266`;
* the recorder needs the privacy service to hold the microphone control ACLs,
  otherwise the grant path denies silently;
* capture needs the audio policy to actually declare a Built-In Mic port.

This module asserts the configuration side of all three, so that after a flash
any remaining failure is a runtime issue rather than a missing declaration.
"""

import glob
import json
import subprocess
import tarfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUR_CFGS = sorted(glob.glob(str(ROOT / 'devices/marble/runtime/init/*.cfg')))
STOCK_VENDOR = ROOT / 'work/marble-stock-vendor/marble-stock-vendor.tar.gz'
DONOR_POLICY = (ROOT / 'work/donor-rom/ADY-AL10-207.0.0.111-SP10/fs/chip_prod/etc/audio/'
                'audio_policy_configuration.xml')


def _services():
    found = {}
    for cfg_path in OUR_CFGS:
        try:
            cfg = json.loads(Path(cfg_path).read_text())
        except Exception:
            continue
        for svc in cfg.get('services') or []:
            found.setdefault(svc['name'], svc)
    return found


class InstallChainTests(unittest.TestCase):
    def test_installer_holds_the_install_permissions(self):
        svc = _services().get('sys_installer_sa')
        self.assertIsNotNone(svc, 'sys_installer_sa missing')
        perms = set(svc.get('permission') or [])
        self.assertIn('ohos.permission.INSTALL_BUNDLE', perms)
        self.assertIn('ohos.permission.UNINSTALL_BUNDLE', perms)

    def test_installs_service_is_defined_exactly_once(self):
        hits = []
        for cfg_path in OUR_CFGS:
            cfg = json.loads(Path(cfg_path).read_text())
            hits += [Path(cfg_path).name for svc in cfg.get('services') or []
                     if svc['name'] == 'installs']
        self.assertEqual(len(hits), 1, f'"installs" defined in {hits}')


class MicrophoneChainTests(unittest.TestCase):
    def test_privacy_service_holds_microphone_control_acls(self):
        svc = _services().get('privacy_service')
        self.assertIsNotNone(svc, 'privacy_service missing')
        acls = set(svc.get('permission_acls') or [])
        for needed in ('ohos.permission.MICROPHONE_CONTROL', 'ohos.permission.CAMERA_CONTROL',
                       'ohos.permission.SET_SUPER_PRIVACY'):
            self.assertIn(needed, acls, f'privacy_service lacks {needed}')

    def test_access_token_service_is_provisioned(self):
        # SA 3503 (ATMS) publishes from libaccesstoken_manager_service.z.so; the
        # permission dialog cannot appear without it.
        self.assertIn('accesstoken_service', _services())

    def test_audio_policy_declares_a_built_in_mic(self):
        if not DONOR_POLICY.is_file():
            self.skipTest('donor chip_prod audio policy not present')
        text = DONOR_POLICY.read_text(errors='replace')
        self.assertIn('Built-In Mic', text)
        self.assertIn('<attachedDevices>', text)

    def test_marble_sku_policy_declares_capture_ports(self):
        if not STOCK_VENDOR.is_file():
            self.skipTest('stock vendor backup not present')
        with tarfile.open(STOCK_VENDOR) as tar:
            member = 'vendor/etc/audio/sku_ukee/audio_policy_configuration.xml'
            try:
                data = tar.extractfile(member).read().decode('utf-8', 'replace')
            except KeyError:
                self.skipTest('sku_ukee policy not in the backup')
        self.assertIn('Built-In Mic', data)


class CaptureNodeTests(unittest.TestCase):
    def test_stage_enumerates_every_pcm_device(self):
        stage = (ROOT / 'devices/marble/runtime/boot/stage-phase4-marble.sh').read_text()
        self.assertIn('/sys/class/sound/pcmC0D*', stage,
                      'capture nodes must come from enumeration, not a fixed list')
        # The old fixed list only covered playback and left the recorder with no
        # capture device at all; make sure it does not come back.
        self.assertNotIn('for ctl in controlC0 pcmC0D0p', stage)


if __name__ == '__main__':
    unittest.main()
