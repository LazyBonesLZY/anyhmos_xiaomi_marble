"""marble WPA staging and offline artifact checks.

Staging runs only against temporary fixture trees with a BusyBox wrapper that
scopes applets and simulates UID/ownership on unprivileged hosts. No device,
network, active supplicant, or SELinux policy is exercised.
"""
from __future__ import annotations

import hashlib
import json
import os
import re
import shlex
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD_SCRIPT = ROOT / 'tools/build/build_wifi_supplicant.sh'
STAGE_SCRIPT = ROOT / 'compat/wifi/marble-wifi-supplicant-stage.sh'
RUNTIME = ROOT / 'compat/wifi/runtime'
CONF = RUNTIME / 'etc/wpa_supplicant.conf'
P2P_CONF = RUNTIME / 'etc/p2p_supplicant.conf'
LIB = RUNTIME / 'lib64/libwpa.z.so'
BIN = RUNTIME / 'bin/wpa_supplicant'
PROVENANCE = RUNTIME / 'PROVENANCE.json'
REPORT = ROOT / 'work/marble-wifi-supplicant-report.md'
SRC_WPA = ROOT / 'work/marble-wifi-src/wpa_supplicant_4.1/wpa_supplicant-2.9'
DONOR_VENDOR = ROOT / 'work/donor-rom/ADY-AL10-207.0.0.111-SP10/fs/vendor/lib64'

ELF_MACHINE_AARCH64 = 0xB7


def _elf_machine(path: Path) -> int:
    with path.open('rb') as fh:
        head = fh.read(20)
    if head[:4] != b'\x7fELF':
        raise AssertionError(f'{path.name}: not an ELF file')
    if head[4] != 2:
        raise AssertionError(f'{path.name}: not ELF64')
    return struct.unpack('<H', head[18:20])[0]


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open('rb') as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b''):
            digest.update(chunk)
    return digest.hexdigest()


def _read_pairs(path: Path) -> list[tuple[str, str]]:
    pairs = []
    for line in path.read_text(encoding='utf-8').splitlines():
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        if '=' not in line:
            raise AssertionError(f'{path.name}: line without "=" -> {line!r}')
        key, value = line.split('=', 1)
        if key != key.strip():
            raise AssertionError(f'{path.name}: padded key {key!r}')
        pairs.append((key.strip(), value.strip()))
    return pairs


