"""Contracts for the notification (ans) / Live View / privacy / mic-chain workstream.

Everything here is *static*: donor partition files, repo cfg/scripts, and the
artifacts produced by this workstream.  Nothing talks to a device.

Two invariants are deliberately written so the suite stays green both before and
after the lead merges the drafts:

* the SA profile either already carries ans (3203) or the fragment provides it;
* the privacy draft's service names may already be declared in the marble init
  set, but then their identity must be identical to the draft's (no silent
  divergence), and no *unexpected* name may collide.
"""

import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DONOR_FS = ROOT / 'work/donor-rom/ADY-AL10-207.0.0.111-SP10/fs/system/system'
DONOR_INIT = DONOR_FS / 'etc/init'
DONOR_PROFILE = DONOR_FS / 'profile'
SELINUX = DONOR_FS / 'etc/selinux/system_common.cil'

MARBLE_INIT = ROOT / 'devices/marble/runtime/init'
MARBLE_BOOT = ROOT / 'devices/marble/runtime/boot'
SHENG_PROFILE = ROOT / 'devices/sheng/runtime/profiles/foundation-app-core.json'
MARBLE_PROFILE = MARBLE_INIT / 'foundation-marble.json'

FRAGMENT = ROOT / 'compat/notify/ans-foundation-sa.json'
MERGE_TOOL = ROOT / 'compat/notify/merge_foundation_profile.py'
PRIVACY_DRAFT = ROOT / 'compat/privacy/init-privacy-marble.cfg'
REPORT = ROOT / 'work/marble-notify-privacy-report.md'
PACKAGING = ROOT / 'tools/packaging/build_marble_phase4_images.py'
STAGE4 = MARBLE_BOOT / 'stage-phase4-marble.sh'
PHASE4_CFG = MARBLE_INIT / 'hmos-init-phase4.cfg'

DONOR_AVAILABLE = DONOR_INIT.is_dir() and DONOR_PROFILE.is_dir()

# The two services the draft *adds*; anything else it declares must either be new
# or already identical to the draft.
DRAFT_NEW_SERVICES = {'super_privacy_server', 'security_privacy_server'}
# Documented replacement: the port already declares privacy_service, with a
# trimmed permission set (18 -> 7 permissions, 0 ACLs).
DRAFT_REPLACES = {'privacy_service'}


def load(path):
    return json.loads(Path(path).read_text(encoding='utf-8'))


def donor(name):
    return load(DONOR_PROFILE / f'{name}.json')


def marble_cfg_files():
    for path in sorted(MARBLE_INIT.glob('*.cfg')):
        try:
            yield path, load(path)
        except (json.JSONDecodeError, UnicodeDecodeError):
            continue


def marble_services():
    """name -> (cfg path, service dict) for every service declared by marble init."""
    found = {}
    for path, cfg in marble_cfg_files():
        for service in cfg.get('services') or []:
            if isinstance(service, dict) and service.get('name'):
                found.setdefault(service['name'], (path, service))
    return found


def sas(profile):
    return {sa['name']: sa for sa in profile['systemability']}


