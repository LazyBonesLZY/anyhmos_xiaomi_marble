import importlib.util
import json
import re
import struct
import subprocess
import sys
import tempfile
import unittest
from hashlib import sha256
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/packaging'))

import build_marble_h20_f2_telephony as pkg  # noqa: E402
import build_marble_phase4_images as p4  # noqa: E402


def _read_cstr(buf: bytes, pos: int) -> tuple[str, int]:
    end = buf.index(0, pos)
    return buf[pos:end].decode('utf-8'), end + 1


def _parse_val(buf: bytes, pos: int) -> tuple[object, int]:
    tag = buf[pos]
    pos += 1
    if tag == 0x10:
        return struct.unpack_from('<B', buf, pos)[0], pos + 1
    if tag == 0x11:
        return struct.unpack_from('<H', buf, pos)[0], pos + 2
    if tag == 0x12:
        return struct.unpack_from('<I', buf, pos)[0], pos + 4
    if tag == 0x13:
        return struct.unpack_from('<Q', buf, pos)[0], pos + 8
    if tag == 0x14:
        return _read_cstr(buf, pos)
    if tag == 0x04:
        count = struct.unpack_from('<H', buf, pos)[0]
        pos += 2
        items = []
        for _ in range(count):
            val, pos = _parse_val(buf, pos)
            items.append(val)
        return items, pos
    raise AssertionError(f'Unsupported HCB value tag {hex(tag)} at offset {pos - 1}')


def _parse_node(buf: bytes, pos: int, end: int) -> tuple[dict, int]:
    assert buf[pos] == 0x01
    name, pos = _read_cstr(buf, pos + 1)
    body_len = struct.unpack_from('<I', buf, pos)[0]
    pos += 4
    body_end = pos + body_len
    assert body_end <= end
    terms, children = {}, {}
    while pos < body_end:
        t = buf[pos]
        if t == 0x01:
            child, pos = _parse_node(buf, pos, body_end)
            children[child['name']] = child
        elif t == 0x02:
            tname, pos = _read_cstr(buf, pos + 1)
            tval, pos = _parse_val(buf, pos)
            terms[tname] = tval
        else:
            raise AssertionError(f'Unknown HCB item tag {hex(t)}')
    assert pos == body_end
    return {'name': name, 'terms': terms, 'children': children}, pos


def parse_hcb(blob: bytes) -> dict:
    magic, v0, v1, v2, root_len = struct.unpack_from('<IIIII', blob, 0)
    assert magic == 0xA00AA00A and (v0, v1, v2) == (0, 7, 0) and 20 + root_len == len(blob)
    node, end_pos = _parse_node(blob, 20, len(blob))
    assert end_pos == len(blob)
    return node

LLVM_READELF = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-readelf'
LLVM_NM = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-nm'
CLANG_OHOS = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/aarch64-unknown-linux-ohos-clang'


