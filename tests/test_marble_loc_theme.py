"""Contract tests for the marble location (GNSS) and theme bring-up artifacts.

Pins the properties the on-device bring-up depends on:

* both runtime scripts parse under ``bash -n``, stay inside the serial one-shot
  budget (<2800 bytes), run under ``set -eu`` and print ``*_STAGE=`` markers;
* every ``.ko`` they load and every ``/dev|/sys|/proc|/compat|/system|/data``
  node they touch is listed, with recorded provenance, in
  ``work/marble-loctheme-evidence/module-manifest.json`` and really exists in
  the local kernel module trees;
* the init cfgs are valid JSON, only create directories whose owner/group exist
  in the donor ``passwd``/``group``, and never invent a service that the marble
  runtime already declares;
* SA ids (2802 locator, 65846 theme, 3705 wallpaper) are the ones the donor
  profiles actually publish, and the GNSS-HDI-coupled SAs (2803 / 4353) are not
  declared while no Qualcomm OHOS ``location_gnss`` HDI exists.

Facts that cannot be checked without the phone stay "未验证" in the report; the
tests only cover what is reproducible on the host.
"""

from __future__ import annotations

import fnmatch
import json
import re
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LOC_DIR = ROOT / 'devices/marble/runtime/location'
THEME_DIR = ROOT / 'devices/marble/runtime/theme'
LOC_SH = LOC_DIR / 'start-location-marble.sh'
LOC_CFG = LOC_DIR / 'init-location-marble.cfg'
LOC_PROFILE = LOC_DIR / 'locationhub-marble.json'
THEME_SH = THEME_DIR / 'start-theme-marble.sh'
THEME_CFG = THEME_DIR / 'init-theme-marble.cfg'
EVIDENCE = ROOT / 'work/marble-loctheme-evidence'
MANIFEST = EVIDENCE / 'module-manifest.json'
REPORT = ROOT / 'work/marble-loctheme-report.md'
DONOR = ROOT / 'work/donor-rom/ADY-AL10-207.0.0.111-SP10/fs'
KERNEL_TREES = {
    'vendor_boot_modules': ROOT / 'work/marble-kernel-work/vendor_boot_modules',
    'vendor_dlkm_modules': ROOT / 'work/marble-kernel-work/vendor_dlkm_modules',
}
DUMP_EVIDENCE = EVIDENCE / 'marble-gnss'
SCRIPTS = {'start-location-marble.sh': LOC_SH, 'start-theme-marble.sh': THEME_SH}
SERIAL_BUDGET = 2800

KO_RE = re.compile(r'[A-Za-z0-9_.+-]+\.ko\b')
NODE_RE = re.compile(r'(?<![\w.])/(?:dev|sys|proc|compat|system|data|chip_prod)[A-Za-z0-9_@./*+-]*')
IGNORED_NODES = {'/dev/null'}
PATH_IN_PROV = re.compile(r'((?:devices|work|compat|tools|docs|tests)/[\w./+-]+?\.(?:cpp|json|cfg|jpeg|xml|md|py|sh|c|h))(?::\d+(?:-\d+)?)?')


def text_of(path: Path) -> str:
    return path.read_text(encoding='utf-8')


def ko_tokens(text: str) -> set[str]:
    return set(KO_RE.findall(text))


def node_tokens(text: str) -> set[str]:
    tokens = {t.rstrip('.,;:)"\'') for t in NODE_RE.findall(text.replace('$R', ''))}
    return {t for t in tokens if t.count('/') >= 2 and t not in IGNORED_NODES}


def load_json(path: Path) -> dict:
    return json.loads(text_of(path))


def manifest() -> dict:
    if not MANIFEST.is_file():
        raise unittest.SkipTest(f'missing {MANIFEST.relative_to(ROOT)}')
    return load_json(MANIFEST)


def dir_of_job_cmds(cfg: dict):
    for job in cfg.get('jobs', []):
        for cmd in job.get('cmds', []):
            if cmd.startswith('mkdir '):
                yield job['name'], cmd.split()


def donor_group_ids() -> dict[str, int]:
    ids: dict[str, int] = {}
    for name, path in (('passwd', DONOR / 'system/system/etc/passwd'),
                       ('group', DONOR / 'system/system/etc/group')):
        if not path.is_file():
            continue
        for line in text_of(path).splitlines():
            parts = line.split(':')
            if len(parts) >= 3 and parts[2].isdigit():
                ids.setdefault(parts[0], int(parts[2]))
    return ids


