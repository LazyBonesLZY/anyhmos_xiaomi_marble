#!/usr/bin/env python3
"""Build Xiaomi marble Batch U3.1 (native hardware JPEG VDI + DisplayEngine V1.1 wrapper + SA 65962).

Packages cleanly on top of Batch U2.1 vendor_boot_h20_u2_loc_media_974b7b75.img
while keeping all Batch F2/F5/U1/U2 protected members byte-identical.
"""
import argparse
import copy
import importlib.util
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / 'tools/packaging'
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import build_marble_phase4_images as p4

U3_BUILD = ROOT / 'work/marble-u3-build'
CODEC_CFG = ROOT / 'devices/marble/runtime/codec/init-u3-codec-display-marble.cfg'
DONOR_FS_DEFAULT = ROOT / 'work/donor-rom/ADY-AL10-207.0.0.111-SP10/fs'

ORACLE = ROOT / 'work/marble-m15-role-storage.1v2ynS/build_m15_role_storage.py'
ORACLE_SHA = '7d4f79857a3458ffdb7c8fb86b1cfd893fd9addb02b13e7e05d7b5fa7093cfaa'

BASE_DEFAULT = Path('/mnt/e/marble-tmp/vendor_boot_h20_u2_loc_media_974b7b75.img')
BASE_SHA = '974b7b7528d2a6d9f9669cf7bc19c2862cdfeb77192e71daf6d75331a1d534aa'
PREFIX = 'hmos-compat-marble/'

HCB_MEMBER = PREFIX + 'graphics/hdf_default_phase4.hcb'
INIT_DEBUG_MEMBER = PREFIX + 'hmos-init-debug.cfg'
STAGE_MEMBER = PREFIX + 'graphics/stage-phase4-marble.sh'
START_PHASE4_MEMBER = PREFIX + 'graphics/start-phase4-marble.sh'

GUARD_MEMBERS = (
    PREFIX + 'graphics/libhmos_audio_vdi.so',
    PREFIX + 'graphics/audio-runtime/lib64/libhmos_pal_server.so',
    PREFIX + 'graphics/init-bluetooth-marble.cfg',
    PREFIX + 'graphics/init-d-sensors-marble.cfg',
    PREFIX + 'graphics/hmos_tqftpserv',
    PREFIX + 'graphics/marble_modem_probe',
    PREFIX + 'graphics/libril_vendor_qmi.z.so',
    PREFIX + 'graphics/start-modem-f5-marble.sh',
    PREFIX + 'graphics/hmos_rmtfs',
    PREFIX + 'graphics/native_ril_hdi_loader',
    PREFIX + 'graphics/telephony-marble.json',
    PREFIX + 'graphics/libhuks_hdi_service_1.1.z.so',
    PREFIX + 'graphics/libhuks_engine_core_standard.z.so',
    PREFIX + 'graphics/libdbkeys.so',
    PREFIX + 'graphics/libhmos_sceneboard_optional_sa.so',
    PREFIX + 'graphics/init-u1-services-marble.cfg',
    PREFIX + 'graphics/libmarble_gnss_hdi_driver.z.so',
    PREFIX + 'graphics/init-u2-location-marble.cfg',
)

STAGED_FILES = (
    ('graphics/libjpeg_vdi_impl.z.so', U3_BUILD / 'libjpeg_vdi_impl.z.so', 0o644),
    ('graphics/libdisplayengine_module.so', U3_BUILD / 'libdisplayengine_module.so', 0o644),
    ('graphics/native_venus_v4l2_probe', U3_BUILD / 'native_venus_v4l2_probe', 0o755),
    ('graphics/init-u3-codec-display-marble.cfg', CODEC_CFG, 0o644),
)

OPT_SA_ANCHOR_OLD = b'65936 65952 65962 66006 66048'
OPT_SA_ANCHOR_NEW = b'65936 65952 66006 66048'