class ScriptSyntaxTest(unittest.TestCase):
    def test_build_script_parses(self):
        result = subprocess.run(['bash', '-n', str(BUILD_SCRIPT)],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_stage_script_parses(self):
        shell = shutil.which('sh') or shutil.which('dash') or shutil.which('busybox')
        if not shell:
            self.skipTest('no POSIX shell available to parse the stage script')
        cmd = [shell, '-n', str(STAGE_SCRIPT)]
        if shell.endswith('busybox'):
            cmd = [shell, 'sh', '-n', str(STAGE_SCRIPT)]
        result = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_build_script_reports_stages(self):
        body = BUILD_SCRIPT.read_text(encoding='utf-8')
        for marker in ('say precheck', 'say stage "compile', 'say stage "link',
                       'WIFI_SHA256'):
            self.assertIn(marker, body)


class SupplicantConfigTest(unittest.TestCase):
    def test_only_donor_per_interface_path_no_network_blocks(self):
        expected = [('ctrl_interface', '/data/service/el1/public/wifi/sockets/wpa')]
        for seed in (CONF, P2P_CONF):
            with self.subTest(seed=seed.name):
                self.assertEqual(_read_pairs(seed), expected)


class StageFixtureTest(unittest.TestCase):
    def setUp(self):
        self.busybox = shutil.which('busybox')
        if not self.busybox:
            self.skipTest('host BusyBox required for staging fixture')
        temp = tempfile.TemporaryDirectory(prefix='marble-wpa-stage-')
        self.addCleanup(temp.cleanup)
        self.fixture = Path(temp.name).resolve()
        self.root = self.fixture / 'hmos-root'
        self.public = self.root / 'data/service/el1/public'
        self.wifi = self.public / 'wifi'
        self.conf_dir = self.wifi / 'wpa_supplicant'
        self.sock_dir = self.wifi / 'sockets/wpa'
        self.public.mkdir(parents=True)
        self.vendor = self.root / 'vendor/lib64/libwpa.z.so'
        self.vendor.parent.mkdir(parents=True)
        self.vendor.write_bytes(b'fixture donor libwpa\n')
        self.payload = self.fixture / 'payload'
        (self.payload / 'etc').mkdir(parents=True)
        for seed in (CONF, P2P_CONF):
            shutil.copyfile(seed, self.payload / 'etc' / seed.name)
        self.owner_log = self.fixture / 'owners.json'
        self.owner_log.write_text('{}', encoding='utf-8')
        wrapper = self.fixture / 'busybox'
        wrapper.write_text('#!' + sys.executable + '\n' + '''
import json, os, stat, sys
from pathlib import Path

box, verb, *args = [os.environ['FIXTURE_BUSYBOX'], *sys.argv[1:]]
allowed = {'readlink', 'id', 'cmp', 'sha256sum', 'stat', 'mkdir', 'chown',
           'chmod', 'rmdir', 'mktemp', 'ln', 'rm'}
if verb not in allowed:
    sys.exit('unexpected staging applet: ' + verb)
if verb == 'id':
    print(os.environ.get('FIXTURE_UID', '0'))
    sys.exit(0)
if verb in {'mkdir', 'chown', 'chmod', 'rmdir', 'mktemp', 'ln', 'rm'}:
    # No applet in the fixture may modify vendor or any path outside the Wi-Fi tree.
    paths = args[1:] if verb in {'chown', 'chmod', 'ln'} else args[-1:]
    wifi = Path(os.environ['FIXTURE_WIFI'])
    for path in paths:
        if not Path(path).resolve().is_relative_to(wifi):
            sys.exit('unexpected staging write: ' + path)
owner_log = Path(os.environ['FIXTURE_OWNERS'])
if verb == 'stat':
    owners = json.loads(owner_log.read_text())
    path = args[-1]
    owner = owners.get(path, '0:0')
    print(owner + ':' + format(stat.S_IMODE(Path(path).stat().st_mode), 'o'))
    sys.exit(0)
if verb == 'chown':
    owner, path = args
    if os.environ.get('FIXTURE_FAIL_FILE_OWNER') and '.marble-wpa.' in path:
        sys.exit(17)
    owners = json.loads(owner_log.read_text())
    owners[path] = owner
    owner_log.write_text(json.dumps(owners))
    sys.exit(0)
if verb == 'ln' and os.environ.get('FIXTURE_LINK_RACE') and args[-1].endswith('/wpa_supplicant.conf'):
    path = Path(args[-1])
    if os.environ['FIXTURE_LINK_RACE'] == 'directory':
        path.mkdir()
    else:
        path.write_bytes(b'fixture: saved by a concurrent writer\\n')
os.execv(box, [box, verb, *args])
''', encoding='utf-8')
        wrapper.chmod(0o700)
        self.script = self.fixture / STAGE_SCRIPT.name
        body = STAGE_SCRIPT.read_text(encoding='utf-8')
        self.assertEqual(body.count('\nB=/busybox\n'), 1)
        self.with_pinned_donor = body.replace('\nB=/busybox\n', f'\nB={shlex.quote(str(wrapper))}\n')
        body, count = re.subn(r'(?m)^DONOR_WPA_SHA256=[0-9a-f]{64}$',
                              f'DONOR_WPA_SHA256={_sha256(self.vendor)}', self.with_pinned_donor)
        self.assertEqual(count, 1)
        self.script.write_text(body, encoding='utf-8')

    def run_stage(self, **overrides: str) -> subprocess.CompletedProcess[str]:
        env = {
            'MARBLE_ROOT': str(self.root), 'WIFI_PAYLOAD': str(self.payload),
            'FIXTURE_BUSYBOX': self.busybox, 'FIXTURE_OWNERS': str(self.owner_log),
            'FIXTURE_WIFI': str(self.wifi), **overrides,
        }
        return subprocess.run([self.busybox, 'sh', str(self.script)], env=env,
                              capture_output=True, text=True, timeout=10)

    def expect_exit(self, code: int, **overrides: str) -> subprocess.CompletedProcess[str]:
        result = self.run_stage(**overrides)
        self.assertEqual(result.returncode, code, result.stdout + result.stderr)
        self.assertIn(f'WIFI_STAGE_EXIT={code}', result.stdout)
        return result

    def test_fresh_seed_and_repeat_have_exact_modes_and_no_vendor_writes(self):
        vendor_before = self.vendor.stat(), _sha256(self.vendor)
        first = self.expect_exit(0)
        self.assertIn('config_only=1 wlan_verified=0', first.stdout)
        owners = json.loads(self.owner_log.read_text())
        for path in (self.wifi, self.conf_dir, self.sock_dir.parent, self.sock_dir):
            self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o770)
            self.assertEqual(owners[str(path)], '1010:1011')
        self.assertEqual(sorted(owners.values()), ['1010:1011'] * 6)
        seen = {}
        for seed in (CONF, P2P_CONF):
            path = self.conf_dir / seed.name
            self.assertEqual(path.read_bytes(), seed.read_bytes())
            self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o660)
            seen[path] = path.stat()
        second = self.expect_exit(0)
        self.assertEqual(second.stdout.count('WIFI_STAGE=config keep'), 2)
        for path, before in seen.items():
            self.assertEqual(path.stat(), before)
        self.assertEqual(sorted(self.conf_dir.iterdir()), sorted(seen))
        self.assertEqual((self.vendor.stat(), _sha256(self.vendor)), vendor_before)

    def test_preserves_saved_sta_while_seeding_missing_p2p(self):
        self.expect_exit(0)
        saved = self.conf_dir / CONF.name
        saved.write_bytes(b'# existing Settings-managed bytes\nnetwork={\n}\n')
        saved.chmod(0o600)
        self.assertEqual(saved.read_bytes(), b'# existing Settings-managed bytes\nnetwork={\n}\n')
        before = saved.stat()
        p2p = self.conf_dir / P2P_CONF.name
        p2p.unlink()
        result = self.expect_exit(0)
        self.assertIn(f'WIFI_STAGE=config keep {CONF.name}', result.stdout)
        self.assertIn(f'WIFI_STAGE=config seed {P2P_CONF.name}', result.stdout)
        self.assertEqual(saved.stat(), before)
        self.assertEqual(saved.read_bytes(), b'# existing Settings-managed bytes\nnetwork={\n}\n')
        self.assertEqual(p2p.read_bytes(), P2P_CONF.read_bytes())
        self.assertNotIn('Settings-managed', result.stdout + result.stderr)

    def test_pinned_donor_artifact_seeds_without_replacement(self):
        donor = DONOR_VENDOR / 'libwpa.z.so'
        if not donor.is_file():
            self.skipTest('donor Wi-Fi artifact not extracted')
        shutil.copyfile(donor, self.vendor)
        self.script.write_text(self.with_pinned_donor, encoding='utf-8')
        before = _sha256(self.vendor)
        self.expect_exit(0)
        self.assertEqual(_sha256(self.vendor), before)

    def test_missing_or_wrong_vendor_library_leaves_userdata_alone(self):
        self.vendor.unlink()
        self.expect_exit(23)
        self.assertFalse(self.wifi.exists())
        self.vendor.write_bytes(b'not the matching donor')
        self.expect_exit(23)
        self.assertFalse(self.wifi.exists())
        self.assertEqual(self.vendor.read_bytes(), b'not the matching donor')

    def test_missing_or_unexpected_seed_leaves_userdata_alone(self):
        seed = self.payload / 'etc' / P2P_CONF.name
        seed.unlink()
        self.expect_exit(22)
        seed.write_bytes(P2P_CONF.read_bytes() + b'network={\n}\n')
        self.expect_exit(22)
        self.assertFalse(self.wifi.exists())

    def test_requires_root_and_init_created_public_directory(self):
        self.expect_exit(21, FIXTURE_UID='1000')
        self.public.rmdir()
        self.expect_exit(21)
        self.assertFalse(self.wifi.exists())

    def test_m15_socket_directory_guard_leaves_existing_sta_and_missing_p2p(self):
        # m15-peripheral-baseline-1795419890.serial.txt:8-13 (metadata only).
        for path in (self.wifi, self.conf_dir, self.sock_dir.parent, self.sock_dir):
            path.mkdir()
            path.chmod(0o770)
        self.sock_dir.chmod(0o755)
        self.owner_log.write_text(json.dumps({str(path): '1010:1011' for path in
            (self.wifi, self.conf_dir, self.sock_dir.parent)}), encoding='utf-8')
        saved = self.conf_dir / CONF.name
        saved.write_bytes(b'# synthetic existing STA\n')
        saved.chmod(0o644)
        files_before = {p: p.stat() for p in (saved, self.sock_dir, self.conf_dir)}
        result = self.expect_exit(31)
        self.assertIn(f'unexpected owner/mode for {self.sock_dir}: 0:0:755 '
                      '(expected 1010:1011:770)', result.stderr)
        self.assertEqual({p: p.stat() for p in files_before}, files_before)
        self.assertEqual(saved.read_bytes(), b'# synthetic existing STA\n')
        self.assertEqual(list(self.conf_dir.iterdir()), [saved])

    def test_symlinked_directory_and_config_are_rejected_without_following(self):
        outside = self.fixture / 'not-wifi'
        outside.mkdir()
        self.wifi.symlink_to(outside, target_is_directory=True)
        self.expect_exit(31)
        self.assertEqual(list(outside.iterdir()), [])
        self.wifi.unlink()
        self.expect_exit(0)
        saved = outside / 'saved'
        saved.write_bytes(b'untouched fixture')
        dest = self.conf_dir / CONF.name
        dest.unlink()
        dest.symlink_to(saved)
        self.expect_exit(31)
        self.assertTrue(dest.is_symlink())
        self.assertEqual(saved.read_bytes(), b'untouched fixture')

    def test_failed_file_owner_assignment_never_publishes_or_leaves_temp(self):
        self.expect_exit(32, FIXTURE_FAIL_FILE_OWNER='1')
        self.assertEqual(list(self.conf_dir.iterdir()), [])
        self.expect_exit(0)

    def test_publish_race_preserves_newly_saved_config(self):
        result = self.expect_exit(0, FIXTURE_LINK_RACE='file')
        self.assertIn(f'WIFI_STAGE=config keep {CONF.name}', result.stdout)
        self.assertEqual((self.conf_dir / CONF.name).read_bytes(),
                         b'fixture: saved by a concurrent writer\n')
        self.assertFalse(list(self.conf_dir.glob('.marble-wpa.*')))

    def test_publish_race_with_directory_does_not_write_into_it(self):
        self.expect_exit(31, FIXTURE_LINK_RACE='directory')
        self.assertEqual(list(self.conf_dir.iterdir()), [self.conf_dir / CONF.name])
        self.assertEqual(list((self.conf_dir / CONF.name).iterdir()), [])


