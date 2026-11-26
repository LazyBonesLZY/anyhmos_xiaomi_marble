#!/usr/bin/env python3
"""Package Batch G2 (OHOS Camera HDI camera_host + marble VDI + SA 3008 camera_service) onto G1 vendor_boot_a."""
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
TOOLS = ROOT / 'tools/packaging'
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import build_marble_phase4_images as p4

G2_BUILD = ROOT / 'work/marble-g2-build'
DONOR_FS = ROOT / 'work/donor-rom/ADY-AL10-207.0.0.111-SP10/fs'
CAM_G2_CFG = ROOT / 'devices/marble/runtime/camera/init-g2-camera-marble.cfg'
CAM_G2_SCRIPT = ROOT / 'devices/marble/runtime/camera/start-camera-g2-marble.sh'
LLVM_READELF = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-readelf'
LLVM_NM = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-nm'

ORACLE = ROOT / 'work/marble-m15-role-storage.1v2ynS/build_m15_role_storage.py'
ORACLE_SHA = '7d4f79857a3458ffdb7c8fb86b1cfd893fd9addb02b13e7e05d7b5fa7093cfaa'

BASE_DEFAULT = Path('/mnt/e/marble-tmp/vendor_boot_h20_g1_camera_hw_0e768215.img')
BASE_SHA = '0e7682159759264f2f904bda12acc4290fdbfe98eff7a98ff0a43d0ff68af30a'

PREFIX = 'hmos-compat-marble/'
HCB_MEMBER = PREFIX + 'graphics/hdf_default_phase4.hcb'
INIT_DEBUG_MEMBER = PREFIX + 'hmos-init-debug.cfg'
STAGE_MEMBER = PREFIX + 'graphics/stage-phase4-marble.sh'
POSTBOOT_MEMBER = PREFIX + 'graphics/start-u2-postboot-marble.sh'

# All 47 prior immutable ramdisk members from G1 (byte-for-byte unchanged).
# Intentionally updated in G2: hdf_default_phase4.hcb (host-9 camera_host), hmos-init-debug.cfg
# (import init-g2-camera-marble.cfg), stage-phase4-marble.sh (unmask SA 3008 + stage/bind VDI & HCB),
# start-u2-postboot-marble.sh (invoke start-camera-g2-marble.sh), marble_camera_probe (keep video0 open for ICP).
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
)

STAGED_FILES = (
    ('graphics/marble_camera_probe', G2_BUILD / 'marble_camera_probe', 0o755, True),
    ('graphics/libmarble_camera_vdi_impl.z.so', G2_BUILD / 'libmarble_camera_vdi_impl.z.so', 0o644, False),
    ('graphics/init-g2-camera-marble.cfg', CAM_G2_CFG, 0o644, False),
    ('graphics/native_camera_hdi_probe', G2_BUILD / 'native_camera_hdi_probe', 0o755, False),
    ('graphics/start-camera-g2-marble.sh', CAM_G2_SCRIPT, 0o755, False),
)

CAM_HOST_HCB_REL = 'graphics/camera_host_config.hcb'

OPT_SA_G2_OLD = b'1907 3006 3008 4300'
OPT_SA_G2_NEW = b'1907 3006 4300'

INIT_IMP_OLD = b'    "/compat/etc/init-dcm-marble.cfg"\n  ],'
INIT_IMP_NEW = (
    b'    "/compat/etc/init-dcm-marble.cfg",\n'
    b'    "/compat/etc/init-g2-camera-marble.cfg"\n  ],'
)