STAGE_ANCHOR = b'$B test -x "$R/compat/bin/init"\necho MARBLE_PHASE4_RAM_COMPAT_READY'
STAGE_U3_BLOCK = (
    b'# === MARBLE_U3_MEDIA_DISPLAY_STAGE_BEGIN ===\n'
    b'u3_bind() {\n'
    b' $B grep -q " $2 " /proc/mounts 2>/dev/null && return 0\n'
    b' $B mount --bind "$1" "$2" 2>/dev/null || echo "MARBLE_U3_BIND_FAIL dst=$2"\n'
    b'}\n'
    b'$B mkdir -p /compat/bin "$R/compat/bin" "$R/compat/lib64" "$R/compat/etc" "$R/system/etc/param" "$R/data/local/tmp/marble-codec"\n'
    b'$B chown 3031:1003 "$R/data/local/tmp/marble-codec" 2>/dev/null || true\n'
    b'$B chmod 0775 "$R/data/local/tmp/marble-codec" 2>/dev/null || true\n'
    b'u3_libs=0\n'
    b'if $B test -f /graphics/libjpeg_vdi_impl.z.so; then\n'
    b' $B cp /graphics/libjpeg_vdi_impl.z.so "$R/compat/lib64/libjpeg_vdi_impl.z.so"\n'
    b' $B chmod 0644 "$R/compat/lib64/libjpeg_vdi_impl.z.so"\n'
    b' u3_libs=$((u3_libs + 1))\n'
    b' if $B test -f "$R/vendor/lib64/libjpeg_vdi_impl.z.so"; then\n'
    b'  u3_bind "$R/compat/lib64/libjpeg_vdi_impl.z.so" "$R/vendor/lib64/libjpeg_vdi_impl.z.so"\n'
    b' fi\n'
    b'fi\n'
    b'if $B test -f /graphics/libdisplayengine_module.so; then\n'
    b' $B cp /graphics/libdisplayengine_module.so "$R/compat/lib64/libdisplayengine_module.so"\n'
    b' $B chmod 0644 "$R/compat/lib64/libdisplayengine_module.so"\n'
    b' u3_libs=$((u3_libs + 1))\n'
    b' if $B test -f "$R/vendor/lib64/hw/libdisplayengine_module.so"; then\n'
    b'  u3_bind "$R/compat/lib64/libdisplayengine_module.so" "$R/vendor/lib64/hw/libdisplayengine_module.so"\n'
    b' fi\n'
    b'fi\n'
    b'if $B test -x /graphics/native_venus_v4l2_probe; then\n'
    b' $B cp /graphics/native_venus_v4l2_probe /compat/bin/native_venus_v4l2_probe\n'
    b' $B cp /graphics/native_venus_v4l2_probe "$R/compat/bin/native_venus_v4l2_probe"\n'
    b' $B chmod 0755 /compat/bin/native_venus_v4l2_probe "$R/compat/bin/native_venus_v4l2_probe"\n'
    b'fi\n'
    b'if $B test -f /graphics/init-u3-codec-display-marble.cfg; then\n'
    b' $B cp /graphics/init-u3-codec-display-marble.cfg "$R/compat/etc/init-u3-codec-display-marble.cfg"\n'
    b' $B chmod 0644 "$R/compat/etc/init-u3-codec-display-marble.cfg"\n'
    b'fi\n'
    b"$B cat <<'EOF' > \"$R/system/etc/param/marble_codec.para\"\n"
    b'persist.multimedia.image.hardwaredecode.enabled=true\n'
    b'EOF\n'
    b'$B chmod 0644 "$R/system/etc/param/marble_codec.para"\n'
    b'echo "MARBLE_U3_STAGE_READY u3_libs=$u3_libs"\n'
    b'# === MARBLE_U3_MEDIA_DISPLAY_STAGE_END ===\n'
)

START_SERVICES_ANCHOR = (
    b'stage=services\n'
    b'for gate in storage kvdata useriam account appspawn theme wallpaper ui diagnostics deviceproviders b1; do'
)
START_U3_BLOCK = (
    b'# === MARBLE_U3_MEDIA_DISPLAY_START_BEGIN ===\n'
    b'stage=u3_media\n'
    b'u3_skip=$(pget marble.debug.skip.u3_media)\n'
    b'u3_skip_list=$($B cat "$R/data/local/tmp/marble-skip-stages" 2>/dev/null | $B tr -d \' \\t\\r\\n\' || true)\n'
    b'case ",$u3_skip_list," in\n'
    b' *,u3_media,*) u3_skip=true ;;\n'
    b'esac\n'
    b'if [ "$u3_skip" = true ] || [ "$u3_skip" = 1 ]; then\n'
    b' log "U3_MEDIA_SKIP=requested"\n'
    b'else\n'
    b' pset marble.debug.start.u3_media true\n'
    b' log "U3_MEDIA_TRIGGERED=1"\n'
    b'fi\n'
    b'# === MARBLE_U3_MEDIA_DISPLAY_START_END ===\n'
)


def require(cond, msg):
    if not cond:
        raise ValueError(msg)


