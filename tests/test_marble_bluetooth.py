"""Contract tests for the marble Bluetooth stack (host-side only).

The marble bring-up reuses the donor OHOS Bluetooth service (SA 1130) and the
A15 Qualcomm vendor HAL lineage, but the hardware path is marble specific:

* ``compat/bluetooth/marble_bt_bridge.c`` owns ``/dev/ttyHS0`` / ``/dev/btpower``
  and the ``bluetooth_a`` firmware, and exposes the HCI byte stream on a UNIX
  socket for the OHOS HDI (which does its own H4 framing);
* ``devices/marble/runtime/bluetooth/start-bluetooth-marble.sh`` prepares nodes,
  mounts ``bluetooth_a`` read-only and starts the bridge;
* ``devices/marble/runtime/bluetooth/init-bluetooth-marble.cfg`` starts the HDI
  publisher and ``bluetooth_service`` once ``marble.debug.start.bluetooth=true``.

Nothing here talks to a device.  The dump-derived baseline
(``work/marble-bluetooth-files.json``) is a local artifact: when it is absent
(fresh checkout, ``/work/`` is ignored) the dump-backed tests skip.
"""

from __future__ import annotations

import ast
import json
import re
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BT_DIR = ROOT / 'devices/marble/runtime/bluetooth'
SCRIPT = BT_DIR / 'start-bluetooth-marble.sh'
INIT_CFG = BT_DIR / 'init-bluetooth-marble.cfg'
BRIDGE_SRC = ROOT / 'compat/bluetooth/marble_bt_bridge.c'
VENDOR_SRC = ROOT / 'compat/bluetooth/bluetooth_vendor_v136.cpp'
BUILD_SCRIPT = ROOT / 'tools/build/build_current.py'
BASELINE = ROOT / 'work/marble-bluetooth-files.json'
DUMP_INDEX = ROOT / 'work/marble-bluetooth-dump/all_files.txt'
IMPL = ROOT / 'work/marble-bluetooth-dump/vendor/lib64/hw/android.hardware.bluetooth@1.0-impl-qti.so'
DONOR = ROOT / 'work/donor-rom/ADY-AL10-207.0.0.111-SP10/fs'
BUILD_DIR = ROOT / 'work/marble-bluetooth-build'


def read(path: Path) -> str:
    return path.read_text(encoding='utf-8')


def load_baseline() -> dict | None:
    if not BASELINE.exists():
        return None
    return json.loads(read(BASELINE))


def script_default(text: str, name: str) -> str:
    match = re.search(rf'^{name}=\$\{{{name}:-([^}}]+)\}}', text, re.MULTILINE)
    if not match:
        raise AssertionError(f'{name} default not found in {SCRIPT.name}')
    return match.group(1)


def source_define(text: str, macro: str) -> str:
    match = re.search(rf'^#define {macro} (\S+)', text, re.MULTILINE)
    if not match:
        raise AssertionError(f'{macro} not defined in {BRIDGE_SRC.name}')
    return match.group(1)


