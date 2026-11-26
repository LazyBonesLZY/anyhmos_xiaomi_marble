"""Exercise the location diagnostic against host-only filesystem and applet mocks."""

import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'devices/marble/runtime/location/start-location-marble.sh'
SLPI = '2400000.remoteproc-slpi'  # stock remoteproc-firmware.txt:5
MOCK = r'''
mock_busybox() {
 case "$1" in
  cat|chmod|cp|grep|mkdir|tr) command "$@" ;;
  chown|sleep) : ;;
  chroot)
   printf '%s\n' "$*" >> effects
   [ "$MOCK_PARAM" = ok ] ;;
  timeout)
   printf '%s\n' "$*" >> effects
   [ "$MOCK_SA" = ready ] ;;
  *) exit 90 ;;
 esac
}
echo() {
 if [ "$1" = 1 ] && [ "$MOCK_BOOT_WRITE" = fail ]; then
  # The shell already opened a writable mock node; fail the actual write.
  (trap '' XFSZ; ulimit -f 0; command echo "$@")
 else
  command echo "$@"
 fi
}
'''


class LocationStageTest(unittest.TestCase):
    def exercise(self, *, opt_in=False, names=(SLPI,), state='offline',
                 knob='present', param='ok', sa='ready'):
        script = SCRIPT.read_text().partition('\n')[2]
        script = script.replace('B=/busybox', 'B=mock_busybox').replace('R=/hmos-root', 'R=./hmos-root')
        # Preserve /compat inside the mock HMOS root and in stubbed chroot arguments.
        script = re.sub(r'(?<!\$R)/(sys/|proc/|dev/|compat/(?:firmware|original-firmware))', r'./\1', script)
        with tempfile.TemporaryDirectory(prefix='marble-location-') as tmp:
            root = Path(tmp)
            for name in ('dev', 'proc', 'compat/original-firmware/image', 'sys/kernel/boot_slpi'):
                (root / name).mkdir(parents=True)
            (root / 'proc/modules').touch()
            (root / 'compat/original-firmware/image/slpi2.mdt').touch()
            boot = root / 'sys/kernel/boot_slpi/boot'
            if knob != 'absent':
                boot.write_text('untouched\n')

            # remoteproc3 being running is not evidence about SLPI.
            entries = [(3, '3000000.remoteproc-adsp', 'running')]
            entries.extend((7 + i, name, state) for i, name in enumerate(names))
            for index, name, rproc_state in entries:
                node = root / f'sys/class/remoteproc/remoteproc{index}'
                node.mkdir(parents=True)
                (node / 'name').write_text(f'{name}\n')
                (node / 'state').write_text(f'{rproc_state}\n')

            env = {'PATH': os.defpath, 'LC_ALL': 'C', 'MOCK_PARAM': param, 'MOCK_SA': sa,
                   'MOCK_BOOT_WRITE': 'fail' if knob == 'write-fails' else 'ok'}
            if opt_in:
                env['LOC_BOOT_SLPI'] = '1'
            busybox = shutil.which('busybox')
            shell = [busybox, 'sh'] if busybox else ['bash']
            run = subprocess.run([*shell, '-c', MOCK + script], cwd=root, env=env,
                                 capture_output=True, text=True, timeout=10)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            lines = run.stdout.splitlines()
            self.assertEqual(lines[-1], 'LOC_START_EXIT=0 STAGE=complete')
            self.assertIn('LOC_GNSS_FIX=unverified', lines)
            request = 'chroot ./hmos-root /system/bin/param set marble.debug.start.location true'
            probe = 'timeout 4 mock_busybox chroot ./hmos-root /compat/bin/native_sa_check 2802'
            self.assertEqual((root / 'effects').read_text().splitlines(),
                             [request] + [probe] * (1 if sa == 'ready' else 25))
            return lines, boot.read_text() if boot.exists() else None

    def test_default_does_not_write_boot_knob(self):
        lines, boot = self.exercise()
        self.assertIn('LOC_SLPI_SKIPPED=1', lines)
        self.assertNotIn('LOC_SLPI_BOOT=requested', lines)
        self.assertEqual(boot, 'untouched\n')

    def test_opt_in_reports_state_of_exact_name_not_an_index(self):
        lines, boot = self.exercise(opt_in=True)
        self.assertEqual(boot, '1\n')
        self.assertIn('LOC_SLPI_BOOT=requested', lines)
        self.assertIn('LOC_SLPI_STATE=offline', lines)
        self.assertIn('LOC_SA_2802=ready', lines)

    def test_zero_or_multiple_exact_matches_do_not_request_boot(self):
        for names, result in (
            (('2400000.remoteproc-slpi-extra', '3000000.remoteproc-slpi'), 'missing'),
            ((SLPI, SLPI), 'ambiguous'),
        ):
            with self.subTest(result=result):
                lines, boot = self.exercise(opt_in=True, names=names)
                self.assertIn(f'LOC_SLPI_RPROC={result}', lines)
                self.assertFalse(any(line.startswith('LOC_SLPI_STATE=') for line in lines))
                self.assertNotIn('LOC_SLPI_BOOT=requested', lines)
                self.assertEqual(boot, 'untouched\n')

    def test_absent_or_failed_knob_never_reports_request(self):
        for knob, marker, data in (
            ('absent', 'LOC_SLPI_KNOB=absent', None),
            ('write-fails', 'LOC_SLPI_BOOT=failed', ''),
        ):
            with self.subTest(knob=knob):
                lines, boot = self.exercise(opt_in=True, knob=knob)
                self.assertIn(marker, lines)
                self.assertNotIn('LOC_SLPI_BOOT=requested', lines)
                self.assertEqual(boot, data)

    def test_hub_start_request_and_sa_publication_are_independent(self):
        lines, _ = self.exercise(opt_in=True, state='running', sa='missing')
        self.assertIn('LOC_SLPI_STATE=running', lines)
        self.assertIn('LOC_HUB_START=requested', lines)
        self.assertIn('LOC_SA_2802=missing', lines)
        lines, _ = self.exercise(param='failed')
        self.assertIn('LOC_HUB_START=failed', lines)
        self.assertIn('LOC_SA_2802=ready', lines)


if __name__ == '__main__':
    unittest.main()
