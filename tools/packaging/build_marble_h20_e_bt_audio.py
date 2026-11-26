#!/usr/bin/env python3
"""Package single-increment H20+E A2DP Audio HDI overlay onto verified H20+D vendor_boot_a."""
import argparse
import copy
from hashlib import sha256
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile

SCRIPT = Path(__file__).resolve()
ROOT = SCRIPT.parents[2]
WORK = ROOT / 'work'
BASE_DEFAULT = Path('/mnt/e/marble-tmp/vendor_boot_h20_d_sensors_8b4c2d35.img')
BASE_SHA = '8b4c2d356dade02f9305365ef6d82c42485257bc9e41329c872fbcbc69607786'
OLD_HCB_SHA = 'a38b5846848c4cdbe9507fde2cdf75f4571543f2893e62787b98f50345274168'
ORACLE = WORK / 'marble-m15-role-storage.1v2ynS/build_m15_role_storage.py'
ORACLE_SHA = '7d4f79857a3458ffdb7c8fb86b1cfd893fd9addb02b13e7e05d7b5fa7093cfaa'

DONOR_FS = WORK / 'donor-rom/ADY-AL10-207.0.0.111-SP10/fs'
BT_CFG = ROOT / 'devices/marble/runtime/bluetooth/init-bluetooth-marble.cfg'
PROBE_BIN = WORK / 'marble-e-build/native_a2dp_hdi_probe'

HCB_REL = 'graphics/hdf_default_phase4.hcb'
BT_CFG_REL = 'graphics/init-bluetooth-marble.cfg'
STAGE_REL = 'graphics/stage-phase4-marble.sh'
PROBE_REL = 'graphics/native_a2dp_hdi_probe'

HCB_MEMBER = f'hmos-compat-marble/{HCB_REL}'
BT_CFG_MEMBER = f'hmos-compat-marble/{BT_CFG_REL}'
STAGE_MEMBER = f'hmos-compat-marble/{STAGE_REL}'
PROBE_MEMBER = f'hmos-compat-marble/{PROBE_REL}'

AUDIO_GUARD_MEMBERS = (
    'hmos-compat-marble/graphics/libhmos_audio_vdi.so',
    'hmos-compat-marble/graphics/audio-runtime/lib64/libhmos_pal_server.so',
)


def require(cond: bool, msg: str) -> None:
    if not cond:
        raise ValueError(msg)


def splice_once(data: bytes, pairs: tuple[tuple[bytes, bytes], ...], label: str) -> bytes:
    out = data
    for old, new in pairs:
        require(out.count(old) == 1, f'{label}: insertion anchor not unique (count={out.count(old)}): {old[:80]!r}')
        out = out.replace(old, new, 1)
    reverted = out
    for old, new in reversed(pairs):
        require(reverted.count(new) == 1, f'{label}: delta not separable')
        reverted = reverted.replace(new, old, 1)
    require(reverted == data, f'{label}: non-target bytes changed')
    return out


def load_p4():
    if str(SCRIPT.parent) not in sys.path:
        sys.path.insert(0, str(SCRIPT.parent))
    import build_marble_phase4_images as p4
    return p4


