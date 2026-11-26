#!/usr/bin/env python3
"""Package Batch F6 (clean 8-KO IPA ordering, multi-SIM Sub1 QMI NAS + delta alerts) onto Batch U4 vendor_boot_a."""
from __future__ import annotations

import argparse
import copy
import gzip
import hashlib
import importlib.util
import io
import json
import re
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[2]
F6_BUILD = ROOT / 'work/marble-f6-build'
TEL_SCRIPTS = ROOT / 'devices/marble/runtime/telephony'
LLVM_READELF = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-readelf'
LLVM_NM = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-nm'

ORACLE = ROOT / 'work/marble-m15-role-storage.1v2ynS/build_m15_role_storage.py'
ORACLE_SHA = '7d4f79857a3458ffdb7c8fb86b1cfd893fd9addb02b13e7e05d7b5fa7093cfaa'

BASE_DEFAULT = Path('/mnt/e/marble-tmp/vendor_boot_h20_u4_dcm_soft_tee_778fb2a2.img')
BASE_SHA = '778fb2a28ba24be140e4909d1b7269f2b09ce44ac8cbf0ed47269d40f6a712da'

PREFIX = 'hmos-compat-marble/'
STAGE_MEMBER = PREFIX + 'graphics/stage-phase4-marble.sh'
START_MEMBER = PREFIX + 'graphics/start-phase4-marble.sh'

# Prior protected ramdisk members from U4 (Audio/BT/Sensors/HUKS/U1/U2/U3/U3.2a/U3.2b/U4/F1-F5) preserved byte-identical.
GUARD_MEMBERS = (
    PREFIX + 'graphics/libhmos_audio_vdi.so',
    PREFIX + 'graphics/audio-runtime/lib64/libhmos_pal_server.so',
    PREFIX + 'graphics/init-bluetooth-marble.cfg',
    PREFIX + 'graphics/init-d-sensors-marble.cfg',
    PREFIX + 'graphics/hmos_tqftpserv',
    PREFIX + 'graphics/hmos_rmtfs',
    PREFIX + 'graphics/native_ril_hdi_loader',
    PREFIX + 'graphics/telephony-marble.json',
    PREFIX + 'graphics/init-modem-f2-marble.cfg',
    PREFIX + 'graphics/init-modem-f3-marble.cfg',
    PREFIX + 'graphics/start-modem-f1-marble.sh',
    PREFIX + 'graphics/start-modem-f2-marble.sh',
    PREFIX + 'graphics/start-modem-f3-marble.sh',
    PREFIX + 'graphics/start-modem-f4-marble.sh',
    PREFIX + 'graphics/start-modem-f5-marble.sh',
    PREFIX + 'graphics/libhuks_hdi_service_1.1.z.so',
    PREFIX + 'graphics/libhuks_engine_core_standard.z.so',
    PREFIX + 'graphics/libdbkeys.so',
    PREFIX + 'graphics/libhmos_sceneboard_optional_sa.so',
    PREFIX + 'graphics/init-u1-services-marble.cfg',
    PREFIX + 'graphics/libmarble_gnss_hdi_driver.z.so',
    PREFIX + 'graphics/init-u2-location-marble.cfg',
    PREFIX + 'graphics/native_gnss_hdi_publisher',
    PREFIX + 'graphics/libjpeg_vdi_impl.z.so',
    PREFIX + 'graphics/libdisplayengine_module.so',
    PREFIX + 'graphics/native_venus_v4l2_probe',
    PREFIX + 'graphics/init-u3-codec-display-marble.cfg',
    PREFIX + 'hmos-init-debug.cfg',
    PREFIX + 'graphics/libhmos_boot_vulkan.so',
    PREFIX + 'graphics/vpu20_4v-fw.tar.gz',
    PREFIX + 'graphics/hdf_default_phase4.hcb',
    PREFIX + 'graphics/libdisplay_buffer_vdi_impl.z.so',
    PREFIX + 'graphics/libheifdecode_vdi_impl.z.so',
    PREFIX + 'graphics/native_heif_probe',
    PREFIX + 'graphics/hmos_dcm_service',
    PREFIX + 'graphics/hmos_dcm_token_probe',
    PREFIX + 'graphics/init-dcm-marble.cfg',
)

