#!/usr/bin/env python3
"""Package Batch G3 (SM7475 Real Silicon Pixel Capture: TPG0 + Physical Sony IMX355 wired into Camera VDI) onto G2 vendor_boot_a."""
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
G3_BUILD = ROOT / 'work/marble-g3-build'
LLVM_READELF = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-readelf'
LLVM_NM = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-nm'

ORACLE = ROOT / 'work/marble-m15-role-storage.1v2ynS/build_m15_role_storage.py'
ORACLE_SHA = '7d4f79857a3458ffdb7c8fb86b1cfd893fd9addb02b13e7e05d7b5fa7093cfaa'

BASE_DEFAULT = Path('/mnt/e/marble-tmp/vendor_boot_h20_g2_camera_hdi_a9b087d7.img')
BASE_SHA = 'a9b087d72c389efaeb719a73ee6986dbe05b20f8b581efbea081ebc4a68c3c53'

PREFIX = 'hmos-compat-marble/'
STAGE_MEMBER = PREFIX + 'graphics/stage-phase4-marble.sh'

# All 54 prior immutable ramdisk members from G2 (byte-for-byte unchanged).
# Intentionally updated in G3:
#   - graphics/libmarble_camera_vdi_impl.z.so (wired to live SM7475 IMX355 optical + TPG0 RDI0 + 2D DCT JPEG)
#   - graphics/native_camera_hdi_probe (dual-camera lcam001 TPG0 + lcam003 IMX355 optical Binder/DMA-BUF probe)
#   - graphics/marble_camera_hw_capture (new standalone SM7475 silicon capture CLI)
#   - graphics/stage-phase4-marble.sh (stage marble_camera_hw_capture into /compat/bin & $R/compat/bin)
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
    PREFIX + 'graphics/libhmos_boot_vulkan.so',
    PREFIX + 'graphics/vpu20_4v-fw.tar.gz',
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
    PREFIX + 'graphics/camera-icp-fw.tar.gz',
    PREFIX + 'graphics/start-camera-g1-marble.sh',
    # G2 immutable members
    PREFIX + 'graphics/hdf_default_phase4.hcb',
    PREFIX + 'hmos-init-debug.cfg',
    PREFIX + 'graphics/start-u2-postboot-marble.sh',
    PREFIX + 'graphics/camera_host_config.hcb',
    PREFIX + 'graphics/init-g2-camera-marble.cfg',
    PREFIX + 'graphics/start-camera-g2-marble.sh',
    PREFIX + 'graphics/marble_camera_probe',
)

STAGED_FILES = (
    ('graphics/libmarble_camera_vdi_impl.z.so', G3_BUILD / 'libmarble_camera_vdi_impl.z.so', 0o644, True),
    ('graphics/native_camera_hdi_probe', G3_BUILD / 'native_camera_hdi_probe', 0o755, True),
    ('graphics/marble_camera_hw_capture', G3_BUILD / 'marble_camera_hw_capture', 0o755, False),
)

STAGE_G3_OLD = b'for g2bin in native_camera_hdi_probe start-camera-g2-marble.sh; do'
STAGE_G3_NEW = b'for g2bin in native_camera_hdi_probe start-camera-g2-marble.sh marble_camera_hw_capture; do'


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
    require(b'marble_camera_hw_capture' not in raw, 'stage script already contains marble_camera_hw_capture')
    require(b'MARBLE_G2_CAMERA_HDI_STAGE_BEGIN' in raw, 'Expected G2 stage marker in base stage script')
    out = splice_once(raw, ((STAGE_G3_OLD, STAGE_G3_NEW),), STAGE_MEMBER)
    require(b'\r' not in out, 'CR byte in patched stage-phase4-marble.sh')
    subprocess.run(['sh', '-n'], input=out, check=True)
    return out