def validate_inputs() -> tuple[bytes, bytes, bytes]:
    require(BT_CFG.is_file(), f'Missing BT init config: {BT_CFG}')
    require(PROBE_BIN.is_file(), f'Missing A2DP HDI probe binary: {PROBE_BIN}')
    require(DONOR_FS.is_dir(), f'Missing donor rootfs: {DONOR_FS}')

    bt_cfg_bytes = BT_CFG.read_bytes()
    doc = json.loads(bt_cfg_bytes)
    svcs = doc.get('services', [])
    a2dp = [s for s in svcs if isinstance(s, dict) and s.get('name') == 'a2dp_host']
    require(len(a2dp) == 1, f'Expected 1 a2dp_host service in {BT_CFG}, found {len(a2dp)}')
    host = a2dp[0]
    require(host.get('path') == ['/vendor/bin/hdf_devhost', '-i', '5', '-n', 'a2dp_host'],
            f'Unexpected a2dp_host path: {host.get("path")}')
    require(host.get('ondemand') is True, 'a2dp_host must have ondemand=true')
    require(host.get('uid') == 'a2dp_host' and host.get('gid') == ['a2dp_host'], 'Unexpected a2dp_host uid/gid')
    require(host.get('secon') == 'u:r:a2dp_host:s0', 'Unexpected a2dp_host secon')
    jobs_cmds = [cmd for j in doc.get('jobs', []) for cmd in j.get('cmds', [])]
    require('stop a2dp_host' in jobs_cmds, 'Expected stop a2dp_host in init-bluetooth-marble.cfg jobs')

    probe_bytes = PROBE_BIN.read_bytes()
    require(probe_bytes[:4] == b'\x7fELF', f'Expected ELF header in {PROBE_BIN}')

    p4 = load_p4()
    hcb5 = p4.build_phase4_hdf_hcb(wifi_chip_donor=DONOR_FS, wpa_on_demand_donor=DONOR_FS, a2dp_donor=None)
    require(sha256(hcb5).hexdigest() == OLD_HCB_SHA, '5-host HCB regression detected')
    hcb6 = p4.build_phase4_hdf_hcb(wifi_chip_donor=DONOR_FS, wpa_on_demand_donor=DONOR_FS, a2dp_donor=DONOR_FS)
    require(len(hcb6) == 2381 and b'a2dp_host' in hcb6 and b'bluetooth_audio_session_service' in hcb6,
            'Unexpected 6-host HCB payload')
    return bt_cfg_bytes, probe_bytes, hcb6


def patch_stage(stage_bytes: bytes) -> bytes:
    require(b'native_a2dp_hdi_probe' not in stage_bytes, 'stage-phase4-marble.sh already stages native_a2dp_hdi_probe')
    old_anchor = b'$B test -x "$R/compat/bin/init"\n'
    new_block = (
        b'# Batch E: Silent A2DP Audio HDI verification probe (no speaker audio changes).\n'
        b'$B cp /graphics/native_a2dp_hdi_probe "$R/compat/bin/native_a2dp_hdi_probe"\n'
        b'$B chmod 0755 "$R/compat/bin/native_a2dp_hdi_probe"\n'
        b'$B test -x "$R/compat/bin/init"\n'
    )
    patched = splice_once(stage_bytes, ((old_anchor, new_block),), 'stage-phase4-marble.sh')
    subprocess.run(['sh', '-n'], input=patched, check=True)
    return patched


