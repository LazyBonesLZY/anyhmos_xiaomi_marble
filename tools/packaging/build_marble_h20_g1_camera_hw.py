#!/usr/bin/env python3
"""Package Batch G1 (SM7475 Camera kernel stack + LX7 ICP FW + 4-sensor CCI probe) onto F9 vendor_boot_a."""
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
G1_BUILD = ROOT / 'work/marble-g1-build'
ICP_FW_DIR = G1_BUILD / 'icp-mdt-fw'
ICP_FW_TAR = G1_BUILD / 'camera-icp-fw.tar.gz'
CAM_SCRIPT = ROOT / 'devices/marble/runtime/camera/start-camera-g1-marble.sh'
LLVM_READELF = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-readelf'

ORACLE = ROOT / 'work/marble-m15-role-storage.1v2ynS/build_m15_role_storage.py'
ORACLE_SHA = '7d4f79857a3458ffdb7c8fb86b1cfd893fd9addb02b13e7e05d7b5fa7093cfaa'

BASE_DEFAULT = Path('/mnt/e/marble-tmp/vendor_boot_h20_f9_sim_cell_d01c4581.img')
BASE_SHA = 'd01c4581674ea5f4e87ebf773c25c613e5822532b1cac2c29848168c6256aa3f'

PREFIX = 'hmos-compat-marble/'
STAGE_MEMBER = PREFIX + 'graphics/stage-phase4-marble.sh'
POSTBOOT_MEMBER = PREFIX + 'graphics/start-u2-postboot-marble.sh'

# All 47 prior protected ramdisk members from F9 (byte-for-byte unchanged).
# stage-phase4-marble.sh and start-u2-postboot-marble.sh are spliced with uniquely reversible checks.
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
    PREFIX + 'graphics/start-modem-f6-marble.sh',
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
    PREFIX + 'graphics/libOMX_Core.z.so',
    PREFIX + 'graphics/libvulkan_freedreno.so.gz',
    PREFIX + 'graphics/zink-runtime-marble.tar.gz',
    PREFIX + 'graphics/libdisplay_composer_vdi_impl.z.so',
    PREFIX + 'graphics/start-phase4-marble.sh',
    PREFIX + 'graphics/marble_modem_probe',
    PREFIX + 'graphics/start-modem-f7-marble.sh',
    PREFIX + 'graphics/carrier-cn-marble.tar.gz',
    PREFIX + 'graphics/libril_vendor_qmi.z.so',
)

STAGED_FILES = (
    ('graphics/camera-icp-fw.tar.gz', ICP_FW_TAR, 0o644, False),
    ('graphics/marble_camera_probe', G1_BUILD / 'marble_camera_probe', 0o755, False),
    ('graphics/start-camera-g1-marble.sh', CAM_SCRIPT, 0o755, False),
)

STAGE_ANCHOR = b'$B test -x "$R/compat/bin/init"\necho MARBLE_PHASE4_RAM_COMPAT_READY'
STAGE_G1_BLOCK = (
    b'# === MARBLE_G1_CAMERA_HW_STAGE_BEGIN ===\n'
    b'if $B test -f /graphics/camera-icp-fw.tar.gz; then\n'
    b' $B mkdir -p /compat/firmware "$R/compat/firmware"\n'
    b' $B tar -xzf /graphics/camera-icp-fw.tar.gz -C /compat/firmware\n'
    b' $B tar -xzf /graphics/camera-icp-fw.tar.gz -C "$R/compat/firmware"\n'
    b' $B chmod 0644 /compat/firmware/CAMERA_ICP.* "$R/compat/firmware/CAMERA_ICP."* 2>/dev/null || true\n'
    b'fi\n'
    b'for g1bin in marble_camera_probe start-camera-g1-marble.sh; do\n'
    b' if $B test -f "/graphics/$g1bin"; then\n'
    b'  $B mkdir -p /compat/bin "$R/compat/bin"\n'
    b'  $B cp "/graphics/$g1bin" "/compat/bin/$g1bin"\n'
    b'  $B cp "/graphics/$g1bin" "$R/compat/bin/$g1bin"\n'
    b'  $B chmod 0755 "/compat/bin/$g1bin" "$R/compat/bin/$g1bin"\n'
    b' fi\n'
    b'done\n'
    b'echo "MARBLE_G1_CAMERA_STAGED=1 icp_fw=$($B ls /compat/firmware/CAMERA_ICP.* 2>/dev/null | $B wc -l)"\n'
    b'# === MARBLE_G1_CAMERA_HW_STAGE_END ===\n'
)

