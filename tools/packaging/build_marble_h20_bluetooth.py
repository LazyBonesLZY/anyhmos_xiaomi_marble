#!/usr/bin/env python3
"""Build a single-increment Bluetooth vendor_boot delta on top of H20+bootanimation.

Preserves all unrelated H20 + bootanimation ramdisk members, DTB, bootconfig,
and AVB footer structure, adding only the verified QCA6490 Bluetooth bridge,
OHOS HDI publisher/vendor adapter, init config, eager SA 1130 profile, and
bounded startup hook after Wi-Fi.
"""
import argparse
import copy
import importlib.util
import json
import stat
import subprocess
import tempfile
from hashlib import sha256
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / 'work'
BASE_DEFAULT = Path('/mnt/e/marble-tmp/vendor_boot_h20_bootanimation_dff03e31.img')
BASE_SHA = 'dff03e315bd2599efda9ad6fbe03b98b4e6a774ca779e557c0183afc1fbff939'
ORACLE = WORK / 'marble-m15-role-storage.1v2ynS/build_m15_role_storage.py'
ORACLE_SHA = '7d4f79857a3458ffdb7c8fb86b1cfd893fd9addb02b13e7e05d7b5fa7093cfaa'

BT_BUILD = WORK / 'marble-bluetooth-build-r46'
BT_RT = ROOT / 'devices/marble/runtime/bluetooth'

SEED_MEMBER = 'hmos-compat-marble/hmos-init-debug.cfg'
STAGE_MEMBER = 'hmos-compat-marble/graphics/stage-phase4-marble.sh'
START_MEMBER = 'hmos-compat-marble/graphics/start-phase4-marble.sh'

ADDED_FILES = (
    ('graphics/hmos_bt_bridge_marble', BT_BUILD / 'hmos_bt_bridge_marble', 0o755),
    ('graphics/libhmos_bt_bridge_marble.so', BT_BUILD / 'libhmos_bt_bridge_marble.so', 0o644),
    ('graphics/libbt_vendor.z.so', BT_BUILD / 'libbt_vendor.z.so', 0o644),
    ('graphics/hmos_bt_hdi_publisher_marble', BT_BUILD / 'hmos_bt_hdi_publisher_marble', 0o755),
    ('graphics/native_bt_host_check_v139', BT_BUILD / 'native_bt_host_check_v139', 0o755),
    ('graphics/start-bluetooth-marble.sh', BT_RT / 'start-bluetooth-marble.sh', 0o755),
    ('graphics/init-bluetooth-marble.cfg', BT_RT / 'init-bluetooth-marble.cfg', 0o644),
    ('graphics/bluetooth-service-eager.json', BT_RT / 'bluetooth-service-eager.json', 0o644),
)

BT_START_BLOCK = b'''# 6d. QCA6490 Bluetooth (UART H4 bridge + OHOS HDI + bluetooth_service SA 1130).
stage=bluetooth
if $B test -x "$R/compat/bin/start-bluetooth-marble.sh"; then
 if $B timeout 25 $B sh "$R/compat/bin/start-bluetooth-marble.sh" > "$R/data/local/tmp/marble-bluetooth-startup.log" 2>&1; then
  log "BLUETOOTH_READY=1"
 else
  log "BLUETOOTH_START_WARN rc=$?"
 fi
fi
wait_sa 1130 12 || true
wait_sa 1191 10 || true

'''


def require(cond, msg):
    if not cond:
        raise ValueError(msg)


def splice_once(data: bytes, pairs: tuple[tuple[bytes, bytes], ...], label: str) -> bytes:
    out = data
    for old, new in pairs:
        require(out.count(old) == 1, f'{label}: insertion anchor not unique: {old[:60]!r}')
        out = out.replace(old, new, 1)
    reverted = out
    for old, new in reversed(pairs):
        require(reverted.count(new) == 1, f'{label}: delta not separable')
        reverted = reverted.replace(new, old, 1)
    require(reverted == data, f'{label}: non-target bytes changed')
    return out


def patch_seed(seed_bytes: bytes) -> bytes:
    old_anchor = b'    "/compat/etc/init-wifi-marble.cfg"\n  ],'
    new_anchor = (
        b'    "/compat/etc/init-wifi-marble.cfg",\n'
        b'    "/compat/etc/init-bluetooth-marble.cfg"\n'
        b'  ],'
    )
    require(b'init-bluetooth-marble.cfg' not in seed_bytes, 'Seed already imports init-bluetooth-marble.cfg')
    patched = splice_once(seed_bytes, ((old_anchor, new_anchor),), 'hmos-init-debug.cfg')
    doc = json.loads(patched)
    require('/compat/etc/init-bluetooth-marble.cfg' in doc['import'], 'Import missing after JSON parse')
    require(len(patched) < 32768, f'hmos-init-debug.cfg exceeds 32768-byte init cap: {len(patched)}')
    return patched