def build(base_path: Path, out: Path) -> Path:
    require(sha256(ORACLE.read_bytes()).hexdigest() == ORACLE_SHA, 'Archive verifier identity changed')
    spec = importlib.util.spec_from_file_location('h20_e_archive_verifier', ORACLE)
    archive = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(archive)
    r = archive.load_tools()

    base = r.read_original(base_path)
    require(r.sha(base) == BASE_SHA, f'Base SHA mismatch: got {r.sha(base)}, want {BASE_SHA}')
    base_id = r.identity(base_path)
    bt_cfg_bytes, probe_bytes, hcb6 = validate_inputs()

    before_layout = r.layout(base)
    before_avb = r.verify_existing_avb(base, before_layout['payload_end'])
    out.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix='pack.', dir=out) as tmp:
        scratch = Path(tmp)
        old, old_raw, kind, old_rd, args = r.extract_entries(base_path, base, before_layout, scratch / 'before')
        require(len(old) == 519, f'Unexpected 8b4c2d35 ramdisk member count: {len(old)}')
        require(r.sha(old[HCB_MEMBER][1]) == OLD_HCB_SHA, 'Existing HCB does not match Batch D 5-host HCB')
        manifest = archive.parse(old[r.MANIFEST][1])
        r.manifest_holds(old, manifest)

        new_stage = patch_stage(old[STAGE_MEMBER][1])

        expected = dict(old)
        next_inode = [max(fields[0] for fields, _ in old.values()) + 1]
        new_manifest = copy.deepcopy(manifest)

        r.set_entry(expected, HCB_MEMBER, hcb6)
        r.set_entry(expected, BT_CFG_MEMBER, bt_cfg_bytes)
        r.set_entry(expected, STAGE_MEMBER, new_stage)
        new_manifest[HCB_REL].update(bytes=len(hcb6), sha256=r.sha(hcb6))
        new_manifest[BT_CFG_REL].update(bytes=len(bt_cfg_bytes), sha256=r.sha(bt_cfg_bytes))
        new_manifest[STAGE_REL].update(bytes=len(new_stage), sha256=r.sha(new_stage))

        require(PROBE_MEMBER not in expected, f'Duplicate ramdisk member: {PROBE_MEMBER}')
        r.image_tools.put_file(expected, PROBE_MEMBER, probe_bytes, 0o755, next_inode)
        expected[PROBE_MEMBER][0][11] = len(PROBE_MEMBER.encode('utf-8')) + 1
        probe_info = {'bytes': len(probe_bytes), 'mode': oct(0o755), 'sha256': r.sha(probe_bytes)}
        new_manifest[PROBE_REL] = probe_info

        r.set_entry(expected, r.MANIFEST, r.dump(new_manifest))
        r.manifest_holds(expected, new_manifest)

        cpio, rd, pack = (scratch / n for n in ('new.cpio', 'new.lz4', 'payload.img'))
        raw = r.image_tools.pack_cpio_newc(expected)
        cpio.write_bytes(raw)
        r.image_tools.compress_ramdisk(cpio, rd, kind)
        require(args.count(str(old_rd)) == 1, 'Ambiguous ramdisk argument')
        flags = [str(rd) if arg == str(old_rd) else arg for arg in args]
        subprocess.run(['mkbootimg', *flags, '--vendor_boot', str(pack)], check=True, capture_output=True)
        payload = pack.read_bytes()
        after_layout = r.layout(payload)
        require(after_layout['payload_end'] == len(payload) <= before_layout['payload_end'] + 4 * 1024 * 1024,
                f'Unexpected payload growth: {before_layout["payload_end"]} -> {after_layout["payload_end"]}')
        r.check_container(base, before_layout, payload, after_layout)
        image, updated = r.image_tools.preserve_and_update_avb_footer(payload, base)
        require(updated, 'Existing AVB footer metadata not updated')
        require(len(image) == 100663296, f'Unexpected vendor_boot size: {len(image)}')
        after_avb = r.verify_existing_avb(image, len(payload))
        require(all(after_avb[k] == before_avb[k] for k in ('vbmeta_masked', 'footer_masked')),
                'Unexpected AVB metadata change')

        verify = scratch / 'roundtrip.img'
        verify.write_bytes(image)
        got, got_raw, got_kind, _, _ = r.extract_entries(verify, image, after_layout, scratch / 'after')
        require(got == expected and got_raw == raw and got_kind == kind, 'Exact unpack round-trip failed')

        old_spans, old_trailer = archive.record_spans(old_raw)
        got_spans, got_trailer = archive.record_spans(got_raw)
        require(got_trailer == old_trailer, 'CPIO trailer changed')
        modified = {HCB_MEMBER, BT_CFG_MEMBER, STAGE_MEMBER, r.MANIFEST}
        unchanged = set(old) - modified
        require(len(unchanged) == 515, f'Unexpected unchanged entry count: {len(unchanged)}')
        for guard in AUDIO_GUARD_MEMBERS:
            require(guard in unchanged, f'Speaker audio binary unexpectedly modified: {guard}')
        require(all(got_raw[slice(*got_spans[n])] == old_raw[slice(*old_spans[n])] for n in unchanged),
                'Unrelated CPIO record changed')

    cand_sha = r.sha(image)
    out_img = out / f'vendor_boot_h20_e_bt_audio_{cand_sha[:8]}.img'
    out_img.write_bytes(image)
    report = {
        'base': base_id,
        'candidate': r.identity(out_img),
        'entries_before': len(old),
        'entries_after': len(expected),
        'unchanged_entries': len(unchanged),
        'modified_entries': sorted(modified),
        'added_entries': {PROBE_REL: probe_info},
        'hcb6_sha256': r.sha(hcb6),
        'bt_cfg_sha256': r.sha(bt_cfg_bytes),
        'audio_guards_unchanged': {m: r.sha(old[m][1]) for m in AUDIO_GUARD_MEMBERS},
        'payload_bytes_before': before_layout['payload_end'],
        'payload_bytes_after': after_layout['payload_end'],
    }
    (out / 'e-build-report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report, indent=2))
    return out_img


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--base', type=Path, default=BASE_DEFAULT)
    ap.add_argument('--out', type=Path, default=Path('/mnt/e/marble-tmp'))
    args = ap.parse_args()
    build(args.base, args.out)


if __name__ == '__main__':
    main()
