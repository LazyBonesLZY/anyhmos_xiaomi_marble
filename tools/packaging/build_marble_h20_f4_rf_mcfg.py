#!/usr/bin/env python3
"""Package Batch F4 (Fixed TQFTPSERV MCFG/RFS + vsimModemCount=0 + enhanced RF/LOC probes) onto F3 vendor_boot_a."""
import argparse
import copy
import importlib.util
import json
import subprocess
import tempfile
from hashlib import sha256
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / 'work'
F4_BUILD = WORK / 'marble-f4-build'
BASE_DEFAULT = Path('/mnt/e/marble-tmp/vendor_boot_h20_f3_rf_gnss_e4043df4.img')
BASE_SHA = 'e4043df42d9c703c064cae8b900ee6305e5f16665330eb2cd31a2fc379cb15d5'
ORACLE = WORK / 'marble-m15-role-storage.1v2ynS/build_m15_role_storage.py'
ORACLE_SHA = '7d4f79857a3458ffdb7c8fb86b1cfd893fd9addb02b13e7e05d7b5fa7093cfaa'

F4_SCRIPTS = ROOT / 'devices/marble/runtime/telephony'
STAGE_MEMBER = 'hmos-compat-marble/graphics/stage-phase4-marble.sh'
STAGE_ANCHOR = b'$B test -x "$R/compat/bin/init"\necho MARBLE_PHASE4_RAM_COMPAT_READY'

# Core protected members that F4 must keep byte-for-byte identical to F3 base.
GUARD_MEMBERS = (
    'hmos-compat-marble/graphics/libhmos_audio_vdi.so',
    'hmos-compat-marble/graphics/audio-runtime/lib64/libhmos_pal_server.so',
    'hmos-compat-marble/graphics/init-bluetooth-marble.cfg',
    'hmos-compat-marble/graphics/hmos_rmtfs',
    'hmos-compat-marble/graphics/start-modem-f1-marble.sh',
    'hmos-compat-marble/graphics/libril_vendor_qmi.z.so',
    'hmos-compat-marble/graphics/native_ril_hdi_loader',
    'hmos-compat-marble/graphics/hdf_default_phase4.hcb',
    'hmos-compat-marble/graphics/init-modem-f3-marble.cfg',
    'hmos-compat-marble/graphics/telephony-marble.json',
)

# New or replaced ramdisk members under hmos-compat-marble/.
STAGED_FILES = (
    ('graphics/hmos_tqftpserv', F4_BUILD / 'hmos_tqftpserv', 0o755, True),
    ('graphics/marble_modem_probe', F4_BUILD / 'marble_modem_probe', 0o755, True),
    ('graphics/native_gnss_hdi_publisher', F4_BUILD / 'native_gnss_hdi_publisher', 0o755, True),
    ('graphics/start-modem-f4-marble.sh', F4_SCRIPTS / 'start-modem-f4-marble.sh', 0o755, False),
)

