#!/usr/bin/env python3
"""Package single-increment H20+B2 media/storage/apps overlay onto H20+C vendor_boot_a."""
import argparse
import copy
import gzip
import importlib.util
import json
import subprocess
import tempfile
from hashlib import sha256
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / 'work'
BASE_DEFAULT = Path('/mnt/e/marble-tmp/vendor_boot_h20_c_display_f99d75ac.img')
BASE_SHA = 'f99d75aca6de70678cee6c945ad3799ed37ed72473b52752beb01032b3af7f42'
ORACLE = WORK / 'marble-m15-role-storage.1v2ynS/build_m15_role_storage.py'
ORACLE_SHA = '7d4f79857a3458ffdb7c8fb86b1cfd893fd9addb02b13e7e05d7b5fa7093cfaa'

B2_BUILD = WORK / 'marble-b2-build'
B2_CFG = ROOT / 'devices/marble/runtime/init/init-b2-apps-marble.cfg'
B2_INDEX_PROFILE = ROOT / 'devices/marble/runtime/init/index_insert_service-marble.json'

SEED_MEMBER = 'hmos-compat-marble/hmos-init-debug.cfg'
STAGE_MEMBER = 'hmos-compat-marble/graphics/stage-phase4-marble.sh'
START_MEMBER = 'hmos-compat-marble/graphics/start-phase4-marble.sh'

ADDED_FILES = (
    ('graphics/init-b2-apps-marble.cfg', B2_CFG, 0o644),
    ('graphics/index_insert_service-marble.json', B2_INDEX_PROFILE, 0o644),
    ('graphics/plane_layout_contract', B2_BUILD / 'plane_layout_contract', 0o755),
)

REPLACED_FILES = (
    ('graphics/libdisplay_buffer_vdi_impl.z.so', B2_BUILD / 'libdisplay_buffer_vdi_impl.z.so', 0o644),
    ('graphics/libvulkan_freedreno.so.gz', B2_BUILD / 'libvulkan_freedreno.so.gz', 0o644),
    ('graphics/libhmos_ram_no_tee.so', B2_BUILD / 'libhmos_ram_no_tee.so', 0o644),
)

OPTIONAL_SA_B1 = (
    '4005 4016 1903 1905 1907 2802 3008 3203 3601 4300 5204 6001 8250 302 3506 3526 8003 10120 '
    '65862 65866 65926 65936 65952 65962 65974 65993 66006 66048 66074 66106 66134 '
    '66208 66234 66250 66288 66358 66532 66588 70633'
)
OPTIONAL_SA_B2 = (
    '4005 4016 1903 1905 1907 2802 3008 3203 3601 4300 5204 6001 8250 302 3506 3526 8003 10120 '
    '65862 65866 65936 65952 65962 65974 65993 66006 66048 66106 66134 '
    '66208 66234 66250 66288 66358 66532 66588 70633'
)


def require(cond, msg):
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


def patch_seed(seed_bytes: bytes) -> bytes:
    old_anchor = b'    "/compat/etc/init-b1-platform-marble.cfg"\n  ],'
    new_anchor = (
        b'    "/compat/etc/init-b1-platform-marble.cfg",\n'
        b'    "/compat/etc/init-b2-apps-marble.cfg"\n'
        b'  ],'
    )
    require(b'init-b2-apps-marble.cfg' not in seed_bytes, 'Seed already imports init-b2-apps-marble.cfg')
    patched = splice_once(seed_bytes, ((old_anchor, new_anchor),), 'hmos-init-debug.cfg')
    doc = json.loads(patched)
    require('/compat/etc/init-b2-apps-marble.cfg' in doc['import'], 'Import missing after JSON parse')
    require(len(patched) < 32768, f'hmos-init-debug.cfg exceeds 32768-byte init cap: {len(patched)}')
    return patched