def validate_inputs() -> None:
    for rel, src, _mode, _replaces in STAGED_FILES:
        require(src.is_file(), f'Missing staged G3 artifact: {src}')
        raw = src.read_bytes()
        require(raw[:4] == b'\x7fELF', f'Expected ELF binary for {rel}')
        if rel.endswith('marble_camera_hw_capture'):
            if LLVM_READELF.is_file():
                dyn = subprocess.check_output([str(LLVM_READELF), '-d', str(src)], text=True)
                needed = re.findall(r'\(NEEDED\)\s+Shared library:\s+\[([^\]]+)\]', dyn)
                require(needed == ['libc.so'], f'Unexpected NEEDED for {rel}: {needed}')
        elif rel.endswith('libmarble_camera_vdi_impl.z.so'):
            if LLVM_READELF.is_file():
                dyn = subprocess.check_output([str(LLVM_READELF), '-d', str(src)], text=True)
                soname = re.findall(r'\(SONAME\)\s+Library soname:\s+\[([^\]]+)\]', dyn)
                require(soname == ['libmarble_camera_vdi_impl.z.so'], f'Unexpected SONAME: {soname}')
                syms = subprocess.check_output([str(LLVM_READELF), '-sW', str(src)], text=True)
                sizes = {}
                for line in syms.splitlines():
                    m = re.match(r'\s*\d+:\s+[0-9a-f]+\s+(\d+)\s+\S+\s+\S+\s+\S+\s+\S+\s+(_ZTV\S+)', line)
                    if m:
                        sizes[m.group(2)] = int(m.group(1))
                host_sz = [sz for n, sz in sizes.items() if 'MarbleCameraHostVdi' in n]
                dev_sz = [sz for n, sz in sizes.items() if 'MarbleCameraDeviceVdi' in n]
                op_sz = [sz for n, sz in sizes.items() if 'MarbleStreamOperatorVdi' in n]
                require(host_sz == [224] and dev_sz == [232] and op_sz == [256],
                        f'VDI vtable ABI mismatch: host={host_sz} dev={dev_sz} op={op_sz}')
            if LLVM_NM.is_file():
                nm = subprocess.check_output([str(LLVM_NM), '-D', str(src)], text=True)
                require(' hdfVdiDesc' in nm, 'libmarble_camera_vdi_impl.z.so missing hdfVdiDesc export')
                require(' marble_hw_session_open' in nm, 'libmarble_camera_vdi_impl.z.so missing marble_hw_session_open')
                require(' marble_encode_baseline_jpeg_rgb24' in nm,
                        'libmarble_camera_vdi_impl.z.so missing marble_encode_baseline_jpeg_rgb24')


def load_oracle():
    require(ORACLE.is_file() and ORACLE.resolve() == ORACLE, f'Missing oracle: {ORACLE}')
    got_sha = hashlib.sha256(ORACLE.read_bytes()).hexdigest()
    require(got_sha == ORACLE_SHA, f'Oracle SHA256 mismatch: {got_sha}')
    spec = importlib.util.spec_from_file_location('archive_oracle_g3', ORACLE)
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
    G3_BUILD.mkdir(parents=True, exist_ok=True)
    validate_inputs()
    out_dir.mkdir(parents=True, exist_ok=True)

    archive, r = load_oracle()
    base_bytes = r.read_original(base_path)
    base_sha = r.sha(base_bytes)
    require(base_sha == BASE_SHA, f'Base SHA256 mismatch: got {base_sha}, want {BASE_SHA}')

    before_layout = r.layout(base_bytes)
    before_avb = r.verify_existing_avb(base_bytes, before_layout['payload_end'])

    with tempfile.TemporaryDirectory(prefix='h20_g3_', dir=str(G3_BUILD)) as tmp:
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
        modified_members = {r.MANIFEST}
        staged_report = {}

        # Splice stage-phase4-marble.sh to stage marble_camera_hw_capture
        new_stage = patch_stage(old[STAGE_MEMBER][1])
        r.set_entry(expected, STAGE_MEMBER, new_stage)
        modified_members.add(STAGE_MEMBER)
        stage_rel = STAGE_MEMBER[len(PREFIX):]
        if stage_rel in new_manifest:
            new_manifest[stage_rel].update(bytes=len(new_stage), sha256=r.sha(new_stage))

        # Replace VDI + HDI probe, add marble_camera_hw_capture
        next_inode = [max(header[0] for header, _ in old.values()) + 1]
        for rel, src, mode, replaces in STAGED_FILES:
            member = PREFIX + rel
            blob = src.read_bytes()
            if replaces:
                require(member in expected, f'Expected existing member to replace: {member}')
                r.set_entry(expected, member, blob)
                modified_members.add(member)
            else:
                require(member not in expected, f'Unexpected pre-existing member: {member}')
                r.image_tools.put_file(expected, member, blob, mode, next_inode)
                expected[member][0][11] = len(member.encode('utf-8')) + 1
            update_manifest_entry(new_manifest, member, blob, mode, r.sha)
            staged_report[rel] = {'bytes': len(blob), 'mode': oct(mode), 'sha256': r.sha(blob), 'replaces': replaces}

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
        out_path = out_dir / f'vendor_boot_h20_g3_camera_capture_{cand_sha[:8]}.img'
        out_path.write_bytes(cand_bytes)

        report = {
            'batch': 'G3',
            'description': 'SM7475 real silicon camera capture (TPG0 + physical Sony IMX355 optical via CSIPHY0/CSID/IFE RDI0) + 2D DCT JPEG wired into OHOS Camera VDI',
            'base': {'path': str(base_path), 'bytes': len(base_bytes), 'sha256': base_sha},
            'candidate': {'path': str(out_path), 'bytes': len(cand_bytes), 'sha256': cand_sha},
            'entries_before': len(old),
            'entries_after': len(got),
            'guard_members_verified': list(GUARD_MEMBERS),
            'staged': staged_report,
            'avb_before': {k: (v.hex() if isinstance(v, bytes) else v) for k, v in before_avb.items()},
            'avb_after': {k: (v.hex() if isinstance(v, bytes) else v) for k, v in after_avb.items()},
        }
        (G3_BUILD / 'g3-build-report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
        print(f'G3_BUILD_OK path={out_path} sha256={cand_sha} entries={len(old)}->{len(got)}')
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
