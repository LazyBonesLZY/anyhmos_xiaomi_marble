import importlib.util
import json
import struct
import tempfile
import unittest
from hashlib import sha256
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/packaging'))
import build_marble_h20_e_bt_audio as pkg  # noqa: E402
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
    tag = buf[pos]
    assert tag == 0x01, f'Expected node tag 0x01 at {pos}, got {hex(tag)}'
    name, pos = _read_cstr(buf, pos + 1)
    body_len = struct.unpack_from('<I', buf, pos)[0]
    pos += 4
    body_end = pos + body_len
    assert body_end <= end, f'Node {name} overruns parent end ({body_end} > {end})'
    terms, children = {}, {}
    while pos < body_end:
        child_tag = buf[pos]
        if child_tag == 0x01:
            child_node, pos = _parse_node(buf, pos, body_end)
            children[child_node['name']] = child_node
        elif child_tag == 0x02:
            tname, pos = _read_cstr(buf, pos + 1)
            tval, pos = _parse_val(buf, pos)
            terms[tname] = tval
        else:
            raise AssertionError(f'Unknown HCB item tag {hex(child_tag)} inside {name}')
    assert pos == body_end, f'Node {name} length mismatch ({pos} != {body_end})'
    return {'name': name, 'terms': terms, 'children': children}, pos


def parse_hcb(blob: bytes) -> dict:
    magic, v0, v1, v2, root_len = struct.unpack_from('<IIIII', blob, 0)
    assert magic == 0xA00AA00A, f'Bad HCB magic: {hex(magic)}'
    assert (v0, v1, v2) == (0, 7, 0), f'Unexpected HCB header ints: {(v0, v1, v2)}'
    assert 20 + root_len == len(blob), f'HCB length mismatch: 20+{root_len} != {len(blob)}'
    root_node, end_pos = _parse_node(blob, 20, len(blob))
    assert end_pos == len(blob)
    return root_node