class ScriptContractTest(unittest.TestCase):
    def test_scripts_parse_with_bash(self) -> None:
        for name, path in SCRIPTS.items():
            with self.subTest(script=name):
                self.assertTrue(path.is_file(), f'{name} missing')
                proc = subprocess.run(['bash', '-n', str(path)], capture_output=True, text=True)
                self.assertEqual(proc.returncode, 0, f'{name}: {proc.stderr}')

    def test_scripts_fit_single_serial_command(self) -> None:
        for name, path in SCRIPTS.items():
            with self.subTest(script=name):
                size = len(path.read_bytes())
                self.assertLess(size, SERIAL_BUDGET, f'{name} is {size} bytes')

    def test_strict_mode_and_stage_markers(self) -> None:
        for name, path in SCRIPTS.items():
            text = text_of(path)
            marker = 'LOC_STAGE' if 'location' in name else 'THEME_STAGE'
            with self.subTest(script=name):
                self.assertIn('set -eu', text)
                self.assertRegex(text, rf'st\(\)\{{ stage=\$1; echo "{marker}=\$1"; \}}')
                self.assertRegex(text, r'trap .*START_EXIT=\$c STAGE=\$stage')
                stages = re.findall(r'^st (\w+)$', text, re.M)
                self.assertGreaterEqual(len(stages), 5, f'{name} prints too few stages')
                self.assertEqual(stages[-1], 'complete')
                self.assertIn('env', stages)

    def test_no_interactive_or_flash_paths(self) -> None:
        forbidden = ('fastboot', 'adb ', 'COM9', 'set_active', 'flash ',
                     'stage-phase4-marble.sh', 'start-phase4-marble.sh', 'tools/packaging')
        for name, path in SCRIPTS.items():
            text = text_of(path)
            with self.subTest(script=name):
                for token in forbidden:
                    self.assertNotIn(token, text)
                self.assertIsNone(re.search(r'^\s*read\s', text, re.M), 'interactive read')
                self.assertIn('$B chroot "$R" /system/bin/param set', text)

    def test_scripts_are_idempotent_guards(self) -> None:
        loc = text_of(LOC_SH)
        self.assertIn('LOC_MOD_PRESENT', loc)
        self.assertIn('LOC_MOD_MISSING', loc)
        self.assertIn("${LOC_BOOT_SLPI:-0}", loc)
        self.assertIn('LOC_SLPI_SKIPPED=1', loc)
        theme = text_of(THEME_SH)
        self.assertIn('THEME_FILE_MISSING', theme)
        self.assertIn('THEME_SA_$1=ready', theme)


