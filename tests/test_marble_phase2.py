"""Offline unit tests for Xiaomi marble Phase 2 bootstrap, packaging, and USB serial client."""

import base64
import importlib.util
from pathlib import Path
import shutil
import stat
import subprocess
import tarfile
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


def load_module(name: str, rel_path: str):
    spec = importlib.util.spec_from_file_location(name, ROOT / rel_path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


PHASE2 = load_module('build_marble_phase2_images', 'tools/packaging/build_marble_phase2_images.py')
SERIAL = load_module('hmos_serial', 'tools/maint/hmos_serial.py')


class MarblePhase2Tests(unittest.TestCase):
    def test_cpio_newc_roundtrip_preserves_entries(self):
        entries = {}
        inode = [1]
        PHASE2.put_file(entries, 'init', b'#!/bin/sh\necho hi\n', 0o755, inode)
        PHASE2.put_file(entries, 'hmos-compat-marble/hmos-probe', b'\x7fELFprobe', 0o755, inode)
        PHASE2.put_file(entries, 'lib/modules/dwc3-msm.ko', b'kmod-bytes', 0o644, inode)
        blob = PHASE2.pack_cpio_newc(entries)
        parsed = PHASE2.parse_cpio_newc(blob)
        self.assertEqual(set(parsed), set(entries))
        self.assertEqual(parsed['init'][1], b'#!/bin/sh\necho hi\n')
        self.assertEqual(parsed['hmos-compat-marble/hmos-probe'][1], b'\x7fELFprobe')
        self.assertTrue(stat.S_ISDIR(parsed['hmos-compat-marble'][0][1]))
        self.assertEqual(parsed['init'][0][1] & 0o777, 0o755)

    def test_serial_shell_command_encoding(self):
        req_id, line = SERIAL.build_shell_command('uname -a; lsmod', req_id='abc123')
        self.assertEqual(req_id, 'abc123')
        prefix, got_id, b64 = line.split(' ')
        self.assertEqual((prefix, got_id), ('cmd', 'abc123'))
        self.assertEqual(base64.b64decode(b64).decode('utf-8'), 'uname -a; lsmod')
        with self.assertRaises(ValueError):
            SERIAL.build_shell_command('id', req_id='bad-id!')

    def test_usb_setup_skips_dummy_udc_and_includes_marble_usb_modules(self):
        probe_src = (ROOT / 'compat/bootstrap/hmos_probe_v121.c').read_text(encoding='utf-8')
        self.assertIn('strncmp(de->d_name,"dummy_udc",9)', probe_src)
        for mod in ('gpi.ko', 'phy-msm-snps-hs.ko', 'nb7vpq904m.ko', 'dwc3-msm.ko'):
            self.assertIn(f'"{mod}"', probe_src)

    @unittest.skipUnless(
        all(shutil.which(tool) for tool in ('aarch64-linux-gnu-gcc', 'mkbootimg', 'unpack_bootimg', 'lz4')),
        'AArch64 GCC + mkbootimg + unpack_bootimg + lz4 required',
    )
    def test_synthetic_boot_and_vendor_boot_v4_roundtrip(self):
        with tempfile.TemporaryDirectory(prefix='marble-phase2-test-') as tmpdir:
            tmp = Path(tmpdir)
            # Create synthetic stock boot v4 (generic ramdisk) and vendor_boot v4
            stock_kernel = b'STOCK_KERNEL_BYTES' * 64
            stock_dtb = b'\xd0\x0d\xfe\xed' + b'\x00' * 124
            new_kernel = b'NEW_MARBLE_510_IMAGE' * 64
            new_dtb = b'\xd0\x0d\xfe\xed' + b'\x11' * 124

            boot_entries = {}
            inode = [1]
            PHASE2.put_file(boot_entries, 'init', b'STOCK_ANDROID_INIT', 0o755, inode)
            boot_rd_raw = tmp / 'stock_boot_rd.cpio'
            boot_rd_raw.write_bytes(PHASE2.pack_cpio_newc(boot_entries))
            boot_rd_lz4 = tmp / 'stock_boot_rd.lz4'
            PHASE2.compress_ramdisk(boot_rd_raw, boot_rd_lz4, 'lz4_legacy')

            stock_kernel_p = tmp / 'stock_kernel'
            stock_kernel_p.write_bytes(stock_kernel)
            stock_boot = tmp / 'boot_a.img'
            subprocess.run(
                [
                    'mkbootimg', '--header_version', '4',
                    '--kernel', str(stock_kernel_p),
                    '--ramdisk', str(boot_rd_lz4),
                    '--cmdline', 'rootwait ro init=/init',
                    '--os_version', '12.0.0', '--os_patch_level', '2025-03',
                    '--output', str(stock_boot),
                ],
                check=True,
            )

            vb_entries = {}
            vb_inode = [1]
            PHASE2.put_file(vb_entries, 'lib/modules/modules.load', b'dwc3-msm.ko\nmetis.ko\n', 0o644, vb_inode)
            PHASE2.put_file(vb_entries, 'lib/modules/modules.dep', b'/lib/modules/dwc3-msm.ko: /lib/modules/metis.ko\n/lib/modules/metis.ko:\n', 0o644, vb_inode)
            PHASE2.put_file(vb_entries, 'lib/modules/dwc3-msm.ko', b'OLD_DWC3_KO', 0o644, vb_inode)
            PHASE2.put_file(vb_entries, 'lib/modules/metis.ko', b'OLD_PROPRIETARY_METIS_KO', 0o644, vb_inode)
            vb_rd_raw = tmp / 'stock_vb_rd.cpio'
            vb_rd_raw.write_bytes(PHASE2.pack_cpio_newc(vb_entries))
            vb_rd_lz4 = tmp / 'stock_vb_rd.lz4'
            PHASE2.compress_ramdisk(vb_rd_raw, vb_rd_lz4, 'lz4_legacy')

            stock_dtb_p = tmp / 'stock.dtb'
            stock_dtb_p.write_bytes(stock_dtb)
            bootconfig_p = tmp / 'bootconfig'
            bootconfig_p.write_text('androidboot.hardware = "qcom"\n', encoding='utf-8')
            stock_vb = tmp / 'vendor_boot_a.img'
            subprocess.run(
                [
                    'mkbootimg', '--header_version', '4', '--pagesize', '4096',
                    '--dtb', str(stock_dtb_p),
                    '--vendor_cmdline', 'console=ttyMSM0,115200n8',
                    '--vendor_bootconfig', str(bootconfig_p),
                    '--ramdisk_type', 'platform',
                    '--ramdisk_name', '',
                    '--vendor_ramdisk_fragment', str(vb_rd_lz4),
                    '--vendor_boot', str(stock_vb),
                ],
                check=True,
            )

            # Create synthetic Phase 1 artifacts directory
            art = tmp / 'artifacts'
            (art / 'devicetree').mkdir(parents=True)
            (art / 'modules').mkdir(parents=True)
            (art / 'tests').mkdir(parents=True)
            (art / 'Image').write_bytes(new_kernel)
            (art / 'devicetree/ukee.dtb').write_bytes(new_dtb)
            (art / 'modules/hmos_ipc.ko').write_bytes(b'NEW_HMOS_IPC_KO')
            (art / 'modules/hmos_dmabuf_sync.ko').write_bytes(b'NEW_HMOS_DMABUF_KO')
            for t in ('token_contract', 'binder_nested', 'binder_oneway_pid'):
                (art / 'tests' / t).write_bytes(f'TEST_{t}'.encode())
            mod_tar = art / 'vendor_boot_modules.tar.gz'
            with tarfile.open(mod_tar, 'w:gz') as tf:
                for name, data in (
                    ('dwc3-msm.ko', b'NEW_510_DWC3_KO'),
                    ('nb7vpq904m.ko', b'NEW_510_NB7_KO'),
                    ('phy-msm-snps-hs.ko', b'NEW_510_SNPS_HS_KO'),
                ):
                    p = tmp / name
                    p.write_bytes(data)
                    tf.add(p, arcname=name)

            out_dir = tmp / 'out'
            report = PHASE2.build_phase2_images(stock_boot, stock_vb, art, out_dir)
            self.assertTrue(report['roundtrip_verified'])
            self.assertEqual(report['vendor_boot_phase2']['replaced_modules_count'], 1)
            self.assertEqual(report['vendor_boot_phase2']['removed_stale_modules'], ['metis.ko'])
            self.assertIn('nb7vpq904m.ko', report['vendor_boot_phase2']['added_usb_modules'])
            self.assertIn('phy-msm-snps-hs.ko', report['vendor_boot_phase2']['added_usb_modules'])
            self.assertIn('hmos-ipc.ko', report['compat_manifest'])
            self.assertIn('hmos-dmabuf-sync.ko', report['compat_manifest'])
            self.assertIn('init.android.original', report['compat_manifest'])


if __name__ == '__main__':
    unittest.main()