def splice_once(data: bytes, pairs: tuple[tuple[bytes, bytes], ...], label: str) -> bytes:
    out = data
    for old, new in pairs:
        cnt = out.count(old)
        require(cnt == 1, f'{label}: anchor count={cnt} (expected 1): {old[:80]!r}')
        out = out.replace(old, new, 1)
    reverted = out
    for old, new in reversed(pairs):
        cnt = reverted.count(new)
        require(cnt == 1, f'{label}: delta not uniquely reversible (count={cnt})')
        reverted = reverted.replace(new, old, 1)
    require(reverted == data, f'{label}: non-target bytes changed during splice check')
    return out


def load_oracle():
    require(ORACLE.is_file() and ORACLE.resolve() == ORACLE, f'Missing oracle: {ORACLE}')
    import hashlib
    got_sha = hashlib.sha256(ORACLE.read_bytes()).hexdigest()
    require(got_sha == ORACLE_SHA, f'Oracle SHA256 mismatch: {got_sha}')
    spec = importlib.util.spec_from_file_location('archive_oracle_u3', ORACLE)
    arc = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(arc)
    return arc, arc.load_tools()


def patch_init_debug(raw: bytes) -> bytes:
    require(b'init-u3-codec-display-marble.cfg' not in raw, 'init-debug already imports U3 cfg')
    old_imp = b'    "/compat/etc/init-u2-location-marble.cfg"\n  ],'
    new_imp = (
        b'    "/compat/etc/init-u2-location-marble.cfg",\n'
        b'    "/compat/etc/init-u3-codec-display-marble.cfg"\n  ],'
    )
    out = splice_once(raw, ((old_imp, new_imp),), INIT_DEBUG_MEMBER)
    doc = json.loads(out.decode('utf-8'))
    require('/compat/etc/init-u3-codec-display-marble.cfg' in doc.get('import', []),
            'U3 init import missing after JSON parse')
    require(len(out) < 32768, f'hmos-init-debug.cfg exceeds 32768-byte init cap: {len(out)}')
    return out


def patch_stage(raw: bytes) -> bytes:
    for marker in (
        b'MARBLE_U3_MEDIA_DISPLAY_STAGE_BEGIN',
        b'libjpeg_vdi_impl.z.so',
        b'libdisplayengine_module.so',
        b'native_venus_v4l2_probe',
        b'init-u3-codec-display-marble.cfg',
        b'marble_codec.para',
    ):
        require(marker not in raw, f'stage script already contains {marker!r}')
    out = splice_once(
        raw,
        (
            (OPT_SA_ANCHOR_OLD, OPT_SA_ANCHOR_NEW),
            (STAGE_ANCHOR, STAGE_U3_BLOCK + STAGE_ANCHOR),
        ),
        STAGE_MEMBER,
    )
    require(b'\r' not in out, 'CR byte in patched stage-phase4-marble.sh')
    subprocess.run(['sh', '-n'], input=out, check=True)
    return out


def patch_start_phase4(raw: bytes) -> bytes:
    for marker in (
        b'MARBLE_U3_MEDIA_DISPLAY_START_BEGIN',
        b'marble.debug.start.u3_media',
    ):
        require(marker not in raw, f'start script already contains {marker!r}')
    out = splice_once(
        raw,
        ((START_SERVICES_ANCHOR, START_U3_BLOCK + START_SERVICES_ANCHOR),),
        START_PHASE4_MEMBER,
    )
    require(b'\r' not in out, 'CR byte in patched start-phase4-marble.sh')
    subprocess.run(['sh', '-n'], input=out, check=True)
    return out


def assert_no_duplicate_services(entries: dict, service_names: tuple[str, ...]) -> None:
    for member, (_, blob) in entries.items():
        if not member.endswith('.cfg'):
            continue
        text = blob.decode('utf-8', errors='ignore')
        try:
            doc = json.loads(text)
        except json.JSONDecodeError:
            continue
        defined = {svc.get('name') for svc in doc.get('services', []) if isinstance(svc, dict)}
        overlap = defined & set(service_names)
        require(not overlap, f'Duplicate service definition {sorted(overlap)} already in {member}')


def update_manifest_entry(manifest: dict, member: str, blob: bytes, sha_fn) -> None:
    if member.startswith(PREFIX):
        rel = member[len(PREFIX):]
        if rel in manifest:
            manifest[rel].update(bytes=len(blob), sha256=sha_fn(blob))


