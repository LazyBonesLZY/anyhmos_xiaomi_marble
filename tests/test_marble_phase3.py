"""Offline unit tests for Xiaomi marble Phase 3 LibLP super builder, runtime configs, and images."""

import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/packaging'))

from android_sparse import SparseImage, inspect_super
from liblp_sparse import build_super_sparse_image


def load_module(name: str, rel_path: str):
    spec = importlib.util.spec_from_file_location(name, ROOT / rel_path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


PHASE2 = load_module('build_marble_phase2_images', 'tools/packaging/build_marble_phase2_images.py')
PHASE3 = load_module('build_marble_phase3_images', 'tools/packaging/build_marble_phase3_images.py')


class MarblePhase3Tests(unittest.TestCase):
    def test_phase3_init_cfg_schema_and_buffer_limit(self):
        cfg_path = ROOT / 'devices/marble/runtime/init/hmos-init-phase3.cfg'
        raw = cfg_path.read_bytes()
        self.assertLess(len(raw), 32768)
        cfg = json.loads(raw.decode('utf-8'))
        services = {s['name']: s for s in cfg['services']}
        expected = {
            'samgr',
            'hilogd',
            'hdf_devmgr',
            'param_watcher',
            'concurrent_task_service',
            'accesstoken_service',
            'privacy_service',
            'huks_service',
            'accountmgr',
        }
        self.assertEqual(set(services), expected)
        for name, svc in services.items():
            self.assertEqual(svc['disabled'], 1, name)
            self.assertEqual(svc['once'], 1, name)
            self.assertEqual(svc['sandbox'], 0, name)
            self.assertEqual(svc['start-mode'], 'condition', name)
        jobs = {j['name']: j for j in cfg['jobs']}
        self.assertIn('HMOS_CORE_START_DONE', '\n'.join(jobs['post-init']['cmds']))
        self.assertEqual(jobs['param:marble-start-hdf']['condition'], 'bootevent.samgr.ready=true')
        core_cmds = jobs['param:marble-debug-coredeps']['cmds']
        self.assertLess(core_cmds.index('start accesstoken_service'), core_cmds.index('start privacy_service'))

    def test_minimal_hdf_hcb_header_and_node_lengths(self):
        hcb = PHASE3.build_minimal_hdf_hcb()
        magic, vmaj, vmin, csum, total_sz = struct.unpack_from('<IIIII', hcb, 0)
        self.assertEqual((magic, vmaj, vmin, csum), (0xA00AA00A, 0, 7, 0))
        self.assertEqual(total_sz, len(hcb) - 20)
        self.assertEqual(hcb[20:26], b'\x01root\x00')
        root_sz = struct.unpack_from('<I', hcb, 26)[0]
        self.assertEqual(root_sz, len(hcb) - 30)
        self.assertIn(b'device_info\x00', hcb)
        self.assertIn(b'match_attr\x00\x14hdf_manager\x00', hcb)

    def test_donor_patch_script_generation_and_syntax(self):
        script = PHASE3.generate_phase3_donor_patch_script(ROOT / 'patches/donor/ady-al10-111-sp10')
        self.assertIn('apply_one "$R/system/bin/init" "$R/compat/bin/init" "0755"', script)
        self.assertIn('libcode_sign_utils.z.so', script)
        self.assertIn('libcli_tool_client.z.so', script)
        self.assertNotIn('librelational_store_crypt.z.so', script)
        with tempfile.TemporaryDirectory() as td:
            p = Path(td) / 'patch.sh'
            p.write_text(script, encoding='utf-8')
            subprocess.run(['bash', '-n', str(p)], check=True)

    def test_liblp_v10_sparse_super_roundtrip_and_c_probe_compat(self):
        with tempfile.TemporaryDirectory(prefix='marble-liblp-test-') as td:
            tmp = Path(td)
            sources = []
            payloads = {}
            for name, size in [
                ('system', 65536 + 123),
                ('sys_prod', 32768),
                ('vendor', 49152 + 7),
                ('preload', 16384),
                ('vendor_dlkm', 20480),
            ]:
                data = (name.encode() * ((size // len(name)) + 1))[:size]
                p = tmp / f'{name}.img'
                p.write_bytes(data)
                sources.append((name, p))
                payloads[name] = data

            super_bytes = 16 * 1024 * 1024
            out_sparse = tmp / 'super.img'
            layout = build_super_sparse_image(
                out_sparse,
                super_bytes,
                sources,
                max_raw_chunk_bytes=32768,
                verify_readback=True,
            )
            self.assertTrue(layout['checksums_and_backups_verified'])
            self.assertEqual(layout['expanded_bytes'], super_bytes)
            self.assertEqual(layout['slots'], 3)
            names = [p['name'] for p in layout['partitions']]
            self.assertEqual(
                names,
                [
                    'system_a', 'sys_prod_a', 'vendor_a', 'preload_a', 'vendor_dlkm_a',
                    'system_b', 'sys_prod_b', 'vendor_b', 'preload_b', 'vendor_dlkm_b',
                ],
            )
            for part in layout['partitions']:
                if part['name'].endswith('_a'):
                    self.assertTrue(part['content_readback_verified'])
                else:
                    self.assertEqual(part['bytes'], 0)

            # Verify against host-compiled C hmos-probe --inspect-super when gcc is available
            if shutil.which('gcc'):
                probe_host = tmp / 'hmos-probe-host'
                subprocess.run(
                    [
                        'gcc', '-O2', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                        '-DHMOS_MARBLE_PHASE3=1',
                        str(ROOT / 'compat/bootstrap/hmos_probe_v121.c'),
                        '-o', str(probe_host),
                    ],
                    check=True,
                )
                raw_super = tmp / 'super.raw'
                sp = SparseImage(out_sparse)
                try:
                    raw_super.write_bytes(sp.read_at(0, sp.size))
                finally:
                    sp.close()
                res = subprocess.run(
                    [str(probe_host), '--inspect-super', str(raw_super)],
                    check=True,
                    capture_output=True,
                    text=True,
                )
                self.assertIn('LP slot=0 primary checksums OK', res.stdout)
                for pname in ('system_a', 'sys_prod_a', 'vendor_a', 'preload_a', 'vendor_dlkm_a'):
                    self.assertIn(f'PARTITION {pname} ', res.stdout)
                    self.assertIn('valid=1', res.stdout)

    @unittest.skipUnless(
        all(shutil.which(t) for t in ('aarch64-linux-gnu-gcc', 'mkbootimg', 'unpack_bootimg', 'lz4', 'mkfs.erofs')),
        'AArch64 GCC + mkbootimg + unpack_bootimg + lz4 + mkfs.erofs required',
    )
    def test_synthetic_phase3_images_roundtrip(self):
        with tempfile.TemporaryDirectory(prefix='marble-phase3-test-') as td:
            tmp = Path(td)
            stock_kernel = b'STOCK_KERNEL' * 64
            stock_dtb = b'\xd0\x0d\xfe\xed' + b'\x00' * 124
            new_kernel = b'MARBLE_510_KERNEL' * 64

            boot_entries = {}
            inode = [1]
            PHASE2.put_file(boot_entries, 'init', b'STOCK_INIT', 0o755, inode)
            boot_rd_raw = tmp / 'boot_rd.cpio'
            boot_rd_raw.write_bytes(PHASE2.pack_cpio_newc(boot_entries))
            boot_rd_lz4 = tmp / 'boot_rd.lz4'
            PHASE2.compress_ramdisk(boot_rd_raw, boot_rd_lz4, 'lz4_legacy')
            stock_kernel_p = tmp / 'kernel'
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
            PHASE2.put_file(vb_entries, 'lib/modules/modules.load', b'dwc3-msm.ko\n', 0o644, vb_inode)
            PHASE2.put_file(vb_entries, 'lib/modules/modules.dep', b'/lib/modules/dwc3-msm.ko:\n', 0o644, vb_inode)
            PHASE2.put_file(vb_entries, 'lib/modules/dwc3-msm.ko', b'OLD_DWC3', 0o644, vb_inode)
            vb_rd_raw = tmp / 'vb_rd.cpio'
            vb_rd_raw.write_bytes(PHASE2.pack_cpio_newc(vb_entries))
            vb_rd_lz4 = tmp / 'vb_rd.lz4'
            PHASE2.compress_ramdisk(vb_rd_raw, vb_rd_lz4, 'lz4_legacy')
            stock_dtb_p = tmp / 'stock.dtb'
            stock_dtb_p.write_bytes(stock_dtb)
            bc_p = tmp / 'bootconfig'
            bc_p.write_text('androidboot.hardware = "qcom"\n', encoding='utf-8')
            stock_vb = tmp / 'vendor_boot_a.img'
            subprocess.run(
                [
                    'mkbootimg', '--header_version', '4', '--pagesize', '4096',
                    '--dtb', str(stock_dtb_p),
                    '--vendor_cmdline', 'console=ttyMSM0,115200n8',
                    '--vendor_bootconfig', str(bc_p),
                    '--ramdisk_type', 'platform', '--ramdisk_name', '',
                    '--vendor_ramdisk_fragment', str(vb_rd_lz4),
                    '--vendor_boot', str(stock_vb),
                ],
                check=True,
            )

            art = tmp / 'artifacts'
            (art / 'modules').mkdir(parents=True)
            (art / 'tests').mkdir(parents=True)
            (art / 'Image').write_bytes(new_kernel)
            (art / 'modules/hmos_ipc.ko').write_bytes(b'HMOS_IPC')
            (art / 'modules/hmos_dmabuf_sync.ko').write_bytes(b'HMOS_DMABUF')
            for t in ('token_contract', 'binder_nested', 'binder_oneway_pid'):
                (art / 'tests' / t).write_bytes(t.encode())
            for tar_name, ko_name in (
                ('vendor_boot_modules.tar.gz', 'dwc3-msm.ko'),
                ('vendor_dlkm_modules.tar.gz', 'msm_drm.ko'),
            ):
                p = tmp / ko_name
                p.write_bytes(b'FAKE_KO_' + ko_name.encode())
                with tarfile.open(art / tar_name, 'w:gz') as tf:
                    tf.add(p, arcname=ko_name)

            busybox = tmp / 'busybox'
            busybox.write_bytes(b'FAKE_BUSYBOX')

            out_dir = tmp / 'out'
            report = PHASE3.build_phase3_images(
                stock_boot=stock_boot,
                stock_vendor_boot=stock_vb,
                artifacts_dir=art,
                donor_dir=None,
                recipes_dir=ROOT / 'patches/donor/ady-al10-111-sp10',
                busybox=busybox,
                out_dir=out_dir,
                skip_super=True,
            )
            self.assertTrue(report['roundtrip_verified'])
            self.assertEqual(report['vendor_dlkm']['module_count'], 1)
            for key in (
                'hmos-probe',
                'hmos-init-debug.cfg',
                'hmos-native-hilog',
                'busybox',
                'graphics/init-userdata-ext4.sh',
                'graphics/stage-phase3-marble.sh',
                'graphics/start-phase3-marble.sh',
                'graphics/apply-donor-patches-phase3.sh',
                'graphics/hdf_default_minimal.hcb',
            ):
                self.assertIn(key, report['compat_manifest'])


if __name__ == '__main__':
    unittest.main()
