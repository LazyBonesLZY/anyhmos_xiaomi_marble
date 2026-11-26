#!/usr/bin/env python3
"""Offline unit tests for the Xiaomi marble Fastboot one-click flash packager and scripts."""

from __future__ import annotations

import hashlib
import json
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
PACKAGING_DIR = REPO_ROOT / 'tools/packaging'
if str(PACKAGING_DIR) not in sys.path:
    sys.path.insert(0, str(PACKAGING_DIR))

import assemble_marble_fastboot_package as packager


def make_synthetic_boot(path: Path) -> None:
    path.write_bytes(b'ANDROID!' + b'\x00' * 8184)


def make_synthetic_vendor_boot(path: Path) -> None:
    path.write_bytes(b'VNDRBOOT' + b'\x00' * 8184)


def make_synthetic_vbmeta(path: Path) -> None:
    buf = bytearray(4096)
    buf[0:4] = b'AVB0'
    struct.pack_into('>I', buf, 120, 0x00000003)
    path.write_bytes(bytes(buf))


def make_synthetic_sparse_super(path: Path, expanded_bytes: int = packager.SUPER_BYTES) -> None:
    blk_sz = 4096
    total_blks = expanded_bytes // blk_sz
    hdr = struct.pack('<IHHHHIIII', packager.ANDROID_SPARSE_MAGIC, 1, 0, 28, 12, blk_sz, total_blks, 1, 0)
    chunk = struct.pack('<HHII', 0xCAC3, 0, total_blks, 12)
    path.write_bytes(hdr + chunk)


def make_fake_platform_tools(dir_path: Path) -> None:
    dir_path.mkdir(parents=True, exist_ok=True)
    for name in packager.REQUIRED_PLATFORM_TOOLS:
        (dir_path / name).write_bytes(f'fake-{name}\n'.encode('ascii'))
    (dir_path / 'mke2fs.exe').write_bytes(b'fake-mke2fs\n')