class ArtifactTest(unittest.TestCase):
    def test_library_is_aarch64_and_exports_versioned_wpa_main(self):
        if not LIB.is_file():
            self.skipTest('compat/wifi/runtime/lib64/libwpa.z.so not built in this workspace')
        self.assertEqual(_elf_machine(LIB), ELF_MACHINE_AARCH64)
        readelf = shutil.which('readelf') or shutil.which('llvm-readelf')
        if not readelf:
            self.skipTest('no readelf to inspect the version definition')
        dyn = subprocess.run([readelf, '-d', str(LIB)], capture_output=True, text=True)
        self.assertIn('libwpa.z.so', dyn.stdout, 'SONAME must be libwpa.z.so')
        ver = subprocess.run([readelf, '-V', str(LIB)], capture_output=True, text=True)
        self.assertIn('1.0', ver.stdout, 'wpa_host imports wpa_main@1.0')
        syms = subprocess.run([readelf, '--dyn-syms', '-W', str(LIB)],
                              capture_output=True, text=True)
        self.assertRegex(syms.stdout, r'wpa_main@+1\.0|wpa_main$')

    def test_launcher_is_aarch64(self):
        if not BIN.is_file():
            self.skipTest('compat/wifi/runtime/bin/wpa_supplicant not built')
        self.assertEqual(_elf_machine(BIN), ELF_MACHINE_AARCH64)

    def test_library_does_not_need_the_build_host_libnl(self):
        if not LIB.is_file():
            self.skipTest('library not built')
        strings = LIB.read_bytes()
        self.assertIn(b'libnl_share.z.so', strings,
                      'must reuse the image libnl like the donor libwpa.z.so')


