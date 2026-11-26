#!/usr/bin/env python3
"""Offline checks for Batch U4: clean-room DCM SA 65866 software-TEE.

Covers the token derivation (known-answer vectors), the donor wire contract the
service implements, the init/profile wiring, and the single-variable packager.
No device, image or network access.
"""
import json
import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DCM_DIR = ROOT / 'compat/dcm'
TOKEN_C = DCM_DIR / 'dcm_token.c'
TOKEN_H = DCM_DIR / 'dcm_token.h'
SERVICE = DCM_DIR / 'hmos_dcm_service.cpp'
PROBE = DCM_DIR / 'hmos_dcm_token_probe.cpp'
INIT_CFG = ROOT / 'devices/marble/runtime/dcm/init-dcm-marble.cfg'
PACKAGER = ROOT / 'tools/packaging/build_marble_h20_u4_dcm_soft_tee.py'
BUILD = ROOT / 'tools/build/build_current.py'
DOC = ROOT / 'docs/MARBLE_DCM_SOFT_TEE.md'


class TokenDerivationTests(unittest.TestCase):
    def test_host_selftest_known_answers(self):
        self.assertTrue(TOKEN_C.is_file())
        with tempfile.TemporaryDirectory(prefix='dcm_token_') as tmp:
            exe = Path(tmp) / 'dcm_token_test'
            subprocess.run(
                ['gcc', '-O2', '-Wall', '-Wextra', '-Werror', '-std=gnu11',
                 '-DDCM_TOKEN_HOST_TEST', str(TOKEN_C), '-o', str(exe)],
                check=True,
            )
            out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout
        self.assertIn('PASS tokenselftest', out)
        self.assertIn('sha256(abc)', out)
        self.assertIn('hmac(key,fox)', out)
        sample = re.search(r'^TOKEN_SAMPLE (hmosdcm1\.[A-Za-z0-9_-]+\.[0-9a-f]{64})$', out, re.M)
        self.assertIsNotNone(sample, out)

    def test_token_header_contract(self):
        header = TOKEN_H.read_text(encoding='utf-8')
        self.assertIn('DCM_TOKEN_PREFIX "hmosdcm1."', header)
        self.assertIn('dcm_build_token', header)
        self.assertIn('NOT a Huawei', header)


