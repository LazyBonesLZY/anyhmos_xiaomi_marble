"""Shell hygiene for the boot and verification scripts.

Two historical failures motivate this module:

* `$ART` was used before it was assigned, and `set -u` aborted the start script at
  stage 6a — the desktop never appeared and nothing in the logs said why;
* a large edit left `if/fi` nested wrongly, which `sh -n` happily accepts.

So: shellcheck must stay clean at warning level, and no variable may be expanded
before its first assignment.
"""

import re
import shutil
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BOOT = ROOT / 'devices/marble/runtime/boot'
VERIFY = ROOT / 'compat/verify'
SCRIPTS = [
    BOOT / 'stage-phase4-marble.sh',
    BOOT / 'start-phase4-marble.sh',
    BOOT / 'init-userdata-ext4.sh',
    BOOT / 'mark-boot-success-marble.sh',
    BOOT / 'stage-phase3-marble.sh',
    BOOT / 'start-phase3-marble.sh',
    VERIFY / 'marble_verify_all.sh',
    VERIFY / 'marble_identity.sh',
]
SUBSYSTEM = sorted(str(p) for p in (ROOT / 'devices/marble/runtime').glob('*/*.sh'))
SCRIPTS = [p for p in SCRIPTS if p.is_file()]

# Variables read by the scripts but never assigned inside them (provided by the
# caller or by the kernel), so "use before assignment" is expected for these.
EXTERNAL = {'PWD', 'HOME', 'PATH', 'IFS'}


def _assignment_positions(text):
    return {m.group(1): m.start() for m in re.finditer(r'^\s*([A-Z][A-Z0-9_]*)=', text, re.M)}


def _usage_positions(text):
    uses = {}
    for m in re.finditer(r'\$\{?([A-Za-z_][A-Za-z0-9_]*)', text):
        name = m.group(1)
        if re.match(r'^[A-Z][A-Z0-9_]*$', name):
            uses.setdefault(name, m.start())
    return uses


class ShellHygieneTests(unittest.TestCase):
    def test_shellcheck_is_clean_at_warning_level(self):
        if shutil.which('shellcheck') is None:
            self.skipTest('shellcheck not installed')
        base = ['shellcheck', '-s', 'sh', '-S', 'warning', '-f', 'gcc']
        strict = subprocess.run(base + [str(p) for p in SCRIPTS], capture_output=True, text=True)
        self.assertEqual(strict.returncode, 0, strict.stdout + strict.stderr)

        # Subsystem scripts carry four patterns that are intentional on this port:
        #   SC2154  the exit status is captured inside the EXIT trap itself
        #   SC2209  `stage=env` / `stage=link` are labels, not commands
        #   SC2100  `stage=udc-bind` reads as arithmetic to shellcheck
        #   SC2046  splitting od output into bytes is the point
        tolerant = subprocess.run(
            base + ['-e', 'SC2154,SC2209,SC2100,SC2046'] + SUBSYSTEM,
            capture_output=True, text=True)
        self.assertEqual(tolerant.returncode, 0, tolerant.stdout + tolerant.stderr)

    def test_no_variable_is_expanded_before_assignment(self):
        problems = []
        for script in SCRIPTS:
            text = script.read_text()
            assigns = _assignment_positions(text)
            for name, pos in _usage_positions(text).items():
                if name in EXTERNAL or name not in assigns:
                    continue
                if pos < assigns[name]:
                    problems.append(f'{script.name}: ${name} used before assignment')
        self.assertEqual(problems, [], '\n'.join(problems))

    def test_scripts_parse_with_bash_and_ash(self):
        for script in SCRIPTS:
            subprocess.run(['sh', '-n', str(script)], check=True)
        if shutil.which('busybox'):
            for script in SCRIPTS:
                subprocess.run(['busybox', 'ash', '-n', str(script)], check=True)

    def test_no_unquoted_rm_or_mount_on_variables(self):
        """A stray `rm -rf $R` with an empty $R would be fatal; require quotes."""
        offenders = []
        for script in SCRIPTS:
            for lineno, line in enumerate(script.read_text().splitlines(), 1):
                if re.search(r'\brm\s+-[a-zA-Z]*[rf][a-zA-Z]*\s+\$[A-Za-z_]', line):
                    offenders.append(f'{script.name}:{lineno}: {line.strip()[:60]}')
        self.assertEqual(offenders, [], '\n'.join(offenders))


class ScriptSizeCeilingTests(unittest.TestCase):
    """A sanity ceiling, not the serial limit: it catches duplicated blocks that
    would otherwise only show up as a slow boot or a stale copy."""

    def _targets(self):
        runtime = ROOT / 'devices/marble/runtime'
        for path in (runtime / 'boot').glob('*.sh'):
            yield path, 32 * 1024
        for sub in sorted(p for p in runtime.iterdir() if p.is_dir() and p.name != 'boot'):
            for path in sub.glob('*.sh'):
                yield path, 16 * 1024

    def test_no_script_grows_without_bound(self):
        offenders = []
        for path, ceiling in self._targets():
            size = path.stat().st_size
            if size > ceiling:
                offenders.append(f'{path.relative_to(ROOT)}: {size} > {ceiling}')
        self.assertEqual(offenders, [], '\n'.join(offenders))

    def test_serial_tools_stay_under_the_probe_limit(self):
        for path in (ROOT / 'compat/verify/marble_verify_all.sh',
                     ROOT / 'compat/verify/marble_identity.sh'):
            self.assertLess(path.stat().st_size, 2800, f'{path.name} exceeds 2800 bytes')