def build(base: Path, out_dir: Path, donor_fs: Path = DONOR_FS_DEFAULT) -> Path:
    require(base.is_file(), f'Missing base image: {base}')
    require(donor_fs.is_dir(), f'Missing donor fs: {donor_fs}')
    out_dir.mkdir(parents=True, exist_ok=True)
    U3_BUILD.mkdir(parents=True, exist_ok=True)

    json.loads(CODEC_CFG.read_text(encoding='utf-8'))
    for _, src, _ in STAGED_FILES:
        require(src.is_file(), f'Missing staged U3 artifact: {src}')

    archive, r = load_oracle()
    base_bytes = r.read_original(base)
    base_sha = r.sha(base_bytes)
    require(base_sha == BASE_SHA, f'Base SHA256 mismatch: got {base_sha}, want {BASE_SHA}')
    base_id = {'path': str(base), 'bytes': len(base_bytes), 'sha256': base_sha}

    before_layout = r.layout(base_bytes)
    before_avb = r.verify_existing_avb(base_bytes, before_layout['payload_end'])

    expected_old_hcb = p4.build_phase4_hdf_hcb(
        wifi_chip_donor=donor_fs,
        wpa_on_demand_donor=donor_fs,
        a2dp_donor=donor_fs,
        ril_donor=donor_fs,
        location_host=True,
    )
    new_hcb = p4.build_phase4_hdf_hcb(
        wifi_chip_donor=donor_fs,
        wpa_on_demand_donor=donor_fs,
        a2dp_donor=donor_fs,
        ril_donor=donor_fs,
        location_host=True,
        displayengine=True,
        codec_image_host=True,
    )

    with tempfile.TemporaryDirectory(prefix='h20_u3_', dir=str(U3_BUILD)) as tmp:
        scratch = Path(tmp)
        old, old_raw, kind, old_rd, args = r.extract_entries(base, base_bytes, before_layout, scratch / 'before')
        old_manifest = json.loads(old[r.MANIFEST][1].decode('utf-8'))
        r.manifest_holds(old, old_manifest)

        require(old[HCB_MEMBER][1] == expected_old_hcb, 'Base HCB does not match U2.1 8-host HCB')
        assert_no_duplicate_services(old, ('codec_host', 'display_engine_service'))

        expected = dict(old)
        new_manifest = copy.deepcopy(old_manifest)
        modified_members = {r.MANIFEST}

        new_init = patch_init_debug(old[INIT_DEBUG_MEMBER][1])
        new_stage = patch_stage(old[STAGE_MEMBER][1])
        new_start = patch_start_phase4(old[START_PHASE4_MEMBER][1])

        for member, blob in (
            (HCB_MEMBER, new_hcb),
            (INIT_DEBUG_MEMBER, new_init),
            (STAGE_MEMBER, new_stage),
            (START_PHASE4_MEMBER, new_start),
        ):
            r.set_entry(expected, member, blob)
            modified_members.add(member)
            update_manifest_entry(new_manifest, member, blob, r.sha)

        next_inode = [max(header[0] for header, _ in old.values()) + 1]
        staged_info = {}
        for rel, src, mode in STAGED_FILES:
            full = PREFIX + rel
            blob = src.read_bytes()
            require(full not in expected, f'Unexpected pre-existing U3 ramdisk member: {full}')
            r.image_tools.put_file(expected, full, blob, mode, next_inode)
            expected[full][0][11] = len(full.encode('utf-8')) + 1
            new_manifest[rel] = {'bytes': len(blob), 'mode': oct(mode), 'sha256': r.sha(blob)}
            staged_info[rel] = {'bytes': len(blob), 'mode': oct(mode), 'sha256': r.sha(blob), 'replaced': False}

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
        r.check_container(base_bytes, before_layout, payload, after_layout)
        image, updated = r.image_tools.preserve_and_update_avb_footer(payload, base_bytes)
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
        unchanged = set(old) - modified_members
        require(all(got_raw[slice(*got_spans[n])] == old_raw[slice(*old_spans[n])] for n in unchanged),
                'Unrelated CPIO record changed')

    cand_sha = r.sha(image)
    out_img = out_dir / f'vendor_boot_h20_u3_media_display_{cand_sha[:8]}.img'
    out_img.write_bytes(image)
    report = {
        'base': base_id,
        'candidate': r.identity(out_img),
        'entries_before': len(old),
        'entries_after': len(expected),
        'guard_members_verified': list(GUARD_MEMBERS),
        'staged': staged_info,
    }
    report_path = U3_BUILD / 'h20-u3-media-display-report.json'
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
