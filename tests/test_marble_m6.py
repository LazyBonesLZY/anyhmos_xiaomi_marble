#!/usr/bin/env python3
"""Offline unit tests for Batch M6 (OpenHarmony kernel code_sign + ECDSA fs-verity + non-destructive ext4 verity)."""
from __future__ import annotations

import json
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CODESIGN_DIR = ROOT / 'devices/marble/kernel/marble-codesign'
CODESIGN_PATCH = ROOT / 'devices/marble/kernel/marble-codesign.patch'
CODESIGN_CFG = ROOT / 'devices/marble/runtime/codesign/init-m6-codesign-marble.cfg'
EXT4_HELPER_SRC = ROOT / 'compat/services/marble_ext4_verity_enable.c'
BUILD_KERNEL_SH = ROOT / 'tools/build/build_marble_kernel.sh'
M6_PACKAGER = ROOT / 'tools/packaging/build_marble_h20_m6_codesign.py'


class TestMarbleM6CodeSign(unittest.TestCase):
    def test_kernel_codesign_sources_and_patch_present(self):
        expected = [
            CODESIGN_DIR / 'crypto/ecdsa.c',
            CODESIGN_DIR / 'crypto/ecdsasignature.asn1',
            CODESIGN_DIR / 'include/linux/code_sign.h',
            CODESIGN_DIR / 'include/linux/xpm_types.h',
            CODESIGN_DIR / 'fs/code_sign/Kconfig',
            CODESIGN_DIR / 'fs/code_sign/Makefile',
            CODESIGN_DIR / 'fs/code_sign/code_sign_ext.c',
            CODESIGN_DIR / 'fs/code_sign/code_sign_ext.h',
            CODESIGN_DIR / 'fs/code_sign/code_sign_ioctl.c',
            CODESIGN_DIR / 'fs/code_sign/code_sign_ioctl.h',
            CODESIGN_DIR / 'fs/code_sign/code_sign_log.h',
            CODESIGN_DIR / 'fs/code_sign/code_sign_misc.c',
            CODESIGN_DIR / 'fs/code_sign/verify_cert_chain.c',
            CODESIGN_DIR / 'fs/code_sign/verify_cert_chain.h',
            CODESIGN_PATCH,
        ]
        for path in expected:
            self.assertTrue(path.is_file() and path.stat().st_size > 0, f'Missing {path}')

        patch_txt = CODESIGN_PATCH.read_text(encoding='utf-8')
        for marker in (
            'FS_IOC_ENABLE_CODE_SIGN',
            'OID_id_ansip384r1',
            'OID_id_ecdsa_with_sha384',
            'OID_ownerid',
            'fsverity_ioctl_enable_code_sign',
            'code_sign_developer_mode',
            'CONFIG_CRYPTO_ECDSA',
            'CONFIG_SECURITY_CODE_SIGN',
            'fs/ext4/ioctl.c',
            'fs/f2fs/file.c',
        ):
            self.assertIn(marker, patch_txt)

        ioctl_src = (CODESIGN_DIR / 'fs/code_sign/code_sign_ioctl.c').read_text(encoding='utf-8')
        for marker in ('ADD_CERT_CHAIN', 'REMOVE_CERT_CHAIN', 'ADD_ENTERPRISE_CERT', 'REMOVE_ENTERPRISE_CERT', 'ACTIVATE_CERT'):
            self.assertIn(marker, ioctl_src)
        self.assertIn('BUILD_BUG_ON(sizeof(struct cert_chain_info) != 64)', ioctl_src)

        verify_src = (CODESIGN_DIR / 'fs/code_sign/verify_cert_chain.c').read_text(encoding='utf-8')
        self.assertIn('pkcs7_parse_message', verify_src)
        self.assertIn('CERT_CHAIN_PATH_LEN_MAX', verify_src)
        self.assertNotIn('path_type = MAY_LOCAL_CODE', verify_src)

    def test_build_marble_kernel_wires_codesign(self):
        sh = BUILD_KERNEL_SH.read_text(encoding='utf-8')
        self.assertIn('marble-codesign', sh)
        self.assertIn('marble-codesign.patch', sh)
        self.assertIn('-e CRYPTO_ECDSA -e SECURITY_CODE_SIGN', sh)

    def test_init_m6_codesign_cfg_valid(self):
        raw = CODESIGN_CFG.read_bytes()
        self.assertLess(len(raw), 32768)
        doc = json.loads(raw.decode('utf-8'))
        svcs = {s['name']: s for s in doc['services']}
        self.assertIn('key_enable', svcs)
        self.assertEqual(svcs['key_enable']['path'], ['/system/bin/key_enable'])
        self.assertEqual(svcs['key_enable']['uid'], 'root')
        self.assertIn('local_code_sign', svcs)
        self.assertEqual(svcs['local_code_sign']['uid'], 'code_sign')
        jobs = {j['name']: j for j in doc['jobs']}
        self.assertIn('param:marble-m6-codesign', jobs)
        self.assertIn('write /proc/sys/fs/verity/require_signatures 1', jobs['param:marble-m6-codesign']['cmds'])
        self.assertIn('start key_enable', jobs['param:marble-m6-codesign']['cmds'])

    def test_ext4_verity_enabler_matches_tune2fs_byte_for_byte(self):
        with tempfile.TemporaryDirectory() as td:
            td_path = Path(td)
            exe = td_path / 'marble_ext4_verity_enable'
            subprocess.run(
                ['gcc', '-O2', '-Wall', '-Wextra', '-Werror', str(EXT4_HELPER_SRC), '-o', str(exe)],
                check=True,
            )
            for label, extra in (('csum', []), ('nocsum', ['-O', '^metadata_csum'])):
                img_ref = td_path / f'{label}_ref.ext4'
                img_test = td_path / f'{label}_test.ext4'
                img_ref.write_bytes(b'\x00' * (8 * 1024 * 1024))
                subprocess.run(['mkfs.ext4', '-q', '-b', '4096', *extra, str(img_ref)], check=True)
                img_test.write_bytes(img_ref.read_bytes())

                subprocess.run(['tune2fs', '-O', 'verity', str(img_ref)], check=True,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                out1 = subprocess.check_output([str(exe), str(img_test)], text=True)
                self.assertIn('EXT4_VERITY_ENABLED', out1)
                self.assertEqual(img_ref.read_bytes(), img_test.read_bytes(), f'{label} mismatch vs tune2fs')

                out2 = subprocess.check_output([str(exe), str(img_test)], text=True)
                self.assertIn('EXT4_VERITY_ALREADY_ENABLED', out2)
                fsck = subprocess.run(['e2fsck', '-fn', str(img_test)], capture_output=True, text=True)
                self.assertEqual(fsck.returncode, 0, f'{label} e2fsck failed: {fsck.stdout} {fsck.stderr}')

            # Dirty journal (INCOMPAT_RECOVER=0x4) without VERITY must return rc=20 untouched
            dirty = td_path / 'dirty.ext4'
            dirty.write_bytes(b'\x00' * (8 * 1024 * 1024))
            subprocess.run(['mkfs.ext4', '-q', '-b', '4096', '-O', '^metadata_csum', str(dirty)], check=True)
            raw_dirty = bytearray(dirty.read_bytes())
            incompat_off = 1024 + 0x60
            incompat_val = int.from_bytes(raw_dirty[incompat_off:incompat_off + 4], 'little') | 0x4
            raw_dirty[incompat_off:incompat_off + 4] = incompat_val.to_bytes(4, 'little')
            dirty.write_bytes(raw_dirty)
            res = subprocess.run([str(exe), str(dirty)], capture_output=True, text=True)
            self.assertEqual(res.returncode, 20, f'Expected rc=20 on dirty journal: {res.stdout} {res.stderr}')
            self.assertIn('EXT4_VERITY_NEEDS_RECOVERY', res.stdout)
            self.assertEqual(dirty.read_bytes(), bytes(raw_dirty), 'Dirty-journal superblock must not be modified')

    def test_m6_packager_syntax_and_guards(self):
        subprocess.run(['python3', '-m', 'py_compile', str(M6_PACKAGER)], check=True)
        src = M6_PACKAGER.read_text(encoding='utf-8')
        self.assertIn('libhmos_ram_no_tee.so', src)
        self.assertIn('audit_kabi', src)
        self.assertIn('crc_mismatches', src)
        self.assertIn('patch_donor_patches', src)
        self.assertIn('HMOS_USERDATA_VERITY_REPLAY_JOURNAL=1', src)


if __name__ == '__main__':
    unittest.main()
