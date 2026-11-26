"""Keep the serial probe bounded and distinguish readiness from acceptance."""

import hashlib
import json
import os
import shlex
import socket
import subprocess
import tempfile
import unittest
from contextlib import ExitStack
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'compat/verify/marble_verify_all.sh'
STAGE = ROOT / 'devices/marble/runtime/boot/stage-phase4-marble.sh'
PACKAGER = ROOT / 'tools/packaging/build_marble_phase4_images.py'


class MarbleVerifyAllTests(unittest.TestCase):
    def test_script_fits_the_serial_probe_budget(self):
        size = SCRIPT.stat().st_size
        self.assertLess(size, 2800, f'{size} bytes exceeds the probe script limit')

    def test_script_parses_under_sh_and_ash(self):
        subprocess.run(['sh', '-n', str(SCRIPT)], check=True)
        busybox = subprocess.run(['sh', '-c', 'command -v busybox'], capture_output=True, text=True)
        if busybox.returncode == 0 and busybox.stdout.strip():
            subprocess.run(['busybox', 'ash', '-n', str(SCRIPT)], check=True)

    def test_checks_cover_every_provisioned_system_ability(self):
        text = SCRIPT.read_text()
        for sa in ('3203', '3505', '66090', '5000', '8640', '8650', '4101', '4700', '1901', '801'):
            self.assertIn(sa, text, f'SA {sa} missing from the verification sweep')

    def test_reports_the_fields_that_previously_regressed(self):
        text = SCRIPT.read_text()
        for token in ('SINK_PID', 'HISYSEVENT_ERRNO2', 'BRIGHT saved=', 'SND dir=', 'total=' ,
                      'GPR ', 'PAL ', 'HDC daemon=', 'WIFI wpa_host='):
            self.assertIn(token, text, f'{token} not reported')

    def run_fixture(self, *, required_absent=False, lazy_probe_error=False,
                    sockets=2, log_fails=False):
        # Real UNIX socket pathname checks; only device-dependent commands are
        # replaced. Do not access any device, block node or real /data.
        with tempfile.TemporaryDirectory(prefix='marble-verify-') as tmp, ExitStack() as stack:
            root = Path(tmp)
            wrapper = root / 'box'
            wrapper.write_text('''#!/bin/sh
case "$1" in
  timeout)
    while [ "$1" != chroot ]; do [ "$#" -gt 0 ] || exit 99; shift; done
    shift 2
    case "$1" in
      /compat/bin/native_sa_check)
        id=$2
        case "$id" in
          1301) [ "$REQUIRED_ABSENT" != 1 ] || { echo "SA $id unavailable" >&2; exit 7; } ;;
          5000|8640|8650|4101)
            if [ "$LAZY_PROBE_ERROR" = 1 ]; then echo 'loader error' >&2; exit 4; fi
            echo "SA $id unavailable" >&2; exit 7 ;;
        esac
        echo "NATIVE_SA_READY id=$id" ;;
      /system/bin/hilog)
        [ "$LOG_FAILS" != 1 ] || exit 124
        echo 'HISYSEVENT_TRANSPORT: /dev/unix/socket/hisysevent write failed, errno=2' ;;
      /system/bin/param) echo 0 ;;
      *) exit 99 ;;
    esac ;;
  sha256sum) echo 'abcdef0123456789  fixture' ;;
  pidof) [ "$2" = hmos_hisysevent_sink ] && echo 1 ;;
  cat|cut|head|awk|ls|wc) exec "$@" ;;
  *) exit 99 ;;
esac
''', encoding='utf-8')
            wrapper.chmod(0o700)
            config = root / 'root'
            sockdir = config / 'dev/unix/socket'
            sockdir.mkdir(parents=True)
            for name in ('hisysevent', 'hisysevent_fast')[:sockets]:
                endpoint = stack.enter_context(socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM))
                endpoint.bind(str(sockdir / name))
            script = SCRIPT.read_text().replace('B=/busybox; R=/hmos-root;',
                f'B={shlex.quote(str(wrapper))}; R={shlex.quote(str(config))};', 1)
            return subprocess.run(['sh', '-c', script], text=True, capture_output=True,
                                  timeout=8, env={**os.environ,
                                  'REQUIRED_ABSENT': str(int(required_absent)),
                                  'LAZY_PROBE_ERROR': str(int(lazy_probe_error)),
                                  'LOG_FAILS': str(int(log_fails))})

    def test_readiness_pass_is_not_functional_acceptance(self):
        result = self.run_fixture()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('SA   4101=not-published load_untested=1', result.stdout)
        self.assertIn('HISYSEVENT_ERRNO2=1', result.stdout)
        self.assertIn('VERDICT pass scope=readiness end_to_end=untested', result.stdout)

    def test_missing_core_service_or_socket_fails_the_census(self):
        for kwargs, evidence in (({'required_absent': True}, 'SA   1301=not-published'),
                                  ({'sockets': 1}, 'SOCK hisysevent=1')):
            with self.subTest(**kwargs):
                result = self.run_fixture(**kwargs)
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn(evidence, result.stdout)
                self.assertIn('VERDICT fail scope=readiness', result.stdout)

    def test_failed_capture_and_probe_do_not_look_like_absence_or_zero(self):
        result = self.run_fixture(lazy_probe_error=True, log_fails=True)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn('SA   4101=probe-error load_untested=1', result.stdout)
        self.assertIn('HISYSEVENT_ERRNO2=unavailable', result.stdout)
        self.assertNotIn('HISYSEVENT_ERRNO2=0', result.stdout)

    def test_ships_in_the_image_and_is_copied_at_boot(self):
        self.assertIn('compat/verify/marble_verify_all.sh', PACKAGER.read_text())
        self.assertIn('marble_verify_all.sh', STAGE.read_text())


