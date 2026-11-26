#!/usr/bin/env python3
"""Package Batch H3 (SM7475 Hardware Power HDI v1.3 + HDF power_host host-12) onto H2 vendor_boot_a."""
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

import build_marble_phase4_images as phase4

H3_BUILD = ROOT / 'work/marble-h3-build'
DONOR_FS = ROOT / 'work/donor-rom/ADY-AL10-207.0.0.111-SP10/fs'
LLVM_READELF = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-readelf'
LLVM_NM = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-nm'

POWER_H3_CFG = ROOT / 'devices/marble/runtime/power/init-h3-power-marble.cfg'
POWER_H3_SCRIPT = ROOT / 'devices/marble/runtime/power/start-power-h3-marble.sh'

ORACLE = ROOT / 'work/marble-m15-role-storage.1v2ynS/build_m15_role_storage.py'
ORACLE_SHA = '7d4f79857a3458ffdb7c8fb86b1cfd893fd9addb02b13e7e05d7b5fa7093cfaa'

BASE_DEFAULT = Path('/mnt/e/marble-tmp/vendor_boot_h20_h2_thermal_b990d6a1.img')
BASE_SHA = 'b990d6a1126103c0cfdcaee9638aead2b084696ee6abb055d66bf7919ca8b875'

PREFIX = 'hmos-compat-marble/'
HCB_MEMBER = PREFIX + 'graphics/hdf_default_phase4.hcb'
INIT_DEBUG_MEMBER = PREFIX + 'hmos-init-debug.cfg'
STAGE_MEMBER = PREFIX + 'graphics/stage-phase4-marble.sh'
POSTBOOT_MEMBER = PREFIX + 'graphics/start-u2-postboot-marble.sh'

# All 64 prior immutable ramdisk members from H2 (byte-for-byte unchanged).
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
)

STAGED_FILES = (
    ('graphics/libmarble_power_interface_service_1.3.z.so', H3_BUILD / 'libmarble_power_interface_service_1.3.z.so', 0o644),
    ('graphics/native_power_hdi_probe', H3_BUILD / 'native_power_hdi_probe', 0o755),
    ('graphics/init-h3-power-marble.cfg', POWER_H3_CFG, 0o644),
    ('graphics/start-power-h3-marble.sh', POWER_H3_SCRIPT, 0o755),
)

INIT_IMP_OLD = b'    "/compat/etc/init-h2-thermal-marble.cfg"\n  ],'
INIT_IMP_NEW = (
    b'    "/compat/etc/init-h2-thermal-marble.cfg",\n'
    b'    "/compat/etc/init-h3-power-marble.cfg"\n  ],'
)

STAGE_ANCHOR = b'$B test -x "$R/compat/bin/init"\necho MARBLE_PHASE4_RAM_COMPAT_READY'
STAGE_H3_BLOCK = (
    b'# === MARBLE_H3_POWER_HDI_STAGE_BEGIN ===\n'
    b'h3_bind() {\n'
    b' $B grep -q " $2 " /proc/mounts 2>/dev/null && return 0\n'
    b' $B mount --bind "$1" "$2" 2>/dev/null || echo "MARBLE_H3_BIND_FAIL dst=$2"\n'
    b'}\n'
    b'$B mkdir -p /compat/bin "$R/compat/bin" "$R/compat/lib64" "$R/compat/etc"\n'
    b'if $B test -f /graphics/libmarble_power_interface_service_1.3.z.so; then\n'
    b' $B cp /graphics/libmarble_power_interface_service_1.3.z.so "$R/compat/lib64/libmarble_power_interface_service_1.3.z.so"\n'
    b' $B chmod 0644 "$R/compat/lib64/libmarble_power_interface_service_1.3.z.so"\n'
    b' if $B test -f "$R/vendor/lib64/libpower_interface_service_1.3.z.so"; then\n'
    b'  h3_bind "$R/compat/lib64/libmarble_power_interface_service_1.3.z.so" "$R/vendor/lib64/libpower_interface_service_1.3.z.so"\n'
    b' fi\n'
    b'fi\n'
    b'if $B test -f /graphics/init-h3-power-marble.cfg; then\n'
    b' $B cp /graphics/init-h3-power-marble.cfg "$R/compat/etc/init-h3-power-marble.cfg"\n'
    b' $B chmod 0644 "$R/compat/etc/init-h3-power-marble.cfg"\n'
    b'fi\n'
    b'for h3bin in native_power_hdi_probe start-power-h3-marble.sh; do\n'
    b' if $B test -f "/graphics/$h3bin"; then\n'
    b'  $B cp "/graphics/$h3bin" "/compat/bin/$h3bin"\n'
    b'  $B cp "/graphics/$h3bin" "$R/compat/bin/$h3bin"\n'
    b'  $B chmod 0755 "/compat/bin/$h3bin" "$R/compat/bin/$h3bin"\n'
    b' fi\n'
    b'done\n'
    b'echo "MARBLE_H3_POWER_HDI_STAGED=1"\n'
    b'# === MARBLE_H3_POWER_HDI_STAGE_END ===\n'
)

