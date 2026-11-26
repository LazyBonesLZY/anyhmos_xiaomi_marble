"""Static + host-side checks for the marble (SM7475) OHOS codec path.

No device, no network, no flashing: these tests only read the local donor rootfs,
the local OHOS SDK, the checked-in dump evidence snapshot and the codec sources.
They exist to keep the *claims* in compat/codec/codec_inventory_marble.json tied to
artefacts that can be re-checked, and to keep the probe/runtime script honest.
"""
import json
import os
import pathlib
import re
import shutil
import subprocess
import tarfile
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
CODEC = ROOT / 'compat' / 'codec'
INVENTORY = CODEC / 'codec_inventory_marble.json'
DUMP_INDEX = CODEC / 'evidence' / 'marble-dump-index.txt'
MEDIA_CODECS = CODEC / 'evidence' / 'marble-media-codecs.json'
PROBE = CODEC / 'native_codec_probe.c'
PROBE_BUILD = CODEC / 'build-native-codec-probe.sh'
START = ROOT / 'devices' / 'marble' / 'runtime' / 'codec' / 'start-codec-marble.sh'
HARDWARE_CFG = ROOT / 'devices' / 'marble' / 'runtime' / 'init' / 'init-hardware-marble.cfg'
DONOR = ROOT / 'work' / 'donor-rom' / 'ADY-AL10-207.0.0.111-SP10' / 'fs'
SDK = ROOT / 'work' / 'ohos-sdk' / 'installed' / 'native'
SDK_INCLUDE = SDK / 'sysroot' / 'usr' / 'include' / 'multimedia' / 'player_framework'
KERNEL_MODULES_TAR = ROOT / 'work' / 'marble-kernel-artifacts' / 'vendor_dlkm_modules.tar.gz'
KERNEL_MODULE = ROOT / 'work' / 'marble-kernel-work' / 'vendor_dlkm_modules' / 'msm_video.ko'

STAGES = {'env', 'capability', 'media-sas', 'hdi-host', 'hardware', 'probe', 'complete'}


def load_inventory():
    return json.loads(INVENTORY.read_text(encoding='utf-8'))


def dump_evidence():
    lines = DUMP_INDEX.read_text(encoding='utf-8').splitlines()
    return {line.strip() for line in lines if line.strip() and not line.startswith('#')}


def script_code():
    """Script text with comment lines removed (comments may mention flashing)."""
    return '\n'.join(
        line for line in START.read_text(encoding='utf-8').splitlines()
        if not line.lstrip().startswith('#')
    )