STAGED_FILES = (
    ('graphics/marble_modem_probe', F6_BUILD / 'marble_modem_probe', 0o755, True),
    ('graphics/libril_vendor_qmi.z.so', F6_BUILD / 'libril_vendor_qmi.z.so', 0o644, True),
    ('graphics/libOMX_Core.z.so', F6_BUILD / 'libOMX_Core.z.so', 0o644, True),
    ('graphics/libvulkan_freedreno.so.gz', F6_BUILD / 'libvulkan_freedreno.so.gz', 0o644, True),
    ('graphics/zink-runtime-marble.tar.gz', F6_BUILD / 'zink-runtime-marble.tar.gz', 0o644, True),
    ('graphics/libdisplay_composer_vdi_impl.z.so', F6_BUILD / 'libdisplay_composer_vdi_impl.z.so', 0o644, True),
    ('graphics/start-u2-postboot-marble.sh', TEL_SCRIPTS / 'start-u2-postboot-marble.sh', 0o755, True),
    ('graphics/start-modem-f6-marble.sh', TEL_SCRIPTS / 'start-modem-f6-marble.sh', 0o755, False),
)

STAGE_ANCHOR = b'$B test -x "$R/compat/bin/init"\necho MARBLE_PHASE4_RAM_COMPAT_READY'
STAGE_F6_BLOCK = (
    b'# === MARBLE_F6_CELLULAR_NAS_STAGE_BEGIN ===\n'
    b'if $B test -f /graphics/start-modem-f6-marble.sh; then\n'
    b' $B cp /graphics/start-modem-f6-marble.sh /compat/bin/start-modem-f6-marble.sh\n'
    b' $B cp /graphics/start-modem-f6-marble.sh "$R/compat/bin/start-modem-f6-marble.sh"\n'
    b' $B chmod 0755 /compat/bin/start-modem-f6-marble.sh "$R/compat/bin/start-modem-f6-marble.sh"\n'
    b' echo "MARBLE_F6_STAGE_READY=1"\n'
    b'fi\n'
    b'# === MARBLE_F6_CELLULAR_NAS_STAGE_END ===\n'
)

START_ANCHOR = b'# BEGIN marble native boot animation\n'
START_VENUS_BLOCK = (
    b'# Load Venus VPU (/dev/video32..33) before boot animation starts so HCODEC HW H.264 decode succeeds.\n'
    b'if [ -x /compat/bin/start-venus-marble.sh ]; then\n'
    b' if $B timeout -k 2 8 $B sh /compat/bin/start-venus-marble.sh > "$R/data/local/tmp/marble-venus-early.log" 2>&1; then\n'
    b'  log "VENUS_EARLY_READY=1"\n'
    b' else\n'
    b'  log "VENUS_EARLY_RC=$?"\n'
    b' fi\n'
    b'fi\n\n'
)


def require(cond: bool, msg: str) -> None:
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


def patch_stage(raw: bytes) -> bytes:
    for marker in (b'MARBLE_F6_CELLULAR_NAS_STAGE_BEGIN', b'start-modem-f6-marble.sh'):
        require(marker not in raw, f'stage script already contains {marker!r}')
    out = splice_once(raw, ((STAGE_ANCHOR, STAGE_F6_BLOCK + STAGE_ANCHOR),), STAGE_MEMBER)
    require(b'\r' not in out, 'CR byte in patched stage-phase4-marble.sh')
    subprocess.run(['sh', '-n'], input=out, check=True)
    return out


def patch_start(raw: bytes) -> bytes:
    require(b'VENUS_EARLY_READY' not in raw, 'start script already contains VENUS_EARLY_READY')
    out = splice_once(raw, ((START_ANCHOR, START_VENUS_BLOCK + START_ANCHOR),), START_MEMBER)
    require(b'\r' not in out, 'CR byte in patched start-phase4-marble.sh')
    subprocess.run(['sh', '-n'], input=out, check=True)
    return out


