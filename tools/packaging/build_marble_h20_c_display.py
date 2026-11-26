#!/usr/bin/env python3
"""Build single-increment Batch C (Display 60/120Hz + Composer FD Leak Fix + VSYNC -22) vendor_boot delta on top of B1-v3 (65a41a75).

Preserves all unrelated H20 + bootanimation + Bluetooth + B1 ramdisk members, DTB,
bootconfig, and AVB footer structure, updating only:
  1. `graphics/libdisplay_composer_vdi_impl.z.so`:
     - Fixes the 32,747 sync_file FD leak in Commit/Submit/SubmitDirect via PresentFenceCache
     - Switches steady-state atomic commits to DRM_MODE_ATOMIC_NONBLOCK with 1-frame deferred
       retiringFb cleanup and -EBUSY fence-poll retry (unlocking 60/120Hz from ~31 FPS)
     - Pauses VsyncLoop before ACTIVE=0 modesets (cold reinit & screen-off) to eliminate
       SDE encoder VSYNC timeout and control_vblank_irq -22 errors
     - Exports GetDisplayClientTargetProperty to silence per-frame -2 proxy errors
  2. `graphics/rcd-config-marble.xml` bind-mounted over
     `/sys_prod/etc/display/RoundCornerDisplay/config.xml` to disable ADY-AL10 1256x2760
     software round-corner mask mismatch (`RCD: RcdProcessInfo is incorrect` every frame).
"""
import argparse
import copy
import importlib.util
import json
import subprocess
import tempfile
import xml.etree.ElementTree as ET
from hashlib import sha256
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / 'work'
BASE_DEFAULT = Path('/mnt/e/marble-tmp/vendor_boot_h20_b1_65a41a75.img')
BASE_SHA = '65a41a75580578813b379ec7c79d82dae535162bd3c152d90d0c049ba70ca33d'
ORACLE = WORK / 'marble-m15-role-storage.1v2ynS/build_m15_role_storage.py'
ORACLE_SHA = '7d4f79857a3458ffdb7c8fb86b1cfd893fd9addb02b13e7e05d7b5fa7093cfaa'

C_BUILD = WORK / 'marble-c-display-build'
RCD_CFG = ROOT / 'devices/marble/runtime/display/rcd-config-marble.xml'

STAGE_MEMBER = 'hmos-compat-marble/graphics/stage-phase4-marble.sh'

ADDED_FILES = (
    ('graphics/rcd-config-marble.xml', RCD_CFG, 0o644),
)

REPLACED_FILES = (
    ('graphics/libdisplay_composer_vdi_impl.z.so', C_BUILD / 'libdisplay_composer_vdi_impl.z.so', 0o644),
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


def patch_stage(stage_bytes: bytes) -> bytes:
    old_anchor = b'$B test -x "$R/compat/bin/init"\n'
    new_anchor = (
        b'# Disable ADY-AL10 1256x2760 software round-corner mask mismatch on marble 1080x2400 OLED.\n'
        b'if $B test -f /graphics/rcd-config-marble.xml && $B test -f "$R/sys_prod/etc/display/RoundCornerDisplay/config.xml"; then\n'
        b' $B mkdir -p "$R/compat/etc/display/RoundCornerDisplay"\n'
        b' $B cp /graphics/rcd-config-marble.xml "$R/compat/etc/display/RoundCornerDisplay/config.xml"\n'
        b' $B chmod 0644 "$R/compat/etc/display/RoundCornerDisplay/config.xml"\n'
        b' $B mount --bind "$R/compat/etc/display/RoundCornerDisplay/config.xml" "$R/sys_prod/etc/display/RoundCornerDisplay/config.xml" 2>/dev/null || true\n'
        b' echo MARBLE_RCD_CONFIG_BOUND=1\n'
        b'fi\n'
        b'$B test -x "$R/compat/bin/init"\n'
    )
    require(b'rcd-config-marble.xml' not in stage_bytes, 'stage-phase4-marble.sh already patches RCD config')
    patched = splice_once(stage_bytes, ((old_anchor, new_anchor),), 'stage-phase4-marble.sh')
    subprocess.run(['sh', '-n'], input=patched, check=True)
    return patched


def build(base_path: Path, out: Path) -> Path:
    require(sha256(ORACLE.read_bytes()).hexdigest() == ORACLE_SHA, 'Archive verifier identity changed')
    spec = importlib.util.spec_from_file_location('h20_c_archive_verifier', ORACLE)
    archive = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(archive)
    r = archive.load_tools()

    base = r.read_original(base_path)
    require(r.sha(base) == BASE_SHA, f'Base SHA mismatch: got {r.sha(base)}, want {BASE_SHA}')
    base_id = r.identity(base_path)

    for rel, src, _mode in ADDED_FILES + REPLACED_FILES:
        require(src.is_file(), f'Missing staged C artifact: {src}')
        raw_src = src.read_bytes()
        if rel.endswith('.xml'):
            ET.fromstring(raw_src)
        else:
            require(raw_src[:4] == b'\x7fELF', f'Expected ELF binary for {rel}')

    before_layout = r.layout(base)
    before_avb = r.verify_existing_avb(base, before_layout['payload_end'])
    out.mkdir(parents=True, exist_ok=False)

    with tempfile.TemporaryDirectory(prefix='pack.', dir=out) as tmp:
        scratch = Path(tmp)
        old, old_raw, kind, old_rd, args = r.extract_entries(base_path, base, before_layout, scratch / 'before')
        require(len(old) == 503, f'Unexpected B1-v3 ramdisk member count: {len(old)}')
        manifest = archive.parse(old[r.MANIFEST][1])
        r.manifest_holds(old, manifest)

        new_stage = patch_stage(old[STAGE_MEMBER][1])

        expected = dict(old)
        next_inode = [max(fields[0] for fields, _ in old.values()) + 1]
        new_manifest = copy.deepcopy(manifest)

        r.set_entry(expected, STAGE_MEMBER, new_stage)
        new_manifest['graphics/stage-phase4-marble.sh'].update(bytes=len(new_stage), sha256=r.sha(new_stage))

        replaced_full = set()
        replaced_info = {}
        for rel, src, mode in REPLACED_FILES:
            full = f'{r.image_tools.COMPAT_DIR}/{rel}'
            require(full in expected, f'Expected existing ramdisk member to replace: {full}')
            blob = src.read_bytes()
            r.set_entry(expected, full, blob)
            new_manifest[rel].update(bytes=len(blob), sha256=r.sha(blob))
            replaced_full.add(full)
            replaced_info[rel] = {'bytes': len(blob), 'mode': oct(mode), 'sha256': r.sha(blob)}

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
        require(got_trailer == old_trailer, 'CPIO trailer changed')
        modified = {STAGE_MEMBER, r.MANIFEST} | replaced_full
        unchanged = set(old) - modified
        require(all(got_raw[slice(*got_spans[n])] == old_raw[slice(*old_spans[n])] for n in unchanged),
                'Unrelated CPIO record changed')

    cand_sha = r.sha(image)
    out_img = out / f'vendor_boot_h20_c_display_{cand_sha[:8]}.img'
    out_img.write_bytes(image)
    report = {
        'base': base_id,
        'candidate': r.identity(out_img),
        'entries_before': len(old),
        'entries_after': len(expected),
        'unchanged_entries': len(unchanged),
        'modified_entries': sorted(modified),
        'replaced_entries': replaced_info,
        'added_entries': added_info,
        'payload_bytes_before': before_layout['payload_end'],
        'payload_bytes_after': after_layout['payload_end'],
    }
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
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