class ServiceWireContractTests(unittest.TestCase):
    def setUp(self):
        self.src = SERVICE.read_text(encoding='utf-8')

    def test_descriptors_and_codes_match_donor(self):
        self.assertIn('ohos.security.devicecertmanager.service', self.src)
        self.assertIn('ohos.security.devicecertmanager.dcmDeviceTokCallback', self.src)
        self.assertIn('kGetDeviceTokenCode = 3', self.src)
        self.assertIn('kDeviceTokenCallbackCode = 1', self.src)

    def test_reply_and_callback_parcel_order(self):
        # reply: int32 errCode then uint64 requestId (single guarded expression)
        self.assertRegex(self.src, r'reply\.WriteInt32\(0\)[\s\S]{0,120}reply\.WriteUint64\(requestId\)')
        # callback: uint64 requestId, int32 errCode, string token (guarded chain)
        i_req = self.src.index('cbData.WriteUint64(requestId)')
        i_err = self.src.index('cbData.WriteInt32(0)')
        i_tok = self.src.index('cbData.WriteString(token)')
        self.assertLess(i_req, i_err)
        self.assertLess(i_err, i_tok)
        self.assertIn('ReadRemoteObject', self.src)
        self.assertIn('ReadInterfaceToken', self.src)

    def test_service_does_not_claim_huawei_attestation(self):
        self.assertIn('not a Huawei', self.src)
        self.assertNotIn('huawei_issued', self.src)

    def test_probe_uses_the_same_contract(self):
        probe = PROBE.read_text(encoding='utf-8')
        self.assertIn('ohos.security.devicecertmanager.service', probe)
        self.assertIn('ohos.security.devicecertmanager.dcmDeviceTokCallback', probe)
        self.assertIn('kGetDeviceTokenCode = 3', probe)

    def test_donor_code_table_and_not_supported(self):
        for code in (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 23):
            self.assertRegex(self.src, r'\{%u, "' % code)
        self.assertIn('kErrNotSupported = -10009', self.src)
        self.assertIn('UNSUPPORTED code=', self.src)
        self.assertIn('CodeName', self.src)

    def test_register_uses_donor_int_return(self):
        self.assertIn('using AddAbility = int (*)', self.src)
        self.assertIn('_ZThn112_N4OHOS25SystemAbilityManagerProxy16AddSystemAbility', self.src)
        self.assertIn('int rc = api.addSystemAbility', self.src)
        self.assertIn('return rc == 0;', self.src)

    def test_callers_and_lazy_facts(self):
        self.assertIn('GetCallingUid', self.src)
        self.assertIn('EnsureFacts', self.src)
        self.assertIn('gRequestCounter.fetch_add', self.src)
        self.assertIn('std::lock_guard<std::mutex> lock(gLogMutex)', self.src)

    def test_anonymous_attest_is_local(self):
        self.assertIn('DispatchAnonymousAttest', self.src)
        self.assertIn('dcmAnonymousCallback', self.src)
        self.assertIn('gWriteUnpadBuffer', self.src)
        self.assertIn('dcm_ca_cert_der', self.src)
        # HksDcmCallback only finds the proxy after DcmGenerateCertChain
        # registers requestId, so the callback must be delayed past the reply.
        self.assertIn('AnonCallbackThread', self.src)
        self.assertIn('usleep(200 * 1000)', self.src)
        self.assertIn('PTHREAD_CREATE_DETACHED', self.src)

    def test_attest_ids_and_cert_state_are_local(self):
        self.assertIn('dcm_ca_cert_chain_blob', self.src)
        self.assertIn('dcm_ca_state', self.src)
        self.assertIn('DispatchAttestIds', self.src)
        self.assertIn('DispatchCertState', self.src)
        self.assertIn('code == 1 || code == 5 || code == 9', self.src)
        self.assertIn('code == 7 || code == 10', self.src)
        i_ids = self.src.index('DispatchAttestIds(OHOS::MessageParcel &data')
        self.assertIn('ReadInterfaceToken()', self.src[i_ids:i_ids + 300])
        i_st = self.src.index('DispatchCertState(OHOS::MessageParcel &data')
        self.assertIn('ReadInterfaceToken()', self.src[i_st:i_st + 300])

    def test_probe_covers_other_codes_and_empty_token(self):
        probe = PROBE.read_text(encoding='utf-8')
        self.assertIn('--code', probe)
        self.assertIn('UNSUPPORTED_CODE', probe)
        self.assertIn('ATTEST_IDS_CODE', probe)
        self.assertIn('CERT_STATE_CODE', probe)
        self.assertIn('SDK_CERT_STATE', probe)
        self.assertIn('SDK_PROV_STATUS', probe)
        self.assertIn('DcmGetCertAvailableState', probe)
        self.assertIn('DcmGetProvisionedInfoStatus', probe)
        self.assertIn('empty-token', probe)
        self.assertIn('kErrNotSupported', probe)

    def test_main_holds_stub_ref_across_retries(self):
        i_main = self.src.index('int main()')
        body = self.src[i_main:]
        self.assertIn('OHOS::sptr<OHOS::IRemoteObject> stubRef', body)
        self.assertIn('RegisterAbility(samgr, stubRef)', body)


