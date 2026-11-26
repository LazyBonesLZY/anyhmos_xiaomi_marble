#!/usr/bin/env python3
"""Offline unit & packaging verification for Batch F6 (IPA 8-KO order + multi-SIM QMI NAS + delta poll)."""
from __future__ import annotations

import json
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / 'tools/packaging'
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import build_marble_h20_f6_cellular_nas as pkg

F6_SH = ROOT / 'devices/marble/runtime/telephony/start-modem-f6-marble.sh'
POSTBOOT_SH = ROOT / 'devices/marble/runtime/telephony/start-u2-postboot-marble.sh'
PROBE_C = ROOT / 'compat/telephony/marble_modem_probe.c'
RIL_C = ROOT / 'compat/telephony/libril_vendor_qmi.c'
LLVM_READELF = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-readelf'
LLVM_NM = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-nm'


class MarbleBatchF6Tests(unittest.TestCase):
    def test_host_self_tests(self):
        with tempfile.TemporaryDirectory() as td:
            tp = Path(td)
            probe_host = tp / 'probe_host'
            ril_host = tp / 'ril_host'
            subprocess.run(
                ['gcc', '-O2', '-Wall', '-Wextra', '-Werror', '-std=c11', '-D_GNU_SOURCE',
                 str(PROBE_C), '-o', str(probe_host)],
                check=True,
            )
            p_res = subprocess.run([str(probe_host), '--self-test'], check=True, capture_output=True, text=True)
            self.assertIn('MODEM_PROBE_SELF_TEST_OK=1', p_res.stdout)

            subprocess.run(
                ['gcc', '-O2', '-Wall', '-Wextra', '-Werror', '-std=c11', '-D_GNU_SOURCE', '-DHRIL_VENDOR_HOST_TEST',
                 str(RIL_C), '-lpthread', '-o', str(ril_host)],
                check=True,
            )
            r_res = subprocess.run([str(ril_host)], check=True, capture_output=True, text=True)
            self.assertIn('RIL_VENDOR_QMI_HOST_TEST_OK=1', r_res.stdout)

    def test_f6_ohos_so_and_static_probe(self):
        probe_bin = pkg.F6_BUILD / 'marble_modem_probe'
        ril_so = pkg.F6_BUILD / 'libril_vendor_qmi.z.so'
        self.assertTrue(probe_bin.is_file(), f'Missing F6 static probe: {probe_bin}')
        self.assertTrue(ril_so.is_file(), f'Missing F6 OHOS RIL .so: {ril_so}')

        dyn = subprocess.check_output([str(LLVM_READELF), '-d', str(ril_so)], text=True)
        needed = re.findall(r'\(NEEDED\)\s+Shared library:\s+\[([^\]]+)\]', dyn)
        soname = re.findall(r'\(SONAME\)\s+Library soname:\s+\[([^\]]+)\]', dyn)
        self.assertEqual(needed, ['libc.so'])
        self.assertEqual(soname, ['libril_vendor.z.so'])

        syms = subprocess.check_output([str(LLVM_NM), '-D', '--defined-only', str(ril_so)], text=True)
        self.assertIn('RilInitOps', syms)

        finfo = subprocess.check_output(['file', str(probe_bin)], text=True)
        self.assertIn('ARM aarch64', finfo)
        self.assertIn('statically linked', finfo)
        self.assertNotIn(b'ARM64 Bionic', probe_bin.read_bytes())

    def test_f6_script_and_postboot_contracts(self):
        self.assertTrue(F6_SH.is_file())
        self.assertTrue(POSTBOOT_SH.is_file())
        subprocess.run(['sh', '-n', str(F6_SH)], check=True)
        subprocess.run(['sh', '-n', str(POSTBOOT_SH)], check=True)

        f6_txt = F6_SH.read_text(encoding='utf-8')
        self.assertNotIn('unbind', f6_txt)
        self.assertNotIn('/bind', f6_txt)
        self.assertIn('IPA driver is now in ready state', f6_txt)
        self.assertIn('rmnet_ipa0', f6_txt)

        order = [
            'ipa_fmwk.ko',
            'gsim.ko',
            'ipam.ko',
            'rndisipam.ko',
            'ipa_clientsm.ko',
            'ipanetm.ko',
            'rmnet_ctl.ko',
            'rmnet_core.ko',
        ]
        positions = [f6_txt.index(k) for k in order]
        self.assertEqual(positions, sorted(positions), '8 IPA/RMNET KOs must load in exact dependency order')
        self.assertLess(f6_txt.index('do_ipa'), f6_txt.index('do_online'))
        self.assertLess(f6_txt.index('do_ipa ||'), f6_txt.index('--set-mode online'))
        for flag in ('--ipa', '--online', '--telephony', '--all', '--status'):
            self.assertIn(flag, f6_txt)

        post_txt = POSTBOOT_SH.read_text(encoding='utf-8')
        self.assertIn('start-modem-f6-marble.sh --all', post_txt)
        self.assertIn('skipped cellular', post_txt)
        self.assertNotIn('--check', post_txt)
        self.assertNotIn('--set-mode online', post_txt)

    def test_f6_package_report_and_guards(self):
        self.assertGreaterEqual(len(pkg.GUARD_MEMBERS), 35)
        report_path = pkg.F6_BUILD / 'f6-build-report.json'
        if not report_path.is_file():
            self.skipTest('f6-build-report.json not generated yet')
        report = json.loads(report_path.read_text(encoding='utf-8'))
        cand = Path(report['candidate']['path'])
        if not cand.is_file():
            self.skipTest(f'Candidate image missing: {cand}')

        self.assertEqual(report['base']['sha256'], pkg.BASE_SHA)
        self.assertGreaterEqual(len(report['guard_members_verified']), 35)
        archive, r = pkg.load_oracle()
        img = r.read_original(cand)
        layout = r.layout(img)
        avb = r.verify_existing_avb(img, layout['payload_end'])
        self.assertIn('footer_masked', avb)
        with tempfile.TemporaryDirectory() as td:
            got, *_ = r.extract_entries(cand, img, layout, Path(td))
            self.assertIn(pkg.PREFIX + 'graphics/start-modem-f6-marble.sh', got)
            stage_bytes = got[pkg.STAGE_MEMBER][1]
            self.assertIn(b'MARBLE_F6_CELLULAR_NAS_STAGE_BEGIN', stage_bytes)
            subprocess.run(['sh', '-n'], input=stage_bytes, check=True)
            start_txt = got[pkg.START_MEMBER][1].decode('utf-8')
            self.assertIn('VENUS_EARLY_READY', start_txt)
            self.assertLess(start_txt.index('VENUS_EARLY_READY'), start_txt.index('# BEGIN marble native boot animation'))
            subprocess.run(['sh', '-n'], input=start_txt.encode('utf-8'), check=True)
            import io, tarfile
            with tarfile.open(fileobj=io.BytesIO(got[pkg.PREFIX + 'graphics/zink-runtime-marble.tar.gz'][1]), mode='r:gz') as tf:
                gal = tf.extractfile(tf.getmember('libgallium-25.0.1.so')).read()
                self.assertIn(b'ZINK_MSAA_EXPAND', gal)
                self.assertIn(b'ZINK_MSAA_EXPAND_CAP', gal)
            manifest = archive.parse(got[r.MANIFEST][1])
            r.manifest_holds(got, manifest)

    def test_track_a_bootanim_and_graphics_fixes(self):
        omx_h = (ROOT / 'compat/codec/omx_il_minimal.h').read_text(encoding='utf-8')
        self.assertRegex(omx_h, r'OMX_IndexConfigCommonInputCrop\s*=\s*0x0700000E')
        self.assertRegex(omx_h, r'OMX_IndexConfigCommonOutputCrop\s*=\s*0x0700000F')
        omx_c = (ROOT / 'compat/codec/omx_core_venus.c').read_text(encoding='utf-8')
        self.assertIn('VENUS_DEV_DECODER', omx_c)
        self.assertIn('VENUS_DEV_ENCODER', omx_c)

        comp_cpp = (ROOT / 'compat/display/composer_vdi.cpp').read_text(encoding='utf-8')
        self.assertIn('candidate.vrefresh < 60', comp_cpp)
        cm_section = comp_cpp.split('int ChangeMode(', 1)[1].split('#define DEVICE_LOCK', 1)[0]
        self.assertIn('PauseVsyncForModeset();', cm_section)
        self.assertIn('ResumeVsyncAfterModeset();', cm_section)

        turnip_patch = (ROOT / 'compat/graphics/mesa-turnip.patch').read_text(encoding='utf-8')
        self.assertIn('kgsl_syncobj_ts_to_fd(&ret)', turnip_patch)
        self.assertIn('if (num_fds && queue != NULL)', turnip_patch)

        mesa_src = ROOT / 'work/mesa-turnip-src'
        if mesa_src.is_dir():
            with tempfile.TemporaryDirectory(prefix='f6_turnip_apply_') as td:
                tp = Path(td)
                p1 = subprocess.Popen(['git', '-C', str(mesa_src), 'archive', 'HEAD'], stdout=subprocess.PIPE)
                subprocess.run(['tar', '-x', '-C', str(tp)], stdin=p1.stdout, check=True)
                p1.wait()
                self.assertEqual(p1.returncode, 0)
                subprocess.run(['git', '-C', str(tp), 'init', '-q', '.'], check=True)
                subprocess.run(['git', '-C', str(tp), 'add', '-A'], check=True)
                subprocess.run(['git', '-C', str(tp), '-c', 'user.email=a@b', '-c', 'user.name=a', 'commit', '-qm', 'up'], check=True)
                subprocess.run([sys.executable, str(ROOT / 'compat/graphics/apply_mesa.py'), '--source', str(tp), '--profile', 'turnip'], check=True)


if __name__ == '__main__':
    unittest.main()
