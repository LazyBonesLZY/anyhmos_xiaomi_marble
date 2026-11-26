"""marble boot_control probe/marker: script contract, gating, read-only proof.

The C probe is exercised for real (host build against a synthetic `misc` image)
so the parser, the CRC check and the desktop-unlock gates are verified instead
of only grepped. The bionic build is compiled when the NDK is present and its
string table is used as evidence that the write path is compiled out of the
shipped read-only binary.
"""

from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'devices/marble/runtime/boot/mark-boot-success-marble.sh'
SOURCE = ROOT / 'compat/bootcontrol/marble_bootcontrol.c'
NDK = Path('/home/lazybones/android-ndk-r27d')
NDK_CLANG = NDK / 'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android35-clang'

BOOT_ID = 'aabbccdd-1122-3344-5566-778899aabbcc'
OTHER_BOOT_ID = 'deadbeef-1122-3344-5566-778899aabbcc'
MARK_GUARD = '#ifdef HMOS_ALLOW_MARK_BOOT_SUCCESS'


def shell():
    return shutil.which('bash') or shutil.which('sh')


def busybox():
    return shutil.which('busybox') or ''


def run_script(args, env_extra, cwd=None):
    env = {
        'PATH': '/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin',
        'MARBLE_BUSYBOX': busybox(),
    }
    env.update(env_extra)
    return subprocess.run(
        [shell(), str(SCRIPT), *args],
        capture_output=True,
        text=True,
        env=env,
        cwd=cwd or str(ROOT),
        timeout=120,
    )


def write_bootconfig(path, sku='marble', slot='_a', hardware='qcom'):
    path.write_text(
        'androidboot.hardware = "%s"\n' % hardware
        + 'androidboot.hardware.sku = "%s"\n' % sku
        + 'androidboot.slot_suffix = "%s"\n' % slot
        + 'androidboot.bootdevice = "1d84000.ufshc"\n',
        encoding='utf-8',
    )
    return path


def make_misc_image(path, tries=1, successful=0, bootable=1, corrupt_crc=False):
    data = bytearray(4096)
    data[0:32] = b'bootonce-bootloader'.ljust(32, b'\0')
    slots = bytearray()
    for index in range(4):
        if index == 0:
            meta = (15, tries, successful, bootable)
        elif index == 1:
            meta = (0, 7, 1, 1)
        else:
            meta = (0, 0, 0, 0)
        pri, tri, suc, boot = meta
        slots += bytes([(pri & 0xF) | ((tri & 0x7) << 4) | ((suc & 1) << 7)])
        slots += bytes([boot & 1])
    body = b'_a\0\0' + struct.pack('<I', 0x42414342) + bytes([2, 2]) + b'\0\0' + slots
    body += b'\0' * 8
    assert len(body) == 28
    crc = zlib.crc32(body) & 0xFFFFFFFF
    data[2048:2076] = body + struct.pack('<I', crc)
    if corrupt_crc:
        data[2075] ^= 0xFF
    path.write_bytes(bytes(data))
    return path


def write_evidence(dirpath, boot_id=BOOT_ID, unlock='SCREENLOCK_VERIFIED after=0 callback=0 elapsed=2',
                   snapshot=133000, done=True, name='marble-phase4-autostart.log'):
    dirpath.mkdir(parents=True, exist_ok=True)
    lines = ['MARBLE_P4_STAGE name=prepare uptime=3 BEGIN boot_id=%s' % boot_id]
    if unlock:
        lines.append('MARBLE_P4_STAGE name=wake_unlock uptime=88 UNLOCK_OUT=%s ' % unlock)
    if snapshot:
        lines.append('MARBLE_P4_STAGE name=capture uptime=99 SNAPSHOT_FILE_READY bytes=%d '
                     'image_content_unverified=1' % snapshot)
    if done:
        lines.append('MARBLE_P4_STAGE name=complete uptime=120 PHASE4_ORCHESTRATOR_DONE procs=42')
    (dirpath / name).write_text('\n'.join(lines) + '\n', encoding='utf-8')
    return dirpath