STAGE_F4_BLOCK = (
    b'# === MARBLE_F4_RF_MCFG_STAGE_BEGIN ===\n'
    b'if $B test -f /graphics/start-modem-f4-marble.sh; then\n'
    b' $B cp /graphics/start-modem-f4-marble.sh /compat/bin/start-modem-f4-marble.sh\n'
    b' $B cp /graphics/start-modem-f4-marble.sh "$R/compat/bin/start-modem-f4-marble.sh"\n'
    b' $B chmod 0755 /compat/bin/start-modem-f4-marble.sh "$R/compat/bin/start-modem-f4-marble.sh"\n'
    b'fi\n'
    b'f4_patch_para() {\n'
    b' f4_src="$1"; f4_dst="$2"; shift 2\n'
    b' if ! $B test -f "$f4_src"; then return 0; fi\n'
    b' $B cp "$f4_src" "$f4_dst"\n'
    b' for f4_kv in "$@"; do\n'
    b'  f4_k="${f4_kv%%=*}"\n'
    b'  f4_v="${f4_kv#*=}"\n'
    b'  $B awk -v k="$f4_k" -v v="$f4_v" \'BEGIN{seen=0} index($0, k "=")==1{print k "=" v; seen=1; next} {print} END{if(!seen) print k "=" v}\' "$f4_dst" > "$f4_dst.tmp"\n'
    b'  $B mv "$f4_dst.tmp" "$f4_dst"\n'
    b' done\n'
    b' $B chmod 0644 "$f4_dst"\n'
    b' if $B cp "$f4_dst" "$f4_src" 2>/dev/null; then\n'
    b'  $B chmod 0644 "$f4_src" 2>/dev/null || true\n'
    b' elif ! $B grep -q " $f4_src " /proc/mounts 2>/dev/null; then\n'
    b'  $B mount --bind "$f4_dst" "$f4_src"\n'
    b' fi\n'
    b'}\n'
    b'f4_patch_para "$R/sys_prod/variant/region_comm/china/etc/param/hw_defaults.para" "$R/compat/etc/f4-sysprod-hw-defaults.para" "const.telephony.vsimModemCount=0"\n'
    b'echo MARBLE_F4_STAGE_READY\n'
    b'# === MARBLE_F4_RF_MCFG_STAGE_END ===\n'
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
    for marker in (
        b'MARBLE_F4_RF_MCFG_STAGE_BEGIN',
        b'start-modem-f4-marble.sh',
        b'const.telephony.vsimModemCount=0',
    ):
        require(marker not in stage_bytes, f'stage script already contains {marker!r}')
    patched = splice_once(
        stage_bytes,
        ((STAGE_ANCHOR, STAGE_F4_BLOCK + STAGE_ANCHOR),),
        'stage-phase4-marble.sh',
    )
    subprocess.run(['sh', '-n'], input=patched, check=True)
    return patched


def validate_inputs() -> None:
    for rel, src, _mode, _replaces in STAGED_FILES:
        require(src.is_file(), f'Missing staged F4 artifact: {src}')
        raw = src.read_bytes()
        if rel.endswith('.sh'):
            subprocess.run(['sh', '-n'], input=raw, check=True)
        else:
            require(raw[:4] == b'\x7fELF', f'Expected ELF binary for {rel}')


def build(base_path: Path, out: Path) -> Path:
    require(sha256(ORACLE.read_bytes()).hexdigest() == ORACLE_SHA, 'Archive verifier identity changed')
    spec = importlib.util.spec_from_file_location('h20_f4_archive_verifier', ORACLE)
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
    F4_BUILD.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix='pack_f4.', dir=out) as tmp:
        scratch = Path(tmp)
        old, old_raw, kind, old_rd, args = r.extract_entries(base_path, base, before_layout, scratch / 'before')
        for gm in GUARD_MEMBERS:
            require(gm in old, f'Guard member missing in base image: {gm}')
        manifest = archive.parse(old[r.MANIFEST][1])
        r.manifest_holds(old, manifest)

        new_stage = patch_stage(old[STAGE_MEMBER][1])

        expected = dict(old)
        next_inode = [max(fields[0] for fields, _ in old.values()) + 1]
        new_manifest = copy.deepcopy(manifest)

        r.set_entry(expected, STAGE_MEMBER, new_stage)
        new_manifest['graphics/stage-phase4-marble.sh'].update(bytes=len(new_stage), sha256=r.sha(new_stage))

        staged_info = {}
        replaced_full = set()
        for rel, src, mode, replaces in STAGED_FILES:
            full = f'{r.image_tools.COMPAT_DIR}/{rel}'
            blob = src.read_bytes()
            if replaces:
                require(full in expected, f'Expected existing member to replace: {full}')
                r.set_entry(expected, full, blob)
                replaced_full.add(full)
                new_manifest[rel].update(bytes=len(blob), mode=oct(mode), sha256=r.sha(blob))
            else:
                require(full not in expected, f'Duplicate ramdisk member: {full}')
                r.image_tools.put_file(expected, full, blob, mode, next_inode)
                expected[full][0][11] = len(full.encode('utf-8')) + 1
                new_manifest[rel] = {'bytes': len(blob), 'mode': oct(mode), 'sha256': r.sha(blob)}
            staged_info[rel] = {'bytes': len(blob), 'mode': oct(mode), 'sha256': r.sha(blob), 'replaced': replaces}

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
        after_avb = r.verify_existing_avb(image, len(payload))
        require(all(after_avb[k] == before_avb[k] for k in ('vbmeta_masked', 'footer_masked')),
                'Unexpected AVB metadata change')

        verify = scratch / 'roundtrip.img'
        verify.write_bytes(image)
        got, got_raw, got_kind, _, _ = r.extract_entries(verify, image, after_layout, scratch / 'after')
        require(got == expected and got_raw == raw and got_kind == kind, 'Exact unpack round-trip failed')

        for gm in GUARD_MEMBERS:
            require(got[gm][1] == old[gm][1], f'Protected member altered: {gm}')

        old_spans, old_trailer = archive.record_spans(old_raw)
        got_spans, got_trailer = archive.record_spans(got_raw)
        require(got_trailer == old_trailer, 'CPIO trailer changed')
        modified = {STAGE_MEMBER, r.MANIFEST} | replaced_full
        unchanged = set(old) - modified
        require(all(got_raw[slice(*got_spans[n])] == old_raw[slice(*old_spans[n])] for n in unchanged),
                'Unrelated CPIO record changed')

    cand_sha = r.sha(image)
    out_img = out / f'vendor_boot_h20_f4_rf_mcfg_{cand_sha[:8]}.img'
    out_img.write_bytes(image)
    report = {
        'base': base_id,
        'candidate': r.identity(out_img),
        'entries_before': len(old),
        'entries_after': len(expected),
        'guard_members_verified': list(GUARD_MEMBERS),
        'staged': staged_info,
    }
    report_path = F4_BUILD / 'f4-build-report.json'
    report_path.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f'CANDIDATE_IMAGE={out_img}')
    print(f'CANDIDATE_SHA256={cand_sha}')
    print(f'CANDIDATE_SHA8={cand_sha[:8]}')
    print(f'BUILD_REPORT={report_path}')
    return out_img


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--base', type=Path, default=BASE_DEFAULT)
    p.add_argument('--out', type=Path, default=Path('/mnt/e/marble-tmp'))
    args = p.parse_args()
    build(args.base, args.out)


if __name__ == '__main__':
    main()