def validate_inputs() -> None:
    for rel, src, _mode, _replaces in STAGED_FILES:
        require(src.is_file(), f'Missing staged F6 artifact: {src}')
        raw = src.read_bytes()
        if rel.endswith('.sh'):
            require(b'\r' not in raw, f'CR byte found in {src}')
            subprocess.run(['sh', '-n'], input=raw, check=True)
        elif rel.endswith('.tar.gz'):
            with tarfile.open(fileobj=io.BytesIO(raw), mode='r:gz') as tf:
                names = {m.name.lstrip('./') for m in tf.getmembers()}
                require('libgallium-25.0.1.so' in names, f'Missing libgallium-25.0.1.so in {rel}')
                m = tf.getmember('libgallium-25.0.1.so')
                gal = tf.extractfile(m).read()
                require(gal[:4] == b'\x7fELF', f'Expected ELF libgallium-25.0.1.so in {rel}')
                require(b'ZINK_MSAA_EXPAND' in gal, f'Missing ZINK_MSAA_EXPAND guard in {rel}')
        elif rel.endswith('.so.gz'):
            require(raw[:2] == b'\x1f\x8b', f'Expected gzip header for {rel}')
            decompressed = gzip.decompress(raw)
            require(decompressed[:4] == b'\x7fELF', f'Expected ELF payload inside gzip for {rel}')
        elif rel.endswith('.z.so'):
            require(raw[:4] == b'\x7fELF', f'Expected ELF shared library for {rel}')
            if LLVM_READELF.is_file():
                dyn = subprocess.check_output([str(LLVM_READELF), '-d', str(src)], text=True)
                needed = re.findall(r'\(NEEDED\)\s+Shared library:\s+\[([^\]]+)\]', dyn)
                soname = re.findall(r'\(SONAME\)\s+Library soname:\s+\[([^\]]+)\]', dyn)
                if rel.endswith('libril_vendor_qmi.z.so'):
                    require(needed == ['libc.so'], f'Unexpected NEEDED for {rel}: {needed}')
                    require(soname == ['libril_vendor.z.so'], f'Unexpected SONAME for {rel}: {soname}')
                elif rel.endswith('libOMX_Core.z.so'):
                    require(needed == ['libc.so'], f'Unexpected NEEDED for {rel}: {needed}')
                    require(soname == ['libOMX_Core.z.so'], f'Unexpected SONAME for {rel}: {soname}')
                elif rel.endswith('libdisplay_composer_vdi_impl.z.so'):
                    require(soname == ['libdisplay_composer_vdi_impl.z.so'], f'Unexpected SONAME for {rel}: {soname}')
            if LLVM_NM.is_file():
                syms = subprocess.check_output([str(LLVM_NM), '-D', '--defined-only', str(src)], text=True)
                if rel.endswith('libril_vendor_qmi.z.so'):
                    require('RilInitOps' in syms, f'Missing RilInitOps export in {rel}')
                elif rel.endswith('libOMX_Core.z.so'):
                    require('OMX_GetHandle' in syms, f'Missing OMX_GetHandle export in {rel}')
                elif rel.endswith('libdisplay_composer_vdi_impl.z.so'):
                    require('LoadVdiImpl' in syms, f'Missing LoadVdiImpl export in {rel}')
        else:
            require(raw[:4] == b'\x7fELF', f'Expected ELF executable for {rel}')
            finfo = subprocess.check_output(['file', str(src)], text=True)
            require('ARM aarch64' in finfo and 'statically linked' in finfo, f'Unexpected probe ELF: {finfo}')
            require(b'ARM64 Bionic' not in raw, f'{rel} must not use static Bionic')


def load_oracle():
    require(ORACLE.is_file() and ORACLE.resolve() == ORACLE, f'Missing oracle: {ORACLE}')
    got_sha = hashlib.sha256(ORACLE.read_bytes()).hexdigest()
    require(got_sha == ORACLE_SHA, f'Oracle SHA256 mismatch: {got_sha}')
    spec = importlib.util.spec_from_file_location('archive_oracle_f6', ORACLE)
    arc = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(arc)
    return arc, arc.load_tools()


def update_manifest_entry(manifest: dict, member: str, blob: bytes, mode: int, sha_fn) -> None:
    require(member.startswith(PREFIX), f'Invalid manifest member prefix: {member}')
    rel = member[len(PREFIX):]
    if rel in manifest:
        manifest[rel].update(bytes=len(blob), mode=oct(mode), sha256=sha_fn(blob))
    else:
        manifest[rel] = {'bytes': len(blob), 'mode': oct(mode), 'sha256': sha_fn(blob)}