class ScriptContractTests(unittest.TestCase):
    def test_script_syntax(self):
        self.assertTrue(SCRIPT.is_file(), SCRIPT)
        for sh in filter(None, [shutil.which('bash'), shutil.which('dash'), busybox()]):
            with self.subTest(shell=sh):
                args = [sh, '-n', str(SCRIPT)]
                if 'busybox' in sh:
                    args = [sh, 'sh', '-n', str(SCRIPT)]
                result = subprocess.run(args, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)

    @unittest.skipUnless(shutil.which('shellcheck'), 'shellcheck not installed')
    def test_script_passes_shellcheck(self):
        result = subprocess.run(
            [shutil.which('shellcheck'), '-s', 'sh', str(SCRIPT)],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_script_is_posix_guarded_and_bionic_driven(self):
        text = SCRIPT.read_text(encoding='utf-8')
        self.assertIn('set -eu', text)
        self.assertIn('linker64', text)
        self.assertIn('MARK_SUCCESS_SKIP reason=', text)
        self.assertIn('/proc/sys/kernel/random/boot_id', text)
        self.assertNotIn('flash ', text)
        self.assertNotIn('dd if=', text)
        self.assertNotIn('O_RDWR', text)


class ScriptGateTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='marble-bootctl-test.'))

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _env(self):
        return {
            'MARBLE_BOOTCONTROL_DIR': str(self.tmp / 'missing-dir'),
            'MARBLE_BIONIC_DIR': str(self.tmp / 'missing-runtime'),
            'MARBLE_BOOTCONFIG': str(write_bootconfig(self.tmp / 'bootconfig')),
            'MARBLE_BOOT_ID_FILE': str(self._boot_id_file()),
        }

    def _boot_id_file(self, value=BOOT_ID):
        path = self.tmp / 'boot_id'
        path.write_text(value + '\n', encoding='utf-8')
        return path

    def test_argc(self):
        result = run_script([], self._env())
        self.assertEqual(result.returncode, 31)
        self.assertIn('MARK_SUCCESS_SKIP reason=argc', result.stdout)

    def test_empty_boot_id(self):
        result = run_script([''], self._env())
        self.assertEqual(result.returncode, 32)
        self.assertIn('MARK_SUCCESS_SKIP reason=empty_boot_id', result.stdout)

    def test_boot_id_mismatch(self):
        env = self._env()
        result = run_script([OTHER_BOOT_ID], env)
        self.assertEqual(result.returncode, 33)
        self.assertIn('MARK_SUCCESS_SKIP reason=boot_id_mismatch', result.stdout)

    def test_bootconfig_sku_mismatch(self):
        env = self._env()
        env['MARBLE_BOOTCONFIG'] = str(
            write_bootconfig(self.tmp / 'bootconfig-sheng', sku='sheng'))
        result = run_script([BOOT_ID], env)
        self.assertEqual(result.returncode, 34)
        self.assertIn('MARK_SUCCESS_SKIP reason=bootconfig_sku_mismatch', result.stdout)

    def test_bootconfig_slot_mismatch(self):
        env = self._env()
        env['MARBLE_BOOTCONFIG'] = str(
            write_bootconfig(self.tmp / 'bootconfig-b', slot='_b'))
        result = run_script([BOOT_ID], env)
        self.assertEqual(result.returncode, 34)
        self.assertIn('MARK_SUCCESS_SKIP reason=bootconfig_slot_mismatch', result.stdout)

    def test_bionic_linker_missing(self):
        result = run_script([BOOT_ID], self._env())
        self.assertEqual(result.returncode, 35)
        self.assertIn('MARK_SUCCESS_SKIP reason=bionic_linker_missing', result.stdout)

    def _fake_harness(self, probe_stdout, mark_stdout='MARK_SUCCESS_DONE boot_id=%s',
                      mark_rc=0):
        stage = self.tmp / 'stage'
        (stage / 'runtime' / 'bin').mkdir(parents=True)
        linker = stage / 'runtime' / 'bin' / 'linker64'
        linker.write_text('#!/bin/sh\nexec "$@"\n', encoding='utf-8')
        linker.chmod(0o755)
        binary = stage / 'marble_bootcontrol'
        lines = ['#!/bin/sh', 'if [ "$1" = probe ]; then']
        lines += ['echo "%s"' % line for line in probe_stdout]
        lines += ['exit 0', 'fi']
        lines += ['echo "%s"' % line for line in mark_stdout.splitlines()]
        lines += ['exit %d' % mark_rc]
        binary.write_text('\n'.join(lines) + '\n', encoding='utf-8')
        binary.chmod(0o755)
        env = self._env()
        env['MARBLE_BOOTCONTROL_DIR'] = str(stage)
        env['MARBLE_BIONIC_DIR'] = str(stage / 'runtime')
        return env

    def test_read_only_build_reports_skip_and_exits_zero(self):
        env = self._fake_harness([
            'MISC_PROBE_OK read_only=1',
            'GATES_ALL_PASS=1',
            'MARBLE_BOOTCONTROL_READONLY=1',
            'MARK_PATH_COMPILED=0 reason=read_only_build',
            'MARK_SUCCESS_SKIP reason=mark_path_not_compiled',
        ])
        result = run_script([BOOT_ID], env)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('MISC_PROBE_OK read_only=1', result.stdout)
        self.assertIn('MARK_SUCCESS_SKIP reason=mark_path_not_compiled', result.stdout)
        self.assertIn('MARBLE_BOOTCONTROL_MODE=read_only', result.stdout)

    def test_gates_failed_blocks_mark(self):
        env = self._fake_harness([
            'MISC_PROBE_OK read_only=1',
            'GATES_ALL_PASS=0',
            'MARK_PATH_COMPILED=1',
        ])
        result = run_script([BOOT_ID], env)
        self.assertEqual(result.returncode, 38)
        self.assertIn('MARK_SUCCESS_SKIP reason=gates_failed', result.stdout)
        self.assertNotIn('MARK_SUCCESS_DONE', result.stdout)

    def test_mark_path_runs_when_gates_pass(self):
        env = self._fake_harness(
            ['MISC_PROBE_OK read_only=1', 'GATES_ALL_PASS=1', 'MARK_PATH_COMPILED=1'],
            mark_stdout='MARK_SUCCESS_OK slot=0 boot_id=%s verified=1' % BOOT_ID,
        )
        result = run_script([BOOT_ID], env)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('MARK_SUCCESS_OK', result.stdout)
        self.assertIn('MARK_SUCCESS_DONE boot_id=', result.stdout)

    def test_mark_failure_is_reported(self):
        env = self._fake_harness(
            ['GATES_ALL_PASS=1', 'MARK_PATH_COMPILED=1'],
            mark_stdout='MARK_SUCCESS_SKIP reason=hal_verify_failed slot=0',
            mark_rc=52,
        )
        result = run_script([BOOT_ID], env)
        self.assertEqual(result.returncode, 39)
        self.assertIn('MARK_SUCCESS_SKIP reason=mark_failed', result.stdout)