class NotifyProfileTests(unittest.TestCase):
    """ans (3203) is an SA inside `foundation`, not an init service."""

    def test_fragment_is_valid_and_matches_donor_verbatim(self):
        fragment = load(FRAGMENT)
        self.assertEqual(fragment['process'], 'foundation')
        names = [sa['name'] for sa in fragment['systemability']]
        self.assertEqual(names, [3203, 3204])
        if not DONOR_AVAILABLE:
            self.skipTest('donor tree not present')
        donor_sas = sas(donor('foundation'))
        for sa in fragment['systemability']:
            self.assertEqual(sa, donor_sas[sa['name']], f'SA {sa["name"]} diverges from donor')
        self.assertEqual(fragment['systemability'][0]['libpath'], 'libans.z.so')
        self.assertEqual(fragment['systemability'][0]['depend'], [3299])
        self.assertIn('backup', fragment['systemability'][0]['extension'])

    def test_ans_is_covered_by_profile_or_fragment(self):
        """Either branch is acceptable; silence about both is not."""
        carried = []
        for path in (SHENG_PROFILE, MARBLE_PROFILE):
            if not path.is_file():
                continue
            if 3203 in sas(load(path)):
                carried.append(path.name)
        fragment = sas(load(FRAGMENT))
        self.assertIn(3203, fragment, 'fragment must carry ans')
        # The dependency must be solvable in whatever profile gets deployed:
        for path in (SHENG_PROFILE, MARBLE_PROFILE):
            if path.is_file():
                self.assertIn(3299, sas(load(path)), f'{path.name} lacks ces (3299), ans would defer')
        # At least one shipped profile source must be able to produce ans.
        self.assertTrue(carried or fragment, 'no path can register SA 3203')

    def test_marble_profile_is_donor_exact_where_it_overlaps(self):
        if not MARBLE_PROFILE.is_file():
            self.skipTest('marble profile not created yet by the packaging workstream')
        if not DONOR_AVAILABLE:
            self.skipTest('donor tree not present')
        marble, donor_sas = sas(load(MARBLE_PROFILE)), sas(donor('foundation'))
        self.assertEqual(load(MARBLE_PROFILE)['process'], 'foundation')
        self.assertIn(3203, marble, 'marble profile must carry ans')
        for name, sa in marble.items():
            self.assertIn(name, donor_sas, f'SA {name} is not a donor foundation SA')
            self.assertEqual(sa, donor_sas[name], f'SA {name} diverges from donor')
        # ans keeps its donor dependency; wms/ces stay first-class.
        self.assertEqual(marble[3203]['depend'], [3299])
        for required in (180, 3299, 4606, 4607):
            self.assertIn(required, marble)

    def test_packaging_and_boot_actually_deploy_the_profile(self):
        packaging = PACKAGING.read_text(encoding='utf-8')
        stage = STAGE4.read_text(encoding='utf-8')
        self.assertIn("payload_files['graphics/foundation-app-core.json']", packaging)
        self.assertIn('init/foundation-marble.json', packaging,
                      'packaging must ship the ans-carrying profile, not sheng 10-SA list')
        self.assertIn('cp /graphics/foundation-app-core.json "$R/system/profile/foundation.json"',
                      stage, 'boot must copy the payload onto the deployed SA profile')
        self.assertIn('"start foundation"', PHASE4_CFG.read_text(encoding='utf-8'))

    def test_notification_dirs_are_created_by_marble_init(self):
        text = PHASE4_CFG.read_text(encoding='utf-8')
        self.assertIn('mkdir /data/service/el1/public/notification 0711 foundation system', text)
        self.assertIn(
            'mkdir /data/service/el1/public/database/notification_service 02770 foundation ddms', text)
        cfg = load(PHASE4_CFG)
        job = next(j for j in cfg['jobs'] if j['name'] == 'param:marble-phase4-screen')
        self.assertEqual(job['condition'], 'marble.debug.start.screen=true')
        self.assertIn('start foundation', job['cmds'])
        boot = (MARBLE_BOOT / 'start-phase4-marble.sh').read_text(encoding='utf-8')
        self.assertIn('pset marble.debug.start.screen true', boot)

    def test_live_view_implementation_lives_inside_ans(self):
        """Guard the claim the report makes: Live View is ans-internal."""
        lib = DONOR_FS / 'lib64/libans.z.so'
        if not lib.is_file():
            self.skipTest('donor libans not present')
        data = lib.read_bytes()
        for needle in (b'advanced_notification_live_view_service.cpp',
                       b'advanced_notification_system_live_view_service.cpp',
                       b'CheckLiveViewRights'):
            self.assertIn(needle, data, f'{needle!r} missing from libans.z.so')


class MergeToolTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='notify-merge.'))
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.profile = self.tmp / 'foundation.json'
        shutil.copyfile(SHENG_PROFILE, self.profile)

    def run_tool(self, *extra):
        return subprocess.run([sys.executable, str(MERGE_TOOL), '--profile', str(self.profile),
                               '--fragment', str(FRAGMENT), '--quiet', *extra],
                              capture_output=True, text=True)

    def test_check_reports_needed_then_merge_is_idempotent(self):
        before = sas(load(self.profile))
        self.assertNotIn(3203, before)
        check = self.run_tool('--check')
        self.assertEqual(check.returncode, 2, check.stderr)
        merged = self.run_tool()
        self.assertEqual(merged.returncode, 0, merged.stderr)
        after = load(self.profile)
        names = [sa['name'] for sa in after['systemability']]
        self.assertEqual(names, [sa['name'] for sa in load(SHENG_PROFILE)['systemability']] + [3203, 3204],
                         'existing SA order must be preserved and ans appended')
        self.assertEqual(after['systemability'][-2], load(FRAGMENT)['systemability'][0])
        digest = self.profile.read_bytes()
        self.assertEqual(self.run_tool('--check').returncode, 0)
        self.assertEqual(self.run_tool().returncode, 0)
        self.assertEqual(self.profile.read_bytes(), digest, 'second merge must not rewrite the file')

    def test_conflicting_existing_sa_is_refused(self):
        data = load(self.profile)
        data['systemability'].append(dict(load(FRAGMENT)['systemability'][0], libpath='libbogus.z.so'))
        self.profile.write_text(json.dumps(data, indent=4), encoding='utf-8')
        result = self.run_tool()
        self.assertEqual(result.returncode, 1)
        self.assertIn('different content', result.stderr)

    def test_missing_dependency_is_refused(self):
        data = load(self.profile)
        data['systemability'] = [sa for sa in data['systemability'] if sa['name'] != 3299]
        self.profile.write_text(json.dumps(data, indent=4), encoding='utf-8')
        result = self.run_tool()
        self.assertEqual(result.returncode, 1)
        self.assertIn('3299', result.stderr)


class PrivacyDraftTests(unittest.TestCase):
    def setUp(self):
        self.draft = load(PRIVACY_DRAFT)
        self.services = {s['name']: s for s in self.draft['services']}
        self.jobs = {j['name']: j for j in self.draft['jobs']}

    def test_shape(self):
        self.assertEqual(sorted(self.services), ['privacy_service', 'security_privacy_server',
                                                 'super_privacy_server'])
        for service in self.services.values():
            self.assertEqual(service['path'][0], '/system/bin/sa_main')
            self.assertTrue(service['secon'].startswith('u:r:'))
            onstart = (service.get('jobs') or {}).get('on-start')
            if onstart:
                self.assertIn(onstart, self.jobs, f'{service["name"]} references an undefined job')
        # privacy_service is started by the existing coredeps job, not by itself.
        self.assertNotIn('jobs', self.services['privacy_service'])

    @unittest.skipUnless(DONOR_AVAILABLE, 'donor tree not present')
    def test_identity_and_permissions_match_donor(self):
        pairs = {
            'privacy_service': 'privacy.cfg',
            'super_privacy_server': 'super_privacy_server.cfg',
            'security_privacy_server': 'security_privacy_server.cfg',
        }
        for name, cfg_name in pairs.items():
            reference = next(s for s in load(DONOR_INIT / cfg_name)['services'] if s['name'] == name)
            draft = self.services[name]
            for field in ('path', 'uid', 'gid', 'secon', 'permission', 'permission_acls'):
                if field in reference:
                    self.assertEqual(draft[field], reference[field],
                                     f'{name}.{field} diverges from donor {cfg_name}')
                else:
                    self.assertNotIn(field, draft, f'{name}.{field} invented')
            for field in ('critical', 'importance', 'ondemand', 'apl'):
                if field in reference:
                    self.assertEqual(draft[field], reference[field], f'{name}.{field} diverges')

    def test_privacy_service_keeps_donor_acls_and_superset_permissions(self):
        draft = self.services['privacy_service']
        self.assertEqual(draft['uid'], 'access_token')
        self.assertEqual(draft['secon'], 'u:r:privacy_service:s0')
        self.assertEqual(draft['gid'], ['access_token'])
        self.assertIn('ohos.permission.MICROPHONE_CONTROL', draft['permission_acls'])
        self.assertIn('ohos.permission.CAMERA_CONTROL', draft['permission_acls'])
        self.assertIn('ohos.permission.MICROPHONE', draft['permission'])
        # Nothing the port already grants may be dropped by the replacement.
        existing = marble_services().get('privacy_service')
        if existing:
            _, current = existing
            missing = set(current.get('permission') or []) - set(draft['permission'])
            self.assertFalse(missing, f'draft would drop permissions: {sorted(missing)}')
            self.assertEqual(current['uid'], draft['uid'])
            self.assertEqual(current['secon'], draft['secon'])

    def test_no_unexpected_service_name_collisions(self):
        existing = marble_services()
        for name in self.services:
            if name in existing:
                self.assertIn(name, DRAFT_REPLACES | DRAFT_NEW_SERVICES,
                              f'draft silently redefines an unrelated service: {name}')
                located = existing[name][1]
                # If the lead already merged the draft, identity must be identical.
                for field in ('uid', 'secon', 'path'):
                    self.assertEqual(located[field], self.services[name][field],
                                     f'merged {name}.{field} diverged from draft')

    @unittest.skipUnless(DONOR_AVAILABLE, 'donor tree not present')
    def test_directory_jobs_match_donor_commands(self):
        super_cmds = [c for c in load(DONOR_INIT / 'super_privacy_server.cfg')['jobs'][0]['cmds']
                      if c.startswith('mkdir ')]
        self.assertEqual(self.jobs['marble-super-privacy-dirs']['cmds'], super_cmds)
        security_cmds = [c for c in load(DONOR_INIT / 'security_privacy_server.cfg')['jobs'][0]['cmds']
                         if c.startswith('mkdir ')]
        self.assertEqual(self.jobs['marble-security-privacy-dirs']['cmds'], security_cmds)

    @unittest.skipUnless(DONOR_AVAILABLE, 'donor tree not present')
    def test_selinux_types_exist_in_shipped_policy(self):
        policy = SELINUX.read_text(errors='replace')
        for domain in ('privacy_service', 'super_privacy_server', 'security_privacy_server', 'foundation'):
            self.assertIn(f'(type {domain})', policy, f'{domain} missing from shipped sepolicy')

    @unittest.skipUnless(DONOR_AVAILABLE, 'donor tree not present')
    def test_companion_uids_are_defined_for_init(self):
        passwd = (DONOR_FS / 'etc/passwd').read_text(encoding='utf-8')
        for name, uid in (('access_token', 3020), ('security_privacy_server', 7788),
                          ('super_privacy_server', 7888)):
            self.assertRegex(passwd, rf'(?m)^{name}:x:{uid}:{uid}:')