class StartScriptContractTests(unittest.TestCase):
    def setUp(self):
        self.text = read(SCRIPT)

    def test_shell_syntax(self):
        result = subprocess.run(['bash', '-n', str(SCRIPT)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_strict_mode_and_failure_reporting(self):
        self.assertIn('set -eu', self.text)
        self.assertIn('BT_START_FAILED stage=', self.text)
        self.assertIn('BT_START_EXIT=', self.text)
        # every stage marker must be a plain assignment so the trap can report it
        self.assertIsNotNone(re.search(r'^stage=[a-z_]+$', self.text, re.MULTILINE))
        self.assertGreaterEqual(len(re.findall(r'^stage=', self.text, re.MULTILINE)), 8)

    def test_no_flash_adb_or_serial_work(self):
        # comments may describe the prohibition; only executed lines are checked
        code = '\n'.join(line for line in self.text.splitlines()
                          if not line.lstrip().startswith('#'))
        forbidden = (r'\bfastboot\b', r'\badb\b', r'\bCOM9\b', r'\bset_active\b',
                     r'\bflash\b', r'reboot bootloader')
        for pattern in forbidden:
            self.assertNotRegex(code, pattern)

    def test_paths_are_variables_not_literals(self):
        for name in ('BT_PART', 'BT_MOUNT', 'BT_FW_ROOT', 'BT_TLV', 'BT_NVM', 'BT_TTY',
                     'BT_POWER', 'BT_SOCKET', 'BT_BRIDGE', 'BT_HDI', 'BT_BRIDGE_LOG',
                     'BT_HDI_LOG'):
            self.assertIn(f'{name}=${{{name}:-', self.text, f'{name} must be env-overridable')

    def test_bridge_exec_path_is_relative_to_chroot(self):
        self.assertEqual(script_default(self.text, 'BT_BRIDGE'), '/compat/bin/hmos_bt_bridge_marble')
        self.assertIn('"$R"/*) BT_BRIDGE=${BT_BRIDGE#"$R"}', self.text)
        self.assertIn('[ -x "$R$BT_BRIDGE" ]', self.text)
        self.assertIn('$B chroot "$R" "$BT_BRIDGE" --check', self.text)
        self.assertIn('$B setsid $B chroot "$R" "$BT_BRIDGE"', self.text)

    def test_bootloader_partition_is_mounted_read_only(self):
        self.assertIn('mount -t vfat -o ro "$BT_PART" "$BT_MOUNT"', self.text)
        self.assertNotRegex(self.text, r'mount -t vfat -o (?!ro)')

    def test_script_expects_bridge_listening_marker(self):
        # mirrors "HMOS_BT BRIDGE_LISTENING=1" printed by the bridge
        self.assertIn("grep -q '^HMOS_BT BRIDGE_LISTENING=1'", self.text)
        self.assertIn('--max-sessions 0', self.text)


class BaselineCrossCheckTests(unittest.TestCase):
    """Script/bridge references must match the recorded dump-derived baseline."""

    def setUp(self):
        self.baseline = load_baseline()
        if self.baseline is None:
            self.skipTest('work/marble-bluetooth-files.json not generated in this checkout')
        self.script = read(SCRIPT)
        self.bridge = read(BRIDGE_SRC)

    def test_firmware_defaults_match_baseline(self):
        confirmed = {f['name'] for f in self.baseline['firmware'] if f['confirmed_on_partition']}
        self.assertIn(script_default(self.script, 'BT_TLV'), confirmed)
        self.assertIn(script_default(self.script, 'BT_NVM'), confirmed)

    def test_node_paths_match_baseline(self):
        nodes = {entry['node'] for entry in self.baseline['nodes']}
        self.assertIn(script_default(self.script, 'BT_TTY'), nodes)
        self.assertIn(script_default(self.script, 'BT_POWER'), nodes)
        sysfs = {entry['sysfs'] for entry in self.baseline['nodes']}
        self.assertIn('/sys/class/tty/ttyHS0/dev', sysfs)
        self.assertIn('/sys/class/bt-dev/btpower/dev', sysfs)

    def test_socket_contract_matches_baseline(self):
        socket_cfg = self.baseline['socket']
        self.assertEqual(script_default(self.script, 'BT_SOCKET'), socket_cfg['path'])
        self.assertIn(f'"{socket_cfg["path"]}"', self.bridge)
        self.assertEqual(socket_cfg['handshake_ready'], 'SBT1')
        for char in socket_cfg['handshake_ready']:
            self.assertIn(f"'{char}'", self.bridge)
        self.assertIn(f"'{socket_cfg['handshake_failure'][-1]}'", self.bridge)
        self.assertIn("0, HMOS_BT_SOCKET_GID)", self.bridge)

    def test_artifacts_match_baseline(self):
        artifacts = self.baseline['artifacts']
        self.assertIn(artifacts['bridge_binary'], script_default(self.script, 'BT_BRIDGE'))
        self.assertIn(artifacts['hdi_publisher'], script_default(self.script, 'BT_HDI'))

    def test_bridge_exports_match_baseline(self):
        declared = {
            match.group(1)
            for line in self.bridge.splitlines()
            if line.startswith('HMOS_BT_EXPORT')
            for match in [re.match(r'HMOS_BT_EXPORT [^(]*?(\w+)\(', line)]
            if match and '_attribute__' not in line
        }
        self.assertEqual(declared, set(self.baseline['bridge_exports']))

    def test_bridge_edl_constants_match_baseline(self):
        edl = self.baseline['edl']
        self.assertEqual(int(source_define(self.bridge, 'HMOS_BT_EDL_OPCODE').rstrip('U'), 16),
                         int(edl['opcode_edl'], 16))
        self.assertEqual(int(source_define(self.bridge, 'HMOS_BT_EDL_BAUD_OPCODE').rstrip('U'), 16),
                         int(edl['opcode_baud'], 16))
        self.assertEqual(int(source_define(self.bridge, 'HMOS_BT_EDL_MODE_OPCODE').rstrip('U'), 16),
                         int(edl['opcode_edl_mode'], 16))
        self.assertEqual(int(source_define(self.bridge, 'HMOS_BT_EDL_SEGMENT')),
                         edl['segment_bytes'])
        mapping = {
            'HMOS_BT_EDL_SUB_VER_REQ': 'ver_req',
            'HMOS_BT_EDL_SUB_APP_VER': 'app_ver',
            'HMOS_BT_EDL_SUB_BUILD_INFO': 'build_info',
            'HMOS_BT_EDL_SUB_BOARD_ID': 'board_id',
            'HMOS_BT_EDL_SUB_TLV': 'tlv_request',
        }
        for macro, key in mapping.items():
            self.assertEqual(int(source_define(self.bridge, macro).rstrip('U'), 16),
                             int(edl['sub_commands'][key], 16), macro)
        self.assertEqual(int(source_define(self.bridge, 'HMOS_BT_POWER_IOCTL').rstrip('U'), 16),
                         0xbfad)

    def test_firmware_names_present_in_a15_hal(self):
        if not IMPL.exists():
            self.skipTest('A15 vendor HAL not fetched in this checkout')
        for entry in self.baseline['firmware']:
            self.assertTrue(entry['present_in_impl_strings'],
                            f'{entry["name"]} is not referenced by the A15 vendor HAL')

    def test_dump_index_lists_every_vendor_file(self):
        if not DUMP_INDEX.exists():
            self.skipTest('dump index not fetched in this checkout')
        listed = set(read(DUMP_INDEX).splitlines())
        for entry in self.baseline['vendor_files'] + self.baseline['out_of_scope_vendor_files']:
            self.assertIn(entry['path'], listed, f'{entry["path"]} missing from dump index')

    def test_closure_is_closed(self):
        closure = self.baseline['closure']
        if not closure:
            self.skipTest('work/marble-bluetooth-closure.json not generated')
        self.assertEqual(closure['unresolved'], {}, 'vendor BLUETOOTH closure has holes')
        self.assertGreaterEqual(closure['files'], 20)

    def test_donor_side_objects_exist(self):
        for entry in self.baseline['ohos_side_donor_files']:
            self.assertTrue(entry['exists'], f'donor file missing: {entry["path"]}')


class InitCfgContractTests(unittest.TestCase):
    def setUp(self):
        self.cfg = json.loads(read(INIT_CFG))
        self.services = {service['name']: service for service in self.cfg['services']}
        self.jobs = {job['name']: job for job in self.cfg['jobs']}

    def test_is_json_object_with_services_and_jobs(self):
        self.assertIn('services', self.cfg)
        self.assertIn('jobs', self.cfg)
        self.assertTrue(self.cfg['services'])
        self.assertTrue(self.cfg['jobs'])

    def test_bluetooth_service_starts_sa_1130(self):
        service = self.services['bluetooth_service']
        self.assertEqual(service['path'][0], '/system/bin/sa_main')
        self.assertEqual(service['path'][1], '/system/profile/bluetooth_service.json')
        self.assertEqual(service['uid'], 'bluetooth')
        self.assertEqual(service['secon'], 'u:r:bluetooth_service:s0')
        self.assertTrue(service['disabled'])
        self.assertIn('ohos.permission.MANAGE_BLUETOOTH', service['permission'])
        self.assertIn('CAP_NET_ADMIN', service['caps'])

    def test_hdi_publisher_precedes_service(self):
        service = self.services['hmos_bluetooth_hdi']
        self.assertEqual(service['path'][0], '/compat/bin/hmos_bt_hdi_publisher_marble')
        self.assertEqual(service['uid'], 'blue_host')
        cmds = self.jobs['param:marble-bluetooth']['cmds']
        self.assertLess(cmds.index('start hmos_bluetooth_hdi'),
                        cmds.index('start bluetooth_service'))

    def test_job_condition_matches_start_script_param(self):
        script = read(SCRIPT)
        self.assertIn('marble.debug.start.bluetooth true', script)
        self.assertEqual(self.jobs['param:marble-bluetooth']['condition'],
                         'marble.debug.start.bluetooth=true')

    def test_socket_owner_matches_script_expectation(self):
        script = read(SCRIPT)
        self.assertIn('BT_BLUE_HOST_GID=${BT_BLUE_HOST_GID:-3021}', script)
        self.assertIn('660:0:$BT_BLUE_HOST_GID', script)
        baseline = load_baseline()
        if baseline is not None:
            self.assertEqual(baseline['socket']['owner'], '0:3021')

    def test_donor_users_and_groups_exist(self):
        passwd = DONOR / 'system/system/etc/passwd'
        group = DONOR / 'system/system/etc/group'
        if not passwd.exists() or not group.exists():
            self.skipTest('donor ROM tree not unpacked in this checkout')
        users = {line.split(':')[0] for line in read(passwd).splitlines() if line}
        groups = {line.split(':')[0] for line in read(group).splitlines() if line}
        for service in self.cfg['services']:
            self.assertIn(service['uid'], users, f'{service["name"]} uid not in donor passwd')
            for name in service['gid']:
                self.assertIn(name, groups, f'{service["name"]} gid {name} not in donor group')

    def test_donor_service_profile_declares_sa_1130(self):
        profile = DONOR / 'system/system/profile/bluetooth_service.json'
        if not profile.exists():
            self.skipTest('donor bluetooth profile not present')
        doc = json.loads(read(profile))
        self.assertEqual(doc['process'], 'bluetooth_service')
        self.assertEqual(doc['systemability'][0]['name'], 1130)
        self.assertIn('libbluetooth_hci_proxy_1.0.z.so',
                      doc['systemability'][0]['min_hdi_proxy_version'])


class BridgeArtifactTests(unittest.TestCase):
    """Optional: only when the marble Bluetooth targets were built locally."""

    def test_shared_library_exports_contract(self):
        library = BUILD_DIR / 'libhmos_bt_bridge_marble.so'
        baseline = load_baseline()
        if not library.exists() or baseline is None:
            self.skipTest('bridge library not built in this checkout')
        header = subprocess.run(['readelf', '-h', str(library)], capture_output=True, text=True,
                                check=True).stdout
        self.assertIn('AArch64', header)
        symbols = subprocess.run(['nm', '-D', '--defined-only', str(library)],
                                 capture_output=True, text=True, check=True).stdout
        exported = {line.split()[-1] for line in symbols.splitlines() if line.strip()}
        missing = set(baseline['bridge_exports']) - exported
        self.assertEqual(missing, set(), f'bridge library misses {sorted(missing)}')

    def test_bridge_binary_and_vendor_lib_names(self):
        baseline = load_baseline()
        if baseline is None:
            self.skipTest('baseline missing')
        artifacts = baseline['artifacts']
        for name in (artifacts['bridge_binary'], artifacts['hdi_publisher'],
                     artifacts['vendor_lib']):
            self.assertIn(name, read(BRIDGE_SRC) + read(VENDOR_SRC) + read(SCRIPT)
                          + read(BUILD_SCRIPT))


class BuildTargetTests(unittest.TestCase):
    def setUp(self):
        self.text = read(BUILD_SCRIPT)

    def test_build_script_parses(self):
        ast.parse(self.text)

    def test_marble_bluetooth_targets_registered(self):
        for target in ('bluetooth-bridge-marble', 'bluetooth-bridge-marble-lib',
                       'bluetooth-vendor-marble', 'bluetooth-hdi-marble'):
            self.assertIn(f"'{target}'", self.text, f'{target} missing from build_current.py')

    def test_bridge_source_is_pure_c(self):
        text = read(BRIDGE_SRC)
        self.assertNotIn('#include <c', text)
        self.assertNotIn('namespace ', text)
        self.assertIn('-DHMOS_BT_BRIDGE_NO_MAIN', self.text)


if __name__ == '__main__':
    unittest.main()