class SourceGatingTests(unittest.TestCase):
    """The write path must be textually fenced and reachable only after gates."""

    @classmethod
    def setUpClass(cls):
        cls.text = SOURCE.read_text(encoding='utf-8')
        guard = cls.text.index(MARK_GUARD)
        end = cls.text.index('#endif /* HMOS_ALLOW_MARK_BOOT_SUCCESS */', guard)
        cls.read_only_region = cls.text[:guard] + cls.text[end:]
        cls.mark_region = cls.text[guard:end]

    def test_read_only_region_has_no_write_primitives(self):
        for token in ('O_WRONLY', 'O_RDWR', 'O_CREAT', 'O_TRUNC', 'pwrite(', 'fwrite(',
                      'ftruncate(', 'write_misc_partition'):
            with self.subTest(token=token):
                self.assertNotIn(token, self.read_only_region)

    def test_probe_opens_misc_read_only(self):
        self.assertIn('O_RDONLY | O_CLOEXEC', self.text)
        self.assertNotRegex(self.text, r'open\([^)]*O_RDWR')

    def _strip_comments(self, text):
        text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
        return re.sub(r'//[^\n]*', '', text)

    def test_uncompiled_build_never_calls_hal(self):
        code = self._strip_comments(self.read_only_region)
        for token in ('api.mark', 'api->mark', 'mark_boot_successful(',
                      'bootcontrol_init(', 'is_slot_marked_successful('):
            with self.subTest(token=token):
                self.assertNotIn(token, code,
                                 '%s leaked outside the guarded region' % token)
        # dlopen without calling an entry point is the only HAL touch allowed.
        self.assertEqual(code.count('dlopen('), 1)
        self.assertIn('entry_points_called=0', self.read_only_region)

    def test_mark_region_is_double_gated(self):
        region = self.mark_region
        boot_id = region.index('strcmp(requested_boot_id, boot_id)')
        identity = region.index('identity_ok(&id)')
        crc = region.index('reason=misc_crc_invalid')
        evidence = region.index('gather_evidence(boot_id, &ev)')
        bootable = region.index('reason=slot_not_bootable')
        mark_call = region.index('api.mark()')
        self.assertLess(boot_id, identity, 'boot_id gate must come first')
        self.assertLess(identity, crc)
        self.assertLess(crc, evidence)
        self.assertLess(evidence, bootable)
        self.assertLess(bootable, mark_call, 'write must be last')
        self.assertIn('MARK_SUCCESS_SKIP', region)
        self.assertIn('verify_failed', region)

    def test_evidence_gate_matches_orchestrator_log(self):
        self.assertIn('SCREENLOCK_VERIFIED after=0 ', self.text)
        self.assertIn('SNAPSHOT_FILE_READY bytes=', self.text)
        self.assertIn('PHASE4_ORCHESTRATOR_DONE', self.text)
        self.assertIn('BEGIN boot_id=', self.text)
        self.assertIn('autostart', self.text)

    def test_no_fabricated_success(self):
        region = self.mark_region
        # The only success line must carry verified=1 and be preceded by a
        # re-read of is_slot_marked_successful().
        self.assertEqual(region.count('MARK_SUCCESS_OK'), 1)
        ok_line = region[region.index('MARK_SUCCESS_OK'):]
        self.assertIn('verified=1', ok_line.split('\n', 1)[0])
        verify = region.index('reason=hal_verify_failed')
        self.assertLess(verify, region.index('MARK_SUCCESS_OK'))
        self.assertEqual(region.count('api.mark()'), 1)
        self.assertIn('if (api.marked(slot) != 1)', region)


