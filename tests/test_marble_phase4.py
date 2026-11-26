import hashlib
import json
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/packaging'))

from build_marble_phase4_images import (
    PHASE4_ALL_RECIPES,
    WIFI_CHIP_DONOR_HCB,
    build_phase4_hdf_hcb,
    build_phase4_images,
    phase4_init_seed,
    patch_appdata_sandbox_with_compat,
    phase4_donor_recipes,
    phase4_wifi_supplicant_payload,
    phase4_wpa_ondemand_init,
)


class MarblePhase4Tests(unittest.TestCase):
    def test_phase4_hcb_contains_five_hosts(self):
        hcb = build_phase4_hdf_hcb()
        magic, vmaj, vmin, csum, total = struct.unpack_from('<IIIII', hcb, 0)
        self.assertEqual(magic, 0xA00AA00A)
        self.assertEqual((vmaj, vmin, csum), (0, 7, 0))
        self.assertEqual(20 + total, len(hcb))
        for token in (
            b'allocator_host\x00',
            b'liballocator_driver_1.0.z.so\x00',
            b'composer_host\x00',
            b'libdisplay_composer_driver_1.0.z.so\x00',
            b'hmos_userauth_host\x00',
            b'libhmos_user_auth_driver.so\x00',
            b'user_auth_interface_service\x00',
            b'wifi_host\x00',
            b'libwifi_hdi_c_device.z.so\x00',
            b'wlan_interface_service\x00',
            b'wpa_host\x00',
            b'libwpa_hdi_c_device.z.so\x00',
            b'wpa_interface_service\x00',
        ):
            self.assertIn(token, hcb)
        self.assertNotIn(b'libdisplayengine_driver.z.so', hcb)
        self.assertNotIn(b'chip_interface_service\x00', hcb)
        self.assertEqual(hashlib.sha256(hcb).hexdigest(),
                         '53817d854dfc612c1f64f962f649732c6aef4ba5a8062d9c71ecbe4094bb0ec9')

    def test_chip_hdi_trial_restores_only_the_original_ad_y_node(self):
        donor = ROOT / 'work/donor-rom/ADY-AL10-207.0.0.111-SP10/fs'
        source = donor / WIFI_CHIP_DONOR_HCB
        if not source.is_file():
            self.skipTest('ADY donor HCB not present')
        old = build_phase4_hdf_hcb()
        trial = build_phase4_hdf_hcb(wifi_chip_donor=donor)
        raw = source.read_bytes()
        # The pinned donor HCB's complete local device subtree, independently
        # delimited by its header, not regenerated using the code under test.
        marker = b'\x01wifi_chip_device\x00'
        i = raw.index(marker)
        body = i + len(marker) + 4
        device = raw[i:body + struct.unpack_from('<I', raw, body - 4)[0]]
        self.assertEqual(len(trial) - len(old), len(device))
        self.assertEqual(trial.count(device), 1)
        at = trial.index(device)
        self.assertLess(trial.index(b'wlan_interface_service\x00'), at)
        self.assertLess(at, trial.index(b'wpa_host\x00'))
        without = bytearray(trial[:at] + trial[at + len(device):])
        # Only ancestor byte lengths may differ after removing the new node.
        for key in (b'\x01root\x00', b'\x01device_info\x00', b'\x01wifi\x00'):
            n = without.index(key, 20) + len(key)
            self.assertEqual(struct.unpack_from('<I', without, n)[0],
                             struct.unpack_from('<I', old, n)[0] + len(device))
            without[n:n + 4] = old[n:n + 4]
        struct.pack_into('<I', without, 16, len(without) - 20)
        self.assertEqual(bytes(without), old)

    def test_chip_hdi_trial_rejects_an_unknown_donor(self):
        with tempfile.TemporaryDirectory() as t:
            root = Path(t)
            with self.assertRaisesRegex(ValueError, 'Missing regular chip HDI donor input'):
                build_phase4_hdf_hcb(wifi_chip_donor=root)
            path = root / WIFI_CHIP_DONOR_HCB
            path.parent.mkdir(parents=True)
            path.write_bytes(build_phase4_hdf_hcb())
            with self.assertRaisesRegex(ValueError, 'chip HDI donor SHA-256 mismatch'):
                build_phase4_hdf_hcb(wifi_chip_donor=root)

    def test_wpa_ondemand_hcb_changes_only_the_wpa_preload_byte(self):
        donor = ROOT / 'work/donor-rom/ADY-AL10-207.0.0.111-SP10/fs'
        if not (donor / WIFI_CHIP_DONOR_HCB).is_file():
            self.skipTest('ADY donor HCB not present')
        eager = build_phase4_hdf_hcb(wifi_chip_donor=donor)
        lazy = build_phase4_hdf_hcb(wifi_chip_donor=donor, wpa_on_demand_donor=donor)
        self.assertEqual(len(eager), len(lazy))
        self.assertEqual([(i, before, after) for i, (before, after) in enumerate(zip(eager, lazy))
                          if before != after], [(1735, 0, 2)])
        self.assertIn(b'\x02preload\x00\x10\x02\x02priority\x00\x10\x64'
                      b'\x02moduleName\x00\x14libwpa_hdi_c_device.z.so\x00', lazy)

    def test_wpa_ondemand_preserves_role_and_other_init_jobs(self):
        donor = ROOT / 'work/donor-rom/ADY-AL10-207.0.0.111-SP10/fs'
        if not (donor / WIFI_CHIP_DONOR_HCB).is_file():
            self.skipTest('ADY donor HCB not present')
        src = (ROOT / 'devices/marble/runtime/init/init-wifi-marble.cfg').read_bytes()
        before = json.loads(src)
        after = json.loads(phase4_wpa_ondemand_init(src, donor))
        wpa, = [host for host in after['services'] if host['name'] == 'wpa_host']
        self.assertIs(wpa.pop('ondemand'), True)
        self.assertFalse(any('start wpa_host' in job['cmds'] for job in after['jobs']))
        before_gate, = [job for job in before['jobs'] if job['name'] == 'param:marble-wifi']
        after_gate, = [job for job in after['jobs'] if job['name'] == 'param:marble-wifi']
        after_gate['cmds'].insert(before_gate['cmds'].index('start wpa_host'), 'start wpa_host')
        self.assertEqual(after, before)
        altered = json.loads(src)
        next(s for s in altered['services'] if s['name'] == 'wpa_host')['uid'] = 'root'
        with self.assertRaisesRegex(ValueError, 'Unexpected marble or donor WPA host identity'):
            phase4_wpa_ondemand_init(json.dumps(altered).encode(), donor)

    def test_patch_appdata_sandbox_idempotent(self):
        sample = json.dumps({
            'common': [{
                'top-sandbox-switch': 'ON',
                'app-base': [{
                    'sandbox-root': '/mnt/sandbox/<currentUserId>/<PackageName>',
                    'mount-paths': [{'src-path': '/config', 'sandbox-path': '/config'}],
                }],
            }],
        }).encode('utf-8')
        patched = patch_appdata_sandbox_with_compat(sample)
        doc = json.loads(patched.decode('utf-8'))
        mps = doc['common'][0]['app-base'][0]['mount-paths']
        self.assertEqual(mps[0]['src-path'], '/compat')
        self.assertEqual(mps[0]['sandbox-path'], '/compat')
        patched_again = patch_appdata_sandbox_with_compat(patched)
        doc2 = json.loads(patched_again.decode('utf-8'))
        compat_entries = [m for m in doc2['common'][0]['app-base'][0]['mount-paths'] if m.get('src-path') == '/compat']
        self.assertEqual(len(compat_entries), 1)

    def test_phase4_recipes_complete(self):
        recipes_dir = ROOT / 'patches/donor/ady-al10-111-sp10'
        self.assertEqual(len(PHASE4_ALL_RECIPES), 16)
        for name in phase4_donor_recipes(True):
            recipe = json.loads((recipes_dir / name).read_bytes())
            self.assertNotIn('retired_reason', recipe)

    def test_ady_sensor_client_stays_native(self):
        from build_marble_phase3_images import generate_phase3_donor_patch_script

        recipes = ROOT / 'patches/donor/ady-al10-111-sp10'
        name = 'libsensor_client.z.so.json'
        self.assertNotIn(name, phase4_donor_recipes(True))
        script = generate_phase3_donor_patch_script(recipes, phase4_donor_recipes(True))
        self.assertNotIn('/platformsdk/libsensor_client.z.so', script)
        with self.assertRaisesRegex(ValueError, r'Retired donor recipe libsensor_client'):
            generate_phase3_donor_patch_script(recipes, [name])
        # Direct one-off patch application must also reject the old recipe.
        with tempfile.TemporaryDirectory() as td:
            src, dst = Path(td) / 'src', Path(td) / 'dest'
            src.touch()
            result = subprocess.run([sys.executable,
                str(ROOT / 'tools/packaging/apply_guarded_elf_patch.py'),
                '--manifest', str(recipes / name), '--input', str(src), '--output', str(dst)],
                capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('Retired donor recipe', result.stderr)
            self.assertFalse(dst.exists())

    def test_ady_rgba_recipe_preserves_sampleable_usage(self):
        path = ROOT / 'patches/donor/ady-al10-111-sp10/librender_service_base.z.so.json'
        recipe = json.loads(path.read_bytes())
        usage_offset = 0x3DEA74  # ADY MakeBackendTextureFromNativeBuffer, file offset
        usage_guard = next(g for g in recipe['guards'] if g['offset'] == usage_offset)
        opcode, = struct.unpack('<I', bytes.fromhex(usage_guard['bytes']))
        self.assertEqual(opcode & 0xFFE0001F, 0x52800009)  # MOVZ W9, #imm
        self.assertEqual((opcode >> 5) & 0xFFFF, 7)  # TRANSFER_SRC | TRANSFER_DST | SAMPLED
        for change in recipe['changes']:
            after_size = len(bytes.fromhex(change['after']))
            self.assertFalse(change['offset'] < usage_offset + 4 and
                             change['offset'] + after_size > usage_offset)
        self.assertEqual({c['offset'] for c in recipe['changes']}, {0x438CFC, 0x3DF000})

    def test_ady_timer_skip_uses_actual_fd_table_and_common_loop_exit(self):
        path = ROOT / 'patches/donor/ady-al10-111-sp10/libtime_system_ability.z.so.json'
        recipe = json.loads(path.read_bytes())
        change, = recipe['changes']
        self.assertEqual(change['offset'], 0x3B454)
        skip_result, store, branch = struct.unpack('<3I', bytes.fromhex(change['after']))
        self.assertEqual(skip_result, 0x12800000)  # MOV W0, #-1
        self.assertEqual(store & 0xFFE0FC00, 0xB8207800)  # STR Wt, [Xn,Xm,LSL #2]
        self.assertEqual((store & 31, (store >> 5) & 31, (store >> 16) & 31), (0, 26, 19))
        self.assertEqual(branch >> 26, 0b000101)
        imm = branch & 0x03FFFFFF
        imm = (imm ^ (1 << 25)) - (1 << 25)
        self.assertEqual(change['offset'] + 8 + 4 * imm, 0x3B54C)  # common index/loop update
        guards = {g['offset']: bytes.fromhex(g['bytes']) for g in recipe['guards']}
        self.assertEqual(guards[0x3B470], struct.pack('<I', store))
        self.assertTrue(guards[0x3B54C].startswith(bytes.fromhex('680600917f1a00f1f7279f1a')))

    def test_screen_order_trial_does_not_change_default_recipe_set(self):
        base = phase4_donor_recipes()
        trial = phase4_donor_recipes(True)
        self.assertEqual(base, PHASE4_ALL_RECIPES)
        self.assertEqual(trial, base + ['librender_service.z.so.json'])
        self.assertEqual(phase4_donor_recipes(), base)

    def test_native_commit_trial_replaces_only_the_render_recipe(self):
        from build_marble_phase4_images import RS_COMMIT_CONTROL_RECIPE, RS_NATIVE_COMMIT_RECIPE

        control_names = phase4_donor_recipes(True)
        trial_names = phase4_donor_recipes(True, rs_native_commit=True)
        self.assertEqual(trial_names, [RS_NATIVE_COMMIT_RECIPE if name == RS_COMMIT_CONTROL_RECIPE
                                       else name for name in control_names])
        recipes = ROOT / 'patches/donor/ady-al10-111-sp10'
        control, trial = (json.loads((recipes / name).read_bytes()) for name in (
            RS_COMMIT_CONTROL_RECIPE, RS_NATIVE_COMMIT_RECIPE))
        self.assertEqual((trial['path'], trial['input_sha256']), (control['path'], control['input_sha256']))
        commit = [c for c in control['changes'] if c['offset'] == 0x438CFC]
        self.assertEqual(commit, [{'offset': 0x438CFC, 'before': '21008052', 'after': '01008052'}])
        self.assertEqual(trial['changes'], [c for c in control['changes'] if c not in commit])
        self.assertEqual(trial['evidence']['control_sha256'], control['output_sha256'])

    def test_yuv_native_handle_selects_one_render_recipe(self):
        from build_marble_phase4_images import RS_COMMIT_CONTROL_RECIPE, RS_YUV_NATIVE_HANDLE_RECIPE

        base = phase4_donor_recipes()
        expected = [RS_YUV_NATIVE_HANDLE_RECIPE if name == RS_COMMIT_CONTROL_RECIPE else name
                    for name in base]
        self.assertEqual(phase4_donor_recipes(rs_yuv_native_handle=True), expected)
        self.assertEqual(phase4_donor_recipes(), base)
        with self.assertRaisesRegex(ValueError, 'cannot be combined'):
            phase4_donor_recipes(rs_native_commit=True, rs_yuv_native_handle=True)

    def test_h20_yuv_checks_the_emitted_command_and_decompressed_driver(self):
        import gzip
        from unittest.mock import patch
        from build_marble_phase3_images import generate_phase3_donor_patch_script
        from build_marble_phase4_images import verify_h20_yuv_inputs

        recipes = ROOT / 'patches/donor/ady-al10-111-sp10'
        script = generate_phase3_donor_patch_script(recipes, phase4_donor_recipes(rs_yuv_native_handle=True))
        old = generate_phase3_donor_patch_script(recipes, phase4_donor_recipes())
        synthetic_driver = b'driver fixture bytes'
        sha = hashlib.sha256(synthetic_driver).hexdigest()
        with patch('build_marble_phase4_images.RS_YUV_NATIVE_TURNIP_SHA256', sha):
            for mtime in (0, 12345):
                verify_h20_yuv_inputs(script, gzip.compress(synthetic_driver, mtime=mtime))
            with self.assertRaisesRegex(ValueError, 'requires the verified Turnip'):
                verify_h20_yuv_inputs(script, gzip.compress(synthetic_driver + b'changed'))
            with self.assertRaisesRegex(ValueError, 'render command differs'):
                verify_h20_yuv_inputs(old, gzip.compress(synthetic_driver))
            with self.assertRaisesRegex(ValueError, 'render command differs'):
                verify_h20_yuv_inputs(script + script, gzip.compress(synthetic_driver))

    def test_h20_yuv_rejects_a_different_driver_before_creating_output(self):
        import gzip
        from build_marble_phase4_images import TURNIP_PAYLOAD

        recipes = ROOT / 'patches/donor/ady-al10-111-sp10'
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            (root / TURNIP_PAYLOAD).write_bytes(gzip.compress(b'unverified driver'))
            out = root / 'unused-build'
            dummy = root / 'unused-input'
            with self.assertRaisesRegex(ValueError, 'requires the verified Turnip'):
                build_phase4_images(dummy, dummy, dummy, dummy, recipes, dummy, dummy, root,
                                    dummy, out, rs_yuv_native_handle=True)
            self.assertFalse(out.exists())

    def test_unverified_diagnostics_stop_before_creating_output(self):
        recipes = ROOT / 'patches/donor/ady-al10-111-sp10'
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            out = root / 'unused-build'
            dummy = root / 'unused-input'
            with self.assertRaisesRegex(ValueError, 'Missing diagnostics build record'):
                build_phase4_images(dummy, dummy, dummy, dummy, recipes, dummy, dummy, dummy,
                                    dummy, out, diagnostics_dir=root / 'unverified')
            self.assertFalse(out.exists())

    def test_existing_build_directory_is_preserved(self):
        with tempfile.TemporaryDirectory() as td:
            out = Path(td)
            work = out / 'work'
            work.mkdir()
            old = work / 'old-evidence'
            old.write_bytes(b'preserve me')
            with self.assertRaisesRegex(FileExistsError, 'choose a fresh --out'):
                build_phase4_images(*([out / 'unread-input'] * 9), out_dir=out)
            self.assertEqual(old.read_bytes(), b'preserve me')

    def test_guarded_patch_cli_accepts_path_only_recipe(self):
        raw = b'\x7fELF\x02\x01' + bytes(8) + b'ab'
        expected = raw[:-1] + b'X'
        spec = {
            'path': '/system/lib64/example.so',
            'input_sha256': hashlib.sha256(raw).hexdigest(),
            'output_sha256': hashlib.sha256(expected).hexdigest(),
            'guards': [{'offset': 14, 'bytes': '61'}],
            'changes': [{'offset': 15, 'before': '62', 'after': '58'}],
        }
        with tempfile.TemporaryDirectory() as td:
            base = Path(td)
            src, dst, manifest = (base / n for n in ('src', 'dst', 'recipe.json'))
            src.write_bytes(raw)
            manifest.write_text(json.dumps(spec), encoding='utf-8')
            cmd = [sys.executable, str(ROOT / 'tools/packaging/apply_guarded_elf_patch.py'),
                   '--input', str(src), '--output', str(dst), '--manifest', str(manifest)]
            result = subprocess.run(cmd, check=True, capture_output=True, text=True)
            self.assertEqual(json.loads(result.stdout)['module'], spec['path'])
            self.assertEqual(dst.read_bytes(), expected)
            self.assertEqual(src.read_bytes(), raw)
            dst.unlink()
            spec['guards'][0]['bytes'] = '00'
            manifest.write_text(json.dumps(spec), encoding='utf-8')
            result = subprocess.run(cmd, check=False, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('Guard mismatch', result.stderr)
            self.assertFalse(dst.exists())

    def test_phase4_seed_restores_only_donor_samgr_role(self):
        donor = ROOT / 'work/donor-rom/ADY-AL10-207.0.0.111-SP10/fs'
        original = donor / 'system/system/etc/init/samgr_standard.cfg'
        if not original.is_file():
            self.skipTest('ADY donor extract not present')
        main = ROOT / 'devices/marble/runtime/init/hmos-init-phase4.cfg'
        restored, report = phase4_init_seed(main, donor)
        before = json.loads(main.read_bytes())
        after = json.loads(restored)
        donor_samgr, = json.loads(original.read_bytes())['services']
        samgr = next(s for s in after['services'] if s['name'] == 'samgr')
        for key in ('permission', 'permission_acls'):
            self.assertIn('ohos.permission.MANAGE_SYSTEM_ABILITY', samgr[key])
            self.assertEqual(samgr.pop(key), donor_samgr[key])
        source_samgr = next(s for s in before['services'] if s['name'] == 'samgr')
        for key in ('permission', 'permission_acls'):
            source_samgr.pop(key, None)
        self.assertEqual(after, before, 'keep the phase4 imports, jobs, and other services')
        self.assertEqual(report['output_sha256'], hashlib.sha256(restored).hexdigest())
        self.assertLess(len(restored), 32768)

    def test_phase4_wifi_payload_keeps_the_donor_library(self):
        payload = phase4_wifi_supplicant_payload(ROOT)
        prefix = 'graphics/wifi-supplicant/'
        self.assertEqual({name: mode for name, (_, mode) in payload.items()}, {
            prefix + 'marble-wifi-supplicant-stage.sh': 0o755,
            prefix + 'etc/wpa_supplicant.conf': 0o644,
            prefix + 'etc/p2p_supplicant.conf': 0o644,
        })
        wifi = (ROOT / 'devices/marble/runtime/wifi/start-wifi-marble.sh').read_text()
        self.assertIn('WPA_STAGE=/hmos-compat-marble/graphics/wifi-supplicant', wifi)
        self.assertLess(wifi.index('sh "$WPA_STAGE/marble-wifi-supplicant-stage.sh"'),
                        wifi.index('param set marble.debug.start.wifi true'))
        self.assertIn('WIFI_SUPPLICANT_STAGE_SKIPPED=consumer-running', wifi)

    def test_phase4_wifi_payload_rejects_nonempty_network_settings(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            wifi = root / 'compat/wifi'
            etc = wifi / 'runtime/etc'
            etc.mkdir(parents=True)
            (wifi / 'marble-wifi-supplicant-stage.sh').write_bytes(b'#!/busybox sh\n')
            (etc / 'wpa_supplicant.conf').write_bytes(b'network={\n}\n')
            with self.assertRaisesRegex(ValueError, 'reviewed blank config'):
                phase4_wifi_supplicant_payload(root)

    def test_phase4_init_cfgs_and_scripts_valid(self):
        rt = ROOT / 'devices/marble/runtime'
        main_cfg = rt / 'init/hmos-init-phase4.cfg'
        self.assertLess(main_cfg.stat().st_size, 32768)
        for rel in (
            'init/hmos-init-phase4.cfg',
            'init/init-desktop-marble.cfg',
            'init/init-platform-marble.cfg',
            'init/init-hardware-marble.cfg',
            'init/init-wifi-marble.cfg',
            'graphics/hmos-turnip.json',
        ):
            json.loads((rt / rel).read_text(encoding='utf-8'))
        for sh in ('boot/stage-phase4-marble.sh', 'boot/start-phase4-marble.sh', 'wifi/start-wifi-marble.sh'):
            subprocess.run(['bash', '-n', str(rt / sh)], check=True)


if __name__ == '__main__':
    unittest.main()