class RecordingChainTests(unittest.TestCase):
    def test_capture_nodes_are_enumerated_not_hardcoded(self):
        stage = STAGE4.read_text(encoding='utf-8')
        self.assertIn('for snd in /sys/class/sound/pcmC0D*; do', stage)
        self.assertIn('[ -r "$snd/dev" ] || continue', stage)
        self.assertIn('mk_char_sys "$snd" "$R/dev/snd/${snd##*/}" 0 1041 0660', stage)
        # The old, wrong shape: a fixed list that includes a capture node this card lacks.
        self.assertNotIn('for ctl in controlC0 pcmC0D0p pcmC0D0c', stage)

    def test_report_documents_the_measured_gap(self):
        text = REPORT.read_text(encoding='utf-8')
        self.assertIn('MARBLE_AUDIO_DEVS=7', text)
        self.assertIn('capture=20', text.replace(' ', ''))
        self.assertIn('PAL_SERVER_AGM_INIT_RC=22', text)
        self.assertIn('pcmC0D6c', text)


class ReportContractTests(unittest.TestCase):
    REQUIRED = (
        '### 1.3 缺什么',
        '## 2. 隐私服务',
        '## 3. 录音机麦克风链路',
        '## 4. 实况窗最小可验证路径',
        '## 5. 上机验证判据',
        '## 6. 未验证项',
    )

    def test_sections_and_unverified_declaration(self):
        text = REPORT.read_text(encoding='utf-8')
        for heading in self.REQUIRED:
            self.assertIn(heading, text, f'missing section {heading!r}')
        self.assertIn('未上机验证声明', text)
        self.assertIn('未上机', text)

    def test_missing_sa_list_is_concrete(self):
        text = REPORT.read_text(encoding='utf-8')
        for sa in ('3203', '3204', '66568', '6200', '6105', '4005'):
            self.assertIn(sa, text)
        self.assertIn('profile/foundation.json:112-126', text)
        self.assertIn('stage-phase4-marble.sh:377', text)

    def test_reports_the_duplicate_declaration_defect(self):
        text = REPORT.read_text(encoding='utf-8')
        self.assertIn('重复声明', text)
        self.assertIn('init-platform-marble.cfg', text)

    def test_states_cfg_is_not_runtime_proof(self):
        text = REPORT.read_text(encoding='utf-8')
        self.assertIn('cfg/json 写好了 ≠ 通知能用', text)


if __name__ == '__main__':
    unittest.main()