def patch_stage(stage_bytes: bytes) -> bytes:
    pairs = (
        (
            b' libhmos_boot_vulkan.so; do\n',
            b' libbt_vendor.z.so \\\n'
            b' libhmos_bt_bridge_marble.so \\\n'
            b' libhmos_boot_vulkan.so; do\n',
        ),
        (
            b' start-wifi-marble.sh; do\n',
            b' hmos_bt_bridge_marble \\\n'
            b' hmos_bt_hdi_publisher_marble \\\n'
            b' native_bt_host_check_v139 \\\n'
            b' start-bluetooth-marble.sh \\\n'
            b' start-wifi-marble.sh; do\n',
        ),
        (
            b' init-wifi-marble.cfg \\\n',
            b' init-wifi-marble.cfg \\\n'
            b' init-bluetooth-marble.cfg \\\n',
        ),
        (
            b' 3601 1130 65862 ',
            b' 3601 65862 ',
        ),
        (
            b'if $B test -f /graphics/wifi-manager-eager.json; then\n'
            b' $B cp /graphics/wifi-manager-eager.json "$R/system/profile/wifi_manager_service.json"\n'
            b' $B chmod 0644 "$R/system/profile/wifi_manager_service.json"\n'
            b'fi\n',
            b'if $B test -f /graphics/wifi-manager-eager.json; then\n'
            b' $B cp /graphics/wifi-manager-eager.json "$R/system/profile/wifi_manager_service.json"\n'
            b' $B chmod 0644 "$R/system/profile/wifi_manager_service.json"\n'
            b'fi\n'
            b'if $B test -f /graphics/bluetooth-service-eager.json; then\n'
            b' $B cp /graphics/bluetooth-service-eager.json "$R/system/profile/bluetooth_service.json"\n'
            b' $B chmod 0644 "$R/system/profile/bluetooth_service.json"\n'
            b'fi\n',
        ),
        (
            b'init_log "$R/data/local/tmp/hmos-wifi-category-v134.txt" 3026:3026 0600\n',
            b'init_log "$R/data/local/tmp/hmos-wifi-category-v134.txt" 3026:3026 0600\n'
            b'init_log "$R/data/local/tmp/hmos-bluetooth-hdi.txt" 3021:3021 0664\n'
            b'init_log "$R/data/local/tmp/hmos-bluetooth-check.txt" 2000:2000 0664\n',
        ),
    )
    patched = splice_once(stage_bytes, pairs, 'stage-phase4-marble.sh')
    subprocess.run(['sh', '-n'], input=patched, check=True)
    return patched


def patch_start(start_bytes: bytes) -> bytes:
    anchor = (
        b'for id in 1120 1124 1126 1158 1151; do\n'
        b' wait_sa "$id" 12 || true\n'
        b'done\n\n'
        b'# 7. Foundation (BMS + AMS + WMS + ScreenLock -> launches SceneBoard!).\n'
    )
    replacement = (
        b'for id in 1120 1124 1126 1158 1151; do\n'
        b' wait_sa "$id" 12 || true\n'
        b'done\n\n'
        + BT_START_BLOCK +
        b'# 7. Foundation (BMS + AMS + WMS + ScreenLock -> launches SceneBoard!).\n'
    )
    require(b'start-bluetooth-marble.sh' not in start_bytes, 'Start script already invokes start-bluetooth-marble.sh')
    patched = splice_once(start_bytes, ((anchor, replacement),), 'start-phase4-marble.sh')
    subprocess.run(['sh', '-n'], input=patched, check=True)
    return patched


