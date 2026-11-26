#!/usr/bin/env python3
"""Package Batch H6 (SM7475 Hardware Video Encoder DYNAMIC_HANDLE EOS Fix + native_venc_probe) onto H5 vendor_boot_a."""
from __future__ import annotations

import argparse
import copy
import hashlib
import importlib.util
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[2]
H6_BUILD = ROOT / 'work/marble-h6-build'
LLVM_READELF = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-readelf'
LLVM_NM = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-nm'

ORACLE = ROOT / 'work/marble-m15-role-storage.1v2ynS/build_m15_role_storage.py'
ORACLE_SHA = '7d4f79857a3458ffdb7c8fb86b1cfd893fd9addb02b13e7e05d7b5fa7093cfaa'

BASE_DEFAULT = Path('/mnt/e/marble-tmp/vendor_boot_h20_h5_memorytracker_9a05fcb7.img')
BASE_SHA = '9a05fcb7a600359938f1eff98d0ee351da324048bbab2d775b17b91f2cad0b93'

PREFIX = 'hmos-compat-marble/'
STAGE_MEMBER = PREFIX + 'graphics/stage-phase4-marble.sh'
OMX_MEMBER = PREFIX + 'graphics/libOMX_Core.z.so'

# All 75 prior immutable ramdisk members from H5 (including HCB, init-debug, postboot, H1-H5, G1-G5, F1-F9, U1-U4).
GUARD_MEMBERS = (
    PREFIX + 'graphics/hdf_default_phase4.hcb',
    PREFIX + 'hmos-init-debug.cfg',
    PREFIX + 'graphics/start-u2-postboot-marble.sh',
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
    PREFIX + 'graphics/libhmos_boot_vulkan.so',
    PREFIX + 'graphics/vpu20_4v-fw.tar.gz',
    PREFIX + 'graphics/libdisplay_buffer_vdi_impl.z.so',
    PREFIX + 'graphics/libheifdecode_vdi_impl.z.so',
    PREFIX + 'graphics/native_heif_probe',
    PREFIX + 'graphics/hmos_dcm_service',
    PREFIX + 'graphics/hmos_dcm_token_probe',
    PREFIX + 'graphics/init-dcm-marble.cfg',
    PREFIX + 'graphics/libvulkan_freedreno.so.gz',
    PREFIX + 'graphics/zink-runtime-marble.tar.gz',
    PREFIX + 'graphics/libdisplay_composer_vdi_impl.z.so',
    PREFIX + 'graphics/start-phase4-marble.sh',
    PREFIX + 'graphics/marble_modem_probe',
    PREFIX + 'graphics/start-modem-f7-marble.sh',
    PREFIX + 'graphics/carrier-cn-marble.tar.gz',
    PREFIX + 'graphics/libril_vendor_qmi.z.so',
    PREFIX + 'graphics/camera-icp-fw.tar.gz',
    PREFIX + 'graphics/start-camera-g1-marble.sh',
    PREFIX + 'graphics/camera_host_config.hcb',
    PREFIX + 'graphics/init-g2-camera-marble.cfg',
    PREFIX + 'graphics/start-camera-g2-marble.sh',
    PREFIX + 'graphics/marble_camera_probe',
    PREFIX + 'graphics/libmarble_camera_vdi_impl.z.so',
    PREFIX + 'graphics/native_camera_hdi_probe',
    PREFIX + 'graphics/marble_camera_hw_capture',
    PREFIX + 'graphics/ohcamera_fwk_probe',
    PREFIX + 'graphics/libmarble_nfc_interface_service_1.1.z.so',
    PREFIX + 'graphics/marble_nfc_hw_probe',
    PREFIX + 'graphics/native_nfc_hdi_probe',
    PREFIX + 'graphics/init-h1-nfc-marble.cfg',
    PREFIX + 'graphics/start-nfc-h1-marble.sh',
    PREFIX + 'graphics/libmarble_thermal_interface_service_1.1.z.so',
    PREFIX + 'graphics/native_thermal_hdi_probe',
    PREFIX + 'graphics/init-h2-thermal-marble.cfg',
    PREFIX + 'graphics/start-thermal-h2-marble.sh',
    PREFIX + 'graphics/libmarble_power_interface_service_1.3.z.so',
    PREFIX + 'graphics/native_power_hdi_probe',
    PREFIX + 'graphics/init-h3-power-marble.cfg',
    PREFIX + 'graphics/start-power-h3-marble.sh',
    PREFIX + 'graphics/libmarble_usb_port_interface_service_2.0.z.so',
    PREFIX + 'graphics/native_usb_port_hdi_probe',
    PREFIX + 'graphics/init-h4-usb-port-marble.cfg',
    PREFIX + 'graphics/start-usb-port-h4-marble.sh',
    PREFIX + 'graphics/libmarble_memory_tracker_interface_service_1.0.z.so',
    PREFIX + 'graphics/native_memorytracker_hdi_probe',
    PREFIX + 'graphics/init-h5-memorytracker-marble.cfg',
    PREFIX + 'graphics/start-memorytracker-h5-marble.sh',
)