class ConfigContractTest(unittest.TestCase):
    def test_cfgs_and_profile_are_valid_json(self) -> None:
        for path in (LOC_CFG, THEME_CFG, LOC_PROFILE):
            with self.subTest(cfg=path.name):
                load_json(path)

    def test_location_service_mirrors_donor_locationsa_cfg(self) -> None:
        cfg = load_json(LOC_CFG)
        services = {s['name']: s for s in cfg['services']}
        self.assertEqual(set(services), {'locationhub'})
        svc = services['locationhub']
        self.assertEqual(svc['path'], ['/system/bin/sa_main', '/compat/etc/locationhub-marble.json'])
        self.assertEqual(svc['uid'], 'gps')
        self.assertEqual(svc['gid'], ['gps', 'shell', 'netsys_socket'])
        self.assertEqual(svc['secon'], 'u:r:locationhub:s0')
        self.assertTrue(svc['ondemand'])
        self.assertEqual(svc['start-mode'], 'condition')
        self.assertIn('ohos.permission.LOCATION', svc['permission'])
        self.assertIn('ohos.permission.APPROXIMATELY_LOCATION', svc['permission'])
        donor = load_json(DONOR / 'system/system/etc/init/locationsa.cfg')
        donor_svc = donor['services'][0]
        self.assertEqual(donor_svc['uid'], svc['uid'])
        self.assertEqual(donor_svc['secon'], svc['secon'])
        self.assertTrue(set(svc['permission']) <= set(donor_svc['permission']))

    def test_location_only_publishes_hdi_free_sa(self) -> None:
        profile = load_json(LOC_PROFILE)
        ids = {sa['name'] for sa in profile['systemability']}
        self.assertEqual(ids, {2802})
        blob = text_of(LOC_PROFILE) + text_of(LOC_CFG)
        for hdi_coupled in ('2803', '4353', 'liblbsservice_gnss', 'liblocation_gnss_proxy'):
            self.assertNotIn(hdi_coupled, blob)
        donor = load_json(DONOR / 'system/system/profile/locationhub.json')
        donor_sa = {sa['name']: sa for sa in donor['systemability']}
        self.assertEqual(donor_sa[2802]['libpath'], profile['systemability'][0]['libpath'])
        self.assertEqual(donor_sa[2803]['min_hdi_proxy_version'],
                         ['liblocation_agnss_proxy_2.0.z.so', 'liblocation_gnss_proxy_2.0.z.so',
                          'liblocation_geofence_proxy_2.0.z.so'])

    def test_location_jobs_start_service_after_dirs(self) -> None:
        cfg = load_json(LOC_CFG)
        jobs = {job['name']: job for job in cfg['jobs']}
        start = jobs['param:marble-location']
        self.assertEqual(start['condition'], 'marble.debug.start.location=true')
        self.assertIn('start locationhub', start['cmds'])
        svc = {s['name']: s for s in cfg['services']}['locationhub']
        on_start = svc['jobs']['on-start']
        self.assertIn(on_start, jobs, 'service on-start hook must reference a real job')
        dirs = jobs[on_start]['cmds']
        self.assertIn('mkdir /data/service/el1/public/location 0711 gps gps', dirs)
        self.assertIn('mkdir /data/vendor/gnss 0770 gps gps', dirs)
        self.assertIn('marble.debug.start.location', text_of(LOC_SH))

    def test_theme_cfg_creates_no_duplicate_service(self) -> None:
        cfg = load_json(THEME_CFG)
        self.assertNotIn('services', cfg)
        desktop = load_json(ROOT / 'devices/marble/runtime/init/init-desktop-marble.cfg')
        declared = {s['name'] for s in desktop['services']}
        self.assertIn('theme_manager_service', declared)
        self.assertIn('wallpaper_service', declared)
        gates = text_of(ROOT / 'devices/marble/runtime/boot/start-phase4-marble.sh')
        self.assertRegex(gates, r'for gate in [^\n]*\btheme\b[^\n]*\bwallpaper\b')
        self.assertIn('pset "marble.debug.start.$gate" true', gates)
        for cond in {job['condition'] for job in cfg['jobs'] if 'condition' in job}:
            self.assertRegex(cond, r'^marble\.debug\.start\.(theme|wallpaper)=true$')

    def test_mkdir_modes_and_owner_groups_are_real(self) -> None:
        ids = donor_group_ids()
        if not ids:
            raise unittest.SkipTest('donor passwd/group unavailable')
        mode_re = re.compile(r'^0[0-7]{3}$')
        for cfg_path in (LOC_CFG, THEME_CFG):
            cfg = load_json(cfg_path)
            for job_name, parts in dir_of_job_cmds(cfg):
                with self.subTest(cfg=cfg_path.name, job=job_name, cmd=' '.join(parts)):
                    self.assertEqual(len(parts), 5, 'mkdir needs PATH MODE USER GROUP')
                    self.assertTrue(parts[1].startswith('/'))
                    self.assertTrue(mode_re.match(parts[2]), parts[2])
                    self.assertIn(parts[3], ids, f'unknown user {parts[3]}')
                    self.assertIn(parts[4], ids, f'unknown group {parts[4]}')

    def test_donor_sa_ids_are_the_ones_used(self) -> None:
        theme_profile = load_json(DONOR / 'system/system/profile/theme_manager_service.json')
        wall_profile = load_json(DONOR / 'system/system/profile/wallpaper_service.json')
        loc_profile = load_json(DONOR / 'system/system/profile/locationhub.json')
        theme_id = str(theme_profile['systemability'][0]['name'])
        wall_id = str(wall_profile['systemability'][0]['name'])
        loc_id = str(loc_profile['systemability'][1]['name'])
        self.assertEqual((theme_id, wall_id, loc_id), ('65846', '3705', '2802'))
        theme_sh = text_of(THEME_SH)
        for sa in (theme_id, wall_id):
            self.assertIn(f'wait_sa {sa}', theme_sh)
        self.assertIn(f'native_sa_check {loc_id}', text_of(LOC_SH))
        theme_lib = theme_profile['systemability'][0]['libpath']
        self.assertIn(f'/system/lib64/{theme_lib}', theme_sh)