POSTBOOT_ANCHOR = b'say "U2_GNSS_MODE=hdf_location_host"'
POSTBOOT_G1_BLOCK = (
    b'# Step C: Batch G1 SM7475 Camera kernel stack + LX7 ICP firmware + 4-sensor CCI probe.\n'
    b'if skipped camera; then\n'
    b'  say "U2_CAMERA_SKIP=requested"\n'
    b'elif [ -x /compat/bin/start-camera-g1-marble.sh ]; then\n'
    b'  $B sh /compat/bin/start-camera-g1-marble.sh || say "U2_CAMERA_RC=$?"\n'
    b'else\n'
    b'  say "U2_CAMERA_SKIP=script_missing"\n'
    b'fi\n\n'
)


def require(cond: bool, msg: str) -> None:
    if not cond:
        raise ValueError(msg)


def build_icp_fw_tarball(out_path: Path = ICP_FW_TAR) -> dict:
    """Build deterministic tar.gz of CAMERA_ICP.mdt + CAMERA_ICP.b00..b21."""
    require(ICP_FW_DIR.is_dir(), f'Missing ICP firmware dir: {ICP_FW_DIR}')
    expected_names = ['CAMERA_ICP.mdt'] + [f'CAMERA_ICP.b{i:02d}' for i in range(22)]
    files = sorted([p for p in ICP_FW_DIR.iterdir() if p.is_file()], key=lambda p: p.name)
    got_names = [p.name for p in files]
    require(sorted(expected_names) == got_names, f'Unexpected ICP FW set: {got_names}')

    tar_buf = io.BytesIO()
    raw_bytes = 0
    with tarfile.open(fileobj=tar_buf, mode='w', format=tarfile.USTAR_FORMAT) as tf:
        for p in files:
            blob = p.read_bytes()
            ti = tarfile.TarInfo(name=p.name)
            ti.uid = 0
            ti.gid = 0
            ti.uname = 'root'
            ti.gname = 'root'
            ti.mtime = 0
            ti.mode = 0o644
            ti.type = tarfile.REGTYPE
            ti.size = len(blob)
            tf.addfile(ti, io.BytesIO(blob))
            raw_bytes += len(blob)

    gz_buf = io.BytesIO()
    with gzip.GzipFile(filename='', mode='wb', fileobj=gz_buf, mtime=0, compresslevel=9) as gz:
        gz.write(tar_buf.getvalue())
    gz_bytes = gz_buf.getvalue()
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(gz_bytes)
    return {
        'path': str(out_path),
        'file_count': len(files),
        'raw_bytes': raw_bytes,
        'gz_bytes': len(gz_bytes),
        'sha256': hashlib.sha256(gz_bytes).hexdigest(),
    }


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
    for marker in (b'MARBLE_G1_CAMERA_HW_STAGE_BEGIN', b'camera-icp-fw.tar.gz'):
        require(marker not in raw, f'stage script already contains {marker!r}')
    require(b'MARBLE_F8_OHOS_CELLULAR_STAGE_BEGIN' in raw, 'Expected F8 stage marker in base stage script')
    out = splice_once(raw, ((STAGE_ANCHOR, STAGE_G1_BLOCK + STAGE_ANCHOR),), STAGE_MEMBER)
    require(b'\r' not in out, 'CR byte in patched stage-phase4-marble.sh')
    subprocess.run(['sh', '-n'], input=out, check=True)
    return out


def patch_postboot(raw: bytes) -> bytes:
    for marker in (b'start-camera-g1-marble.sh', b'U2_CAMERA_SKIP'):
        require(marker not in raw, f'postboot script already contains {marker!r}')
    out = splice_once(raw, ((POSTBOOT_ANCHOR, POSTBOOT_G1_BLOCK + POSTBOOT_ANCHOR),), POSTBOOT_MEMBER)
    require(b'\r' not in out, 'CR byte in patched start-u2-postboot-marble.sh')
    subprocess.run(['sh', '-n'], input=out, check=True)
    return out