class HostBehaviourTests(unittest.TestCase):
    """Build the probe for the host and feed it a synthetic misc + evidence."""

    @classmethod
    def setUpClass(cls):
        cls.cc = shutil.which('cc') or shutil.which('gcc') or shutil.which('clang')
        if not cls.cc:
            raise unittest.SkipTest('no host C compiler')
        cls.tmpdir = Path(tempfile.mkdtemp(prefix='marble-bootctl-host.'))
        cls.binary = cls.tmpdir / 'probe'
        cls.mark_binary = cls.tmpdir / 'probe_mark'
        for out, extra in ((cls.binary, []),
                           (cls.mark_binary, ['-DHMOS_ALLOW_MARK_BOOT_SUCCESS'])):
            result = subprocess.run(
                [cls.cc, '-O2', '-Wall', '-Wextra', '-Werror', *extra,
                 '-o', str(out), str(SOURCE)],
                capture_output=True, text=True)
            if result.returncode != 0:
                shutil.rmtree(cls.tmpdir, ignore_errors=True)
                raise AssertionError(result.stderr)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmpdir, ignore_errors=True)

    def _fixture(self, **kwargs):
        base = Path(tempfile.mkdtemp(prefix='marble-fixture.', dir=self.tmpdir))
        write_bootconfig(base / 'bootconfig', **kwargs.pop('bootconfig', {}))
        (base / 'boot_id').write_text(BOOT_ID + '\n', encoding='utf-8')
        misc = make_misc_image(base / 'misc.img', **kwargs.pop('misc', {}))
        evdir = write_evidence(base / 'ev', **kwargs.pop('evidence', {}))
        env = {
            'PATH': '/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin',
            'MARBLE_MISC_DEVICE': str(misc),
            'MARBLE_MISC_DEVICE_FALLBACK': str(base / 'none.img'),
            'MARBLE_BOOTCONFIG': str(base / 'bootconfig'),
            'MARBLE_BOOT_ID_FILE': str(base / 'boot_id'),
            'MARBLE_EVIDENCE_DIRS': str(evdir),
            'MARBLE_BOOTCONTROL_DIR': str(base / 'no-hal'),
        }
        return base, env

    def _probe(self, env, binary=None, *args):
        return subprocess.run([str(binary or self.binary), *(args or ('probe',))],
                              capture_output=True, text=True, env=env, timeout=60)

    def test_probe_reports_slots_and_retry_risk(self):
        _, env = self._fixture()
        result = self._probe(env)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('MISC_CONTROL offset=2048 version=2 nb_slot=2 slot_suffix=_a', result.stdout)
        self.assertIn('crc_valid=1', result.stdout)
        self.assertIn('MISC_SLOT index=0 suffix=_a priority=15 tries_remaining=1 '
                      'successful=0 bootable=1', result.stdout)
        self.assertIn('MISC_SLOT index=1 suffix=_b priority=0 tries_remaining=7 '
                      'successful=1 bootable=1', result.stdout)
        self.assertIn('MISC_RETRY_RISK active=0 tries_remaining=1 successful=0', result.stdout)
        self.assertIn('GATES_ALL_PASS=1', result.stdout)
        self.assertIn('MARK_SUCCESS_SKIP reason=mark_path_not_compiled', result.stdout)
        self.assertIn('HAL_PRESENT=0', result.stdout)

    def test_probe_is_read_only(self):
        base, env = self._fixture()
        misc_before = (base / 'misc.img').read_bytes()
        self._probe(env)
        self._probe(env, self.mark_binary, 'mark', BOOT_ID)
        self.assertEqual((base / 'misc.img').read_bytes(), misc_before,
                         'misc image must not change')
        self.assertFalse((base / 'no-hal').exists())

    def test_crc_mismatch_blocks_gates(self):
        _, env = self._fixture(misc={'corrupt_crc': True})
        result = self._probe(env)
        self.assertIn('crc_valid=0', result.stdout)
        self.assertIn('MISC_CRC_MISMATCH', result.stdout)
        self.assertIn('GATES_ALL_PASS=0', result.stdout)

    def test_identity_mismatch_blocks_gates(self):
        _, env = self._fixture(bootconfig={'sku': 'sheng'})
        result = self._probe(env)
        self.assertIn('GATE sku mismatch expected=marble got=sheng', result.stdout)
        self.assertIn('GATE identity=0', result.stdout)
        self.assertIn('GATES_ALL_PASS=0', result.stdout)

    def test_missing_unlock_evidence_blocks_gates(self):
        _, env = self._fixture(evidence={'unlock': None})
        result = self._probe(env)
        self.assertIn('EVIDENCE boot_id=1 unlock=0', result.stdout)
        self.assertIn('GATE evidence=0', result.stdout)
        self.assertIn('GATES_ALL_PASS=0', result.stdout)

    def test_unlock_after_1_is_not_accepted(self):
        _, env = self._fixture(
            evidence={'unlock': 'SCREENLOCK_VERIFIED after=1 already_unlocked=1'})
        result = self._probe(env)
        self.assertIn('EVIDENCE boot_id=1 unlock=0', result.stdout)

    def test_foreign_boot_id_log_is_not_accepted(self):
        _, env = self._fixture(evidence={'boot_id': OTHER_BOOT_ID})
        result = self._probe(env)
        self.assertIn('EVIDENCE boot_id=0', result.stdout)
        self.assertIn('GATE evidence=0', result.stdout)

    def test_small_snapshot_is_not_accepted(self):
        _, env = self._fixture(evidence={'snapshot': 4096})
        result = self._probe(env)
        self.assertIn('EVIDENCE boot_id=1 unlock=1 snapshot=0', result.stdout)

    def test_mark_refuses_without_boot_id_and_with_wrong_boot_id(self):
        base, env = self._fixture()
        no_arg = self._probe(env, self.mark_binary, 'mark')
        self.assertEqual(no_arg.returncode, 40)
        self.assertIn('MARK_SUCCESS_SKIP reason=bad_boot_id_argument', no_arg.stdout)
        wrong = self._probe(env, self.mark_binary, 'mark', OTHER_BOOT_ID)
        self.assertEqual(wrong.returncode, 42)
        self.assertIn('MARK_SUCCESS_SKIP reason=boot_id_mismatch', wrong.stdout)
        self.assertNotIn('MARK_SUCCESS_OK', wrong.stdout)

    def test_mark_never_claims_success_without_hal(self):
        base, env = self._fixture()
        result = self._probe(env, self.mark_binary, 'mark', BOOT_ID)
        self.assertEqual(result.returncode, 50, result.stdout)
        self.assertIn('MARK_SUCCESS_SKIP reason=dlopen_failed', result.stdout)
        self.assertNotIn('MARK_SUCCESS_OK', result.stdout)
        # The gate sequence must have completed before the HAL was touched.
        self.assertIn('EVIDENCE boot_id=1 unlock=1 snapshot=1 done=1', result.stdout)

    def test_mark_refuses_on_bad_crc_before_dlopen(self):
        base, env = self._fixture(misc={'corrupt_crc': True})
        result = self._probe(env, self.mark_binary, 'mark', BOOT_ID)
        self.assertEqual(result.returncode, 46, result.stdout)
        self.assertIn('MARK_SUCCESS_SKIP reason=misc_crc_invalid', result.stdout)
        self.assertNotIn('dlopen_failed', result.stdout)

    def test_mark_refuses_without_evidence_before_dlopen(self):
        base, env = self._fixture(evidence={'done': False})
        result = self._probe(env, self.mark_binary, 'mark', BOOT_ID)
        self.assertEqual(result.returncode, 47, result.stdout)
        self.assertIn('MARK_SUCCESS_SKIP reason=desktop_evidence_incomplete', result.stdout)
        self.assertNotIn('dlopen_failed', result.stdout)