class MarbleIdentityScriptTests(unittest.TestCase):
    """The identity script is what tells us whether the device runs our change."""

    IDENTITY = ROOT / 'compat/verify/marble_identity.sh'
    STAGE = ROOT / 'devices/marble/runtime/boot/stage-phase4-marble.sh'

    def test_fits_the_serial_budget_and_parses(self):
        self.assertLess(self.IDENTITY.stat().st_size, 2800)
        subprocess.run(['sh', '-n', str(self.IDENTITY)], check=True)

    def test_reports_every_world_marker(self):
        text = self.IDENTITY.read_text()
        for token in ('vendor_boot=', 'boot_a=', 'bin/$f=',
                      'hmos_bt_bridge_marble', 'hmos_hisysevent_sink',
                      'cfg/init-platform=', 'cfg/foundation=', 'audio/ukee-policy=',
                      'cfg/init-seed=', 'nodes gpr=', 'ID done'):
            self.assertIn(token, text, f'{token} missing from identity dump')
        self.assertIn('h /graphics/stage-phase4-marble.sh', text)
        self.assertIn('h /graphics/start-phase4-marble.sh', text)
        self.assertIn('h /proc/1/root/system/etc/init.cfg', text)

    def test_ships_in_the_image_and_is_copied_at_boot(self):
        self.assertIn('compat/verify/marble_identity.sh', PACKAGER.read_text())
        self.assertIn('marble_identity.sh', self.STAGE.read_text())


class ExpectedValuesDocTests(unittest.TestCase):
    """The post-flash expectation table must track the current artifacts; a stale
    table is worse than none because it turns a real regression into "expected"."""

    DOC = ROOT / 'docs/MARBLE_EXPECTED_AFTER_FLASH.md'
    MATRIX = ROOT / 'docs/MARBLE_NEGATIVE_CONTROLS.md'

    def test_expected_doc_matches_the_current_bridge_and_configs(self):
        import hashlib
        text = self.DOC.read_text()
        pairs = {
            'bin/hmos_bt_bridge_marble': ROOT / 'work/marble-bluetooth-build/hmos_bt_bridge_marble',
            'cfg/init-platform': ROOT / 'devices/marble/runtime/init/init-platform-marble.cfg',
            'cfg/foundation': ROOT / 'devices/marble/runtime/init/foundation-marble.json',
        }
        for label, path in pairs.items():
            if not path.is_file():
                continue
            want = hashlib.sha256(path.read_bytes()).hexdigest()[:16]
            row = [l for l in text.splitlines() if label in l]
            self.assertTrue(row, f'{label} row missing from the expected table')
            self.assertIn(want, row[0], f'{label} expectation is stale (want {want})')


    def test_expected_doc_matches_named_candidate_reports(self):
        text = self.DOC.read_text()
        candidates = ('marble-phase4-samgr-app.eKC4oH', 'marble-phase4-wlan-stage.cykCHh')
        checked = 0
        for name in candidates:
            directory = ROOT / 'work' / name / 'images'
            report = directory / 'phase4-build-report.json'
            if not report.is_file():
                continue
            checked += 1
            item = json.loads(report.read_bytes())['vendor_boot_phase4']
            image = directory / 'vendor_boot_phase4.img'
            with self.subTest(candidate=name), image.open('rb') as stream:
                self.assertEqual(image.stat().st_size, item['bytes'])
                self.assertEqual(hashlib.file_digest(stream, 'sha256').hexdigest(), item['sha256'])
                self.assertIn(f'../work/{name}/images/vendor_boot_phase4.img', text)
                self.assertIn(item['sha256'], text)
        if not checked:
            self.skipTest('documented local candidate reports not present')

    def test_negative_controls_cover_the_known_regressions(self):
        text = self.MATRIX.read_text()
        for token in ('Error:22', 'rc=127', 'now=3', '9568266', '1f4d9f9b',
                      'if/fi', 'permission'):
            self.assertIn(token, text, f'{token} not recorded as a negative control')


if __name__ == '__main__':
    unittest.main()
