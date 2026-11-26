#!/usr/bin/env python3
"""Offline unit tests and artifact checks for Batch F4 (TQFTPSERV MCFG/RFS fix + RF/LOC diagnostics + vsimModemCount=0)."""
import json
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
F4_BUILD = ROOT / 'work/marble-f4-build'
TELEPHONY_DIR = ROOT / 'devices/marble/runtime/telephony'
PACKAGER = ROOT / 'tools/packaging/build_marble_h20_f4_rf_mcfg.py'
FLASH_CTL = ROOT / 'work/marble-dual-20261123/flash-h20-f4-control.sh'

BASE_F3_IMG = Path('/mnt/e/marble-tmp/vendor_boot_h20_f3_rf_gnss_e4043df4.img')


class TestBatchF4(unittest.TestCase):
    def test_host_tqftpserv_self_test(self):
        with tempfile.TemporaryDirectory(prefix='f4_tftp_') as tmp:
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
        with tempfile.TemporaryDirectory(prefix='f4_probe_') as tmp:
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
        with tempfile.TemporaryDirectory(prefix='f4_gnss_') as tmp:
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
        tqftp = F4_BUILD / 'hmos_tqftpserv'
        probe = F4_BUILD / 'marble_modem_probe'
        gnss = F4_BUILD / 'native_gnss_hdi_publisher'
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

    def test_scripts_source_guards_and_audio_safety(self):
        sh_path = TELEPHONY_DIR / 'start-modem-f4-marble.sh'
        sh_txt = sh_path.read_text(encoding='utf-8')
        self.assertTrue(sh_txt.startswith('#!/busybox sh\n'))
        subprocess.run(['sh', '-n', str(sh_path)], check=True)
        self.assertIn('$B sh /compat/bin/start-modem-f3-marble.sh --tftp', sh_txt)
        self.assertIn('$B sh /compat/bin/start-modem-f1-marble.sh', sh_txt)
        self.assertIn('--services-all', sh_txt)
        self.assertIn('--dms-raw', sh_txt)
        self.assertIn('--loc-probe', sh_txt)
        self.assertIn('const.telephony.vsimModemCount', sh_txt)

        tqftp_src = (ROOT / 'compat/telephony/hmos_tqftpserv.c').read_text(encoding='utf-8')
        gnss_src = (ROOT / 'compat/location/native_gnss_hdi_publisher.c').read_text(encoding='utf-8')
        probe_src = (ROOT / 'compat/telephony/marble_modem_probe.c').read_text(encoding='utf-8')
        self.assertIn('compact_ro_dirs', tqftp_src)
        self.assertNotIn('bind(', tqftp_src)
        self.assertNotIn('bind(', gnss_src)
        self.assertIn('QMI_LOC_INFORM_CLIENT_REVISION', gnss_src)
        self.assertIn('GNSS_QMI_LOC_REG_ERR', gnss_src)
        self.assertIn('QMI_PDC_GET_SELECTED_CONFIG', probe_src)
        self.assertIn('QMI_DMS_GET_BAND_CAPABILITY', probe_src)

        # Nighttime audio gain untouched
        self.assertIn('0.03f', (ROOT / 'compat/audio/audio_vdi.c').read_text(encoding='utf-8'))
        self.assertIn('0.03f', (ROOT / 'compat/audio/pal_server.c').read_text(encoding='utf-8'))

        subprocess.run(['bash', '-n', str(FLASH_CTL)], check=True)

    def test_stage_and_built_image_report_and_guards(self):
        import importlib.util
        spec = importlib.util.spec_from_file_location('build_f4', PACKAGER)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)

        self.assertNotIn(b'bind_once', mod.STAGE_F4_BLOCK)
        self.assertIn(b'$B mount --bind "$f4_dst" "$f4_src"', mod.STAGE_F4_BLOCK)
        self.assertIn(b'const.telephony.vsimModemCount=0', mod.STAGE_F4_BLOCK)
        sample_stage = b'#!/bin/sh\nset -eu\n' + mod.STAGE_ANCHOR + b'\n'
        patched = mod.patch_stage(sample_stage)
        self.assertIn(b'start-modem-f4-marble.sh', patched)
        with self.assertRaises(ValueError):
            mod.patch_stage(patched)

        report_path = F4_BUILD / 'f4-build-report.json'
        if report_path.is_file() and BASE_F3_IMG.is_file():
            rep = json.loads(report_path.read_text(encoding='utf-8'))
            self.assertEqual(rep['base']['sha256'], mod.BASE_SHA)
            self.assertEqual(len(rep['guard_members_verified']), 10)
            cand_path = Path(rep['candidate']['path'])
            self.assertTrue(cand_path.is_file())
            self.assertIn(cand_path.name, FLASH_CTL.read_text(encoding='utf-8'))


if __name__ == '__main__':
    unittest.main()