@unittest.skipUnless(NDK_CLANG.is_file(), 'Android NDK clang not installed')
class BionicBuildTests(unittest.TestCase):
    """Compile the real aarch64 binary and prove the write path is absent."""

    @classmethod
    def setUpClass(cls):
        cls.tmpdir = Path(tempfile.mkdtemp(prefix='marble-bootctl-ndk.'))
        cls.read_only = cls.tmpdir / 'marble_bootcontrol'
        cls.marked = cls.tmpdir / 'marble_bootcontrol_mark'
        def build(out, extra):
            return subprocess.run(
                [str(NDK_CLANG), '-O2', '-Wall', '-Wextra', '-Werror', '-fPIE', '-pie',
                 *extra, '-o', str(out), str(SOURCE)],
                capture_output=True, text=True)

        ro = build(cls.read_only, [])
        mk = build(cls.marked, ['-DHMOS_ALLOW_MARK_BOOT_SUCCESS'])
        if ro.returncode or mk.returncode:
            shutil.rmtree(cls.tmpdir, ignore_errors=True)
            raise AssertionError((ro.stderr, mk.stderr))

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmpdir, ignore_errors=True)

    def test_read_only_binary_has_no_write_path(self):
        blob = self.read_only.read_bytes()
        for token in (b'MARK_SUCCESS_OK', b'MARK_HAL_CALL', b'MARK_HAL_INIT',
                      b'MARK_PATH_COMPILED=1'):
            with self.subTest(token=token):
                self.assertNotIn(token, blob)
        self.assertIn(b'MARK_PATH_COMPILED=0', blob)

    def test_mark_binary_contains_gated_path(self):
        blob = self.marked.read_bytes()
        for token in (b'MARK_SUCCESS_OK', b'MARK_HAL_CALL', b'MARK_SUCCESS_SKIP',
                      b'MARK_PATH_COMPILED=1'):
            with self.subTest(token=token):
                self.assertIn(token, blob)

    def test_binaries_are_aarch64_pie(self):
        for path in (self.read_only, self.marked):
            head = path.read_bytes()[:20]
            self.assertEqual(head[0:4], b'\x7fELF')
            self.assertEqual(head[4], 2)      # ELFCLASS64
            self.assertEqual(head[18:20], struct.pack('<H', 183), 'EM_AARCH64')


if __name__ == '__main__':
    unittest.main()