REPLACED_FILES = (
    ('graphics/libOMX_Core.z.so', H6_BUILD / 'libOMX_Core.z.so', 0o644),
)

ADDED_FILES = (
    ('graphics/native_venc_probe', H6_BUILD / 'native_venc_probe', 0o755),
)

STAGE_ANCHOR = b'$B test -x "$R/compat/bin/init"\necho MARBLE_PHASE4_RAM_COMPAT_READY'
STAGE_H6_BLOCK = (
    b'# === MARBLE_H6_VENC_PROBE_STAGE_BEGIN ===\n'
    b'if $B test -f /graphics/native_venc_probe; then\n'
    b' $B cp /graphics/native_venc_probe /compat/bin/native_venc_probe\n'
    b' $B cp /graphics/native_venc_probe "$R/compat/bin/native_venc_probe"\n'
    b' $B chmod 0755 /compat/bin/native_venc_probe "$R/compat/bin/native_venc_probe"\n'
    b' echo "MARBLE_H6_VENC_PROBE_STAGED=1"\n'
    b'fi\n'
    b'# === MARBLE_H6_VENC_PROBE_STAGE_END ===\n'
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
    for marker in (b'MARBLE_H6_VENC_PROBE_STAGE_BEGIN', b'native_venc_probe'):
        require(marker not in raw, f'stage script already contains {marker!r}')
    require(b'MARBLE_H5_MEMORYTRACKER_HDI_STAGE_BEGIN' in raw, 'Expected H5 stage marker in base stage script')
    out = splice_once(raw, ((STAGE_ANCHOR, STAGE_H6_BLOCK + STAGE_ANCHOR),), STAGE_MEMBER)
    require(b'\r' not in out, 'CR byte in patched stage-phase4-marble.sh')
    subprocess.run(['sh', '-n'], input=out, check=True)
    return out


def validate_inputs() -> None:
    for rel, src, _mode in (*REPLACED_FILES, *ADDED_FILES):
        require(src.is_file(), f'Missing H6 artifact: {src}')
        raw = src.read_bytes()
        require(raw[:4] == b'\x7fELF', f'Expected ELF for {rel}')
        if rel.endswith('libOMX_Core.z.so') and LLVM_READELF.is_file():
            dyn = subprocess.check_output([str(LLVM_READELF), '-d', str(src)], text=True)
            soname = re.findall(r'\(SONAME\)\s+Library soname:\s+\[([^\]]+)\]', dyn)
            needed = re.findall(r'\(NEEDED\)\s+Shared library:\s+\[([^\]]+)\]', dyn)
            require(soname == ['libOMX_Core.z.so'], f'Unexpected SONAME: {soname}')
            require(needed == ['libc.so'], f'Unexpected NEEDED for libOMX_Core.z.so: {needed}')
            if LLVM_NM.is_file():
                nm = subprocess.check_output([str(LLVM_NM), '-D', '--defined-only', str(src)], text=True)
                for sym in ('OMX_Init', 'OMX_Deinit', 'OMX_GetHandle', 'OMX_FreeHandle'):
                    require(f' {sym}' in nm, f'Missing OMX export: {sym}')
        elif rel.endswith('native_venc_probe') and LLVM_READELF.is_file():
            dyn = subprocess.check_output([str(LLVM_READELF), '-d', str(src)], text=True)
            needed = re.findall(r'\(NEEDED\)\s+Shared library:\s+\[([^\]]+)\]', dyn)
            for req in ('libnative_media_venc.so', 'libnative_media_vdec.so', 'libnative_media_codecbase.so'):
                require(req in needed, f'Missing NEEDED {req} in {rel}: {needed}')


def load_oracle():
    require(ORACLE.is_file() and ORACLE.resolve() == ORACLE, f'Missing oracle: {ORACLE}')
    got_sha = hashlib.sha256(ORACLE.read_bytes()).hexdigest()
    require(got_sha == ORACLE_SHA, f'Oracle SHA256 mismatch: {got_sha}')
    spec = importlib.util.spec_from_file_location('archive_oracle_h6', ORACLE)
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
    H6_BUILD.mkdir(parents=True, exist_ok=True)
    validate_inputs()
    out_dir.mkdir(parents=True, exist_ok=True)

    archive, r = load_oracle()
    base_bytes = r.read_original(base_path)
    base_sha = r.sha(base_bytes)
    require(base_sha == BASE_SHA, f'Base SHA256 mismatch: got {base_sha}, want {BASE_SHA}')

    before_layout = r.layout(base_bytes)
    before_avb = r.verify_existing_avb(base_bytes, before_layout['payload_end'])

    with tempfile.TemporaryDirectory(prefix='h20_h6_', dir=str(H6_BUILD)) as tmp:
        scratch = Path(tmp)
        old, old_raw, kind, old_rd, args = r.extract_entries(
            base_path, base_bytes, before_layout, scratch / 'before'
        )
        old_manifest = archive.parse(old[r.MANIFEST][1])
        r.manifest_holds(old, old_manifest)

        for member in GUARD_MEMBERS:
            require(member in old, f'Missing guarded member in base image: {member}')

        expected = dict(old)
        new_manifest = copy.deepcopy(old_manifest)
        modified_members = {r.MANIFEST, STAGE_MEMBER, OMX_MEMBER}
        staged_report = {}

        new_stage = patch_stage(old[STAGE_MEMBER][1])
        r.set_entry(expected, STAGE_MEMBER, new_stage)
        stage_rel = STAGE_MEMBER[len(PREFIX):]
        if stage_rel in new_manifest:
            new_manifest[stage_rel].update(bytes=len(new_stage), sha256=r.sha(new_stage))

        for rel, src, mode in REPLACED_FILES:
            member = PREFIX + rel
            blob = src.read_bytes()
            require(member in expected, f'Expected existing member to replace: {member}')
            r.set_entry(expected, member, blob)
            update_manifest_entry(new_manifest, member, blob, mode, r.sha)
            staged_report[rel] = {'bytes': len(blob), 'mode': oct(mode), 'sha256': r.sha(blob), 'replaced': True}

        next_inode = [max(header[0] for header, _ in old.values()) + 1]
        for rel, src, mode in ADDED_FILES:
            member = PREFIX + rel
            blob = src.read_bytes()
            require(member not in expected, f'Unexpected pre-existing member: {member}')
            r.image_tools.put_file(expected, member, blob, mode, next_inode)
            expected[member][0][11] = len(member.encode('utf-8')) + 1
            update_manifest_entry(new_manifest, member, blob, mode, r.sha)
            staged_report[rel] = {'bytes': len(blob), 'mode': oct(mode), 'sha256': r.sha(blob), 'replaced': False}

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
        cand_bytes, updated = r.image_tools.preserve_and_update_avb_footer(payload, base_bytes)
        require(updated, 'Existing AVB footer metadata not updated')
        after_avb = r.verify_existing_avb(cand_bytes, len(payload))
        require(
            all(after_avb[k] == before_avb[k] for k in ('vbmeta_masked', 'footer_masked')),
            'Unexpected AVB metadata change',
        )

        verify = scratch / 'roundtrip.img'
        verify.write_bytes(cand_bytes)
        got, got_raw, got_kind, _, _ = r.extract_entries(verify, cand_bytes, after_layout, scratch / 'after')
        require(got == expected and got_raw == raw and got_kind == kind, 'Exact unpack round-trip failed')

        for member in GUARD_MEMBERS:
            require(got[member][1] == old[member][1], f'Guarded member drifted: {member}')

        old_spans, old_trailer = archive.record_spans(old_raw)
        got_spans, got_trailer = archive.record_spans(got_raw)
        require(got_trailer == old_trailer, 'CPIO trailer changed')
        unchanged = set(old) - modified_members
        require(
            all(got_raw[slice(*got_spans[n])] == old_raw[slice(*old_spans[n])] for n in unchanged),
            'Unrelated CPIO record changed',
        )

        cand_sha = r.sha(cand_bytes)
        out_path = out_dir / f'vendor_boot_h20_h6_venc_eos_{cand_sha[:8]}.img'
        out_path.write_bytes(cand_bytes)

        report = {
            'batch': 'H6',
            'description': 'SM7475 Hardware Video Encoder DYNAMIC_HANDLE EOS Fix (exact N-in/N-out AVC+HEVC) + public AVCodecKit OH_VideoEncoder loopback probe',
            'base': {'path': str(base_path), 'bytes': len(base_bytes), 'sha256': base_sha},
            'candidate': {'path': str(out_path), 'bytes': len(cand_bytes), 'sha256': cand_sha},
            'entries_before': len(old),
            'entries_after': len(got),
            'guard_members_verified': list(GUARD_MEMBERS),
            'staged': staged_report,
            'avb_before': {k: (v.hex() if isinstance(v, bytes) else v) for k, v in before_avb.items()},
            'avb_after': {k: (v.hex() if isinstance(v, bytes) else v) for k, v in after_avb.items()},
        }
        (H6_BUILD / 'h6-build-report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
        print(f'H6_BUILD_OK path={out_path} sha256={cand_sha} entries={len(old)}->{len(got)}')
        return out_path


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--base', type=Path, default=BASE_DEFAULT)
    ap.add_argument('--out-dir', type=Path, default=Path('/mnt/e/marble-tmp'))
    ns = ap.parse_args()
    build(ns.base, ns.out_dir)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