def build(base_path: Path, out: Path) -> Path:
    require(sha256(ORACLE.read_bytes()).hexdigest() == ORACLE_SHA, 'Archive verifier identity changed')
    spec = importlib.util.spec_from_file_location('h20_bt_archive_verifier', ORACLE)
    archive = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(archive)
    r = archive.load_tools()

    base = r.read_original(base_path)
    require(r.sha(base) == BASE_SHA, f'Base SHA mismatch: got {r.sha(base)}, want {BASE_SHA}')
    base_id = r.identity(base_path)

    # Validate added source files before touching ramdisk.
    for rel, src, _mode in ADDED_FILES:
        require(src.is_file(), f'Missing staged BT artifact: {src}')
        raw_src = src.read_bytes()
        if rel.endswith('.json') or rel.endswith('.cfg'):
            json.loads(raw_src)
        elif rel.endswith('.sh'):
            subprocess.run(['sh', '-n'], input=raw_src, check=True)
        else:
            require(raw_src[:4] == b'\x7fELF', f'Expected ELF binary for {rel}')

    before_layout = r.layout(base)
    before_avb = r.verify_existing_avb(base, before_layout['payload_end'])
    out.mkdir(parents=True, exist_ok=False)

    with tempfile.TemporaryDirectory(prefix='pack.', dir=out) as tmp:
        scratch = Path(tmp)
        old, old_raw, kind, old_rd, args = r.extract_entries(base_path, base, before_layout, scratch / 'before')
        require(len(old) == 493, f'Unexpected H20+bootanim ramdisk member count: {len(old)}')
        manifest = archive.parse(old[r.MANIFEST][1])
        r.manifest_holds(old, manifest)

        new_seed = patch_seed(old[SEED_MEMBER][1])
        new_stage = patch_stage(old[STAGE_MEMBER][1])
        new_start = patch_start(old[START_MEMBER][1])

        expected = dict(old)
        next_inode = [max(fields[0] for fields, _ in old.values()) + 1]
        new_manifest = copy.deepcopy(manifest)

        r.set_entry(expected, SEED_MEMBER, new_seed)
        r.set_entry(expected, STAGE_MEMBER, new_stage)
        r.set_entry(expected, START_MEMBER, new_start)
        new_manifest['hmos-init-debug.cfg'].update(bytes=len(new_seed), sha256=r.sha(new_seed))
        new_manifest['graphics/stage-phase4-marble.sh'].update(bytes=len(new_stage), sha256=r.sha(new_stage))
        new_manifest['graphics/start-phase4-marble.sh'].update(bytes=len(new_start), sha256=r.sha(new_start))

        added_full = set()
        added_info = {}
        for rel, src, mode in ADDED_FILES:
            full = f'{r.image_tools.COMPAT_DIR}/{rel}'
            require(full not in expected, f'Duplicate ramdisk member: {full}')
            blob = src.read_bytes()
            r.image_tools.put_file(expected, full, blob, mode, next_inode)
            expected[full][0][11] = len(full.encode('utf-8')) + 1
            new_manifest[rel] = {'bytes': len(blob), 'mode': oct(mode), 'sha256': r.sha(blob)}
            added_full.add(full)
            added_info[rel] = {'bytes': len(blob), 'mode': oct(mode), 'sha256': r.sha(blob)}

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
        require(after_layout['payload_end'] == len(payload) <= before_layout['payload_end'] + 262144,
                f'Unexpected payload growth: {before_layout["payload_end"]} -> {after_layout["payload_end"]}')
        r.check_container(base, before_layout, payload, after_layout)
        image, updated = r.image_tools.preserve_and_update_avb_footer(payload, base)
        require(updated, 'Existing AVB footer metadata not updated')
        after_avb = r.verify_existing_avb(image, len(payload))
        require(all(after_avb[k] == before_avb[k] for k in ('vbmeta_masked', 'footer_masked')),
                'Unexpected AVB metadata change')

        verify = scratch / 'roundtrip.img'
        verify.write_bytes(image)
        got, got_raw, got_kind, _, _ = r.extract_entries(verify, image, after_layout, scratch / 'after')
        require(got == expected and got_raw == raw and got_kind == kind, 'Exact unpack round-trip failed')

        old_spans, old_trailer = archive.record_spans(old_raw)
        got_spans, got_trailer = archive.record_spans(got_raw)
        require(old_trailer == got_trailer, 'CPIO trailer changed')
        modified = {SEED_MEMBER, STAGE_MEMBER, START_MEMBER, r.MANIFEST}
        require(set(got_spans) - set(old_spans) == added_full, 'Unexpected added CPIO members')
        require(set(old_spans) - set(got_spans) == set(), 'Unexpected removed CPIO members')
        unchanged = 0
        for name, (olo, ohi) in old_spans.items():
            glo, ghi = got_spans[name]
            if name in modified:
                require(old[name][0][:6] == got[name][0][:6] and old[name][0][7:] == got[name][0][7:],
                        f'Non-size CPIO metadata changed for {name}')
            else:
                require(old_raw[olo:ohi] == got_raw[glo:ghi], f'Unrelated CPIO record changed: {name}')
                unchanged += 1

        target = out / ('vendor_boot_h20_bluetooth_' + r.sha(image)[:8] + '.img')
        with target.open('xb') as f:
            f.write(image)
        report = {
            'scope': 'QCA6490 Bluetooth single-increment delta on H20+bootanimation base',
            'base': base_id,
            'output': r.identity(target),
            'entries_before': len(old),
            'entries_after': len(got),
            'unchanged_entries_byte_identical': unchanged,
            'modified_members': sorted(modified),
            'added_members': added_info,
            'seed_bytes': {'before': len(old[SEED_MEMBER][1]), 'after': len(new_seed)},
            'payload_bytes': {'before': before_layout['payload_end'], 'after': after_layout['payload_end']},
            'dtb_and_bootconfig_unchanged': True,
            'avb_hash_verified': True,
            'runtime_tested': False,
            'flashed': False,
        }
        (out / 'report.json').write_bytes(r.dump(report))
    return target


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument('--base', type=Path, default=BASE_DEFAULT)
    cli.add_argument('--out', type=Path, required=True, help='New output directory')
    args = cli.parse_args()
    print(build(args.base.resolve(strict=True), args.out.resolve()))


if __name__ == '__main__':
    main()
