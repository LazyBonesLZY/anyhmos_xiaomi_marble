#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Offline unit tests for Xiaomi marble M4 Wi-Fi Hotspot / SoftAP stack.

Verifies:
  1. Clean-room C shim compiles with -Wall -Wextra -Werror and passes all assertions.
  2. Dual STA+SoftAP work/marble-m4-build/wifi/libwpa.z.so ELF contract:
     EM_AARCH64, SONAME=libwpa.z.so, minimal NEEDED (libnl_share.z.so + libc.so),
     verdef 1.0, exports wpa_main@@1.0 + ap_main@@1.0 + getHostapd@@1.0,
     full strong-UND closure for donor libhostapd_hdi_c_device.z.so,
     libhostapd_interface_service_1.0.z.so, and libwpa_interface_service_2.0.z.so.
  3. Runtime init-m4-softap-marble.cfg, hostapd-marble.conf, and
     start-softap-m4-marble.sh (including offline BusyBox fixture execution).
  4. Exact HCB node byte-identity check against donor ADY-AL10 hdf_default.hcb.
"""
from __future__ import annotations

import json
import os
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SDK_BIN = ROOT / 'work/ohos-sdk/installed/native/llvm/bin'
DONOR_FS = ROOT / 'work/donor-rom/ADY-AL10-207.0.0.111-SP10/fs'
BUILD_DIR = ROOT / 'work/marble-m4-build/wifi'
LIBWPA = BUILD_DIR / 'libwpa.z.so'
WPA_BIN = BUILD_DIR / 'wpa_supplicant'
HAPD_BIN = BUILD_DIR / 'hostapd'
BUILD_SCRIPT = ROOT / 'compat/wifi/build_wifi_softap.sh'
SHIM_C = ROOT / 'compat/wifi/wpa_softap_hdi_shim.c'
SHIM_TEST_C = ROOT / 'compat/wifi/test_wifi_softap_shim.c'
INIT_CFG = ROOT / 'devices/marble/runtime/wifi/init-m4-softap-marble.cfg'
HOSTAPD_CONF = ROOT / 'devices/marble/runtime/wifi/hostapd-marble.conf'
START_SH = ROOT / 'devices/marble/runtime/wifi/start-softap-m4-marble.sh'


def _elf_machine(path: Path) -> int:
    head = path.read_bytes()[:20]
    if head[:4] != b'\x7fELF' or head[4] != 2:
        raise AssertionError(f'{path} is not ELF64')
    return struct.unpack('<H', head[18:20])[0]


def _hcb_val(kind: str, value) -> bytes:
    if kind == 'u8':
        return b'\x10' + struct.pack('<B', value)
    if kind == 'u16':
        return b'\x11' + struct.pack('<H', value)
    if kind == 'u32':
        return b'\x12' + struct.pack('<I', value)
    if kind == 'str':
        return b'\x14' + value.encode('utf-8') + b'\x00'
    if kind == 'arr':
        body = b''.join(_hcb_val(k, v) for k, v in value)
        return b'\x04' + struct.pack('<H', len(value)) + body
    raise ValueError(kind)


def _hcb_term(name: str, kind: str, value) -> bytes:
    return b'\x02' + name.encode('utf-8') + b'\x00' + _hcb_val(kind, value)


def _hcb_node(name: str, children: list[bytes]) -> bytes:
    body = b''.join(children)
    return b'\x01' + name.encode('utf-8') + b'\x00' + struct.pack('<I', len(body)) + body


class SoftapShimTest(unittest.TestCase):
    def test_host_shim_compiles_cleanly_and_passes(self):
        with tempfile.TemporaryDirectory(prefix='marble-softap-shim-') as tmp:
            exe = Path(tmp) / 'test_wifi_softap_shim'
            subprocess.run(
                [
                    'cc', '-Wall', '-Wextra', '-Werror', '-O2',
                    f'-I{ROOT / "compat/wifi"}',
                    str(SHIM_C), str(SHIM_TEST_C),
                    '-lpthread', '-ldl', '-o', str(exe),
                ],
                check=True,
            )
            res = subprocess.run([str(exe)], capture_output=True, text=True, check=True)
            self.assertIn('TEST_WIFI_SOFTAP_SHIM=PASS', res.stdout)


class SoftapElfContractTest(unittest.TestCase):
    def test_libwpa_elf_exports_and_minimal_needed(self):
        self.assertTrue(LIBWPA.is_file(), f'Missing {LIBWPA}')
        self.assertEqual(_elf_machine(LIBWPA), 0xB7)
        self.assertEqual(_elf_machine(WPA_BIN), 0xB7)
        self.assertEqual(_elf_machine(HAPD_BIN), 0xB7)

        readelf = str(SDK_BIN / 'llvm-readelf')
        nm = str(SDK_BIN / 'llvm-nm')

        dyn = subprocess.check_output([readelf, '-d', str(LIBWPA)], text=True)
        needed = [
            line.split('[', 1)[1].split(']', 1)[0]
            for line in dyn.splitlines()
            if '(NEEDED)' in line and '[' in line
        ]
        self.assertEqual(needed, ['libnl_share.z.so', 'libc.so'])
        self.assertIn('Library soname: [libwpa.z.so]', dyn)

        ver = subprocess.check_output([readelf, '-V', str(LIBWPA)], text=True)
        self.assertIn('Name: 1.0', ver)

        defs = {
            line.split()[-1]
            for line in subprocess.check_output(
                [nm, '-D', '--defined-only', str(LIBWPA)], text=True
            ).splitlines()
            if line.strip()
        }
        self.assertEqual(defs, {'wpa_main@@1.0', 'ap_main@@1.0', 'getHostapd@@1.0'})

    def test_donor_hostapd_and_wpa_hdi_und_closure(self):
        if not DONOR_FS.is_dir():
            self.skipTest('donor FS not extracted')
        readelf = str(SDK_BIN / 'llvm-readelf')
        nm = str(SDK_BIN / 'llvm-nm')
        search_dirs = [
            DONOR_FS / 'vendor/lib64',
            DONOR_FS / 'vendor/lib64/chipsetsdk',
            DONOR_FS / 'system/system/lib64/chipset-sdk',
            DONOR_FS / 'system/system/lib64/chipset-sdk-sp',
            DONOR_FS / 'system/system/lib64/chipset-pub-sdk',
            DONOR_FS / 'system/system/lib64/ndk',
            DONOR_FS / 'system/system/lib64',
        ]

        def locate(soname: str) -> Path:
            if soname == 'libwpa.z.so':
                return LIBWPA
            for d in search_dirs:
                cand = d / soname
                if cand.is_file():
                    return cand
            raise AssertionError(f'Cannot locate NEEDED library {soname}')

        for name in (
            'libhostapd_hdi_c_device.z.so',
            'libhostapd_interface_service_1.0.z.so',
            'libwpa_interface_service_2.0.z.so',
        ):
            target = DONOR_FS / 'vendor/lib64' / name
            dyn = subprocess.check_output([readelf, '-d', str(target)], text=True)
            needed = [
                line.split('[', 1)[1].split(']', 1)[0]
                for line in dyn.splitlines()
                if '(NEEDED)' in line and '[' in line
            ]
            defined: set[str] = set()
            for lib in needed:
                p = locate(lib)
                for line in subprocess.check_output(
                    [nm, '-D', '--defined-only', str(p)], text=True
                ).splitlines():
                    parts = line.split()
                    if parts:
                        s = parts[-1]
                        defined.add(s)
                        defined.add(s.replace('@@', '@'))
                        defined.add(s.split('@')[0])
            unresolved = []
            for line in subprocess.check_output([nm, '-D', '-u', str(target)], text=True).splitlines():
                parts = line.split()
                if len(parts) >= 2 and parts[-2] != 'w':
                    sym = parts[-1]
                    if sym not in defined and sym.split('@')[0] not in defined:
                        unresolved.append(sym)
            self.assertEqual(unresolved, [], f'{name} unresolved strong UND: {unresolved}')


class RuntimeConfigAndStartScriptTest(unittest.TestCase):
    def test_scripts_parse(self):
        subprocess.run(['bash', '-n', str(BUILD_SCRIPT)], check=True)
        subprocess.run(['bash', '-n', str(START_SH)], check=True)
        sh = shutil.which('sh') or shutil.which('busybox')
        if sh:
            cmd = [sh, 'sh', '-n', str(START_SH)] if sh.endswith('busybox') else [sh, '-n', str(START_SH)]
            subprocess.run(cmd, check=True)

    def test_init_cfg_schema_and_host_index_16(self):
        doc = json.loads(INIT_CFG.read_text(encoding='utf-8'))
        jobs = {j['name']: j for j in doc['jobs']}
        self.assertIn('marble-softap-dirs', jobs)
        self.assertIn(
            'mkdir /data/service/el1/public/wifi/sockets/wpa 0770 wifi wifi_group',
            jobs['marble-softap-dirs']['cmds'],
        )
        self.assertIn('param:marble-softap-ready', jobs)
        self.assertEqual(jobs['param:marble-softap-ready']['condition'], 'marble.softap.ready=true')
        self.assertIn('start hostapd_host', jobs['param:marble-softap-ready']['cmds'])

        svcs = {s['name']: s for s in doc['services']}
        self.assertIn('hostapd_host', svcs)
        host = svcs['hostapd_host']
        self.assertEqual(host['path'], ['/vendor/bin/hdf_devhost', '-i', '16', '-n', 'hostapd_host'])
        self.assertEqual(host['uid'], 'wifi')
        self.assertEqual(host['gid'], ['wifi', 'wifi_group', 'wifi_host'])
        self.assertEqual(host['caps'], ['DAC_OVERRIDE', 'DAC_READ_SEARCH', 'NET_ADMIN', 'NET_RAW'])
        self.assertEqual(host['secon'], 'u:r:wifi_host:s0')
        self.assertTrue(host['ondemand'])

    def test_hostapd_conf_template(self):
        pairs = {}
        for line in HOSTAPD_CONF.read_text(encoding='utf-8').splitlines():
            s = line.strip()
            if not s or s.startswith('#'):
                continue
            k, v = s.split('=', 1)
            pairs[k.strip()] = v.strip()
        self.assertEqual(pairs['interface'], 'wlan0')
        self.assertEqual(pairs['driver'], 'nl80211')
        self.assertEqual(pairs['ctrl_interface'], '/data/service/el1/public/wifi/sockets/wpa')
        self.assertEqual(pairs['hw_mode'], 'g')
        self.assertEqual(pairs['ieee80211n'], '1')
        self.assertEqual(pairs['wmm_enabled'], '1')

    def test_start_script_offline_fixture(self):
        busybox = shutil.which('busybox')
        if not busybox:
            self.skipTest('host busybox required')
        with tempfile.TemporaryDirectory(prefix='marble-softap-start-') as tmp:
            base = Path(tmp)
            root = base / 'hmos-root'
            for rel in (
                'vendor/lib64/libhostapd_hdi_c_device.z.so',
                'vendor/lib64/libhostapd_interface_service_1.0.z.so',
                'vendor/lib64/libwpa.z.so',
            ):
                p = root / rel
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_bytes(b'\x7fELF')
            (root / 'compat/etc').mkdir(parents=True, exist_ok=True)
            shutil.copyfile(HOSTAPD_CONF, root / 'compat/etc/hostapd-marble.conf')
            param_bin = root / 'system/bin/param'
            param_bin.parent.mkdir(parents=True, exist_ok=True)
            param_bin.write_text('#!/bin/sh\nexit 0\n', encoding='utf-8')
            param_bin.chmod(0o755)
            ip_fwd = base / 'ip_forward'
            ip_fwd.write_text('0\n', encoding='utf-8')
            log_file = base / 'mock.log'

            # Wrapper simulates chroot/chown/pgrep on unprivileged host
            wrapper = base / 'busybox'
            wrapper.write_text(
                '#!' + sys.executable + '\n' + '''
import os, sys
box, verb, *args = [os.environ['REAL_BB'], *sys.argv[1:]]
log = os.environ['MOCK_LOG']
if verb == 'chown':
    with open(log, 'a') as fh: fh.write('chown ' + ' '.join(args) + '\\n')
    sys.exit(0)
if verb == 'chroot':
    with open(log, 'a') as fh: fh.write('chroot ' + ' '.join(args) + '\\n')
    sys.exit(0)
if verb == 'pgrep':
    if os.environ.get('MOCK_PGREP_RUNNING') == '1':
        print('4242')
        sys.exit(0)
    sys.exit(1)
os.execv(box, [box, verb, *args])
''',
                encoding='utf-8',
            )
            wrapper.chmod(0o755)

            env = {
                'PATH': os.environ.get('PATH', '/usr/bin:/bin'),
                'REAL_BB': busybox,
                'MOCK_LOG': str(log_file),
                'MARBLE_BB': str(wrapper),
                'MARBLE_ROOT': str(root),
                'MARBLE_IP_FORWARD_PATH': str(ip_fwd),
                'MOCK_PGREP_RUNNING': '1',
            }

            # 1) Fresh run seeds hostapd.conf, enables ip_forward, triggers param
            stale_sock = root / 'data/service/el1/public/wifi/sockets/wpa/hostapd'
            stale_sock.parent.mkdir(parents=True, exist_ok=True)
            stale_sock.write_text('stale', encoding='utf-8')

            r1 = subprocess.run([busybox, 'sh', str(START_SH)], env=env, capture_output=True, text=True, check=True)
            self.assertIn('SOFTAP_M4_IP_FORWARD=1', r1.stdout)
            self.assertIn('SOFTAP_M4_CONF=seed', r1.stdout)
            self.assertIn('SOFTAP_M4_PROCS host_pid=4242', r1.stdout)
            self.assertIn('SOFTAP_M4_DONE=1', r1.stdout)
            self.assertEqual(ip_fwd.read_text().strip(), '1')

            dst_conf = root / 'data/service/el1/public/wifi/wpa_supplicant/hostapd.conf'
            self.assertEqual(dst_conf.read_bytes(), HOSTAPD_CONF.read_bytes())
            self.assertEqual(stat.S_IMODE(dst_conf.stat().st_mode), 0o660)
            self.assertEqual(stat.S_IMODE((root / 'data/service/el1/public/wifi/sockets/wpa').stat().st_mode), 0o770)
            self.assertIn('/system/bin/param set marble.softap.ready true', log_file.read_text())

            # 2) Repeat run keeps existing user-modified hostapd.conf
            dst_conf.write_text('interface=wlan0\nssid=CustomUserAP\n', encoding='utf-8')
            r2 = subprocess.run([busybox, 'sh', str(START_SH)], env=env, capture_output=True, text=True, check=True)
            self.assertIn('SOFTAP_M4_CONF=keep', r2.stdout)
            self.assertEqual(dst_conf.read_text(), 'interface=wlan0\nssid=CustomUserAP\n')

            # 3) Skip stage respected
            skip_file = root / 'data/local/tmp/marble-skip-stages'
            skip_file.parent.mkdir(parents=True, exist_ok=True)
            skip_file.write_text('nfc,softap\n', encoding='utf-8')
            r3 = subprocess.run([busybox, 'sh', str(START_SH)], env=env, capture_output=True, text=True, check=True)
            self.assertIn('SOFTAP_M4_SKIP=requested', r3.stdout)


class HcbNodeByteIdentityTest(unittest.TestCase):
    def test_hostapd_hcb_node_matches_donor_ady_al10_byte_for_byte(self):
        donor_hcb_path = DONOR_FS / 'chip_prod/etc/hdfconfig/hdf_default.hcb'
        if not donor_hcb_path.is_file():
            self.skipTest('donor hdf_default.hcb not present')
        donor_hcb = donor_hcb_path.read_bytes()

        hostapd_dev = _hcb_node('wpa_device', [_hcb_node('device0', [
            _hcb_term('policy', 'u8', 2),
            _hcb_term('preload', 'u8', 2),
            _hcb_term('priority', 'u8', 100),
            _hcb_term('moduleName', 'str', 'libhostapd_hdi_c_device.z.so'),
            _hcb_term('serviceName', 'str', 'hostapd_interface_service'),
            _hcb_term('permission', 'u16', 436),
            _hcb_term('deviceMatchAttr', 'str', ''),
        ])])
        hostapd_host = _hcb_node('hostapd', [
            _hcb_term('hostName', 'str', 'hostapd_host'),
            _hcb_term('priority', 'u8', 50),
            _hcb_term('caps', 'arr', [
                ('str', 'DAC_OVERRIDE'),
                ('str', 'DAC_READ_SEARCH'),
                ('str', 'NET_ADMIN'),
                ('str', 'NET_RAW'),
            ]),
            _hcb_term('initconfig', 'arr', [('str', '"secon" : "u:r:wifi_host:s0"')]),
            _hcb_term('uid', 'str', 'wifi'),
            _hcb_term('gid', 'arr', [('str', 'wifi'), ('str', 'wifi_group'), ('str', 'wifi_host')]),
            hostapd_dev,
        ])
        self.assertEqual(
            donor_hcb.count(hostapd_dev), 1,
            'Expected byte-identical donor hostapd wpa_device subnode',
        )
        self.assertEqual(
            donor_hcb.count(hostapd_host), 1,
            'Expected byte-identical donor hostapd host node',
        )


if __name__ == '__main__':
    unittest.main()