class ProvenanceTest(unittest.TestCase):
    def test_provenance_records_commit_and_hashes(self):
        if not PROVENANCE.is_file():
            self.skipTest('compat/wifi/runtime/PROVENANCE.json not generated yet')
        data = json.loads(PROVENANCE.read_text(encoding='utf-8'))
        self.assertRegex(data.get('wpa_supplicant', {}).get('commit', ''),
                         r'^[0-9a-f]{40}$')
        self.assertRegex(data.get('communication_wifi', {}).get('commit', ''),
                         r'^[0-9a-f]{40}$')
        self.assertTrue(data.get('sources', {}), 'no source hashes recorded')
        for name, digest in data.get('artifacts', {}).items():
            self.assertRegex(digest, r'^[0-9a-f]{64}$', f'bad sha256 for {name}')

    def test_artifact_hash_matches_provenance(self):
        if not (PROVENANCE.is_file() and LIB.is_file()):
            self.skipTest('artifact or provenance missing')
        data = json.loads(PROVENANCE.read_text(encoding='utf-8'))
        recorded = data.get('artifacts', {}).get('lib64/libwpa.z.so')
        if not recorded:
            self.skipTest('provenance has no hash for the library')
        self.assertEqual(recorded, _sha256(LIB))


class SourceTreeTest(unittest.TestCase):
    def test_contract_symbols_exist_in_source(self):
        if not SRC_WPA.is_dir():
            self.skipTest('OpenHarmony wpa_supplicant source not cloned')
        main_c = (SRC_WPA / 'wpa_supplicant/main.c').read_text(encoding='utf-8')
        self.assertIn('wpa_main', main_c)
        self.assertIn('visibility ("default")', main_c)
        ctrl = (SRC_WPA / 'wpa_supplicant/ctrl_iface_unix.c').read_text(encoding='utf-8')
        self.assertIn('@abstract:', ctrl)
        client = (SRC_WPA / 'src/common/wpa_ctrl.c').read_text(encoding='utf-8')
        self.assertIn('@abstract:', client)

    def test_report_declares_what_is_not_verified(self):
        if not REPORT.is_file():
            self.skipTest('report not written yet')
        text = REPORT.read_text(encoding='utf-8')
        self.assertIn('未上机验证', text)
        self.assertIn('2501000', text)
        self.assertRegex(text, r'wpa_main@1\.0')
        self.assertIn('WpaMainThread', text)


if __name__ == '__main__':
    unittest.main()