import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
BUILDER = ROOT / 'tools/build/build_current.py'


class MarbleDisplayBuildTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        # Missing dependencies (with spaces) ensure this is a plan-only test.
        base = Path(temp.name).resolve()
        self.sdk = base / 'OHOS SDK'
        self.upstream = base / 'upstream headers'
        self.stage = base / 'libdrm stage'
        self.donor = base / 'donor libraries'
        self.out = base / 'candidate output'
        self.env = dict(os.environ, HMOS_SDK=str(self.sdk), HMOS_UPSTREAM=str(self.upstream),
                        HMOS_STAGE=str(self.stage), HMOS_DONOR_LINK=str(self.donor))

    def run_builder(self, *args):
        result = subprocess.run(
            [sys.executable, str(BUILDER), '--out', str(self.out), *args],
            env=self.env, capture_output=True, text=True,
        )
        self.assertFalse(self.out.exists(), 'a plan-only invocation created its output directory')
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout

    def plan(self, *args):
        return json.loads(self.run_builder('--dry-run', *args))

    def test_single_dsi_only_changes_composer_consumers(self):
        # Check all targets, not just those expected to change.
        targets = self.run_builder('--list').splitlines()
        default = self.plan(*targets)
        single = self.plan('--single-dsi', *targets)
        split_macro = '-DHMOS_SPLIT_SCANOUT=0'
        # direct-contract asserts two active planes; it stays on split scanout.
        affected = {'composer', 'mode-contract', 'present-contract', 'route-policy-contract'}
        self.assertEqual({target for target in default if default[target] != single[target]}, affected)
        for target in affected:
            with self.subTest(target=target):
                self.assertFalse(any(arg.startswith('-DHMOS_SPLIT_SCANOUT') for arg in default[target]))
                self.assertEqual(single[target].count(split_macro), 1)
                self.assertEqual([arg for arg in single[target] if arg != split_macro], default[target])

    def test_single_dsi_and_route_default_are_independent(self):
        route = '-DHMOS_DEFAULT_DISPLAY_ROUTE=2'
        split = '-DHMOS_SPLIT_SCANOUT=0'
        for options, expected in (
            ((), set()),
            (('--single-dsi',), {split}),
            (('--default-display-route', '2'), {route}),
            (('--single-dsi', '--default-display-route', '2'), {route, split}),
        ):
            with self.subTest(options=options):
                plan = self.plan(*options, 'composer', 'direct-contract', 'allocator', 'display-scanout')
                self.assertEqual({route, split}.intersection(plan['composer']), expected)
                self.assertEqual({route, split}.intersection(plan['direct-contract']), expected - {split})
                for target in ('allocator', 'display-scanout'):
                    self.assertFalse({route, split}.intersection(plan[target]))

    def test_scanout_diagnostics_use_target_headers_and_runtime(self):
        self.assertTrue({'display-scanout', 'kms-diagnostic', 'capture-scanout'}
                        <= set(self.run_builder('--list').splitlines()))
        plan = self.plan('display-scanout', 'kms-diagnostic', 'capture-scanout')
        suffix = '.exe' if os.name == 'nt' else ''
        for target, artifact, ext, driver in (
            ('display-scanout', 'hmos_display_scanout', 'cpp', 'clang++'),
            ('kms-diagnostic', 'hmos_kms_diagnostic', 'cpp', 'clang++'),
            ('capture-scanout', 'capture_scanout', 'c', 'clang'),
        ):
            with self.subTest(target=target):
                command = plan[target]
                self.assertEqual(command[0], str(self.sdk / 'llvm/bin' / (driver + suffix)))
                self.assertIn('--target=aarch64-linux-ohos', command)
                self.assertIn('--sysroot=' + str(self.sdk / 'sysroot'), command)
                self.assertIn(str(ROOT / 'compat/services' / f'{artifact}.{ext}'), command)
                self.assertEqual(command[-2:], ['-o', str(self.out / artifact)])
                if ext == 'cpp':
                    self.assertIn('-nostdlib++', command)
                    self.assertIn(str(ROOT / 'compat/platform/platform_cpp_config.h'), command)
                    self.assertIn(str(self.donor / 'libc++.so'), command)

        scanout = plan['display-scanout']
        self.assertIn('-I' + str(ROOT / 'compat/display/include/vdi-only'), scanout)
        self.assertIn('-I' + str(self.upstream / 'drivers_peripheral/display/buffer/hdi_service/include'), scanout)
        self.assertIn('-ldl', scanout)
        kms = plan['kms-diagnostic']
        for command in (scanout, kms):
            self.assertIn('-I' + str(self.stage / 'include'), command)
            self.assertIn('-I' + str(self.stage / 'include/libdrm'), command)
        drm = str(self.stage / 'lib/libdrm.a')
        self.assertIn(drm, kms)
        self.assertNotIn(drm, scanout)
        self.assertNotIn(drm, plan['capture-scanout'])
        for command in plan.values():
            self.assertNotIn('-ldrm', command)


if __name__ == '__main__':
    unittest.main()