class MarbleBatchEBtAudioTests(unittest.TestCase):
    def test_hcb_six_hosts_and_a2dp_node(self):
        if not pkg.DONOR_FS.is_dir():
            self.skipTest(f'Missing donor fs: {pkg.DONOR_FS}')
        hcb5 = p4.build_phase4_hdf_hcb(
            wifi_chip_donor=pkg.DONOR_FS,
            wpa_on_demand_donor=pkg.DONOR_FS,
            a2dp_donor=None,
        )
        self.assertEqual(sha256(hcb5).hexdigest(), pkg.OLD_HCB_SHA)

        hcb6 = p4.build_phase4_hdf_hcb(
            wifi_chip_donor=pkg.DONOR_FS,
            wpa_on_demand_donor=pkg.DONOR_FS,
            a2dp_donor=pkg.DONOR_FS,
        )
        self.assertEqual(len(hcb6), 2381)

        root = parse_hcb(hcb6)
        self.assertEqual(root['name'], 'root')
        dev_info = root['children']['device_info']
        host_names = [node['terms']['hostName'] for node in dev_info['children'].values()]
        self.assertEqual(
            host_names,
            ['allocator_host', 'composer_host', 'hmos_userauth_host', 'wifi_host', 'wpa_host', 'a2dp_host'],
        )

        audio_bt = dev_info['children']['audio_bluetooth']
        self.assertEqual(audio_bt['terms']['hostName'], 'a2dp_host')
        self.assertEqual(audio_bt['terms']['caps'], ['SYS_NICE'])
        self.assertEqual(audio_bt['terms']['initconfig'], ['"importance" : -20'])
        self.assertEqual(audio_bt['terms']['uid'], '')
        self.assertEqual(audio_bt['terms']['gid'], [''])

        bt_dev0 = audio_bt['children']['bluetooth_device']['children']['device0']['terms']
        self.assertEqual(bt_dev0['serviceName'], 'audio_bluetooth_hdi_service')
        self.assertEqual(bt_dev0['moduleName'], 'libaudio_bluetooth_hdi_adapter_server.z.so')
        self.assertEqual(bt_dev0['preload'], 2)

        sess_dev0 = audio_bt['children']['a2dp_interface_device']['children']['device0']['terms']
        self.assertEqual(sess_dev0['serviceName'], 'bluetooth_audio_session_service')
        self.assertEqual(sess_dev0['moduleName'], 'libbluetooth_a2dp_hdi_driver.z.so')
        self.assertEqual(sess_dev0['preload'], 2)

        donor_hcb = (pkg.DONOR_FS / p4.WIFI_CHIP_DONOR_HCB).read_bytes()
        expected_node = p4._phase4_a2dp_host(pkg.DONOR_FS)
        self.assertEqual(donor_hcb.count(expected_node), 1)
        self.assertEqual(hcb6.count(expected_node), 1)

    def test_init_bluetooth_cfg_a2dp_host(self):
        cfg = json.loads(pkg.BT_CFG.read_text(encoding='utf-8'))
        svc_names = [s['name'] for s in cfg['services']]
        self.assertEqual(
            svc_names,
            [
                'hmos_bluetooth_hdi',
                'bluetooth_service',
                'marble_bluetooth_check',
                'fusion_connectivity_manager_ext',
                'a2dp_host',
            ],
        )
        jobs_cmds = [cmd for j in cfg['jobs'] for cmd in j.get('cmds', [])]
        self.assertIn('stop a2dp_host', jobs_cmds)

        host = cfg['services'][-1]
        self.assertEqual(host['name'], 'a2dp_host')
        self.assertIs(host['ondemand'], True)
        self.assertEqual(host['path'], ['/vendor/bin/hdf_devhost', '-i', '5', '-n', 'a2dp_host'])
        self.assertEqual(host['uid'], 'a2dp_host')
        self.assertEqual(host['gid'], ['a2dp_host'])
        self.assertEqual(host['caps'], ['SYS_NICE'])
        self.assertEqual(host['secon'], 'u:r:a2dp_host:s0')
        self.assertEqual(host['importance'], -20)
        self.assertEqual(host['disabled'], 1)
        self.assertEqual(host['once'], 1)
        self.assertEqual(host['sandbox'], 0)
        self.assertEqual(host['start-mode'], 'condition')

    def test_package_roundtrip_on_d_base(self):
        if not pkg.BASE_DEFAULT.is_file():
            self.skipTest(f'Missing base image: {pkg.BASE_DEFAULT}')
        scratch = '/mnt/e/marble-tmp' if Path('/mnt/e/marble-tmp').is_dir() else None
        with tempfile.TemporaryDirectory(dir=scratch) as td:
            out_dir = Path(td)
            out_img = pkg.build(pkg.BASE_DEFAULT, out_dir)
            self.assertTrue(out_img.is_file())
            report = json.loads((out_dir / 'e-build-report.json').read_text(encoding='utf-8'))
            self.assertEqual(report['entries_before'], 519)
            self.assertEqual(report['entries_after'], 520)
            self.assertEqual(report['unchanged_entries'], 515)
            self.assertEqual(
                report['modified_entries'],
                [
                    'hmos-compat-marble/MANIFEST.json',
                    'hmos-compat-marble/graphics/hdf_default_phase4.hcb',
                    'hmos-compat-marble/graphics/init-bluetooth-marble.cfg',
                    'hmos-compat-marble/graphics/stage-phase4-marble.sh',
                ],
            )
            self.assertEqual(list(report['added_entries']), ['graphics/native_a2dp_hdi_probe'])

            spec = importlib.util.spec_from_file_location('arc', pkg.ORACLE)
            arc = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(arc)
            r = arc.load_tools()

            base_bytes = pkg.BASE_DEFAULT.read_bytes()
            base_entries, _, _, _, _ = r.extract_entries(
                pkg.BASE_DEFAULT, base_bytes, r.layout(base_bytes), out_dir / 'base_extract'
            )

            img_bytes = out_img.read_bytes()
            self.assertEqual(len(img_bytes), 100663296)
            entries, _, _, _, _ = r.extract_entries(
                out_img, img_bytes, r.layout(img_bytes), out_dir / 'cand_extract'
            )
            self.assertEqual(len(entries), 520)
            for member in pkg.AUDIO_GUARD_MEMBERS:
                self.assertEqual(r.sha(entries[member][1]), r.sha(base_entries[member][1]))
                self.assertEqual(report['audio_guards_unchanged'][member], r.sha(base_entries[member][1]))

            expected_hcb6 = p4.build_phase4_hdf_hcb(
                wifi_chip_donor=pkg.DONOR_FS,
                wpa_on_demand_donor=pkg.DONOR_FS,
                a2dp_donor=pkg.DONOR_FS,
            )
            self.assertEqual(entries[pkg.HCB_MEMBER][1], expected_hcb6)
            self.assertEqual(entries[pkg.BT_CFG_MEMBER][1], pkg.BT_CFG.read_bytes())
            self.assertEqual(entries[pkg.PROBE_MEMBER][1], pkg.PROBE_BIN.read_bytes())

            packed_stage = entries[pkg.STAGE_MEMBER][1].decode('utf-8')
            self.assertIn('$B cp /graphics/native_a2dp_hdi_probe "$R/compat/bin/native_a2dp_hdi_probe"', packed_stage)
            self.assertIn('$B chmod 0755 "$R/compat/bin/native_a2dp_hdi_probe"', packed_stage)


if __name__ == '__main__':
    unittest.main()