STAGE_ANCHOR = b'$B test -x "$R/compat/bin/init"\necho MARBLE_PHASE4_RAM_COMPAT_READY'
STAGE_G2_BLOCK = (
    b'# === MARBLE_G2_CAMERA_HDI_STAGE_BEGIN ===\n'
    b'g2_bind() {\n'
    b' $B grep -q " $2 " /proc/mounts 2>/dev/null && return 0\n'
    b' $B mount --bind "$1" "$2" 2>/dev/null || echo "MARBLE_G2_BIND_FAIL dst=$2"\n'
    b'}\n'
    b'$B mkdir -p /compat/bin "$R/compat/bin" "$R/compat/lib64" "$R/compat/etc"\n'
    b'if $B test -f /graphics/libmarble_camera_vdi_impl.z.so; then\n'
    b' $B cp /graphics/libmarble_camera_vdi_impl.z.so "$R/compat/lib64/libmarble_camera_vdi_impl.z.so"\n'
    b' $B chmod 0644 "$R/compat/lib64/libmarble_camera_vdi_impl.z.so"\n'
    b' if $B test -f "$R/vendor/lib64/libcamera_external_usb_camera.so"; then\n'
    b'  g2_bind "$R/compat/lib64/libmarble_camera_vdi_impl.z.so" "$R/vendor/lib64/libcamera_external_usb_camera.so"\n'
    b' fi\n'
    b'fi\n'
    b'if $B test -f /graphics/camera_host_config.hcb; then\n'
    b' $B cp /graphics/camera_host_config.hcb "$R/compat/etc/camera_host_config.hcb"\n'
    b' $B chmod 0644 "$R/compat/etc/camera_host_config.hcb"\n'
    b' if $B test -f "$R/vendor/etc/hdfconfig/camera_host_config.hcb"; then\n'
    b'  g2_bind "$R/compat/etc/camera_host_config.hcb" "$R/vendor/etc/hdfconfig/camera_host_config.hcb"\n'
    b' fi\n'
    b'fi\n'
    b'if $B test -f /graphics/init-g2-camera-marble.cfg; then\n'
    b' $B cp /graphics/init-g2-camera-marble.cfg "$R/compat/etc/init-g2-camera-marble.cfg"\n'
    b' $B chmod 0644 "$R/compat/etc/init-g2-camera-marble.cfg"\n'
    b'fi\n'
    b'for g2bin in native_camera_hdi_probe start-camera-g2-marble.sh; do\n'
    b' if $B test -f "/graphics/$g2bin"; then\n'
    b'  $B cp "/graphics/$g2bin" "/compat/bin/$g2bin"\n'
    b'  $B cp "/graphics/$g2bin" "$R/compat/bin/$g2bin"\n'
    b'  $B chmod 0755 "/compat/bin/$g2bin" "$R/compat/bin/$g2bin"\n'
    b' fi\n'
    b'done\n'
    b'echo "MARBLE_G2_CAMERA_HDI_STAGED=1"\n'
    b'# === MARBLE_G2_CAMERA_HDI_STAGE_END ===\n'
)

