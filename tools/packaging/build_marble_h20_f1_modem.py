#!/usr/bin/env python3
"""Package Batch F1 (RAM-COW hmos_rmtfs + marble_modem_probe + manual start-modem-f1-marble.sh) onto Batch E."""
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
BASE_DEFAULT = Path('/mnt/e/marble-tmp/vendor_boot_h20_e_bt_audio_2cff13e2.img')
BASE_SHA = '2cff13e2544112780506c91c90c7e797f67c3836feaa722c46c556fe7b4bea43'
ORACLE = WORK / 'marble-m15-role-storage.1v2ynS/build_m15_role_storage.py'
ORACLE_SHA = '7d4f79857a3458ffdb7c8fb86b1cfd893fd9addb02b13e7e05d7b5fa7093cfaa'

F1_BUILD = WORK / 'marble-f1-build'
F1_START = ROOT / 'devices/marble/runtime/telephony/start-modem-f1-marble.sh'

STAGE_REL = 'graphics/stage-phase4-marble.sh'
STAGE_MEMBER = f'hmos-compat-marble/{STAGE_REL}'

ADDED_FILES = (
    ('graphics/hmos_rmtfs', F1_BUILD / 'hmos_rmtfs', 0o755, 'elf'),
    ('graphics/marble_modem_probe', F1_BUILD / 'marble_modem_probe', 0o755, 'elf'),
    ('graphics/start-modem-f1-marble.sh', F1_START, 0o755, 'sh'),
)

GUARD_MEMBERS = (
    'hmos-compat-marble/graphics/libhmos_audio_vdi.so',
    'hmos-compat-marble/graphics/audio-runtime/lib64/libhmos_pal_server.so',
    'hmos-compat-marble/graphics/hdf_default_phase4.hcb',
    'hmos-compat-marble/graphics/init-bluetooth-marble.cfg',
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


def validate_inputs() -> None:
    for rel, src, _mode, kind in ADDED_FILES:
        require(src.is_file(), f'Missing F1 artifact: {src}')
        raw = src.read_bytes()
        if kind == 'elf':
            require(raw[:4] == b'\x7fELF', f'Expected ELF header in {src}')
        elif kind == 'sh':
            subprocess.run(['sh', '-n'], input=raw, check=True)
        else:
            raise ValueError(f'Unknown artifact kind {kind} for {rel}')


def patch_stage(stage_bytes: bytes) -> bytes:
    for marker in (b'hmos_rmtfs', b'marble_modem_probe', b'start-modem-f1-marble.sh'):
        require(marker not in stage_bytes, f'stage-phase4-marble.sh already contains {marker!r}')
    old_anchor = b'$B test -x "$R/compat/bin/init"\n'
    new_block = (
        b'# Batch F1: Stage manual modem MSS tools only (NOT auto-started on boot).\n'
        b'$B mkdir -p /compat/bin "$R/compat/bin"\n'
        b'$B cp /graphics/hmos_rmtfs /compat/bin/hmos_rmtfs\n'
        b'$B cp /graphics/hmos_rmtfs "$R/compat/bin/hmos_rmtfs"\n'
        b'$B cp /graphics/marble_modem_probe /compat/bin/marble_modem_probe\n'
        b'$B cp /graphics/marble_modem_probe "$R/compat/bin/marble_modem_probe"\n'
        b'$B cp /graphics/start-modem-f1-marble.sh /compat/bin/start-modem-f1-marble.sh\n'
        b'$B cp /graphics/start-modem-f1-marble.sh "$R/compat/bin/start-modem-f1-marble.sh"\n'
        b'$B chmod 0755 /compat/bin/hmos_rmtfs /compat/bin/marble_modem_probe /compat/bin/start-modem-f1-marble.sh '
        b'"$R/compat/bin/hmos_rmtfs" "$R/compat/bin/marble_modem_probe" "$R/compat/bin/start-modem-f1-marble.sh"\n'
        b'$B test -x "$R/compat/bin/init"\n'
    )
    patched = splice_once(stage_bytes, ((old_anchor, new_block),), 'stage-phase4-marble.sh')
    subprocess.run(['sh', '-n'], input=patched, check=True)
    return patched


def build(base_path: Path, out: Path) -> Path:
    require(sha256(ORACLE.read_bytes()).hexdigest() == ORACLE_SHA, 'Archive verifier identity changed')
    spec = importlib.util.spec_from_file_location('h20_f1_archive_verifier', ORACLE)
    archive = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(archive)
    r = archive.load_tools()

    base = r.read_original(base_path)
    require(r.sha(base) == BASE_SHA, f'Base SHA mismatch: got {r.sha(base)}, want {BASE_SHA}')
    base_id = r.identity(base_path)
    validate_inputs()

    before_layout = r.layout(base)
    before_avb = r.verify_existing_avb(base, before_layout['payload_end'])
    out.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix='pack.', dir=out) as tmp:
        scratch = Path(tmp)
        old, old_raw, kind, old_rd, args = r.extract_entries(base_path, base, before_layout, scratch / 'before')
        require(len(old) == 520, f'Unexpected 2cff13e2 ramdisk member count: {len(old)}')
        manifest = archive.parse(old[r.MANIFEST][1])
        r.manifest_holds(old, manifest)

        new_stage = patch_stage(old[STAGE_MEMBER][1])

        expected = dict(old)
        next_inode = [max(fields[0] for fields, _ in old.values()) + 1]
        new_manifest = copy.deepcopy(manifest)

        r.set_entry(expected, STAGE_MEMBER, new_stage)
        new_manifest[STAGE_REL].update(bytes=len(new_stage), sha256=r.sha(new_stage))

        added_info = {}
        for rel, src, mode, _kind in ADDED_FILES:
            full = f'{r.image_tools.COMPAT_DIR}/{rel}'
            require(full not in expected, f'Duplicate ramdisk member: {full}')
            blob = src.read_bytes()
            r.image_tools.put_file(expected, full, blob, mode, next_inode)
            expected[full][0][11] = len(full.encode('utf-8')) + 1
            info = {'bytes': len(blob), 'mode': oct(mode), 'sha256': r.sha(blob)}
            new_manifest[rel] = info
            added_info[rel] = info

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
        modified = {STAGE_MEMBER, r.MANIFEST}
        unchanged = set(old) - modified
        require(len(unchanged) == 518, f'Unexpected unchanged entry count: {len(unchanged)}')
        for guard in GUARD_MEMBERS:
            require(guard in unchanged, f'Protected guard entry modified: {guard}')
        require(all(got_raw[slice(*got_spans[n])] == old_raw[slice(*old_spans[n])] for n in unchanged),
                'Unrelated CPIO record changed')

    cand_sha = r.sha(image)
    out_img = out / f'vendor_boot_h20_f1_modem_{cand_sha[:8]}.img'
    out_img.write_bytes(image)
    report = {
        'base': base_id,
        'candidate': r.identity(out_img),
        'entries_before': len(old),
        'entries_after': len(expected),
        'unchanged_entries': len(unchanged),
        'modified_entries': sorted(modified),
        'added_entries': added_info,
        'guards_unchanged': {m: r.sha(old[m][1]) for m in GUARD_MEMBERS},
        'payload_bytes_before': before_layout['payload_end'],
        'payload_bytes_after': after_layout['payload_end'],
    }
    (out / 'f1-build-report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
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