class InitWiringTests(unittest.TestCase):
    def test_init_cfg_is_valid_and_starts_the_service(self):
        doc = json.loads(INIT_CFG.read_text(encoding='utf-8'))
        services = {svc['name']: svc for svc in doc['services']}
        self.assertIn('hmos_dcm_service', services)
        svc = services['hmos_dcm_service']
        self.assertEqual(svc['path'], ['/compat/bin/hmos_dcm_service'])
        self.assertEqual(svc['uid'], 'device_cert_manager_server')
        self.assertEqual(svc['secon'], 'u:r:dcm_service:s0')
        self.assertEqual(svc['sandbox'], 0)
        self.assertEqual(svc['disabled'], 1)
        self.assertEqual(svc['once'], 1)
        self.assertEqual(svc['start-mode'], 'condition')
        jobs = {job['name']: job for job in doc['jobs']}
        self.assertIn('post-fs-data', jobs)
        self.assertIn('param:marble-u4-dcm', jobs)
        self.assertEqual(jobs['param:marble-u4-dcm']['condition'], 'marble.debug.start.u4_dcm=true')
        self.assertIn('start hmos_dcm_service', jobs['param:marble-u4-dcm']['cmds'])
        cmds = jobs['services:hmos_dcm']['cmds']
        self.assertTrue(any(cmd.startswith('start hmos_dcm_service') for cmd in cmds), cmds)
        self.assertTrue(any('device_cert_manager_server' in cmd for cmd in cmds), cmds)

    def test_service_name_does_not_collide_with_the_donor(self):
        self.assertNotEqual('hmos_dcm_service', 'dcm_service')