class StartScriptTests(unittest.TestCase):
    def test_parses_under_sh_and_bash(self):
        for shell in ('sh', 'bash'):
            with self.subTest(shell=shell):
                result = subprocess.run([shell, '-n', str(START)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_is_idempotent_guarded_and_staged(self):
        text = START.read_text(encoding='utf-8')
        self.assertIn('set -eu', text)
        self.assertIn('CODEC_STAGE=', text)
        stages = set(re.findall(r'^set_stage (\S+)$', text, re.M) +
                     re.findall(r'^stage=(\S+)$', text, re.M))
        self.assertEqual(stages, STAGES)
        # The script must state that a running SA is not proof of decoding.
        self.assertIn('process_running_is_not_decode_capability', text)
        # Re-running must not accumulate state: firmware copies overwrite, modules are
        # guarded by /proc/modules, and the media param is idempotent.
        self.assertIn('/proc/modules', text)
        self.assertIn('param set marble.debug.start.media true', text)

    def test_never_flashes_or_touches_storage(self):
        code = script_code()
        for forbidden in ('fastboot', 'set_active', '/dev/block', 'adb ', 'mtd '):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, code)

    def test_write_targets_stay_inside_codec_scope(self):
        code = script_code()
        writes = [line for line in code.splitlines()
                  if re.search(r'\bcp\s|\bmkdir -p\b|\binsmod\b|\bchmod\b|>\s*"', line)]
        self.assertTrue(writes)
        allowed = ('$R$WORK', '$WORK', '/compat/firmware', '$R/compat/firmware',
                   '$R/vendor_dlkm', '$R/lib/modules', '/sys/module/firmware_class',
                   '/data/local/tmp', '$LOG', '$dst', '$PROBE', '$FW_SRC', '"$R"',
                   '"$d/$ko"')
        for line in writes:
            with self.subTest(line=line.strip()):
                self.assertTrue(any(token in line for token in allowed),
                                f'write outside codec scope: {line.strip()}')
                self.assertNotIn('$R/system', line)
                self.assertNotIn('/system/etc', line)
        # The module search directories are the two documented ones only.
        for directory in re.findall(r'for d in ([^;]+); do', code):
            with self.subTest(module_dir=directory):
                self.assertEqual(directory.strip(),
                                 '"$R/vendor_dlkm/lib/modules" "$R/lib/modules"')


class InitConfigTests(unittest.TestCase):
    def test_hardware_cfg_is_valid_json_and_requests_media_sas(self):
        cfg = json.loads(HARDWARE_CFG.read_text(encoding='utf-8'))
        self.assertIn('jobs', cfg)
        self.assertIn('services', cfg)
        media_job = next(job for job in cfg['jobs'] if job['name'] == 'param:marble-media')
        self.assertIn('marble.debug.start.media=true', media_job['condition'])
        for service in ('media_service', 'av_session', 'av_codec_service'):
            self.assertIn(f'start {service}', media_job['cmds'])
        names = {service['name'] for service in cfg['services']}
        self.assertTrue({'av_codec_service', 'media_service', 'av_session'} <= names)
        codec_service = next(s for s in cfg['services'] if s['name'] == 'av_codec_service')
        self.assertEqual(codec_service['secon'], 'u:r:av_codec_service:s0')
        self.assertEqual(codec_service['path'],
                         ['/system/bin/sa_main', '/system/profile/av_codec_service.json'])
        self.assertIn('dev_dma_heap', codec_service['gid'])

    def test_service_ids_match_donor_profiles(self):
        if not DONOR.exists():
            self.skipTest('donor rootfs not present')
        for service in load_inventory()['ohos']['services']:
            profile = json.loads((DONOR / service['profile']).read_text(encoding='utf-8'))
            ids = [entry['name'] for entry in profile['systemability']]
            with self.subTest(service=service['name']):
                self.assertEqual(ids, [service['sa']])
                self.assertEqual(profile['process'], service['name'])


class ProbeSourceTests(unittest.TestCase):
    def test_api_names_exist_in_source_and_local_sdk(self):
        source = PROBE.read_text(encoding='utf-8')
        apis = load_inventory()['probe']['apis']
        self.assertGreaterEqual(len(apis), 20)
        headers = ''
        if SDK_INCLUDE.is_dir():
            for header in SDK_INCLUDE.glob('*.h'):
                headers += header.read_text(encoding='utf-8', errors='replace')
        else:
            self.skipTest('OHOS SDK headers not present')
        for api in apis:
            with self.subTest(api=api):
                self.assertIn(api, source)
                self.assertIn(api, headers)

    def test_default_mode_enumerates_without_creating_a_codec(self):
        source = PROBE.read_text(encoding='utf-8')
        body = source.split('static int probe_enumerate(void)', 1)[1]
        body = body.split('typedef struct {', 1)[0]
        for creator in ('CreateByMime', 'CreateByName', 'Prepare', 'Configure'):
            self.assertNotIn(creator, body)
        # Enumerate-only path must still be reachable when argv has no sub-command.
        self.assertIn('if (argc == 1)', source)

    def test_output_contract_is_parseable(self):
        source = PROBE.read_text(encoding='utf-8')
        self.assertIn('CODEC_PROBE name=%s mime=%s type=%s', source)
        self.assertIn('CODEC_PROBE_SUMMARY', source)
        self.assertIn('CODEC_INSTANCE', source)
        self.assertIn('CODEC_DECODE', source)
        # type must be derived from OH_AVCapability_IsHardware, not from the request.
        self.assertIn('OH_AVCapability_IsHardware(cap) ? "hw" : "sw"', source)

    def test_compiles_with_local_ohos_ndk(self):
        clang = SDK / 'llvm' / 'bin' / 'clang'
        if not clang.exists() or not SDK_INCLUDE.is_dir():
            self.skipTest('OHOS SDK toolchain not present')
        with tempfile.TemporaryDirectory() as tmp:
            out = pathlib.Path(tmp) / 'native_codec_probe'
            # Exercise the unchanged production build, including mutex/condvar linkage.
            result = subprocess.run(['sh', str(PROBE_BUILD)],
                                    env={**os.environ, 'HMOS_SDK': str(SDK), 'CODEC_PROBE_OUT': tmp},
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn(f'PROBE_BUILD_OK out={out}', result.stdout)
            self.assertTrue(out.is_file())
            needs = subprocess.run(['readelf', '-d', str(out)], check=True, capture_output=True,
                                   text=True, timeout=10).stdout
            for lib in ('libnative_media_codecbase.so', 'libnative_media_vdec.so',
                        'libnative_media_core.so'):
                self.assertIn(lib, needs)

    def test_build_helper_is_shell_clean(self):
        result = subprocess.run(['sh', '-n', str(PROBE_BUILD)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('PROBE_BUILD_OK', PROBE_BUILD.read_text(encoding='utf-8'))


class InventoryTests(unittest.TestCase):
    def test_inventory_is_valid_json_with_expected_sections(self):
        inventory = load_inventory()
        for key in ('ohos', 'marble_dump', 'marble_kernel', 'probe', 'verdict'):
            self.assertIn(key, inventory)
        self.assertIsInstance(inventory['ohos']['libraries'], list)

    def test_ohos_paths_exist_in_donor_rootfs(self):
        if not DONOR.exists():
            self.skipTest('donor rootfs not present')
        inventory = load_inventory()
        paths = [lib['path'] for lib in inventory['ohos']['libraries']]
        paths += [service['profile'] for service in inventory['ohos']['services']]
        paths += [inventory['ohos']['hdi'][key] for key in
                  ('client_proxy', 'passthrough_impl', 'hdf_driver', 'hdf_stub', 'omx_core')]
        paths += inventory['ohos']['hdi']['omx_video_components']
        paths.append(inventory['ohos']['hdi']['vdec_core']['path'])
        paths += inventory['ohos']['hdi']['vendor_soft_libs']
        paths += inventory['ohos']['soft_audio_plugins']
        paths.append(inventory['ohos']['codec_caps_xml']['path'])
        for path in paths:
            with self.subTest(path=path):
                self.assertTrue((DONOR / path).is_file(), f'missing donor artifact {path}')

    def test_no_soft_video_plugin_claim_is_backed_by_the_plugin_dir(self):
        if not DONOR.exists():
            self.skipTest('donor rootfs not present')
        inventory = load_inventory()
        self.assertEqual(inventory['ohos']['soft_video_plugins'], [])
        plugins = DONOR / 'system/system/lib64/media/media_plugins'
        names = {entry.name for entry in plugins.iterdir()}
        self.assertIn('libmedia_plugin_FFmpegAudioDecoders.z.so', names)
        self.assertFalse([name for name in names if 'Video' in name])
        # The only video codec plugin is the HDI-backed HiStreamer CodecAdapter.
        adapter = DONOR / 'system/system/lib64/media/histreamer_plugins/libhistreamer_plugin_CodecAdapter.z.so'
        blob = adapter.read_bytes()
        self.assertIn(b'libcodec_proxy_4.0.z.so', blob)
        self.assertIn(b'video/avc', blob)
        self.assertIn(b'video/hevc', blob)

    def test_hdi_component_names_come_from_the_vendor_omx_libs(self):
        if not DONOR.exists():
            self.skipTest('donor rootfs not present')
        inventory = load_inventory()
        names = inventory['ohos']['hdi']['omx_component_names']
        self.assertTrue(names)
        self.assertTrue(all(name.startswith('OMX.hisi.') for name in names))
        omx_core = (DONOR / inventory['ohos']['hdi']['omx_core']).read_bytes()
        for name in names:
            with self.subTest(name=name):
                self.assertIn(name.encode(), omx_core)
        decoder = (DONOR / 'vendor/lib64/passthrough/indirect/libomxvdec.so').read_bytes()
        self.assertIn(b'video_decoder.avc', decoder)
        self.assertIn(b'video_decoder.hevc', decoder)
        # The Huawei vdec backend opens a Huawei-only kernel node, which marble lacks.
        vdec_core = (DONOR / inventory['ohos']['hdi']['vdec_core']['path']).read_bytes()
        for node in inventory['ohos']['hdi']['vdec_core']['kernel_nodes']:
            self.assertIn(node.encode(), vdec_core)

    def test_dump_lists_match_the_checked_in_evidence(self):
        inventory = load_inventory()
        evidence = dump_evidence()
        self.assertGreater(len(evidence), 100)
        dump = inventory['marble_dump']
        keys = ('hardware_services', 'hardware_libs', 'software_libs_64bit',
                'software_libs_32bit', 'media_codecs_xml', 'firmware', 'kernel_modules',
                'seccomp_policies')
        for key in keys:
            for path in dump[key]:
                with self.subTest(key=key, path=path):
                    self.assertIn(path, evidence)

    def test_absent_codec2_soft_components_are_really_absent(self):
        inventory = load_inventory()
        evidence = dump_evidence()
        listed = {line for line in evidence if 'libcodec2_soft' in line}
        self.assertEqual(listed, {
            'vendor/lib/libcodec2_soft_common.so',
            'vendor/lib64/libcodec2_soft_common.so',
        })
        for name in inventory['marble_dump']['soft_component_absent']:
            with self.subTest(name=name):
                self.assertFalse([line for line in evidence if line.endswith('/' + name)])

    def test_media_codecs_components_match_the_dump_xml(self):
        inventory = load_inventory()
        evidence = json.loads(MEDIA_CODECS.read_text(encoding='utf-8'))
        ukee = {(item['name'], item['type']) for item in evidence['media_codecs_ukee']['components']}
        listed = {(item['name'], item['mime']) for item in inventory['marble_dump']['hardware_components']}
        self.assertEqual(listed, ukee)
        # AOSP soft components are declared by the google xml but not built by this dump.
        google = {item['name'] for item in evidence['media_codecs_google_c2_video']['components']}
        self.assertTrue(all(name.startswith('c2.android.') for name in google))
        self.assertFalse(google & {name for name, _ in listed})


class KernelSideTests(unittest.TestCase):
    def test_msm_video_module_is_shipped_for_the_running_kernel(self):
        if not KERNEL_MODULES_TAR.exists():
            self.skipTest('kernel artifacts not present')
        with tarfile.open(KERNEL_MODULES_TAR, 'r:gz') as archive:
            names = {name.lstrip('./') for name in archive.getnames()}
        self.assertIn('msm_video.ko', names)
        self.assertIn('qcom-scm.ko', names)
        self.assertIn('mdt_loader.ko', names)
        self.assertIn('msm-mmrm.ko', names)
        self.assertNotIn('subsys-pil-tz.ko', names)  # CONFIG_MSM_SUBSYSTEM_RESTART is off

    def test_module_vermagic_matches_the_documented_kernel(self):
        if not KERNEL_MODULE.exists():
            self.skipTest('built msm_video.ko not present')
        blob = KERNEL_MODULE.read_bytes()
        self.assertIn(b'5.10.274-Bouquet-anyhmos', blob)
        self.assertIn(b'of:N*T*Cqcom,msm-vidc', blob)
        self.assertIn(b'%s.mbn', blob)


class VerdictTests(unittest.TestCase):
    def test_verdict_never_claims_decoding_from_process_state(self):
        verdict = load_inventory()['verdict']
        self.assertEqual(verdict['software_video_decode']['state'], 'blocked-on-this-stack')
        self.assertEqual(verdict['hardware_decode_ohos_hdi']['state'], 'blocked')
        self.assertEqual(verdict['android_codec2_soft_via_bionic']['state'],
                         'blocked-by-missing-artifact')
        self.assertEqual(verdict['software_audio_decode']['state'], 'expected-ok')
        for name, entry in verdict.items():
            with self.subTest(entry=name):
                self.assertNotEqual(entry['state'], 'working')
                self.assertTrue(entry.get('why'))


if __name__ == '__main__':
    unittest.main()