def patch_stage(stage_bytes: bytes) -> bytes:
    require(b'init-b2-apps-marble.cfg' not in stage_bytes, 'stage script already mentions init-b2-apps-marble.cfg')
    require(b'index_insert_service-marble.json' not in stage_bytes, 'stage script already mentions index_insert_service-marble.json')
    require(b'plane_layout_contract' not in stage_bytes, 'stage script already mentions plane_layout_contract')
    pairs = (
        (
            b' init-b1-platform-marble.cfg \\\n'
            b' useriam-core.json \\\n'
            b' powermgr.json; do\n',
            b' init-b1-platform-marble.cfg \\\n'
            b' init-b2-apps-marble.cfg \\\n'
            b' index_insert_service-marble.json \\\n'
            b' useriam-core.json \\\n'
            b' powermgr.json; do\n',
        ),
        (
            b' echo MARBLE_RCD_CONFIG_BOUND=1\n'
            b'fi\n'
            b'$B test -x "$R/compat/bin/init"\n',
            b' echo MARBLE_RCD_CONFIG_BOUND=1\n'
            b'fi\n'
            b'if $B test -f /graphics/index_insert_service-marble.json; then\n'
            b' $B cp /graphics/index_insert_service-marble.json "$R/system/profile/index_insert_service.json"\n'
            b' $B chmod 0644 "$R/system/profile/index_insert_service.json"\n'
            b'fi\n'
            b'if $B test -f /graphics/plane_layout_contract; then\n'
            b' $B cp /graphics/plane_layout_contract "$R/compat/bin/plane_layout_contract"\n'
            b' $B chmod 0755 "$R/compat/bin/plane_layout_contract"\n'
            b'fi\n'
            b'$B test -x "$R/compat/bin/init"\n',
        ),
        (
            f"printf '%s\\n' '{OPTIONAL_SA_B1}' > \"$R/compat/etc/sceneboard-optional-sa.list\"\n".encode('utf-8'),
            f"printf '%s\\n' '{OPTIONAL_SA_B2}' > \"$R/compat/etc/sceneboard-optional-sa.list\"\n".encode('utf-8'),
        ),
    )
    patched = splice_once(stage_bytes, pairs, 'stage-phase4-marble.sh')
    subprocess.run(['sh', '-n'], input=patched, check=True)
    return patched


def patch_start(start_bytes: bytes) -> bytes:
    require(b'marble_late_b2_apps' not in start_bytes, 'start script already contains marble_late_b2_apps')
    pairs = (
        (
            b' log "RESOURCE_LATE_END carrier=$carrier"\n'
            b'}\n'
            b'marble_late_resource\n',
            b' log "RESOURCE_LATE_END carrier=$carrier"\n'
            b'}\n'
            b'marble_late_resource\n'
            b'marble_late_b2_apps() {\n'
            b' stage=late_b2_apps\n'
            b' owner="$R/dev/marble-b2-apps-late-owner"\n'
            b' if ! $B mkdir "$owner" 2>/dev/null; then\n'
            b'  log \'B2_APPS_LATE_SKIP owner_exists=1\'\n'
            b'  return 0\n'
            b' fi\n'
            b' if ! $B timeout -k 1 3 $B chroot "$R" /system/bin/param set marble.debug.start.b2_apps true >/dev/null 2>&1; then\n'
            b'  log \'B2_APPS_LATE_SET_FAILED\'\n'
            b'  return 0\n'
            b' fi\n'
            b' log \'B2_APPS_LATE_TRIGGERED\'\n'
            b' wait_sa 65926 12 || true\n'
            b' wait_sa 66054 12 || true\n'
            b' wait_sa 66074 12 || true\n'
            b' sa65926=$($B timeout -k 1 2 $B chroot "$R" /compat/bin/native_sa_check 65926 >/dev/null 2>&1 && echo 1 || echo 0)\n'
            b' sa66054=$($B timeout -k 1 2 $B chroot "$R" /compat/bin/native_sa_check 66054 >/dev/null 2>&1 && echo 1 || echo 0)\n'
            b' sa66074=$($B timeout -k 1 2 $B chroot "$R" /compat/bin/native_sa_check 66074 >/dev/null 2>&1 && echo 1 || echo 0)\n'
            b' log "B2_APPS_LATE_END sa65926=$sa65926 sa66054=$sa66054 sa66074=$sa66074"\n'
            b'}\n'
            b'marble_late_b2_apps\n',
        ),
    )
    patched = splice_once(start_bytes, pairs, 'start-phase4-marble.sh')
    subprocess.run(['sh', '-n'], input=patched, check=True)
    return patched