def validate_inputs() -> None:
    for rel, src, _mode, _replaces in STAGED_FILES:
        require(src.is_file(), f'Missing staged G1 artifact: {src}')
        raw = src.read_bytes()
        if rel.endswith('marble_camera_probe'):
            require(raw[:4] == b'\x7fELF', f'Expected ELF executable for {rel}')
            if LLVM_READELF.is_file():
                dyn = subprocess.check_output([str(LLVM_READELF), '-d', str(src)], text=True)
                needed = re.findall(r'\(NEEDED\)\s+Shared library:\s+\[([^\]]+)\]', dyn)
                require(needed == ['libc.so'], f'Unexpected NEEDED for {rel}: {needed}')
        elif rel.endswith('.sh'):
            require(b'\r' not in raw, f'CR byte in {rel}')
            subprocess.run(['sh', '-n'], input=raw, check=True)
        elif rel.endswith('.tar.gz'):
            with tarfile.open(fileobj=io.BytesIO(raw), mode='r:gz') as tf:
                names = set(tf.getnames())
                require('CAMERA_ICP.mdt' in names, f'Missing CAMERA_ICP.mdt in {rel}')
                require('CAMERA_ICP.b08' in names, f'Missing CAMERA_ICP.b08 in {rel}')
                require('CAMERA_ICP.b21' in names, f'Missing CAMERA_ICP.b21 in {rel}')
                require(len(names) == 23, f'Expected 23 ICP FW files in {rel}, got {len(names)}')


def load_oracle():
    require(ORACLE.is_file() and ORACLE.resolve() == ORACLE, f'Missing oracle: {ORACLE}')
    got_sha = hashlib.sha256(ORACLE.read_bytes()).hexdigest()
    require(got_sha == ORACLE_SHA, f'Oracle SHA256 mismatch: {got_sha}')
    spec = importlib.util.spec_from_file_location('archive_oracle_g1', ORACLE)
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
    G1_BUILD.mkdir(parents=True, exist_ok=True)
    icp_meta = build_icp_fw_tarball(ICP_FW_TAR)
    validate_inputs()
    out_dir.mkdir(parents=True, exist_ok=True)

    archive, r = load_oracle()
    base_bytes = r.read_original(base_path)
    base_sha = r.sha(base_bytes)
    require(base_sha == BASE_SHA, f'Base SHA256 mismatch: got {base_sha}, want {BASE_SHA}')
    base_id = r.identity(base_path)

    before_layout = r.layout(base_bytes)
    before_avb = r.verify_existing_avb(base_bytes, before_layout['payload_end'])

    with tempfile.TemporaryDirectory(prefix='h20_g1_', dir=str(G1_BUILD)) as tmp:
        scratch = Path(tmp)
        old, old_raw, kind, old_rd, args = r.extract_entries(base_path, base_bytes, before_layout, scratch / 'before')
        for gm in GUARD_MEMBERS:
            require(gm in old, f'Guard member missing in F9 base image: {gm}')

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

        new_postboot = patch_postboot(old[POSTBOOT_MEMBER][1])
        r.set_entry(expected, POSTBOOT_MEMBER, new_postboot)
        modified_members.add(POSTBOOT_MEMBER)
        postboot_rel = POSTBOOT_MEMBER[len(PREFIX):]
        if postboot_rel in new_manifest:
            new_manifest[postboot_rel].update(bytes=len(new_postboot), sha256=r.sha(new_postboot))

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
            after_layout['payload_end'] == len(payload) <= before_layout['payload_end'] + 2 * 1024 * 1024,
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
    out_img = out_dir / f'vendor_boot_h20_g1_camera_hw_{cand_sha[:8]}.img'
    out_img.write_bytes(image)
    report = {
        'base': base_id,
        'candidate': r.identity(out_img),
        'entries_before': len(old),
        'entries_after': len(expected),
        'guard_members_verified': list(GUARD_MEMBERS),
        'icp_fw_bundle': icp_meta,
        'staged': staged_info,
    }
    report_path = G1_BUILD / 'g1-build-report.json'
    report_path.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f'CANDIDATE_IMAGE={out_img}')
    print(f'CANDIDATE_SHA256={cand_sha}')
    print(f'CANDIDATE_SHA8={cand_sha[:8]}')
    print(f'ICP_FW_BYTES={icp_meta["gz_bytes"]} FILES={icp_meta["file_count"]}')
    print(f'BUILD_REPORT={report_path}')
    return out_img


def main() -> None:
    parser = argparse.ArgumentParser(description='Package Batch G1 SM7475 Camera HW vendor_boot image.')
    parser.add_argument('--base', type=Path, default=BASE_DEFAULT)
    parser.add_argument('--out', type=Path, default=Path('/mnt/e/marble-tmp'))
    args = parser.parse_args()
    build(args.base, args.out)


if __name__ == '__main__':
    main()