class ManifestContractTest(unittest.TestCase):
    def test_every_ko_is_in_manifest_and_local_tree(self) -> None:
        man = manifest()
        for name, path in SCRIPTS.items():
            listed = {m['name'] for m in man['scripts'][name]['modules']}
            found = ko_tokens(text_of(path))
            with self.subTest(script=name):
                self.assertEqual(found, listed, 'manifest out of date; rerun gen_manifest.py')
                for ko in found:
                    present = [tree for tree in KERNEL_TREES if (KERNEL_TREES[tree] / ko).is_file()]
                    self.assertTrue(present, f'{ko} missing from local kernel trees')

    def test_every_node_has_recorded_provenance(self) -> None:
        man = manifest()
        globs = [entry['glob'] for entry in man['node_provenance']]
        for name, path in SCRIPTS.items():
            for node in sorted(node_tokens(text_of(path))):
                with self.subTest(script=name, node=node):
                    self.assertTrue(any(fnmatch.fnmatch(node, g) for g in globs),
                                    f'{node} has no provenance entry')

    def test_provenance_files_exist(self) -> None:
        man = manifest()
        checked = 0
        for entry in man['node_provenance']:
            for rel in PATH_IN_PROV.findall(entry['provenance']):
                target = ROOT / rel
                if target.exists():
                    checked += 1
                    continue
                self.fail(f'provenance path {rel} (from {entry["glob"]}) does not exist')
        self.assertGreater(checked, 5)

    def test_manifest_module_lists_match_local_trees(self) -> None:
        man = manifest()
        for tree, path in KERNEL_TREES.items():
            if not path.is_dir():
                raise unittest.SkipTest(f'{tree} not assembled')
            with self.subTest(tree=tree):
                self.assertEqual(man[tree], sorted(p.name for p in path.glob('*.ko')))

    def test_gnss_dump_evidence_is_recorded(self) -> None:
        if not DUMP_EVIDENCE.is_dir():
            raise unittest.SkipTest('marble dump evidence not fetched')
        gps_conf = (DUMP_EVIDENCE / 'vendor_etc_gps.conf').read_text(encoding='utf-8', errors='replace')
        self.assertIn('/dev/pps0', gps_conf)
        rc = (DUMP_EVIDENCE / 'vendor_etc_init_android.hardware.gnss-aidl-service-qti.rc').read_text(
            encoding='utf-8', errors='replace')
        self.assertIn('android.hardware.gnss.IGnss/default', rc)
        self.assertIn('user gps', rc)
        launcher = (DUMP_EVIDENCE / 'vendor_etc_init_loc-launcher.rc').read_text(
            encoding='utf-8', errors='replace')
        self.assertIn('/dev/socket/location', launcher)
        index = EVIDENCE / 'marble-dump-all_files.txt'
        if index.is_file():
            text = index.read_text(encoding='utf-8', errors='replace')
            for entry in ('modem/image/slpi2.mdt', 'modem/image/adsp2.mdt',
                          'vendor/bin/loc_launcher', 'vendor/lib64/libgnss.so'):
                self.assertIn(entry, text)
            self.assertNotIn('vendor/lib64/liblocation_gnss_hdi_driver.z.so', text)


class DonorArtifactTest(unittest.TestCase):
    REQUIRED = (
        'system/system/profile/locationhub.json',
        'system/system/profile/theme_manager_service.json',
        'system/system/profile/wallpaper_service.json',
        'system/system/etc/init/locationsa.cfg',
        'system/system/etc/init/themeManagerServer.cfg',
        'system/system/etc/param/location.para',
        'system/system/lib64/liblbsservice_locator.z.so',
        'system/system/lib64/liblocation_gnss_proxy_2.0.z.so',
        'system/system/lib64/libtheme_manager_server.z.so',
        'system/system/lib64/libwallpaper_service.z.so',
        'system/system/lib64/libfactory_proxy_1.1.z.so',
        'system/system/etc/wallpaperdefault.jpeg',
        'system/system/etc/wallpaperlockdefault.jpeg',
        'chip_prod/etc/gps/grs_app_global_route_config.json',
        'vendor/lib64/liblocation_gnss_hdi_driver.z.so',
        'vendor/bin/hignss_1105_ohos',
    )

    def test_donor_artifacts_used_by_the_scripts_exist(self) -> None:
        if not DONOR.is_dir():
            raise unittest.SkipTest('donor fs not unpacked')
        missing = [rel for rel in self.REQUIRED if not (DONOR / rel).exists()]
        self.assertEqual(missing, [], 'donor artifacts referenced by the report are gone')

    def test_location_para_and_dac_are_known(self) -> None:
        if not DONOR.is_dir():
            raise unittest.SkipTest('donor fs not unpacked')
        para = (DONOR / 'system/system/etc/param/location.para').read_text(encoding='utf-8')
        dac = (DONOR / 'system/system/etc/param/location.para.dac').read_text(encoding='utf-8')
        self.assertIn('persist.location.locationhub_state=0', para)
        self.assertIn('persist.location.switch_mode=2', para)
        self.assertIn('gps:gps:775', dac)


class ReportContractTest(unittest.TestCase):
    SECTIONS = ('## 结论', '## 依赖清单', '## 上机顺序', '## 怎么验证', '## 未验证项')

    def test_report_has_required_sections(self) -> None:
        self.assertTrue(REPORT.is_file(), f'missing {REPORT.relative_to(ROOT)}')
        text = REPORT.read_text(encoding='utf-8')
        for section in self.SECTIONS:
            with self.subTest(section=section):
                self.assertIn(section, text)
        self.assertIn('未验证', text)
        self.assertGreater(len(text), 3000, 'report is too thin to carry the evidence')


if __name__ == '__main__':
    unittest.main()