import importlib.util
import json
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/packaging'))
import build_marble_h20_f1_modem as pkg  # noqa: E402


class MarbleBatchF1ModemTests(unittest.TestCase):
    def test_rmtfs_and_modem_probe_host_self_tests(self):
        scratch = '/mnt/e/marble-tmp' if Path('/mnt/e/marble-tmp').is_dir() else None
        with tempfile.TemporaryDirectory(dir=scratch) as td:
            d = Path(td)
            pd = ROOT / 'compat/audio/pd-mapper'
            tel = ROOT / 'compat/telephony'
            objs = []
            for name in ('qrtr.c', 'qmi.c', 'logging.c'):
                obj = d / f'{name}.o'
                subprocess.run(['gcc', '-O2', '-D_GNU_SOURCE', '-I', str(pd), '-c', str(pd / name), '-o', str(obj)], check=True)
                objs.append(str(obj))

            rmtfs_exe = d / 'hmos_rmtfs'
            subprocess.run(
                ['gcc', '-O2', '-Wall', '-Wextra', '-Werror', '-D_GNU_SOURCE', '-I', str(pd), '-I', str(tel),
                 str(tel / 'hmos_rmtfs.c'), *objs, '-o', str(rmtfs_exe)],
                check=True,
            )
            r1 = subprocess.run([str(rmtfs_exe), '--self-test'], check=True, capture_output=True, text=True)
            self.assertIn('RMTFS_SELF_TEST_OK=1', r1.stdout)

            probe_exe = d / 'marble_modem_probe'
            subprocess.run(
                ['gcc', '-O2', '-Wall', '-Wextra', '-Werror', '-std=c11', '-D_GNU_SOURCE', '-I', str(pd),
                 str(tel / 'marble_modem_probe.c'), '-o', str(probe_exe)],
                check=True,
            )
            r2 = subprocess.run([str(probe_exe), '--self-test'], check=True, capture_output=True, text=True)
            self.assertIn('MODEM_PROBE_SELF_TEST_OK=1', r2.stdout)

    def test_aarch64_binaries_statically_linked(self):
        for name in ('hmos_rmtfs', 'marble_modem_probe'):
            p = pkg.F1_BUILD / name
            self.assertTrue(p.is_file(), f'Missing built binary: {p}')
            res = subprocess.run(['file', str(p)], check=True, capture_output=True, text=True)
            self.assertIn('ARM aarch64', res.stdout)
            self.assertIn('statically linked', res.stdout)

    def test_rmtfs_ro_only_and_script_safety(self):
        rmtfs_c = (ROOT / 'compat/telephony/hmos_rmtfs.c').read_text(encoding='utf-8')
        # Block partitions loaded inside load_ro_shadow_parts must strictly use O_RDONLY
        start = rmtfs_c.index('load_ro_shadow_parts')
        end = rmtfs_c.index('resolve_path')
        block_loader = rmtfs_c[start:end]
        self.assertIn('open(path, O_RDONLY)', block_loader)
        self.assertNotIn('O_RDWR', block_loader)
        self.assertNotIn('O_WRONLY', block_loader)

        script = pkg.F1_START.read_text(encoding='utf-8')
        subprocess.run(['sh', '-n'], input=script.encode('utf-8'), check=True)
        # Safety checks: recovery=enabled must precede writing start to state
        self.assertIn("printf 'enabled'", script)
        self.assertLess(script.index('/recovery'), script.index('echo start > "$rproc/state"'))
        # Firmware symlinks only (no bulk copy), expose RO modem_a inside PID 1 /hmos-root, never touch persist/EFS rw
        self.assertIn('ln -sf', script)
        self.assertIn('$R/compat/original-firmware', script)
        self.assertNotIn('$B cp "$IMG"', script)
        self.assertIn('mknod "/dev/uio$uio_idx"', script)
        self.assertNotRegex(script, r'mount\b[^\n]*-o[^\n]*\brw\b[^\n]*(modem|persist|modemst|fsg|fsc)')
        self.assertNotIn('/persist', script)

    def test_package_roundtrip_on_e_base(self):
        if not pkg.BASE_DEFAULT.is_file():
            self.skipTest(f'Missing base image: {pkg.BASE_DEFAULT}')
        scratch = '/mnt/e/marble-tmp' if Path('/mnt/e/marble-tmp').is_dir() else None
        with tempfile.TemporaryDirectory(dir=scratch) as td:
            out_dir = Path(td)
            out_img = pkg.build(pkg.BASE_DEFAULT, out_dir)
            self.assertTrue(out_img.is_file())
            report = json.loads((out_dir / 'f1-build-report.json').read_text(encoding='utf-8'))
            self.assertEqual(report['entries_before'], 520)
            self.assertEqual(report['entries_after'], 523)
            self.assertEqual(report['unchanged_entries'], 518)
            self.assertEqual(
                report['modified_entries'],
                ['hmos-compat-marble/MANIFEST.json', 'hmos-compat-marble/graphics/stage-phase4-marble.sh'],
            )
            self.assertEqual(
                sorted(report['added_entries']),
                [
                    'graphics/hmos_rmtfs',
                    'graphics/marble_modem_probe',
                    'graphics/start-modem-f1-marble.sh',
                ],
            )

            spec = importlib.util.spec_from_file_location('arc', pkg.ORACLE)
            arc = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(arc)
            r = arc.load_tools()

            base_bytes = pkg.BASE_DEFAULT.read_bytes()
            base_entries, _, _, _, _ = r.extract_entries(
                pkg.BASE_DEFAULT, base_bytes, r.layout(base_bytes), out_dir / 'base_extract'
            )
            img_bytes = out_img.read_bytes()
            self.assertEqual(len(img_bytes), 100663296)
            entries, _, _, _, _ = r.extract_entries(
                out_img, img_bytes, r.layout(img_bytes), out_dir / 'cand_extract'
            )
            self.assertEqual(len(entries), 523)
            for g in pkg.GUARD_MEMBERS:
                self.assertEqual(r.sha(entries[g][1]), r.sha(base_entries[g][1]))
                self.assertEqual(report['guards_unchanged'][g], r.sha(base_entries[g][1]))

            packed_stage = entries[pkg.STAGE_MEMBER][1].decode('utf-8')
            self.assertIn('$B cp /graphics/hmos_rmtfs /compat/bin/hmos_rmtfs', packed_stage)
            self.assertIn('$B cp /graphics/marble_modem_probe "$R/compat/bin/marble_modem_probe"', packed_stage)
            # Manual runner only: stage script must NOT execute start-modem-f1-marble.sh
            self.assertNotRegex(
                packed_stage,
                r'(?:^|\s)(?:\$B\s+)?sh\s+[^\n]*start-modem-f1-marble\.sh',
            )


if __name__ == '__main__':
    unittest.main()