def build(base_path: Path, out_dir: Path) -> Path:
    require(base_path.is_file(), f'Missing base image: {base_path}')
    validate_inputs()
    out_dir.mkdir(parents=True, exist_ok=True)
    F6_BUILD.mkdir(parents=True, exist_ok=True)

    archive, r = load_oracle()
    base_bytes = r.read_original(base_path)
    base_sha = r.sha(base_bytes)
    require(base_sha == BASE_SHA, f'Base SHA256 mismatch: got {base_sha}, want {BASE_SHA}')
    base_id = r.identity(base_path)

    before_layout = r.layout(base_bytes)
    before_avb = r.verify_existing_avb(base_bytes, before_layout['payload_end'])

    with tempfile.TemporaryDirectory(prefix='h20_f6_', dir=str(F6_BUILD)) as tmp:
        scratch = Path(tmp)
        old, old_raw, kind, old_rd, args = r.extract_entries(base_path, base_bytes, before_layout, scratch / 'before')
        for gm in GUARD_MEMBERS:
            require(gm in old, f'Guard member missing in U4 base image: {gm}')

        old_manifest = archive.parse(old[r.MANIFEST][1])
        r.manifest_holds(old, old_manifest)

        expected = dict(old)
        new_manifest = copy.deepcopy(old_manifest)
        modified_members = {r.MANIFEST}

        new_stage = patch_stage(old[STAGE_MEMBER][1])
        r.set_entry(expected, STAGE_MEMBER, new_stage)
        modified_members.add(STAGE_MEMBER)
        stage_rel = STAGE_MEMBER[len(PREFIX):]
        if stage_rel in new_manifest:
            new_manifest[stage_rel].update(bytes=len(new_stage), sha256=r.sha(new_stage))

        new_start = patch_start(old[START_MEMBER][1])
        r.set_entry(expected, START_MEMBER, new_start)
        modified_members.add(START_MEMBER)
        start_rel = START_MEMBER[len(PREFIX):]
        if start_rel in new_manifest:
            new_manifest[start_rel].update(bytes=len(new_start), sha256=r.sha(new_start))

        staged_info = {}
        next_inode = [max(header[0] for header, _ in old.values()) + 1]
        for rel, src, mode, replaces in STAGED_FILES:
            full = PREFIX + rel
            blob = src.read_bytes()
            if replaces:
                require(full in expected, f'Expected existing member to replace: {full}')
                r.set_entry(expected, full, blob)
                modified_members.add(full)
            else:
                require(full not in expected, f'Unexpected existing member: {full}')
                r.image_tools.put_file(expected, full, blob, mode, next_inode)
                expected[full][0][11] = len(full.encode('utf-8')) + 1
            update_manifest_entry(new_manifest, full, blob, mode, r.sha)
            staged_info[rel] = {
                'bytes': len(blob),
                'mode': oct(mode),
                'sha256': r.sha(blob),
                'replaced': replaces,
            }

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
        require(
            after_layout['payload_end'] == len(payload) <= before_layout['payload_end'] + 4 * 1024 * 1024,
            f'Unexpected payload growth: {before_layout["payload_end"]} -> {after_layout["payload_end"]}',
        )
        r.check_container(base_bytes, before_layout, payload, after_layout)
        image, updated = r.image_tools.preserve_and_update_avb_footer(payload, base_bytes)
        require(updated, 'Existing AVB footer metadata not updated')
        after_avb = r.verify_existing_avb(image, len(payload))
        require(
            all(after_avb[k] == before_avb[k] for k in ('vbmeta_masked', 'footer_masked')),
            'Unexpected AVB metadata change',
        )

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
        require(
            all(got_raw[slice(*got_spans[n])] == old_raw[slice(*old_spans[n])] for n in unchanged),
            'Unrelated CPIO record changed',
        )

    cand_sha = r.sha(image)
    out_img = out_dir / f'vendor_boot_h20_f6_cellular_nas_{cand_sha[:8]}.img'
    out_img.write_bytes(image)
    report = {
        'base': base_id,
        'candidate': r.identity(out_img),
        'entries_before': len(old),
        'entries_after': len(expected),
        'guard_members_verified': list(GUARD_MEMBERS),
        'staged': staged_info,
    }
    report_path = F6_BUILD / 'f6-build-report.json'
    report_path.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f'CANDIDATE_IMAGE={out_img}')
    print(f'CANDIDATE_SHA256={cand_sha}')
    print(f'CANDIDATE_SHA8={cand_sha[:8]}')
    print(f'BUILD_REPORT={report_path}')
    return out_img


def main() -> None:
    parser = argparse.ArgumentParser(description='Package Batch F6 cellular NAS vendor_boot image.')
    parser.add_argument('--base', type=Path, default=BASE_DEFAULT)
    parser.add_argument('--out', type=Path, default=Path('/mnt/e/marble-tmp'))
    args = parser.parse_args()
    build(args.base, args.out)


if __name__ == '__main__':
    main()