class MarbleBatchF2ModemTests(unittest.TestCase):
    def test_host_self_tests(self):
        with tempfile.TemporaryDirectory() as td:
            tp = Path(td)
            probe_host = tp / 'marble_modem_probe_host'
            ril_host = tp / 'test_ril_vendor_qmi_host'
            subprocess.run(
                ['gcc', '-O2', '-Wall', '-Wextra', '-Werror', '-std=c11', '-D_GNU_SOURCE',
                 str(ROOT / 'compat/telephony/marble_modem_probe.c'), '-o', str(probe_host)],
                check=True,
            )
            p_res = subprocess.run([str(probe_host), '--self-test'], check=True, capture_output=True, text=True)
            self.assertIn('MODEM_PROBE_SELF_TEST_OK=1', p_res.stdout)

            subprocess.run(
                ['gcc', '-O2', '-Wall', '-Wextra', '-Werror', '-std=c11', '-D_GNU_SOURCE', '-DHRIL_VENDOR_HOST_TEST',
                 str(ROOT / 'compat/telephony/libril_vendor_qmi.c'), '-lpthread', '-o', str(ril_host)],
                check=True,
            )
            r_res = subprocess.run([str(ril_host)], check=True, capture_output=True, text=True)
            self.assertIn('RIL_VENDOR_QMI_HOST_TEST_OK=1', r_res.stdout)

    def test_aarch64_musl_ril_so_and_static_probe(self):
        self.assertTrue(pkg.RIL_VENDOR_SO.is_file())
        self.assertTrue(pkg.RIL_LOADER_BIN.is_file())
        self.assertTrue(pkg.PROBE_BIN.is_file())
        dyn = subprocess.check_output([str(LLVM_READELF), '-d', str(pkg.RIL_VENDOR_SO)], text=True)
        needed = re.findall(r'\(NEEDED\)\s+Shared library:\s+\[([^\]]+)\]', dyn)
        self.assertEqual(needed, ['libc.so'])
        soname = re.findall(r'\(SONAME\)\s+Library soname:\s+\[([^\]]+)\]', dyn)
        self.assertEqual(soname, ['libril_vendor.z.so'])

        ldyn = subprocess.check_output([str(LLVM_READELF), '-d', str(pkg.RIL_LOADER_BIN)], text=True)
        lneeded = re.findall(r'\(NEEDED\)\s+Shared library:\s+\[([^\]]+)\]', ldyn)
        self.assertEqual(lneeded, ['libc.so'])

        syms = subprocess.check_output(
            [str(LLVM_NM), '-D', '--defined-only', str(pkg.RIL_VENDOR_SO)], text=True
        )
        self.assertIn('RilInitOps', syms)
        self.assertNotIn('__packed', syms)

        probe_file = subprocess.check_output(['file', str(pkg.PROBE_BIN)], text=True)
        self.assertIn('ARM aarch64', probe_file)
        self.assertIn('statically linked', probe_file)
        self.assertNotIn(b'ARM64 Bionic', pkg.PROBE_BIN.read_bytes())

    def test_seven_host_hcb_preserves_six_host_identity(self):
        if not pkg.DONOR_FS.is_dir():
            self.skipTest(f'Missing donor fs: {pkg.DONOR_FS}')
        h6 = p4.build_phase4_hdf_hcb(
            wifi_chip_donor=pkg.DONOR_FS, wpa_on_demand_donor=pkg.DONOR_FS, a2dp_donor=pkg.DONOR_FS
        )
        self.assertEqual(len(h6), 2381)

        h7 = p4.build_phase4_hdf_hcb(
            wifi_chip_donor=pkg.DONOR_FS,
            wpa_on_demand_donor=pkg.DONOR_FS,
            a2dp_donor=pkg.DONOR_FS,
            ril_donor=pkg.DONOR_FS,
        )
        root = parse_hcb(h7)
        hosts = list(root['children']['device_info']['children'].values())
        self.assertEqual(
            [h['terms']['hostName'] for h in hosts],
            ['allocator_host', 'composer_host', 'hmos_userauth_host', 'wifi_host', 'wpa_host', 'a2dp_host', 'riladapter_host'],
        )
        ril = root['children']['device_info']['children']['riladapter']
        self.assertEqual(ril['terms']['uid'], 'radio')
        self.assertNotIn('riladapterext_device', ril['children'])
        self.assertEqual(ril['children']['ril_device']['children']['device0']['terms']['preload'], 2)
        self.assertEqual(ril['children']['riladapter_device']['children']['device0']['terms']['preload'], 2)

    def test_f2_init_profile_and_safety_invariants(self):
        cfg = json.loads(pkg.INIT_F2_CFG.read_text(encoding='utf-8'))
        svcs = {s['name']: s for s in cfg['services']}
        self.assertEqual(set(svcs), {'riladapter_host', 'telephony_sa'})
        for name, svc in svcs.items():
            self.assertEqual(svc['disabled'], 1, name)
            self.assertEqual(svc['sandbox'], 0, name)
            self.assertEqual(svc['start-mode'], 'condition', name)
        self.assertEqual(svcs['riladapter_host']['path'], ['/vendor/bin/hdf_devhost', '-i', '6', '-n', 'riladapter_host'])
        self.assertEqual(svcs['telephony_sa']['path'], ['/system/bin/sa_main', '/compat/etc/telephony-marble.json'])

        prof = json.loads(pkg.TELEPHONY_JSON.read_text(encoding='utf-8'))
        self.assertEqual(prof['process'], 'telephony')
        sa_ids = [item['name'] for item in prof['systemability']]
        self.assertEqual(sa_ids, [4010, 4007, 4008])

        script = pkg.START_F2_SH.read_text(encoding='utf-8')
        self.assertTrue(script.startswith('#!/busybox sh\n'))
        self.assertIn('bb sh "$F1_START"', script)
        self.assertIn('name=dms', script)
        self.assertIn('native_ril_hdi_loader --load', script)
        self.assertIn('NATIVE_SA_READY id=4010', script)
        subprocess.run(['sh', '-n', str(pkg.START_F2_SH)], check=True)
        for bad in ('remount,rw', 'efs1', 'efs2', 'modemst', 'fsg', 'audio_vdi', 'pal_server'):
            self.assertNotIn(bad, script)

    def test_f2_package_roundtrip_on_f1_base(self):
        if not pkg.BASE_DEFAULT.is_file():
            self.skipTest(f'Missing F1 base image: {pkg.BASE_DEFAULT}')
        scratch = '/mnt/e/marble-tmp' if Path('/mnt/e/marble-tmp').is_dir() else None
        with tempfile.TemporaryDirectory(dir=scratch) as td:
            out_dir = Path(td)
            out_img = pkg.build(pkg.BASE_DEFAULT, out_dir)
            self.assertTrue(out_img.is_file())
            self.assertEqual(out_img.stat().st_size, 100663296)
            report = json.loads((out_dir / 'f2-build-report.json').read_text(encoding='utf-8'))
            self.assertEqual(report['entries_before'], 523)
            self.assertEqual(report['entries_after'], 528)
            self.assertEqual(report['unchanged_entries'], 518)
            self.assertEqual(
                set(report['added_entries']),
                {
                    'graphics/native_ril_hdi_loader',
                    'graphics/libril_vendor_qmi.z.so',
                    'graphics/init-modem-f2-marble.cfg',
                    'graphics/telephony-marble.json',
                    'graphics/start-modem-f2-marble.sh',
                },
            )
            self.assertEqual(set(report['guards_unchanged']), set(pkg.GUARD_MEMBERS))


if __name__ == '__main__':
    unittest.main()
