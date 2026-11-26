#!/usr/bin/env python3
"""Offline unit tests and artifact checks for Batch F3 (CoreService 2-slot + TQFTPSERV + GNSS HDI V2.0)."""
import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
F3_BUILD = ROOT / 'work/marble-f3-build'
TELEPHONY_DIR = ROOT / 'devices/marble/runtime/telephony'
PACKAGER = ROOT / 'tools/packaging/build_marble_h20_f3_rf_gnss.py'
FLASH_CTL = ROOT / 'work/marble-dual-20261123/flash-h20-f3-control.sh'

BASE_F2_IMG = Path('/mnt/e/marble-tmp/vendor_boot_h20_f2_telephony_8da1f9b1.img')


class TestBatchF3(unittest.TestCase):
    def test_host_tqftpserv_self_test(self):
        with tempfile.TemporaryDirectory(prefix='f3_tftp_') as tmp:
            bin_path = Path(tmp) / 'tqftp_host'
            subprocess.run(
                [
                    'gcc', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread',
                    str(ROOT / 'compat/telephony/hmos_tqftpserv.c'),
                    '-o', str(bin_path),
                ],
                check=True,
            )
            res = subprocess.run([str(bin_path), '--self-test'], check=True, capture_output=True, text=True)
            self.assertIn('TQFTPSERV_SELF_TEST_OK=1', res.stdout)

    def test_host_modem_probe_self_test(self):
        with tempfile.TemporaryDirectory(prefix='f3_probe_') as tmp:
            bin_path = Path(tmp) / 'probe_host'
            subprocess.run(
                [
                    'gcc', '-O2', '-Wall', '-Wextra', '-Werror',
                    str(ROOT / 'compat/telephony/marble_modem_probe.c'),
                    '-o', str(bin_path),
                ],
                check=True,
            )
            res = subprocess.run([str(bin_path), '--self-test'], check=True, capture_output=True, text=True)
            self.assertIn('MODEM_PROBE_SELF_TEST_OK=1', res.stdout)

    def test_host_gnss_hdi_self_test_and_abi_headers(self):
        with tempfile.TemporaryDirectory(prefix='f3_gnss_') as tmp:
            bin_path = Path(tmp) / 'gnss_host'
            subprocess.run(
                [
                    'gcc', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread', '-DGNSS_HDI_HOST_TEST',
                    str(ROOT / 'compat/location/native_gnss_hdi_publisher.c'),
                    '-o', str(bin_path), '-lm',
                ],
                check=True,
            )
            res = subprocess.run([str(bin_path)], check=True, capture_output=True, text=True)
            self.assertIn('GNSS_HDI_HOST_TEST_OK=1', res.stdout)

            # Verify struct offsets compile identically against upstream OHOS HDF headers when present.
            hdf_inc = ROOT / 'work/upstream/drivers_hdf_core/interfaces/inner_api'
            if hdf_inc.is_dir():
                hdr_bin = Path(tmp) / 'gnss_host_hdr'
                subprocess.run(
                    [
                        'gcc', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread', '-DGNSS_HDI_HOST_TEST',
                        f'-I{hdf_inc}/ipc',
                        f'-I{hdf_inc}/utils',
                        f'-I{hdf_inc}/core',
                        f'-I{hdf_inc}/osal/uhdf',
                        str(ROOT / 'compat/location/native_gnss_hdi_publisher.c'),
                        '-o', str(hdr_bin), '-lm',
                    ],
                    check=True,
                )

    def test_cross_compiled_elfs(self):
        tqftp = F3_BUILD / 'hmos_tqftpserv'
        probe = F3_BUILD / 'marble_modem_probe'
        gnss = F3_BUILD / 'native_gnss_hdi_publisher'
        for p in (tqftp, probe, gnss):
            self.assertTrue(p.is_file(), f'Missing cross-compiled binary: {p}')

        for static_bin in (tqftp, probe):
            out = subprocess.check_output(['file', str(static_bin)], text=True)
            self.assertIn('ELF 64-bit LSB executable, ARM aarch64', out)
            self.assertIn('statically linked', out)

        readelf_tool = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-readelf'
        if readelf_tool.is_file():
            dyn = subprocess.check_output([str(readelf_tool), '-d', str(gnss)], text=True)
            needed = [line for line in dyn.splitlines() if '(NEEDED)' in line]
            self.assertEqual(len(needed), 1, f'Expected single NEEDED libc.so, got: {needed}')
            self.assertIn('[libc.so]', needed[0])
            interp = subprocess.check_output([str(readelf_tool), '-l', str(gnss)], text=True)
            self.assertIn('/lib/ld-musl-aarch64.so.1', interp)

    def test_configs_scripts_and_audio_safety(self):
        # F2 baseline json untouched (Option A)
        f2_tel = json.loads((TELEPHONY_DIR / 'telephony-marble.json').read_text(encoding='utf-8'))
        self.assertEqual([x['name'] for x in f2_tel['systemability']], [4010, 4007, 4008])

        f3_tel = json.loads((TELEPHONY_DIR / 'telephony-f3-marble.json').read_text(encoding='utf-8'))
        self.assertEqual([x['name'] for x in f3_tel['systemability']], [4010, 4007, 4008, 66250])
        sa_map = {x['name']: x for x in f3_tel['systemability']}
        self.assertEqual(sa_map[66250]['libpath'], 'liblpa_esim_service.z.so')
        self.assertFalse(sa_map[66250]['run-on-create'])

        cfg = json.loads((TELEPHONY_DIR / 'init-modem-f3-marble.cfg').read_text(encoding='utf-8'))
        svc = next(s for s in cfg['services'] if s['name'] == 'gnss_hdi_marble')
        self.assertEqual(svc['disabled'], 1)
        self.assertEqual(svc['start-mode'], 'condition')
        self.assertEqual(svc['uid'], 'gps')
        self.assertEqual(svc['secon'], 'u:r:location_host:s0')
        job = next(j for j in cfg['jobs'] if j['name'] == 'param:marble-gnss-f3')
        self.assertEqual(job['condition'], 'marble.debug.start.gnss=true')

        sh_path = TELEPHONY_DIR / 'start-modem-f3-marble.sh'
        sh_txt = sh_path.read_text(encoding='utf-8')
        self.assertTrue(sh_txt.startswith('#!/busybox sh\n'))
        subprocess.run(['sh', '-n', str(sh_path)], check=True)
        self.assertIn('mount -t ext4 -o ro,noload', sh_txt)
        self.assertIn('F3_PERSIST_UNMOUNTED=1', sh_txt)
        self.assertIn('hmos_tqftpserv', sh_txt)
        self.assertIn('$B sh /compat/bin/start-modem-f1-marble.sh', sh_txt)
        self.assertIn('$B sh /compat/bin/start-modem-f2-marble.sh --telephony', sh_txt)
        do_modem_body = sh_txt[sh_txt.index('do_modem()'):]
        self.assertLess(do_modem_body.index('do_tftp'), do_modem_body.index('start-modem-f1-marble.sh'))

        tqftp_src = (ROOT / 'compat/telephony/hmos_tqftpserv.c').read_text(encoding='utf-8')
        gnss_src = (ROOT / 'compat/location/native_gnss_hdi_publisher.c').read_text(encoding='utf-8')
        self.assertIn('0x0101u', tqftp_src)
        self.assertNotIn('bind(', tqftp_src)
        self.assertNotIn('bind(', gnss_src)

        # Nighttime audio gain untouched
        self.assertIn('0.03f', (ROOT / 'compat/audio/audio_vdi.c').read_text(encoding='utf-8'))
        self.assertIn('0.03f', (ROOT / 'compat/audio/pal_server.c').read_text(encoding='utf-8'))

        # Syntax check flash control script
        subprocess.run(['bash', '-n', str(FLASH_CTL)], check=True)

    def test_stage_and_built_image_report_and_guards(self):
        import importlib.util
        spec = importlib.util.spec_from_file_location('build_f3', PACKAGER)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)

        # Verify STAGE_F3_BLOCK syntax, self-contained copy/bind, and idempotent splice
        self.assertNotIn(b'bind_once', mod.STAGE_F3_BLOCK)
        self.assertIn(b'$B mount --bind "$f3_dst" "$f3_src"', mod.STAGE_F3_BLOCK)
        sample_stage = b'#!/bin/sh\nset -eu\n' + mod.STAGE_ANCHOR + b'\n'
        patched = mod.patch_stage(sample_stage)
        self.assertIn(b'const.booster.virtual_modem_switch=false', patched)
        self.assertIn(b'persist.distributedmodem.multidevice.enable=false', patched)
        self.assertIn(b'const.ril.esim_type=0', patched)
        self.assertIn(b'vendor.hw_mc.ril.esim.close_esim=false', patched)
        with self.assertRaises(ValueError):
            mod.patch_stage(patched)

        report_path = F3_BUILD / 'f3-build-report.json'
        if report_path.is_file() and BASE_F2_IMG.is_file():
            rep = json.loads(report_path.read_text(encoding='utf-8'))
            self.assertEqual(rep['base']['sha256'], mod.BASE_SHA)
            self.assertEqual(len(rep['guard_members_verified']), 8)
            cand_path = Path(rep['candidate']['path'])
            self.assertTrue(cand_path.is_file())
            self.assertIn(cand_path.name, FLASH_CTL.read_text(encoding='utf-8'))


if __name__ == '__main__':
    unittest.main()