def build(base_path: Path, out: Path) -> Path:
    require(sha256(ORACLE.read_bytes()).hexdigest() == ORACLE_SHA, 'Archive verifier identity changed')
    spec = importlib.util.spec_from_file_location('h20_b2_archive_verifier', ORACLE)
    archive = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(archive)
    r = archive.load_tools()

    base = r.read_original(base_path)
    require(r.sha(base) == BASE_SHA, f'Base SHA mismatch: got {r.sha(base)}, want {BASE_SHA}')
    base_id = r.identity(base_path)

    for rel, src, _mode in ADDED_FILES + REPLACED_FILES:
        require(src.is_file(), f'Missing staged B2 artifact: {src}')
        raw_src = src.read_bytes()
        if rel.endswith('.json') or rel.endswith('.cfg'):
            json.loads(raw_src)
        elif rel.endswith('.gz'):
            require(raw_src[:2] == b'\x1f\x8b', f'Expected gzip header for {rel}')
            decompressed = gzip.decompress(raw_src)
            require(decompressed[:4] == b'\x7fELF', f'Expected ELF payload inside gzip for {rel}')
        else:
            require(raw_src[:4] == b'\x7fELF', f'Expected ELF binary for {rel}')

    before_layout = r.layout(base)
    before_avb = r.verify_existing_avb(base, before_layout['payload_end'])
    out.mkdir(parents=True, exist_ok=False)

    with tempfile.TemporaryDirectory(prefix='pack.', dir=out) as tmp:
        scratch = Path(tmp)
        old, old_raw, kind, old_rd, args = r.extract_entries(base_path, base, before_layout, scratch / 'before')
        require(len(old) == 504, f'Unexpected f99d75ac ramdisk member count: {len(old)}')
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

        replaced_full = set()
        for rel, src, _mode in REPLACED_FILES:
            full = f'{r.image_tools.COMPAT_DIR}/{rel}'
            require(full in expected, f'Expected existing ramdisk member to replace: {full}')
            blob = src.read_bytes()
            r.set_entry(expected, full, blob)
            new_manifest[rel].update(bytes=len(blob), sha256=r.sha(blob))
            replaced_full.add(full)

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
        require(after_layout['payload_end'] == len(payload) <= before_layout['payload_end'] + 393216,
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
        require(got_trailer == old_trailer, 'CPIO trailer changed')
        modified = {SEED_MEMBER, STAGE_MEMBER, START_MEMBER, r.MANIFEST} | replaced_full
        unchanged = set(old) - modified
        require(all(got_raw[slice(*got_spans[n])] == old_raw[slice(*old_spans[n])] for n in unchanged),
                'Unrelated CPIO record changed')

    cand_sha = r.sha(image)
    out_img = out / f'vendor_boot_h20_b2_{cand_sha[:8]}.img'
    out_img.write_bytes(image)
    report = {
        'base': base_id,
        'candidate': r.identity(out_img),
        'entries_before': len(old),
        'entries_after': len(expected),
        'unchanged_entries': len(unchanged),
        'modified_entries': sorted(modified),
        'added_entries': added_info,
        'seed_bytes': len(new_seed),
        'payload_bytes_before': before_layout['payload_end'],
        'payload_bytes_after': after_layout['payload_end'],
    }
    (out / 'b2-build-report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report, indent=2))
    return out_img


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--base', type=Path, default=BASE_DEFAULT)
    ap.add_argument('--out', type=Path, required=True)
    args = ap.parse_args()
    build(args.base, args.out)


if __name__ == '__main__':
    main()