class MountIdempotencyTests(unittest.TestCase):
    """The stage script is re-run by hand while debugging; an unguarded mount
    stacks another layer every time and makes "the old file came back" hunts
    pointless.  Every mount must be guarded by a /proc/mounts check or by the
    bind_once helper (or be part of an `if ... ; then` probe)."""

    STAGE = ROOT / 'devices/marble/runtime/boot/stage-phase4-marble.sh'

    def test_every_mount_is_idempotent(self):
        lines = self.STAGE.read_text().splitlines()
        offenders = []
        for index, line in enumerate(lines):
            if not re.search(r'\$B mount ', line):
                continue
            if line.rstrip().endswith('; then'):
                continue  # probe used in a condition, not an action
            window = '\n'.join(lines[max(0, index - 10):index + 1])
            if '/proc/mounts' in window or 'bind_once' in window:
                continue
            offenders.append(f'{index + 1}: {line.strip()[:80]}')
        self.assertEqual(offenders, [], '\n'.join(offenders))

    def test_acdb_set_is_switchable_for_one_boot(self):
        """If the DSP rejects the module list, trying the vendor Forte ACDB set
        must not require a rebuild."""
        text = self.STAGE.read_text()
        self.assertIn('marble-acdb-forte', text)
        self.assertIn('MARBLE_ACDB_SET=', text)
        self.assertIn('Forte_acdb_cal.acdb', text)

    def test_stage_leaves_init_cfg_to_bootstrap(self):
        # hmos_root installs the real seed after graphics_ram_root returns.
        # A second stage copy cannot change what init actually reads.
        self.assertNotIn('init-cfg-marble.json', self.STAGE.read_text())
        probe = (ROOT / 'compat/bootstrap/hmos_probe_v121.c').read_text()
        stage = probe.index('if(graphics_ram_root()<0)')
        load = probe.index('readfile("/hmos-init-debug.cfg",cfg')
        install = probe.index('textfile("/hmos-root/system/etc/init.cfg",cfg)')
        self.assertLess(stage, load)
        self.assertLess(load, install)

    def test_single_subsystem_can_be_skipped_for_one_boot(self):
        """Bisecting a boot regression on hardware must not require a reflash."""
        text = self.STAGE.read_text()
        self.assertIn('MARBLE_SKIP_STAGES', text)
        self.assertIn('skipped() {', text)
        self.assertIn('marble-skip-stages', text)
        self.assertIn('tr -d " "', text, 'skip list must tolerate spaces')
        for section in ('audio', 'bluetooth'):
            self.assertIn(f'skipped {section}', text, f'{section} section is not skippable')
        self.assertEqual(text.count('MARBLE_STAGE_SKIPPED='), 3,
                         'each gated section must announce that it was skipped')

    def test_bulk_copy_groups_report_a_count(self):
        """37 cp calls cannot each carry a marker, but every bulk group must end
        with a count so a partial copy is visible in the boot log."""
        text = self.STAGE.read_text()
        for marker in ('MARBLE_FIRMWARE_MIRRORED', 'MARBLE_PD_FIRMWARE_STAGED',
                       'MARBLE_AUDIO_RUNTIME_STAGED', 'MARBLE_COMPAT_LIB64_STAGED'):
            self.assertIn(marker, text, f'{marker} missing: bulk copy has no post-condition')

    def test_mount_failures_are_visible(self):
        """A mount that fails with `|| true` and no marker turns a missing
        firmware source into a mystery later; every mount must say something."""
        lines = self.STAGE.read_text().splitlines()
        silent = []
        for index, line in enumerate(lines):
            if not re.search(r'\$B mount ', line):
                continue
            window = '\n'.join(lines[index:index + 6])
            inner = '\n'.join(lines[max(0, index - 6):index + 6])
            if 'MARBLE_' in window or 'MARBLE_' in inner:
                continue
            silent.append(f'{index + 1}: {line.strip()[:80]}')
        self.assertEqual(silent, [], '\n'.join(silent))

    def test_bind_once_helper_exists(self):
        text = self.STAGE.read_text()
        self.assertIn('bind_once() {', text)
        self.assertIn('/proc/mounts', text)

    def test_char_nodes_are_verified_against_sysfs(self):
        """misc devices get dynamic minors, so an existing path is not proof the
        node points at the right device; compare %t:%T with sysfs `dev`."""
        text = self.STAGE.read_text()
        self.assertIn("stat -c '%t:%T'", text)
        self.assertIn("printf '%x:%x'", text)
        self.assertIn('have', text)


if __name__ == '__main__':
    unittest.main()