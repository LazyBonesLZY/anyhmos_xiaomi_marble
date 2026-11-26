#!/usr/bin/env python3
"""Offline unit tests and artifact checks for Batch U1 (Soft-HUKS, HDR fix, OEM/CPU/version, on-demand SAs, Live Window)."""
import importlib.util
import io
import json
import subprocess
import tarfile
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
U1_BUILD = ROOT / 'work/marble-u1-build'
INIT_U1 = ROOT / 'devices/marble/runtime/init/init-u1-services-marble.cfg'
PACKAGER = ROOT / 'tools/packaging/build_marble_h20_u1_ux_security.py'
FLASH_CTL = ROOT / 'work/marble-dual-20261123/flash-h20-u1-control.sh'
BASE_F5_IMG = Path('/mnt/e/marble-tmp/vendor_boot_h20_f5_cellular_gnss_34b842d0.img')

EXPECTED_SERVICES = (
    'huks_service',
    'cert_manager_service',
    'security_privacy_server',
    'security_component_service',
    'super_privacy_server',
    'sandbox_manager_service',
    'dlp_permission_service',
    'cloudinterfaceauth',
    'appgallery_service',
    'push_manager_service',
    'video_processing_service',
    'custom_config_service',
    'drm_service',
    'dlp_hide_service_sa',
    'hmos_fusion_manager',
)


