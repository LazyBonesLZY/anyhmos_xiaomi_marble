#!/usr/bin/env python3
"""Offline unit tests and artifact checks for Batch F5 (DSDS/NAS, QMI_LOC AFW, QMI_PDC IDs, TQFTP unlink, sim_slot_cfg=0,1)."""
import importlib.util
import json
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
F5_BUILD = ROOT / 'work/marble-f5-build'
TELEPHONY_DIR = ROOT / 'devices/marble/runtime/telephony'
PACKAGER = ROOT / 'tools/packaging/build_marble_h20_f5_cellular_gnss.py'
FLASH_CTL = ROOT / 'work/marble-dual-20261123/flash-h20-f5-control.sh'

BASE_F4_IMG = Path('/mnt/e/marble-tmp/vendor_boot_h20_f4_rf_mcfg_7d92e2a0.img')


class TestBatchF5(unittest.TestCase):
    def test_host_self_tests_all_four_c_targets(self):
        with tempfile.TemporaryDirectory(prefix='f5_host_') as tmp:
            tp = Path(tmp)

            tqftp_bin = tp / 'tqftp_host'
            subprocess.run(
                ['gcc', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread',
                 str(ROOT / 'compat/telephony/hmos_tqftpserv.c'), '-o', str(tqftp_bin)],
                check=True,
            )
            r_tftp = subprocess.run([str(tqftp_bin), '--self-test'], check=True, capture_output=True, text=True)
            self.assertIn('TQFTPSERV_SELF_TEST_OK=1', r_tftp.stdout)

            probe_bin = tp / 'probe_host'
            subprocess.run(
                ['gcc', '-O2', '-Wall', '-Wextra', '-Werror', '-std=c11', '-D_GNU_SOURCE',
                 str(ROOT / 'compat/telephony/marble_modem_probe.c'), '-o', str(probe_bin)],
                check=True,
            )
            r_probe = subprocess.run([str(probe_bin), '--self-test'], check=True, capture_output=True, text=True)
            self.assertIn('MODEM_PROBE_SELF_TEST_OK=1', r_probe.stdout)

            gnss_bin = tp / 'gnss_host'
            subprocess.run(
                ['gcc', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread', '-DGNSS_HDI_HOST_TEST',
                 str(ROOT / 'compat/location/native_gnss_hdi_publisher.c'), '-o', str(gnss_bin), '-lm'],
                check=True,
            )
            r_gnss = subprocess.run([str(gnss_bin)], check=True, capture_output=True, text=True)
            self.assertIn('GNSS_HDI_HOST_TEST_OK=1', r_gnss.stdout)

            hdf_inc = ROOT / 'work/upstream/drivers_hdf_core/interfaces/inner_api'
            if hdf_inc.is_dir():
                hdr_bin = tp / 'gnss_host_hdr'
                subprocess.run(
                    ['gcc', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread', '-DGNSS_HDI_HOST_TEST',
                     f'-I{hdf_inc}/ipc', f'-I{hdf_inc}/utils', f'-I{hdf_inc}/core', f'-I{hdf_inc}/osal/uhdf',
                     str(ROOT / 'compat/location/native_gnss_hdi_publisher.c'), '-o', str(hdr_bin), '-lm'],
                    check=True,
                )

            ril_bin = tp / 'ril_host'
            subprocess.run(
                ['gcc', '-O2', '-Wall', '-Wextra', '-Werror', '-std=c11', '-D_GNU_SOURCE', '-DHRIL_VENDOR_HOST_TEST',
                 str(ROOT / 'compat/telephony/libril_vendor_qmi.c'), '-lpthread', '-o', str(ril_bin)],
                check=True,
            )
            r_ril = subprocess.run([str(ril_bin)], check=True, capture_output=True, text=True)
            self.assertIn('RIL_VENDOR_QMI_HOST_TEST_OK=1', r_ril.stdout)

    def test_cross_compiled_elfs(self):
        tqftp = F5_BUILD / 'hmos_tqftpserv'
        probe = F5_BUILD / 'marble_modem_probe'
        gnss = F5_BUILD / 'native_gnss_hdi_publisher'
        ril_so = F5_BUILD / 'libril_vendor_qmi.z.so'
        for p in (tqftp, probe, gnss, ril_so):
            self.assertTrue(p.is_file(), f'Missing cross-compiled binary: {p}')

        for static_bin in (tqftp, probe):
            out = subprocess.check_output(['file', str(static_bin)], text=True)
            self.assertIn('ELF 64-bit LSB executable, ARM aarch64', out)
            self.assertIn('statically linked', out)

        readelf_tool = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-readelf'
        nm_tool = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-nm'
        if readelf_tool.is_file():
            for dyn_bin in (gnss, ril_so):
                dyn = subprocess.check_output([str(readelf_tool), '-d', str(dyn_bin)], text=True)
                needed = re.findall(r'\(NEEDED\)\s+Shared library:\s+\[([^\]]+)\]', dyn)
                self.assertEqual(needed, ['libc.so'], f'{dyn_bin}: {needed}')
            interp = subprocess.check_output([str(readelf_tool), '-l', str(gnss)], text=True)
            self.assertIn('/lib/ld-musl-aarch64.so.1', interp)

            ril_dyn = subprocess.check_output([str(readelf_tool), '-d', str(ril_so)], text=True)
            soname = re.findall(r'\(SONAME\)\s+Library soname:\s+\[([^\]]+)\]', ril_dyn)
            self.assertEqual(soname, ['libril_vendor.z.so'])

        if nm_tool.is_file():
            syms = subprocess.check_output([str(nm_tool), '-D', '--defined-only', str(ril_so)], text=True)
            self.assertIn('RilInitOps', syms)
            self.assertNotIn('__packed', syms)

    def test_scripts_source_guards_and_audio_safety(self):
        sh_path = TELEPHONY_DIR / 'start-modem-f5-marble.sh'
        sh_txt = sh_path.read_text(encoding='utf-8')
        self.assertTrue(sh_txt.startswith('#!/busybox sh\n'))
        subprocess.run(['sh', '-n', str(sh_path)], check=True)
        for token in (
            '$B sh /compat/bin/start-modem-f3-marble.sh --tftp',
            '$B sh /compat/bin/start-modem-f1-marble.sh',
            'TQFTP_UNLINK_OK',
            '--dms-raw',
            '--nas-set-dsb both',
            '--loc-probe',
            'persist.radio.sim_slot_cfg',
            'const.telephony.vsimModemCount',
        ):
            self.assertIn(token, sh_txt)

        tqftp_src = (ROOT / 'compat/telephony/hmos_tqftpserv.c').read_text(encoding='utf-8')
        gnss_src = (ROOT / 'compat/location/native_gnss_hdi_publisher.c').read_text(encoding='utf-8')
        probe_src = (ROOT / 'compat/telephony/marble_modem_probe.c').read_text(encoding='utf-8')
        ril_src = (ROOT / 'compat/telephony/libril_vendor_qmi.c').read_text(encoding='utf-8')

        self.assertIn('has_unlink', tqftp_src)
        self.assertIn('compact_ro_dirs', tqftp_src)
        self.assertNotIn('bind(', tqftp_src)
        self.assertNotIn('bind(', gnss_src)
        self.assertIn('QMI_LOC_SET_NMEA_TYPES', gnss_src)
        self.assertIn('"MHAL"', gnss_src)
        self.assertIn('QMI_NAS_SET_DUAL_STANDBY_PREF', probe_src)
        self.assertIn('QMI_UIM_CHANGE_PROV_SESSION', probe_src)
        self.assertIn('QMI_PDC_LIST_CONFIGS 0x0024', probe_src)
        self.assertIn('QMI_PDC_GET_SELECTED_CONFIG 0x0022', probe_src)
        self.assertIn('QMI_NAS_SET_DUAL_STANDBY_PREF', ril_src)
        self.assertIn('nas_ensure_dsb_locked', ril_src)

        # Nighttime audio gain untouched
        self.assertIn('0.03f', (ROOT / 'compat/audio/audio_vdi.c').read_text(encoding='utf-8'))
        self.assertIn('0.03f', (ROOT / 'compat/audio/pal_server.c').read_text(encoding='utf-8'))

        if FLASH_CTL.is_file():
            subprocess.run(['bash', '-n', str(FLASH_CTL)], check=True)

    def test_stage_awk_bind_and_built_image_report(self):
        spec = importlib.util.spec_from_file_location('build_f5', PACKAGER)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)

        stage_txt = mod.STAGE_F5_BLOCK.decode('utf-8')
        self.assertNotIn(b'bind_once', mod.STAGE_F5_BLOCK)
        self.assertIn(b'$B mount --bind "$f5_dst" "$f5_src"', mod.STAGE_F5_BLOCK)
        self.assertIn(b'persist.radio.sim_slot_cfg=0,1', mod.STAGE_F5_BLOCK)

        # Verify space-tolerant awk logic against donor vsim_service.para format and tight '=' format.
        m = re.search(r"\$B awk -v k=\"\$f5_k\" -v v=\"\$f5_v\" '([^']+)'", stage_txt)
        self.assertIsNotNone(m)
        awk_prog = m.group(1)
        for sample_in in (
            '# Copyright\npersist.radio.sim_slot_cfg = 0,1,2\n',
            'persist.radio.sim_slot_cfg=0,1,2\n',
            '# Empty\n',
        ):
            res = subprocess.run(
                ['awk', '-v', 'k=persist.radio.sim_slot_cfg', '-v', 'v=0,1', awk_prog],
                input=sample_in,
                capture_output=True,
                text=True,
                check=True,
            )
            self.assertEqual(res.stdout.count('persist.radio.sim_slot_cfg = 0,1'), 1, res.stdout)
            self.assertNotIn('0,1,2', res.stdout)

        sample_stage = b'#!/bin/sh\nset -eu\n' + mod.STAGE_ANCHOR + b'\n'
        patched = mod.patch_stage(sample_stage)
        self.assertIn(b'start-modem-f5-marble.sh', patched)
        with self.assertRaises(ValueError):
            mod.patch_stage(patched)

        report_path = F5_BUILD / 'f5-build-report.json'
        if report_path.is_file() and BASE_F4_IMG.is_file():
            rep = json.loads(report_path.read_text(encoding='utf-8'))
            self.assertEqual(rep['base']['sha256'], mod.BASE_SHA)
            self.assertEqual(len(rep['guard_members_verified']), 9)
            cand_path = Path(rep['candidate']['path'])
            self.assertTrue(cand_path.is_file())
            if FLASH_CTL.is_file():
                ctl_txt = FLASH_CTL.read_text(encoding='utf-8')
                self.assertIn(cand_path.name, ctl_txt)
                self.assertIn(rep['candidate']['sha256'], ctl_txt)


if __name__ == '__main__':
    unittest.main()