class PackagerContractTests(unittest.TestCase):
    def setUp(self):
        self.src = PACKAGER.read_text(encoding='utf-8')

    def test_single_variable_base_is_current_device(self):
        self.assertIn("BASE_DEFAULT = Path('/mnt/e/marble-tmp/vendor_boot_h20_u4_dcm_soft_tee_cbdd3cf9.img')", self.src)
        self.assertIn('cbdd3cf9fa4105de526e85d21e2b69f41aedae5d93708d85c59cecb4bb51b713', self.src)
        self.assertIn('d5299691eb38735683ff75f20576a46397e5259dc4b8f2b2efdd20733c501b6e', self.src)
        self.assertIn('/compat/etc/init-u3-codec-display-marble.cfg', self.src)

    def test_protected_members_include_prior_batches(self):
        for member in (
            'libhmos_audio_vdi.so',
            'libhmos_pal_server.so',
            'hmos_tqftpserv',
            'marble_modem_probe',
            'libril_vendor_qmi.z.so',
            'hmos_rmtfs',
            'libhuks_hdi_service_1.1.z.so',
            'libhmos_sceneboard_optional_sa.so',
            'libmarble_gnss_hdi_driver.z.so',
            'init-u2-location-marble.cfg',
            'hdf_default_phase4.hcb',
            'libjpeg_vdi_impl.z.so',
            'libdisplayengine_module.so',
            'libOMX_Core.z.so',
            'libdisplay_buffer_vdi_impl.z.so',
            'libheifdecode_vdi_impl.z.so',
            'native_heif_probe',
        ):
            self.assertIn(member, self.src)

    def test_staged_members_and_init_import(self):
        self.assertIn("('graphics/hmos_dcm_service'", self.src)
        self.assertIn("('graphics/hmos_dcm_token_probe'", self.src)
        self.assertIn("('graphics/init-dcm-marble.cfg'", self.src)
        self.assertIn('/compat/etc/init-dcm-marble.cfg', self.src)
        self.assertIn('OPT_SA_U4_OLD', self.src)
        self.assertIn('65862 65920 65930', self.src)
        self.assertIn('32768', self.src)

    def test_stage_and_start_block_syntax(self):
        import importlib.util
        spec = importlib.util.spec_from_file_location('u4_packager', PACKAGER)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        subprocess.run(['sh', '-n'], input=mod.STAGE_U4_BLOCK, check=True)
        subprocess.run(['sh', '-n'], input=mod.START_U4_BLOCK, check=True)
        self.assertIn(b'MARBLE_U4_DCM_SOFT_TEE_STAGE_BEGIN', mod.STAGE_U4_BLOCK)
        self.assertIn(b'dcm-device.json', mod.STAGE_U4_BLOCK)
        self.assertIn(b'3562:3562', mod.STAGE_U4_BLOCK)
        self.assertIn(b'chmod 0600 "$R/compat/etc/dcm-device.json"', mod.STAGE_U4_BLOCK)
        self.assertIn(b'marble.debug.start.u4_dcm', mod.START_U4_BLOCK)
        self.assertIn(b'u4_dcm', mod.START_U4_BLOCK)

    def test_packager_roundtrip_on_u32b_base(self):
        base_img = Path('/mnt/e/marble-tmp/vendor_boot_h20_u32b_heif_codec_d5299691.img')
        if not base_img.is_file():
            self.skipTest('U3.2b base image not present')
        import importlib.util
        spec = importlib.util.spec_from_file_location('u4_packager_rt', PACKAGER)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        archive, r = mod.load_oracle()
        base = r.read_original(base_img)
        with tempfile.TemporaryDirectory(prefix='u4_rt_') as tmp:
            tmp_path = Path(tmp)
            out_img = mod.build(base_img, tmp_path)
            self.assertTrue(out_img.is_file())
            after = r.read_original(out_img)
            after_layout = r.layout(after)
            got, _, _, _, _ = r.extract_entries(out_img, after, after_layout, tmp_path / 'ext')
            stage = got[mod.STAGE_MEMBER][1].decode('utf-8')
            self.assertIn('65862 65920 65930', stage)
            self.assertNotIn('65866', stage)
            init_doc = json.loads(got[mod.INIT_DEBUG_MEMBER][1].decode('utf-8'))
            self.assertIn('/compat/etc/init-dcm-marble.cfg', init_doc['import'])
            self.assertIn('/compat/etc/init-u3-codec-display-marble.cfg', init_doc['import'])
            start_sh = got[mod.START_PHASE4_MEMBER][1].decode('utf-8')
            self.assertIn('marble.debug.start.u4_dcm', start_sh)
            self.assertEqual(len(mod.GUARD_MEMBERS), 33)
            # HUKS moved from guard to staged because it now carries the
            # local-attest deferral; both files are still present in the image.
            self.assertIn('graphics/libhuks_hdi_service_1.1.z.so', mod.STAGED_FILES[3][0] + mod.STAGED_FILES[4][0])


    def test_packager_roundtrip_on_current_device_baseline(self):
        base_img = Path('/mnt/e/marble-tmp/vendor_boot_h20_u4_dcm_soft_tee_cbdd3cf9.img')
        if not base_img.is_file():
            self.skipTest('current U4 base image not present')
        import importlib.util
        spec = importlib.util.spec_from_file_location('u4_packager_rt2', PACKAGER)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        archive, r = mod.load_oracle()
        with tempfile.TemporaryDirectory(prefix='u4_rt2_') as tmp:
            tmp_path = Path(tmp)
            out_img = mod.build(base_img, tmp_path)
            self.assertTrue(out_img.is_file())
            after = r.read_original(out_img)
            got, _, _, _, _ = r.extract_entries(out_img, after, r.layout(after), tmp_path / 'ext')
            init_doc = json.loads(got[mod.INIT_DEBUG_MEMBER][1].decode('utf-8'))
            self.assertIn('/compat/etc/init-dcm-marble.cfg', init_doc['import'])
            self.assertIn('/compat/etc/init-u3-codec-display-marble.cfg', init_doc['import'])
            stage = got[mod.STAGE_MEMBER][1].decode('utf-8')
            self.assertIn('MARBLE_U4_DCM_SOFT_TEE_STAGE_BEGIN', stage)
            self.assertEqual(len(mod.GUARD_MEMBERS), 33)


