"""Check delivery of the offline Web fixture without launching a browser or listener."""
import base64
import importlib.util
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / 'tests/fixtures/marble-webview'
BOOT = '12345678-abcd-1234-abcd-123456789abc'
RUN = 'abcdefghijkl'

spec = importlib.util.spec_from_file_location('web_fixture_prepare', FIXTURE / 'prepare.py')
prepare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare)


class WebFixtureTests(unittest.TestCase):
    def test_generated_scripts_fit_serial_and_keep_original_page_bytes(self):
        scripts = prepare.scripts(RUN, BOOT)
        self.assertEqual(set(scripts), {'push-p.sh', 'push-n.sh', 'serve.sh', 'cleanup.sh'})
        for name, script in scripts.items():
            with self.subTest(script=name):
                self.assertLess(len(script), 2800)
                self.assertLess(len(b'cmd ' + b'a' * 16 + b' ' + base64.b64encode(script) + b'\n'), 4096)
                self.assertIn(BOOT.encode(), script)
                subprocess.run(['sh', '-n'], input=script, capture_output=True, check=True, timeout=5)
        for suffix, name in prepare.PAGES.items():
            payload = scripts[f'push-{suffix}.sh'].split(b"<<'END_MARBLE_HTML'\n", 1)[1]
            self.assertEqual(payload.split(b'END_MARBLE_HTML\n', 1)[0], (FIXTURE / name).read_bytes())

    def test_run_and_boot_validation_reject_wrong_shapes(self):
        for run, boot in (('x/../../y', BOOT), ('a' * 13, BOOT), ('a-b', BOOT), (RUN, 'bad'), (RUN, BOOT.upper())):
            with self.subTest(run=run, boot=boot), self.assertRaises(ValueError):
                prepare.scripts(run, boot)

    def test_transfer_serve_plan_and_cleanup_in_temporary_files(self):
        scripts = prepare.scripts(RUN, BOOT)
        # Run the generated shell logic, but stub /proc and the exact httpd
        # request. No target executable, network socket or system path is used.
        mock = r'''mock_bb() {
 case "$1 ${2-}" in
  'cat /proc/sys/kernel/random/boot_id') printf '%s\n' "$BOOT_FIXTURE" ;;
  'cat /proc/uptime') echo 50.00 ;;
  'timeout -k')
   [ "$*" = "timeout -k 2 40 mock_bb httpd -f -vv -c /dev/null -p 127.0.0.1:18719 -h $D" ] || exit 93
   echo LOOPBACK_PLAN_ONLY ;;
  *)
   case "$1" in
    awk) [ "$MOUNT_FIXTURE" = ram ] || return 1 ;;
    cat|mkdir|sha256sum|rm|rmdir) command "$@" ;;
    *) exit 94 ;;
   esac ;;
 esac
}
'''
        with tempfile.TemporaryDirectory(prefix='web-fixture-') as tmp:
            folder = Path(tmp) / f'marble-web-fixture-{RUN}'

            def run(name, *, boot=BOOT, mount='ram', ok=True):
                shell = scripts[name].decode('ascii').replace('B=/busybox\n', 'B=mock_bb\n', 1)
                shell = shell.replace(f'D=/dev/marble-web-fixture-{RUN}\n', 'D=$3\n', 1)
                # The stub is a function, not an executable to exec. Keep
                # production's bounded exec path intact in the generated file.
                if name == 'serve.sh':
                    shell = shell.replace('exec $B timeout', '$B timeout', 1)
                proc = subprocess.run(['sh', '-c', 'BOOT_FIXTURE=$1; MOUNT_FIXTURE=$2\n' + mock + shell,
                                       'mock', boot, mount, str(folder)], capture_output=True, text=True, timeout=5)
                self.assertEqual(proc.returncode == 0, ok, proc.stdout + proc.stderr)
                return proc

            self.assertEqual(run('push-p.sh', boot='other-boot', ok=False).returncode, 19)
            run('push-p.sh', mount='disk', ok=False)
            self.assertFalse(folder.exists())
            run('push-p.sh')
            run('push-p.sh', ok=False)  # the RAM directory belongs to this run
            run('serve.sh', ok=False)  # negative page required before serving
            run('push-n.sh')
            run('push-n.sh', ok=False)  # preserve already transferred files
            self.assertIn('LOOPBACK_PLAN_ONLY', run('serve.sh').stdout)
            (folder / f'{RUN}n.html').write_bytes(b'tampered')
            self.assertNotIn('LOOPBACK_PLAN_ONLY', run('serve.sh', ok=False).stdout)
            run('cleanup.sh')
            self.assertFalse(folder.exists())

    @unittest.skipUnless(shutil.which('node'), 'Node.js unavailable for the host DOM/Canvas mock')
    def test_html_and_canvas_controls(self):
        subprocess.run(['node', str(FIXTURE / 'verify.mjs')], check=True, capture_output=True, timeout=5)