class TestBatchU1(unittest.TestCase):
    def test_optional_sa_source_and_binary_symbols(self):
        src = (ROOT / 'compat/framework/fallbacks/sceneboard_optional_sa.cpp').read_text(encoding='utf-8')
        self.assertIn('hmos_optional_load_callback', src)
        cb_body = src[src.index('hmos_optional_load_callback('):]
        self.assertIn('hmos_optional_check(self, id, &exists, &error)', cb_body)
        self.assertIn('if (!existing.pointer)', cb_body)

        so_path = U1_BUILD / 'libhmos_sceneboard_optional_sa.so'
        self.assertTrue(so_path.is_file(), f'Missing rebuilt shared library: {so_path}')
        nm_tool = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-nm'
        if nm_tool.is_file():
            syms = subprocess.check_output([str(nm_tool), '-D', '--defined-only', str(so_path)], text=True)
            for sym in (
                '_ZN4OHOS25SystemAbilityManagerProxy18CheckSystemAbilityEiRbRi',
                '_ZN4OHOS25SystemAbilityManagerProxy17LoadSystemAbilityEii',
                '_ZN4OHOS25SystemAbilityManagerProxy17LoadSystemAbilityEiRKNS_4sptrINS_26ISystemAbilityLoadCallbackEEE',
            ):
                self.assertIn(sym, syms)

    def test_init_u1_services_cfg_contract(self):
        obj = json.loads(INIT_U1.read_text(encoding='utf-8'))
        services = {s['name']: s for s in obj['services']}
        self.assertEqual(set(services), set(EXPECTED_SERVICES))

        huks = services['huks_service']
        self.assertEqual(huks['uid'], 'huks_server')
        self.assertEqual(huks['gid'], ['huks_server'])
        self.assertEqual(huks['bootevents'], ['bootevent.huksService.ready'])
        self.assertNotIn('once', huks)
        self.assertNotIn('disabled', huks)

        for name in EXPECTED_SERVICES:
            s = services[name]
            self.assertNotIn('once', s, f'{name} must not have once=1')
            self.assertNotIn('disabled', s, f'{name} must not have disabled=1')
            self.assertEqual(s['sandbox'], 0, f'{name} missing sandbox=0')
            self.assertEqual(s['saspawn'], False, f'{name} missing saspawn=false')
            if name not in ('huks_service', 'push_manager_service'):
                self.assertTrue(s.get('ondemand'), f'{name} must have ondemand=true')

        push = services['push_manager_service']
        self.assertEqual(push['uid'], 'push')
        self.assertEqual(push['gid'], ['push', 'netsys_socket', 'vibrator_host'])
        self.assertIs(push['ondemand'], False)
        self.assertEqual(len(push['permission']), 49)
        self.assertEqual(len(set(push['permission'])), 49)
        self.assertEqual(len(push['permission_acls']), 9)

        jobs = {j['name']: j for j in obj['jobs']}
        self.assertIn('mkdir /data/service/el1/public/huks_soft 0700 huks_server huks_server', jobs['param:marble-u1-huks']['cmds'])
        self.assertEqual(jobs['param:marble-u1-hwid']['cmds'], ['start huawei_id_svc', 'start push_manager_service'])
        self.assertIn('mkdir /data/service/el1/public/push/push_manager_service/files/aod 0700 push push', jobs['param:marble-u1-dirs']['cmds'])
        self.assertIn('chown cert_manager_server cert_manager_server /data/service/el1/public/cert_manager_service', jobs['param:marble-u1-dirs']['cmds'])
        self.assertIn('mkdir /data/service/el1/public/dlp_hide_service 0710 dlp_hide dlp_credential', jobs['param:marble-u1-dirs']['cmds'])
        self.assertIn('mkdir /data/service/el1/public/dlp_hide_service 0710 dlp_hide dlp_credential', jobs['services:dlp_hide_service_sa']['cmds'])
        self.assertIn('mkdir /data/service/el1/public/hmos_fusion_manager 0711 hmos_fusion_manager hmos_fusion_manager', jobs['param:marble-u1-dirs']['cmds'])
        self.assertIn('mkdir /data/service/el1/public/hmos_fusion_manager 0711 hmos_fusion_manager hmos_fusion_manager', jobs['services:hmos_fusion_manager']['cmds'])

    def test_cust_version_tarball_contents(self):
        tb = U1_BUILD / 'u1-cust-version.tar.gz'
        self.assertTrue(tb.is_file(), f'Missing tarball: {tb}')
        with tarfile.open(tb, 'r:gz') as tf:
            names = set(tf.getnames())
            for req in (
                'version/etc/param/meta.para',
                'version/etc/component_version.txt',
                'cust/etc/param/meta.para',
                'cust/hw/cn/etc/param/hw_defaults.para',
            ):
                self.assertIn(req, names, f'{req} missing from tarball')
            vmeta = tf.extractfile('version/etc/param/meta.para').read().decode('utf-8')
            cmeta = tf.extractfile('cust/etc/param/meta.para').read().decode('utf-8')
            self.assertIn('const.comp.hl.product_cust_version=ADY-AL10-CUST 7.0.0.111(C00)', vmeta)
            self.assertIn('const.comp.cust_version=Cust-CHN 207.0.0.1(0001)', cmeta)

    def test_packager_transforms_and_audio_safety(self):
        spec = importlib.util.spec_from_file_location('build_u1', PACKAGER)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)

        # Synthetic shell check on STAGE_U1_BLOCK
        sample_stage = b'#!/bin/sh\nset -eu\nprintf \'%s\\n\' \'' + mod.OPTIONAL_SA_F5 + b'\'\n' + mod.STAGE_ANCHOR + b'\n'
        patched_stage = mod.patch_stage(sample_stage)
        self.assertIn(b'MARBLE_U1_STAGE_READY', patched_stage)
        self.assertIn(b'libhuks_ext_hdi_service_1.1.z.so', patched_stage)
        self.assertIn(b'libdevice_certificate_manager_interfaces_service_1.2.z.so', patched_stage)
        self.assertIn(b'53484b31', patched_stage)
        self.assertIn(b'el2/*/huks_service', patched_stage)
        self.assertIn(b'1767225600', patched_stage)
        self.assertIn(b'1795478400', patched_stage)
        self.assertIn(b'.hmos-dbkeys/$u1_u', patched_stage)
        self.assertIn(b'.u15_crypto_db_reset_v2', patched_stage)
        self.assertIn(b'gen_natural_store.db', patched_stage)
        self.assertIn(b'SecretKey###%', patched_stage)
        self.assertIn(b'AccountDataShare.db*', patched_stage)
        self.assertIn(b'AccountDataShare.pub_key_v2', patched_stage)
        self.assertIn(b'AccountSecretData.db*', patched_stage)
        self.assertIn(b'AccountSecretData.pub_key_v2', patched_stage)
        self.assertIn(b'system_data_el2_100.db*', patched_stage)
        self.assertIn(b'.hmos-dbkeys/7023', patched_stage)
        self.assertIn(b'.u17_nativetoken_refresh_v3', patched_stage)
        self.assertIn(b'.u16_location_kv_v1', patched_stage)
        self.assertIn(b'.u17_ota_block_v1', patched_stage)
        self.assertIn(b'MARBLE_U1_OTA_BLOCK_BEGIN', patched_stage)
        self.assertIn(b'update-drcn.platform.hicloud.com', patched_stage)
        self.assertIn(b'query.hicloud.com', patched_stage)
        self.assertIn(b'hw_new_system_update', patched_stage)
        self.assertIn(b'nativetoken.json', patched_stage)
        self.assertIn(b'libuser_status_service.z.so', patched_stage)
        self.assertIn(b'2907', patched_stage)
        self.assertIn(b'msdp.json', patched_stage)
        self.assertIn(b'is_ota_finished', patched_stage)
        self.assertIn(b'wal_checkpoint(TRUNCATE)', patched_stage)
        self.assertIn(b'-dwr', patched_stage)
        self.assertIn(b'hosts_user/hosts', patched_stage)
        self.assertIn(b'/sys_prod/variant/hw_oem/ADY-AL10', patched_stage)
        self.assertIn(b'/chip_prod/variant/special_cust/ADY-AL10/all/cn', patched_stage)
        for skipped_sa in (b'3006', b'65866', b'65920', b'65930', b'66406', b'70634'):
            self.assertIn(skipped_sa, mod.OPTIONAL_SA_U1.split())
        for enabled_sa in (b'3203', b'3506', b'65728', b'65974', b'65993', b'66134', b'66464'):
            self.assertNotIn(enabled_sa, mod.OPTIONAL_SA_U1.split())
        with self.assertRaises(ValueError):
            mod.patch_stage(patched_stage)

        # Verify soft-HUKS IDL struct ABI offsets (data @ 0, dataLen @ 8), KeyMaterial/ParamSet tags, and bundle CLI DroiTong allowlist
        huks_hdr = (ROOT / 'compat/huks/soft_huks.h').read_text(encoding='utf-8')
        self.assertIn('struct HuksBlob {\n    uint8_t *data;\n    uint32_t dataLen;\n};', huks_hdr)
        self.assertIn('struct HuksParamSet {\n    uint8_t *data;\n    uint32_t dataLen;\n};', huks_hdr)
        self.assertIn('HKS_TAG_KEY_VERSION', huks_hdr)
        self.assertIn('struct KeyMaterialRsa', huks_hdr)
        self.assertIn('struct KeyMaterialEcc', huks_hdr)
        cli_src = (ROOT / 'compat/services/native_bundle_cli.c').read_text(encoding='utf-8')
        self.assertIn('install-region', cli_src)
        self.assertIn('"DroiTong"', cli_src)

        # User-0 appspawn sandbox keydir fix + rebuilt libdbkeys.so exports
        db_key_src = (ROOT / 'compat/framework/database/db_key.c').read_text(encoding='utf-8')
        self.assertIn('access("/data/storage/el1/base",W_OK|X_OK)==0', db_key_src)
        dbk_so = U1_BUILD / 'libdbkeys.so'
        self.assertTrue(dbk_so.is_file(), f'Missing rebuilt libdbkeys.so: {dbk_so}')
        nm_tool = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-nm'
        if nm_tool.is_file():
            dbk_syms = subprocess.check_output([str(nm_tool), '-D', '--defined-only', str(dbk_so)], text=True)
            self.assertIn('SksGenerateKey', dbk_syms)

        # Nighttime audio gain untouched
        self.assertIn('0.03f', (ROOT / 'compat/audio/audio_vdi.c').read_text(encoding='utf-8'))
        self.assertIn('0.03f', (ROOT / 'compat/audio/pal_server.c').read_text(encoding='utf-8'))

        # Verify built U1 image & report if present
        report_path = U1_BUILD / 'u1-build-report.json'
        if report_path.is_file() and BASE_F5_IMG.is_file():
            rep = json.loads(report_path.read_text(encoding='utf-8'))
            self.assertEqual(rep['base']['sha256'], mod.BASE_SHA)
            self.assertEqual(len(rep['guard_members_verified']), 13)
            cand_path = Path(rep['candidate']['path'])
            self.assertTrue(cand_path.is_file())

            oracle_spec = importlib.util.spec_from_file_location('arc_u1', mod.ORACLE)
            arc = importlib.util.module_from_spec(oracle_spec)
            oracle_spec.loader.exec_module(arc)
            r = arc.load_tools()
            img_bytes = r.read_original(cand_path)
            lay = r.layout(img_bytes)
            with tempfile.TemporaryDirectory(prefix='u1_inspect_') as tmp:
                got, _, _, _, _ = r.extract_entries(cand_path, img_bytes, lay, Path(tmp))
                dbg = json.loads(got[mod.INIT_DEBUG_MEMBER][1].decode('utf-8'))
                self.assertIn('/compat/etc/init-u1-services-marble.cfg', dbg['import'])
                priv = next(s for s in dbg['services'] if s['name'] == 'privacy_service')
                self.assertIn('ohos.permission.GET_SENSITIVE_PERMISSIONS', priv['permission'])
                self.assertIn('ohos.permission.ACCESS_SECURITY_PRIVACY_CENTER', priv['permission'])
                self.assertEqual(len(priv['permission']), 21)
                self.assertEqual(len(set(priv['permission'])), 21)
                self.assertEqual(len(priv.get('permission_acls', [])), 6)

                b2 = json.loads(got[mod.INIT_B2_MEMBER][1].decode('utf-8'))
                hwid = next(s for s in b2['services'] if s['name'] == 'huawei_id_svc')
                self.assertNotIn('once', hwid)
                self.assertNotIn('disabled', hwid)
                self.assertNotIn('ondemand', hwid)

                desk = json.loads(got[mod.INIT_DESKTOP_MEMBER][1].decode('utf-8'))
                thm = next(s for s in desk['services'] if s['name'] == 'theme_manager_service')
                self.assertNotIn('once', thm)
                self.assertNotIn('disabled', thm)
                self.assertTrue(thm.get('ondemand'))
                inp = next(s for s in desk['services'] if s['name'] == 'marble_input_dispatch')
                self.assertIn('ohos.permission.START_ABILITIES_FROM_BACKGROUND', inp.get('permission', []))

                stage_txt = got[mod.STAGE_MEMBER][1].decode('utf-8')
                subprocess.run(['sh', '-n'], input=stage_txt.encode('utf-8'), check=True)
                self.assertIn('const.display.hdr_picture_support=false', stage_txt)
                self.assertIn('const.settings.processor_info_show=true', stage_txt)
                self.assertIn('const.settings.processor_info_display=Qualcomm Snapdragon 7+ Gen 2', stage_txt)

                sp4_txt = got[mod.START_PHASE4_MEMBER][1].decode('utf-8')
                subprocess.run(['sh', '-n'], input=sp4_txt.encode('utf-8'), check=True)
                self.assertIn('pset marble.debug.start.huks true', sp4_txt)
                self.assertIn('pset marble.debug.start.hwid true', sp4_txt)

                # Zero duplicate service names across all CFGs in U1 ramdisk
                seen = set()
                for k, (_fields, blob) in got.items():
                    if k.endswith('.cfg'):
                        c = json.loads(blob.decode('utf-8'))
                        for s in c.get('services', []):
                            self.assertNotIn(s['name'], seen, f"Duplicate service {s['name']} in {k}")
                            seen.add(s['name'])
                for name in EXPECTED_SERVICES:
                    self.assertIn(name, seen)

                for req in (
                    'hmos-compat-marble/graphics/libdbkeys.so',
                    'hmos-compat-marble/graphics/libhuks_hdi_service_1.1.z.so',
                    'hmos-compat-marble/graphics/libhuks_engine_core_standard.z.so',
                    'hmos-compat-marble/graphics/tee_backend_probe',
                    'hmos-compat-marble/graphics/native_bundle_cli',
                    'hmos-compat-marble/graphics/init-u1-services-marble.cfg',
                    'hmos-compat-marble/graphics/u1-cust-version.tar.gz',
                ):
                    self.assertIn(req, got)
                self.assertEqual(
                    got['hmos-compat-marble/graphics/libdbkeys.so'][1],
                    (U1_BUILD / 'libdbkeys.so').read_bytes(),
                )

            if FLASH_CTL.is_file():
                subprocess.run(['bash', '-n', str(FLASH_CTL)], check=True)
                ctl_txt = FLASH_CTL.read_text(encoding='utf-8')
                self.assertIn(cand_path.name, ctl_txt)
                self.assertIn(rep['candidate']['sha256'], ctl_txt)


if __name__ == '__main__':
    unittest.main()