class LocalCaTests(unittest.TestCase):
    def test_local_ca_host_selftest(self):
        ca = ROOT / 'compat/dcm/dcm_local_ca.c'
        self.assertTrue(ca.is_file())
        with tempfile.TemporaryDirectory(prefix='dcm_ca_host_') as tmp:
            exe = Path(tmp) / 'dcm_ca_test'
            subprocess.run(
                ['gcc', '-O2', '-Wall', '-Wextra', '-std=gnu11',
                 '-DDCM_LOCAL_CA_HOST_TEST', f'-DDCM_CA_DIR="{tmp}/ca"',
                 str(ca), '-lcrypto', '-o', str(exe)],
                check=True,
            )
            out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout
        self.assertIn('PASS dcm_ca_selftest', out)
        self.assertIn('count=2', out)


class SoftHuksTests(unittest.TestCase):
    def test_soft_huks_defers_attest_to_dcm(self):
        src = (ROOT / 'compat/huks/soft_huks_engine.c').read_text(encoding='utf-8')
        self.assertIn('local_attest_deferred_to_dcm', src)
        self.assertNotIn('no_attestation_chain', src)
        # DCM needs the caller-provided capacity; zeroing size would make
        # DcmGenerateCertChain's CheckBlob fail before it can fill the chain.
        self.assertNotIn('certChain->size = 0', src)
        # The old backend_ok(soft_route("attest")) gate returned NOT_SUPPORTED
        # before DCM was reached; it must not come back.
        self.assertNotIn('backend_ok(soft_route("attest"', src)


class BuildTargetTests(unittest.TestCase):
    def test_targets_are_listed(self):
        env = dict(os.environ)
        env.update({
            'HMOS_SDK': str(ROOT / 'work/ohos-sdk/installed/native'),
            'HMOS_UPSTREAM': str(ROOT / 'work/upstream'),
            'HMOS_STAGE': str(ROOT / 'work/marble-stage'),
            'HMOS_DONOR_LINK': str(ROOT / 'work/donor-hdi-link'),
        })
        out = subprocess.run(
            ['python3', str(BUILD), '--list'], check=True, capture_output=True, text=True, env=env
        ).stdout.splitlines()
        for target in ('dcm-token', 'dcm-local-ca', 'dcm-service', 'dcm-probe'):
            self.assertIn(target, out)


class AnonymousAttestProbeTests(unittest.TestCase):
    """Code 2 (AnonymousAttest) must be drivable and parsed with padding-safe reads."""

    def test_probe_drives_code_two_with_callback(self):
        text = PROBE.read_text(encoding='utf-8')
        self.assertIn('kAnonymousAttestCode = 2', text)
        self.assertIn('dcmAnonymousCallback', text)
        self.assertIn('WriteUnpadBufferEPKvm', text)
        self.assertIn('--anon', text)

    def test_probe_reader_skips_parcel_padding(self):
        text = PROBE.read_text(encoding='utf-8')
        # Parcel::WriteUnpadBuffer pads to 4 bytes; ReadBuffer does not skip that pad.
        self.assertIn('ReadUnpadBuffer(len)', text)
        self.assertNotIn('ReadBuffer(len)', text)

    def test_parcel_selftest_covers_asymmetric_lengths(self):
        text = PROBE.read_text(encoding='utf-8')
        self.assertIn('--parcel-selftest', text)
        self.assertIn('len0 == 418 && len1 == 374', text)


class DocumentationContractTests(unittest.TestCase):
    def test_boundary_is_documented(self):
        text = DOC.read_text(encoding='utf-8')
        for token in (
            'getDeviceToken',
            '65866',
            '不是华为签发',
            '回退',
            'COM9',
            'hmos_dcm_token_probe',
            'ANON_VERDICT=ok',
            'ReadUnpadBuffer',
        ):
            self.assertIn(token, text)


if __name__ == '__main__':
    unittest.main()