POSTBOOT_ANCHOR = b'say "U2_GNSS_MODE=hdf_location_host"'
POSTBOOT_G2_BLOCK = (
    b'# Step D: Batch G2 OHOS Camera HDI (camera_host host-9) + SA 3008 (camera_service).\n'
    b'if skipped camera || skipped camera_hdi; then\n'
    b'  say "U2_CAMERA_HDI_SKIP=requested"\n'
    b'elif [ -x /compat/bin/start-camera-g2-marble.sh ]; then\n'
    b'  $B sh /compat/bin/start-camera-g2-marble.sh || say "U2_CAMERA_HDI_RC=$?"\n'
    b'else\n'
    b'  say "U2_CAMERA_HDI_SKIP=script_missing"\n'
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


def patch_init_debug(raw: bytes) -> bytes:
    require(b'init-g2-camera-marble.cfg' not in raw, 'hmos-init-debug.cfg already contains G2 import')
    out = splice_once(raw, ((INIT_IMP_OLD, INIT_IMP_NEW),), INIT_DEBUG_MEMBER)
    doc = json.loads(out.decode('utf-8'))
    require('/compat/etc/init-g2-camera-marble.cfg' in doc.get('import', []),
            'G2 camera init import missing after JSON parse')
    require(len(out) < 32768, f'hmos-init-debug.cfg exceeds 32768-byte init cap: {len(out)}')
    return out


def patch_stage(raw: bytes) -> bytes:
    for marker in (b'MARBLE_G2_CAMERA_HDI_STAGE_BEGIN', b'libmarble_camera_vdi_impl.z.so'):
        require(marker not in raw, f'stage script already contains {marker!r}')
    require(b'MARBLE_G1_CAMERA_HW_STAGE_BEGIN' in raw, 'Expected G1 stage marker in base stage script')
    out = splice_once(
        raw,
        (
            (OPT_SA_G2_OLD, OPT_SA_G2_NEW),
            (STAGE_ANCHOR, STAGE_G2_BLOCK + STAGE_ANCHOR),
        ),
        STAGE_MEMBER,
    )
    require(b'\r' not in out, 'CR byte in patched stage-phase4-marble.sh')
    subprocess.run(['sh', '-n'], input=out, check=True)
    return out


def patch_postboot(raw: bytes) -> bytes:
    for marker in (b'start-camera-g2-marble.sh', b'U2_CAMERA_HDI_SKIP'):
        require(marker not in raw, f'postboot script already contains {marker!r}')
    require(b'start-camera-g1-marble.sh' in raw, 'Expected G1 postboot call in base postboot script')
    out = splice_once(raw, ((POSTBOOT_ANCHOR, POSTBOOT_G2_BLOCK + POSTBOOT_ANCHOR),), POSTBOOT_MEMBER)
    require(b'\r' not in out, 'CR byte in patched start-u2-postboot-marble.sh')
    subprocess.run(['sh', '-n'], input=out, check=True)
    return out


def validate_inputs() -> None:
    for rel, src, _mode, _replaces in STAGED_FILES:
        require(src.is_file(), f'Missing staged G2 artifact: {src}')
        raw = src.read_bytes()
        if rel.endswith('marble_camera_probe'):
            require(raw[:4] == b'\x7fELF', f'Expected ELF executable for {rel}')
            if LLVM_READELF.is_file():
                dyn = subprocess.check_output([str(LLVM_READELF), '-d', str(src)], text=True)
                needed = re.findall(r'\(NEEDED\)\s+Shared library:\s+\[([^\]]+)\]', dyn)
                require(needed == ['libc.so'], f'Unexpected NEEDED for {rel}: {needed}')
        elif rel.endswith('libmarble_camera_vdi_impl.z.so'):
            require(raw[:4] == b'\x7fELF', f'Expected ELF shared object for {rel}')
            if LLVM_READELF.is_file():
                dyn = subprocess.check_output([str(LLVM_READELF), '-d', str(src)], text=True)
                soname = re.findall(r'\(SONAME\)\s+Library soname:\s+\[([^\]]+)\]', dyn)
                require(soname == ['libmarble_camera_vdi_impl.z.so'], f'Unexpected SONAME: {soname}')
            if LLVM_NM.is_file():
                nm = subprocess.check_output([str(LLVM_NM), '-D', str(src)], text=True)
                require(' hdfVdiDesc' in nm, 'libmarble_camera_vdi_impl.z.so missing hdfVdiDesc export')
        elif rel.endswith('native_camera_hdi_probe'):
            require(raw[:4] == b'\x7fELF', f'Expected ELF executable for {rel}')
        elif rel.endswith('.cfg'):
            doc = json.loads(raw.decode('utf-8'))
            svcs = {s['name']: s for s in doc.get('services', [])}
            require('camera_host' in svcs and 'camera_service' in svcs, 'Missing camera_host/camera_service in cfg')
            require(svcs['camera_host']['path'] == ['/vendor/bin/hdf_devhost', '-i', '9', '-n', 'camera_host'],
                    'Unexpected camera_host path/index')
            require(svcs['camera_service']['path'] == ['/system/bin/sa_main', '/system/profile/camera_service.json'],
                    'Unexpected camera_service path')
        elif rel.endswith('.sh'):
            require(b'\r' not in raw, f'CR byte in {rel}')
            subprocess.run(['sh', '-n'], input=raw, check=True)


def load_oracle():
    require(ORACLE.is_file() and ORACLE.resolve() == ORACLE, f'Missing oracle: {ORACLE}')
    got_sha = hashlib.sha256(ORACLE.read_bytes()).hexdigest()
    require(got_sha == ORACLE_SHA, f'Oracle SHA256 mismatch: {got_sha}')
    spec = importlib.util.spec_from_file_location('archive_oracle_g2', ORACLE)
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


def build(base_path: Path, out_dir: Path, donor_fs: Path = DONOR_FS) -> Path:
    require(base_path.is_file(), f'Missing base image: {base_path}')
    require(donor_fs.is_dir(), f'Missing donor fs: {donor_fs}')
    G2_BUILD.mkdir(parents=True, exist_ok=True)
    validate_inputs()
    out_dir.mkdir(parents=True, exist_ok=True)

    archive, r = load_oracle()
    base_bytes = r.read_original(base_path)
    base_sha = r.sha(base_bytes)
    require(base_sha == BASE_SHA, f'Base SHA256 mismatch: got {base_sha}, want {BASE_SHA}')

    before_layout = r.layout(base_bytes)
    before_avb = r.verify_existing_avb(base_bytes, before_layout['payload_end'])

    expected_old_hcb = p4.build_phase4_hdf_hcb(
        wifi_chip_donor=donor_fs,
        wpa_on_demand_donor=donor_fs,
        a2dp_donor=donor_fs,
        ril_donor=donor_fs,
        location_host=True,
        displayengine=True,
        codec_image_host=True,
        codec_video_omx=True,
        codec_heif_hw=True,
    )
    new_hcb = p4.build_phase4_hdf_hcb(
        wifi_chip_donor=donor_fs,
        wpa_on_demand_donor=donor_fs,
        a2dp_donor=donor_fs,
        ril_donor=donor_fs,
        location_host=True,
        displayengine=True,
        codec_image_host=True,
        codec_video_omx=True,
        codec_heif_hw=True,
        camera_host=True,
    )
    cam_host_cfg_hcb = p4.build_marble_camera_host_config_hcb()
    (G2_BUILD / 'camera_host_config.hcb').write_bytes(cam_host_cfg_hcb)

    with tempfile.TemporaryDirectory(prefix='h20_g2_', dir=str(G2_BUILD)) as tmp:
        scratch = Path(tmp)
        old, old_raw, kind, old_rd, args = r.extract_entries(
            base_path, base_bytes, before_layout, scratch / 'before'
        )
        old_manifest = archive.parse(old[r.MANIFEST][1])
        r.manifest_holds(old, old_manifest)

        require(old[HCB_MEMBER][1] == expected_old_hcb, 'Base HCB does not match expected 9-host G1 HCB')
        for member in GUARD_MEMBERS:
            require(member in old, f'Missing guarded member in base image: {member}')

        expected = dict(old)
        new_manifest = copy.deepcopy(old_manifest)
        modified_members = {r.MANIFEST}
        staged_report = {}

        # Update HCB (host-9 camera_host)
        r.set_entry(expected, HCB_MEMBER, new_hcb)
        modified_members.add(HCB_MEMBER)
        hcb_rel = HCB_MEMBER[len(PREFIX):]
        if hcb_rel in new_manifest:
            new_manifest[hcb_rel].update(bytes=len(new_hcb), sha256=r.sha(new_hcb))

        # Splice init debug, stage, postboot
        for member, patch_fn in (
            (INIT_DEBUG_MEMBER, patch_init_debug),
            (STAGE_MEMBER, patch_stage),
            (POSTBOOT_MEMBER, patch_postboot),
        ):
            new_blob = patch_fn(old[member][1])
            r.set_entry(expected, member, new_blob)
            modified_members.add(member)
            rel = member[len(PREFIX):]
            if rel in new_manifest:
                new_manifest[rel].update(bytes=len(new_blob), sha256=r.sha(new_blob))

        # Add/replace staged files + camera_host_config.hcb
        next_inode = [max(header[0] for header, _ in old.values()) + 1]
        all_staged = list(STAGED_FILES) + [
            (CAM_HOST_HCB_REL, G2_BUILD / 'camera_host_config.hcb', 0o644, False),
        ]
        for rel, src, mode, replaces in all_staged:
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
        out_path = out_dir / f'vendor_boot_h20_g2_camera_hdi_{cand_sha[:8]}.img'
        out_path.write_bytes(cand_bytes)

        report = {
            'batch': 'G2',
            'description': 'OHOS Camera HDI (camera_host host-9 + libmarble_camera_vdi_impl.z.so) + SA 3008 camera_service',
            'base': {'path': str(base_path), 'bytes': len(base_bytes), 'sha256': base_sha},
            'candidate': {'path': str(out_path), 'bytes': len(cand_bytes), 'sha256': cand_sha},
            'entries_before': len(old),
            'entries_after': len(got),
            'guard_members_verified': list(GUARD_MEMBERS),
            'hcb_before_bytes': len(expected_old_hcb),
            'hcb_after_bytes': len(new_hcb),
            'staged': staged_report,
            'avb_before': {k: (v.hex() if isinstance(v, bytes) else v) for k, v in before_avb.items()},
            'avb_after': {k: (v.hex() if isinstance(v, bytes) else v) for k, v in after_avb.items()},
        }
        (G2_BUILD / 'g2-build-report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
        print(f'G2_BUILD_OK path={out_path} sha256={cand_sha} entries={len(old)}->{len(got)} hcb={len(expected_old_hcb)}->{len(new_hcb)}')
        return out_path


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--base', type=Path, default=BASE_DEFAULT)
    ap.add_argument('--out-dir', type=Path, default=Path('/mnt/e/marble-tmp'))
    ap.add_argument('--donor-fs', type=Path, default=DONOR_FS)
    ns = ap.parse_args()
    build(ns.base, ns.out_dir, ns.donor_fs)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