class TestMarbleFastbootPackage(unittest.TestCase):
    def test_assemble_upgrade_package_verify_and_tamper_detection(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            boot = root / 'boot.img'
            vb = root / 'vendor_boot.img'
            pt = root / 'platform-tools'
            report = root / 'm6-build-report.json'
            out_dir = root / 'pkg-upgrade'

            make_synthetic_boot(boot)
            make_synthetic_vendor_boot(vb)
            make_fake_platform_tools(pt)
            report.write_text(
                json.dumps(
                    {
                        'batch': 'M6',
                        'boot': {'candidate': {'path': '/mnt/e/private-user/boot_m6.img', 'bytes': 8192}},
                        'vendor_boot': {'base': {'path': '/home/secret/work/vb.img'}},
                    }
                ),
                encoding='utf-8',
            )

            summary = packager.assemble_package(
                out_dir=out_dir,
                boot_path=boot,
                vendor_boot_path=vb,
                platform_tools_dir=pt,
                build_report_path=report,
                device_boot_tested=True,
                verified_boot_id='52d5c34b-ba82-46a6-8679-2b1099e2e688',
            )
            self.assertEqual(summary['package_type'], 'upgrade')
            self.assertFalse(summary['device_flashed'])

            manifest = json.loads((out_dir / 'PACKAGE.json').read_text(encoding='utf-8'))
            self.assertEqual(manifest['kind'], 'marble-anyhmos-fastboot')
            self.assertEqual(manifest['device'], 'marble')
            self.assertEqual(manifest['target_slot'], 'a')
            self.assertFalse(manifest['has_init_boot'])
            self.assertTrue(manifest['device_boot_tested'])
            self.assertIsNone(manifest['super_bytes'])

            # Sanitized report must strip host paths
            embedded_report = (out_dir / 'reports/build-report.json').read_text(encoding='utf-8')
            self.assertNotIn('/mnt/e/', embedded_report)
            self.assertNotIn('/home/secret', embedded_report)
            self.assertIn('boot_m6.img', embedded_report)

            # Upgrade-only wrappers exist; full-install wrappers must NOT exist
            self.assertTrue((out_dir / '1-Verify-Package.bat').is_file())
            self.assertTrue((out_dir / '2-Inspect-Device.bat').is_file())
            self.assertTrue((out_dir / '3-Flash-Upgrade-KeepData.bat').is_file())
            self.assertFalse((out_dir / '4-Flash-Full-Install.bat').exists())

            # SHA256SUMS matches every recorded file
            for line in (out_dir / 'SHA256SUMS').read_text(encoding='utf-8').splitlines():
                expected_sha, rel = line.split('  ', 1)
                actual_sha = hashlib.sha256((out_dir / rel).read_bytes()).hexdigest()
                self.assertEqual(actual_sha, expected_sha, rel)

            # flash-marble.sh verify passes
            res = subprocess.run(
                ['bash', str(out_dir / 'flash-marble.sh'), 'verify'],
                capture_output=True,
                text=True,
            )
            self.assertEqual(res.returncode, 0, res.stderr + res.stdout)
            self.assertIn('Offline verification passed', res.stdout)

            # Full mode must be rejected on upgrade-only package
            res_full = subprocess.run(
                ['bash', str(out_dir / 'flash-marble.sh'), 'full'],
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(res_full.returncode, 0)
            self.assertIn('upgrade-only', res_full.stderr)

            # Tampering 1 byte must fail verify
            tampered = bytearray((out_dir / 'images/boot.img').read_bytes())
            tampered[-1] ^= 0xFF
            (out_dir / 'images/boot.img').write_bytes(bytes(tampered))
            res_bad = subprocess.run(
                ['bash', str(out_dir / 'flash-marble.sh'), 'verify'],
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(res_bad.returncode, 0)
            self.assertIn('SHA-256 mismatch', res_bad.stderr)

    def test_assemble_full_package_and_verify(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            boot = root / 'boot.img'
            vb = root / 'vendor_boot.img'
            vbm = root / 'vbmeta.img'
            vbms = root / 'vbmeta_system.img'
            sup = root / 'super.img'
            out_dir = root / 'pkg-full'

            make_synthetic_boot(boot)
            make_synthetic_vendor_boot(vb)
            make_synthetic_vbmeta(vbm)
            make_synthetic_vbmeta(vbms)
            make_synthetic_sparse_super(sup)

            summary = packager.assemble_package(
                out_dir=out_dir,
                boot_path=boot,
                vendor_boot_path=vb,
                super_path=sup,
                vbmeta_path=vbm,
                vbmeta_system_path=vbms,
                device_boot_tested=True,
                verified_boot_id='52d5c34b-ba82-46a6-8679-2b1099e2e688',
            )
            self.assertEqual(summary['package_type'], 'full')
            self.assertIsNotNone(summary['super_sha256'])
            self.assertTrue((out_dir / '4-Flash-Full-Install.bat').is_file())
            self.assertTrue((out_dir / '5-Flash-Full-Install-FormatUserdata.bat').is_file())

            manifest = json.loads((out_dir / 'PACKAGE.json').read_text(encoding='utf-8'))
            self.assertEqual(manifest['super_bytes'], packager.SUPER_BYTES)

            res = subprocess.run(
                ['bash', str(out_dir / 'flash-marble.sh'), 'verify'],
                capture_output=True,
                text=True,
            )
            self.assertEqual(res.returncode, 0, res.stderr + res.stdout)

    def test_rejects_invalid_headers_and_wrong_super_size(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            bad_boot = root / 'bad_boot.img'
            good_boot = root / 'good_boot.img'
            bad_vb = root / 'bad_vb.img'
            good_vb = root / 'good_vb.img'
            vbm = root / 'vbmeta.img'
            vbms = root / 'vbmeta_system.img'
            wrong_sup = root / 'wrong_super.img'

            bad_boot.write_bytes(b'NOTABOOT' + b'\x00' * 4096)
            make_synthetic_boot(good_boot)
            bad_vb.write_bytes(b'NOTAVB!!' + b'\x00' * 4096)
            make_synthetic_vendor_boot(good_vb)
            make_synthetic_vbmeta(vbm)
            make_synthetic_vbmeta(vbms)
            make_synthetic_sparse_super(wrong_sup, expanded_bytes=8 * 1024 * 1024 * 1024)

            with self.assertRaises(ValueError):
                packager.assemble_package(out_dir=root / 'o1', boot_path=bad_boot, vendor_boot_path=good_vb)
            with self.assertRaises(ValueError):
                packager.assemble_package(out_dir=root / 'o2', boot_path=good_boot, vendor_boot_path=bad_vb)
            with self.assertRaises(ValueError):
                packager.assemble_package(
                    out_dir=root / 'o3',
                    boot_path=good_boot,
                    vendor_boot_path=good_vb,
                    super_path=wrong_sup,
                    vbmeta_path=vbm,
                    vbmeta_system_path=vbms,
                )
            with self.assertRaises(ValueError):
                packager.assemble_package(
                    out_dir=root / 'o4',
                    boot_path=good_boot,
                    vendor_boot_path=good_vb,
                    super_path=wrong_sup,
                )

    def test_flash_scripts_syntax_and_safety_guards(self) -> None:
        ps1 = (PACKAGING_DIR / 'Flash-Marble.ps1').read_text(encoding='utf-8')
        sh = (PACKAGING_DIR / 'flash-marble.sh').read_text(encoding='utf-8')

        # Bash syntax check
        subprocess.run(['bash', '-n', str(PACKAGING_DIR / 'flash-marble.sh')], check=True)

        for name, text in [('Flash-Marble.ps1', ps1), ('flash-marble.sh', sh)]:
            self.assertIn('marble', text, name)
            self.assertIn('set_active', text, name)
            self.assertIn('UPGRADE MARBLE', text, name)
            self.assertIn('FLASH MARBLE FULL', text, name)
            self.assertIn('FORMAT MARBLE USERDATA EXT4', text, name)
            self.assertIn('--disable-verity', text, name)
            self.assertIn('--disable-verification', text, name)
            # Never flash init_boot or protected firmware partitions
            self.assertNotIn('init_boot_a', text, name)
            self.assertNotIn('init_boot.img', text, name)
            self.assertNotIn('flash init_boot', text, name)
            self.assertNotIn('flash abl', text, name)
            self.assertNotIn('flash xbl', text, name)
            self.assertNotIn('flash persist', text, name)
            # No hardcoded personal paths or fixed fastboot serials
            self.assertNotRegex(text, r'(?i)(/home/|/mnt/[a-z]/|[a-z]:\\users\\)', name)
            self.assertNotRegex(text, r'fastboot(?:\.exe)?\s+-s\s+[0-9a-f]{6,}', name)


if __name__ == '__main__':
    unittest.main()