POSTBOOT_ANCHOR = b'say "U2_GNSS_MODE=hdf_location_host"'
POSTBOOT_H3_BLOCK = (
    b'# Step G: Batch H3 SM7475 Hardware Power HDI v1.3 (power_host host-12).\n'
    b'if skipped power || skipped power_hdi; then\n'
    b'  say "U2_POWER_HDI_SKIP=requested"\n'
    b'elif [ -x /compat/bin/start-power-h3-marble.sh ]; then\n'
    b'  $B sh /compat/bin/start-power-h3-marble.sh || say "U2_POWER_HDI_RC=$?"\n'
    b'else\n'
    b'  say "U2_POWER_HDI_SKIP=script_missing"\n'
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
    require(b'init-h3-power-marble.cfg' not in raw, 'hmos-init-debug.cfg already contains H3 import')
    out = splice_once(raw, ((INIT_IMP_OLD, INIT_IMP_NEW),), INIT_DEBUG_MEMBER)
    doc = json.loads(out.decode('utf-8'))
    require('/compat/etc/init-h3-power-marble.cfg' in doc.get('import', []),
            'H3 Power init import missing after JSON parse')
    require(len(out) < 32768, f'hmos-init-debug.cfg exceeds 32768-byte init cap: {len(out)}')
    return out


def patch_stage(raw: bytes) -> bytes:
    for marker in (b'MARBLE_H3_POWER_HDI_STAGE_BEGIN', b'libmarble_power_interface_service_1.3.z.so'):
        require(marker not in raw, f'stage script already contains {marker!r}')
    require(b'MARBLE_H2_THERMAL_HDI_STAGE_BEGIN' in raw, 'Expected H2 stage marker in base stage script')
    out = splice_once(raw, ((STAGE_ANCHOR, STAGE_H3_BLOCK + STAGE_ANCHOR),), STAGE_MEMBER)
    require(b'\r' not in out, 'CR byte in patched stage-phase4-marble.sh')
    subprocess.run(['sh', '-n'], input=out, check=True)
    return out


def patch_postboot(raw: bytes) -> bytes:
    for marker in (b'start-power-h3-marble.sh', b'U2_POWER_HDI_SKIP'):
        require(marker not in raw, f'postboot script already contains {marker!r}')
    require(b'start-thermal-h2-marble.sh' in raw, 'Expected H2 postboot call in base postboot script')
    out = splice_once(raw, ((POSTBOOT_ANCHOR, POSTBOOT_H3_BLOCK + POSTBOOT_ANCHOR),), POSTBOOT_MEMBER)
    require(b'\r' not in out, 'CR byte in patched start-u2-postboot-marble.sh')
    subprocess.run(['sh', '-n'], input=out, check=True)
    return out


def validate_inputs() -> None:
    for rel, src, _mode in STAGED_FILES:
        require(src.is_file(), f'Missing staged H3 artifact: {src}')
        raw = src.read_bytes()
        if rel.endswith('.cfg'):
            doc = json.loads(raw.decode('utf-8'))
            svcs = {s['name']: s for s in doc.get('services', [])}
            require('power_host' in svcs and svcs['power_host']['path'] == ['/vendor/bin/hdf_devhost', '-i', '12', '-n', 'power_host'],
                    'Invalid power_host service config')
        elif rel.endswith('.sh'):
            require(b'\r' not in raw, f'CR byte in {rel}')
            subprocess.run(['sh', '-n'], input=raw, check=True)
        elif rel.endswith('libmarble_power_interface_service_1.3.z.so'):
            require(raw[:4] == b'\x7fELF', f'Expected ELF for {rel}')
            if LLVM_READELF.is_file():
                dyn = subprocess.check_output([str(LLVM_READELF), '-d', str(src)], text=True)
                soname = re.findall(r'\(SONAME\)\s+Library soname:\s+\[([^\]]+)\]', dyn)
                require(soname == ['libpower_interface_service_1.3.z.so'], f'Unexpected SONAME: {soname}')
                syms = subprocess.check_output([str(LLVM_READELF), '-sW', str(src)], text=True)
                vtv_sizes = []
                for line in syms.splitlines():
                    m = re.match(r'\s*\d+:\s+[0-9a-f]+\s+(\d+)\s+\S+\s+\S+\s+\S+\s+\S+\s+(_ZTVN4OHOS3HDI5Power4V1_318PowerInterfaceImplE)', line)
                    if m:
                        vtv_sizes.append(int(m.group(1)))
                require(vtv_sizes and all(sz == 360 for sz in vtv_sizes),
                        f'PowerInterfaceImpl vtable ABI mismatch: got {vtv_sizes}, want 360')
            if LLVM_NM.is_file():
                nm = subprocess.check_output([str(LLVM_NM), '-D', str(src)], text=True)
                require(' PowerInterfaceImplGetInstance' in nm, 'Missing PowerInterfaceImplGetInstance export')
        elif rel.endswith('native_power_hdi_probe'):
            require(raw[:4] == b'\x7fELF', f'Expected ELF for {rel}')
            if LLVM_READELF.is_file():
                dyn = subprocess.check_output([str(LLVM_READELF), '-d', str(src)], text=True)
                needed = re.findall(r'\(NEEDED\)\s+Shared library:\s+\[([^\]]+)\]', dyn)
                require('libpower_proxy_1.3.z.so' in needed and 'libpower_proxy_1.2.z.so' in needed,
                        f'Unexpected NEEDED for {rel}: {needed}')


def load_oracle():
    require(ORACLE.is_file() and ORACLE.resolve() == ORACLE, f'Missing oracle: {ORACLE}')
    got_sha = hashlib.sha256(ORACLE.read_bytes()).hexdigest()
    require(got_sha == ORACLE_SHA, f'Oracle SHA256 mismatch: {got_sha}')
    spec = importlib.util.spec_from_file_location('archive_oracle_h3', ORACLE)
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
    H3_BUILD.mkdir(parents=True, exist_ok=True)
    validate_inputs()
    out_dir.mkdir(parents=True, exist_ok=True)

    archive, r = load_oracle()
    base_bytes = r.read_original(base_path)
    base_sha = r.sha(base_bytes)
    require(base_sha == BASE_SHA, f'Base SHA256 mismatch: got {base_sha}, want {BASE_SHA}')

    before_layout = r.layout(base_bytes)
    before_avb = r.verify_existing_avb(base_bytes, before_layout['payload_end'])

    expected_base_hcb = phase4.build_phase4_hdf_hcb(
        wifi_chip_donor=DONOR_FS,
        wpa_on_demand_donor=DONOR_FS,
        a2dp_donor=DONOR_FS,
        ril_donor=DONOR_FS,
        location_host=True,
        displayengine=True,
        codec_image_host=True,
        codec_video_omx=True,
        codec_heif_hw=True,
        camera_host=True,
        nfc_host=True,
        thermal_host=True,
        power_host=False,
    )
    new_hcb = phase4.build_phase4_hdf_hcb(
        wifi_chip_donor=DONOR_FS,
        wpa_on_demand_donor=DONOR_FS,
        a2dp_donor=DONOR_FS,
        ril_donor=DONOR_FS,
        location_host=True,
        displayengine=True,
        codec_image_host=True,
        codec_video_omx=True,
        codec_heif_hw=True,
        camera_host=True,
        nfc_host=True,
        thermal_host=True,
        power_host=True,
    )
    require(b'power_host' in new_hcb and b'libpower_driver.z.so' in new_hcb and b'power_interface_service' in new_hcb,
            'Generated H3 HCB missing power_host declaration')

    with tempfile.TemporaryDirectory(prefix='h20_h3_', dir=str(H3_BUILD)) as tmp:
        scratch = Path(tmp)
        old, old_raw, kind, old_rd, args = r.extract_entries(
            base_path, base_bytes, before_layout, scratch / 'before'
        )
        old_manifest = archive.parse(old[r.MANIFEST][1])
        r.manifest_holds(old, old_manifest)

        require(old[HCB_MEMBER][1] == expected_base_hcb,
                'Base hdf_default_phase4.hcb does not match expected H2 12-host HCB')
        for member in GUARD_MEMBERS:
            require(member in old, f'Missing guarded member in base image: {member}')

        expected = dict(old)
        new_manifest = copy.deepcopy(old_manifest)
        modified_members = {r.MANIFEST, HCB_MEMBER, INIT_DEBUG_MEMBER, STAGE_MEMBER, POSTBOOT_MEMBER}
        staged_report = {}

        r.set_entry(expected, HCB_MEMBER, new_hcb)
        update_manifest_entry(new_manifest, HCB_MEMBER, new_hcb, 0o644, r.sha)

        new_init_debug = patch_init_debug(old[INIT_DEBUG_MEMBER][1])
        r.set_entry(expected, INIT_DEBUG_MEMBER, new_init_debug)
        if 'hmos-init-debug.cfg' in new_manifest:
            new_manifest['hmos-init-debug.cfg'].update(bytes=len(new_init_debug), sha256=r.sha(new_init_debug))

        new_stage = patch_stage(old[STAGE_MEMBER][1])
        r.set_entry(expected, STAGE_MEMBER, new_stage)
        stage_rel = STAGE_MEMBER[len(PREFIX):]
        if stage_rel in new_manifest:
            new_manifest[stage_rel].update(bytes=len(new_stage), sha256=r.sha(new_stage))

        new_postboot = patch_postboot(old[POSTBOOT_MEMBER][1])
        r.set_entry(expected, POSTBOOT_MEMBER, new_postboot)
        pb_rel = POSTBOOT_MEMBER[len(PREFIX):]
        if pb_rel in new_manifest:
            new_manifest[pb_rel].update(bytes=len(new_postboot), sha256=r.sha(new_postboot))

        next_inode = [max(header[0] for header, _ in old.values()) + 1]
        for rel, src, mode in STAGED_FILES:
            member = PREFIX + rel
            blob = src.read_bytes()
            require(member not in expected, f'Unexpected pre-existing member: {member}')
            r.image_tools.put_file(expected, member, blob, mode, next_inode)
            expected[member][0][11] = len(member.encode('utf-8')) + 1
            update_manifest_entry(new_manifest, member, blob, mode, r.sha)
            staged_report[rel] = {'bytes': len(blob), 'mode': oct(mode), 'sha256': r.sha(blob)}

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
        out_path = out_dir / f'vendor_boot_h20_h3_power_{cand_sha[:8]}.img'
        out_path.write_bytes(cand_bytes)

        report = {
            'batch': 'H3',
            'description': 'SM7475 Hardware Power HDI v1.3 (real /sys/power/wake_lock + PowerDump + v1.2/v1.3 callbacks) + HDF power_host (host-12) & Binder probe',
            'base': {'path': str(base_path), 'bytes': len(base_bytes), 'sha256': base_sha},
            'candidate': {'path': str(out_path), 'bytes': len(cand_bytes), 'sha256': cand_sha},
            'entries_before': len(old),
            'entries_after': len(got),
            'guard_members_verified': list(GUARD_MEMBERS),
            'hcb_sha256': r.sha(new_hcb),
            'staged': staged_report,
            'avb_before': {k: (v.hex() if isinstance(v, bytes) else v) for k, v in before_avb.items()},
            'avb_after': {k: (v.hex() if isinstance(v, bytes) else v) for k, v in after_avb.items()},
        }
        (H3_BUILD / 'h3-build-report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
        print(f'H3_BUILD_OK path={out_path} sha256={cand_sha} entries={len(old)}->{len(got)}')
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
