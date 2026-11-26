#!/usr/bin/env python3
"""Package Batch M1 (F13b Zero-Race WDA QMAPv5 + Vibrator HDI v2.0 + SA 3602 + Display Color/Eye-Comfort/AOD + DT2W) onto F13 vendor_boot_a."""
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
M1_BUILD = ROOT / 'work/marble-m1-build'
DONOR_FS = ROOT / 'work/donor-rom/ADY-AL10-207.0.0.111-SP10/fs'
LLVM_READELF = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-readelf'
LLVM_NM = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-nm'

sys.path.insert(0, str(ROOT / 'tools/packaging'))
import build_marble_phase4_images as phase4  # noqa: E402

ORACLE = ROOT / 'work/marble-m15-role-storage.1v2ynS/build_m15_role_storage.py'
ORACLE_SHA = '7d4f79857a3458ffdb7c8fb86b1cfd893fd9addb02b13e7e05d7b5fa7093cfaa'

BASE_DEFAULT = Path('/mnt/e/marble-tmp/vendor_boot_h20_f13_tel_loc_cutout_41368e8d.img')
BASE_SHA = '41368e8d63e89935287d1535853c493d64afe1a23910b8b62fcb194814c000fd'

PREFIX = 'hmos-compat-marble/'
INIT_DEBUG_MEMBER = PREFIX + 'hmos-init-debug.cfg'
HCB_MEMBER = PREFIX + 'graphics/hdf_default_phase4.hcb'
STAGE_MEMBER = PREFIX + 'graphics/stage-phase4-marble.sh'
POSTBOOT_MEMBER = PREFIX + 'graphics/start-u2-postboot-marble.sh'

SENSORS_JSON = ROOT / 'devices/marble/runtime/sensors/sensors-marble.json'
VIB_CFG = ROOT / 'devices/marble/runtime/vibrator/init-m1-vibrator-marble.cfg'
VIB_SH = ROOT / 'devices/marble/runtime/vibrator/start-vibrator-m1-marble.sh'
TOUCH_CFG = ROOT / 'devices/marble/runtime/touch/init-m1-touch-wakeup-marble.cfg'

# All immutable non-M1 ramdisk members from F13/F12/F11/H7/G5/U1-U4 preserved byte-for-byte.
GUARD_MEMBERS = (
    PREFIX + 'graphics/start-phase4-marble.sh',
    PREFIX + 'graphics/init-modem-f12-marble.cfg',
    PREFIX + 'graphics/hmos_ims_core_service',
    PREFIX + 'graphics/start-modem-f7-marble.sh',
    PREFIX + 'graphics/display_manager_config.xml',
    PREFIX + 'graphics/libmarble_gnss_hdi_driver.z.so',
    PREFIX + 'graphics/native_gnss_hdi_publisher',
    PREFIX + 'graphics/libhmos_audio_vdi.so',
    PREFIX + 'graphics/audio-runtime/lib64/libhmos_pal_server.so',
    PREFIX + 'graphics/native_voice_audio_probe',
    PREFIX + 'graphics/telephony-marble.json',
    PREFIX + 'graphics/foundation-app-core.json',
    PREFIX + 'graphics/init-bluetooth-marble.cfg',
    PREFIX + 'graphics/init-d-sensors-marble.cfg',
    PREFIX + 'graphics/hmos_tqftpserv',
    PREFIX + 'graphics/hmos_rmtfs',
    PREFIX + 'graphics/native_ril_hdi_loader',
    PREFIX + 'graphics/init-modem-f2-marble.cfg',
    PREFIX + 'graphics/init-modem-f3-marble.cfg',
    PREFIX + 'graphics/start-modem-f1-marble.sh',
    PREFIX + 'graphics/start-modem-f2-marble.sh',
    PREFIX + 'graphics/start-modem-f3-marble.sh',
    PREFIX + 'graphics/start-modem-f4-marble.sh',
    PREFIX + 'graphics/start-modem-f5-marble.sh',
    PREFIX + 'graphics/start-modem-f6-marble.sh',
    PREFIX + 'graphics/carrier-cn-marble.tar.gz',
    PREFIX + 'graphics/libhuks_hdi_service_1.1.z.so',
    PREFIX + 'graphics/libhuks_engine_core_standard.z.so',
    PREFIX + 'graphics/libdbkeys.so',
    PREFIX + 'graphics/libhmos_sceneboard_optional_sa.so',
    PREFIX + 'graphics/init-u1-services-marble.cfg',
    PREFIX + 'graphics/init-u2-location-marble.cfg',
    PREFIX + 'graphics/libjpeg_vdi_impl.z.so',
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
    PREFIX + 'graphics/native_venc_probe',
    PREFIX + 'graphics/libvulkan_freedreno.so.gz',
    PREFIX + 'graphics/zink-runtime-marble.tar.gz',
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
    PREFIX + 'graphics/libmarble_nci_native_vendor.z.so',
    PREFIX + 'graphics/native_nfc_fwk_probe',
    PREFIX + 'graphics/init-h7-nfc-service-marble.cfg',
    PREFIX + 'graphics/start-nfc-service-h7-marble.sh',
    PREFIX + 'graphics/libnfc-nci-sn110.conf',
    PREFIX + 'graphics/libnfc-nxp-sn110.conf',
)

STAGED_REPLACE = (
    ('graphics/marble_modem_probe', M1_BUILD / 'telephony/marble_modem_probe', 0o755),
    ('graphics/libril_vendor_qmi.z.so', M1_BUILD / 'telephony/libril_vendor_qmi.z.so', 0o644),
    ('graphics/sensors-marble.json', SENSORS_JSON, 0o644),
    ('graphics/libdisplay_composer_vdi_impl.z.so', M1_BUILD / 'display/libdisplay_composer_vdi_impl.z.so', 0o644),
    ('graphics/libdisplayengine_module.so', M1_BUILD / 'display/libdisplayengine_module.so', 0o644),
)

STAGED_ADD = (
    ('graphics/libmarble_vibrator_interface_service_2.0.z.so', M1_BUILD / 'vibrator/libmarble_vibrator_interface_service_2.0.z.so', 0o644),
    ('graphics/native_vibrator_hdi_probe', M1_BUILD / 'vibrator/native_vibrator_hdi_probe', 0o755),
    ('graphics/init-m1-vibrator-marble.cfg', VIB_CFG, 0o644),
    ('graphics/start-vibrator-m1-marble.sh', VIB_SH, 0o755),
    ('graphics/marble_touch_wakeup', M1_BUILD / 'display/marble_touch_wakeup', 0o755),
    ('graphics/init-m1-touch-wakeup-marble.cfg', TOUCH_CFG, 0o644),
)

INIT_IMP_OLD = b'    "/compat/etc/init-modem-f12-marble.cfg"\n  ],'
INIT_IMP_NEW = (
    b'    "/compat/etc/init-modem-f12-marble.cfg",\n'
    b'    "/compat/etc/init-m1-vibrator-marble.cfg",\n'
    b'    "/compat/etc/init-m1-touch-wakeup-marble.cfg"\n  ],'
)

STAGE_ANCHOR = b'$B test -x "$R/compat/bin/init"\necho MARBLE_PHASE4_RAM_COMPAT_READY'
STAGE_M1_BLOCK = (
    b'# === MARBLE_M1_VIB_DISP_STAGE_BEGIN ===\n'
    b'$B mkdir -p /compat/bin "$R/compat/bin" "$R/compat/lib64" "$R/compat/etc" "$R/dev/input"\n'
    b'if $B test -f /graphics/libmarble_vibrator_interface_service_2.0.z.so; then\n'
    b' $B cp /graphics/libmarble_vibrator_interface_service_2.0.z.so "$R/compat/lib64/libmarble_vibrator_interface_service_2.0.z.so"\n'
    b' $B chmod 0644 "$R/compat/lib64/libmarble_vibrator_interface_service_2.0.z.so"\n'
    b' if $B test -f "$R/vendor/lib64/libvibrator_interface_service_2.0.z.so"; then\n'
    b'  if ! $B grep -q " $R/vendor/lib64/libvibrator_interface_service_2.0.z.so " /proc/mounts 2>/dev/null; then\n'
    b'   $B mount --bind "$R/compat/lib64/libmarble_vibrator_interface_service_2.0.z.so" "$R/vendor/lib64/libvibrator_interface_service_2.0.z.so" 2>/dev/null || true\n'
    b'  fi\n'
    b' fi\n'
    b'fi\n'
    b'for m1cfg in init-m1-vibrator-marble.cfg init-m1-touch-wakeup-marble.cfg; do\n'
    b' if $B test -f "/graphics/$m1cfg"; then\n'
    b'  $B cp "/graphics/$m1cfg" "$R/compat/etc/$m1cfg"\n'
    b'  $B chmod 0644 "$R/compat/etc/$m1cfg"\n'
    b' fi\n'
    b'done\n'
    b'for m1bin in native_vibrator_hdi_probe start-vibrator-m1-marble.sh marble_touch_wakeup; do\n'
    b' if $B test -f "/graphics/$m1bin"; then\n'
    b'  $B cp "/graphics/$m1bin" "/compat/bin/$m1bin"\n'
    b'  $B cp "/graphics/$m1bin" "$R/compat/bin/$m1bin"\n'
    b'  $B chmod 0755 "/compat/bin/$m1bin" "$R/compat/bin/$m1bin"\n'
    b' fi\n'
    b'done\n'
    b'if [ ! -c "$R/dev/uinput" ]; then\n'
    b' $B mknod -m 0600 "$R/dev/uinput" c 10 223 2>/dev/null || true\n'
    b'fi\n'
    b'for dispnode in /sys/class/mi_display/disp-DSI-0/doze_brightness /sys/class/mi_display/disp-DSI-0/disp_param; do\n'
    b' if $B test -e "$dispnode"; then\n'
    b'  $B chown 3036:1003 "$dispnode" 2>/dev/null || true\n'
    b'  $B chmod 0664 "$dispnode" 2>/dev/null || true\n'
    b' fi\n'
    b'done\n'
    b'echo "MARBLE_M1_VIB_DISP_STAGED=1"\n'
    b'# === MARBLE_M1_VIB_DISP_STAGE_END ===\n'
)

POSTBOOT_ANCHOR = b'say "U2_GNSS_MODE=hdf_location_host"'
POSTBOOT_M1_BLOCK = (
    b'# Step K: Batch M1 SM7475 Vibrator HDI v2.0 (vibrator_host host-15).\n'
    b'if skipped vibrator || skipped haptics || skipped vibrator_hdi; then\n'
    b'  say "U2_VIBRATOR_M1_SKIP=requested"\n'
    b'elif [ -x /compat/bin/start-vibrator-m1-marble.sh ]; then\n'
    b'  $B sh /compat/bin/start-vibrator-m1-marble.sh || say "U2_VIBRATOR_M1_RC=$?"\n'
    b'else\n'
    b'  say "U2_VIBRATOR_M1_SKIP=script_missing"\n'
    b'fi\n\n'
    b'# Step L: Batch M1 Display AOD sysfs permissions + DT2W daemon readiness.\n'
    b'for dispnode in /sys/class/mi_display/disp-DSI-0/doze_brightness /sys/class/mi_display/disp-DSI-0/disp_param; do\n'
    b'  if [ -e "$dispnode" ]; then\n'
    b'    $B chown 3036:1003 "$dispnode" 2>/dev/null || true\n'
    b'    $B chmod 0664 "$dispnode" 2>/dev/null || true\n'
    b'  fi\n'
    b'done\n'
    b'if ! skipped touch_wakeup && ! skipped dt2w && [ -x "$R/system/bin/param" ]; then\n'
    b'  $B chroot "$R" /system/bin/param set marble.touch_wakeup.ready true >/dev/null 2>&1 || true\n'
    b'  say "U2_TOUCH_WAKEUP_M1_TRIGGERED=1"\n'
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
    for m in (b'init-m1-vibrator-marble.cfg', b'init-m1-touch-wakeup-marble.cfg'):
        require(m not in raw, f'hmos-init-debug.cfg already contains {m!r}')
    out = splice_once(raw, ((INIT_IMP_OLD, INIT_IMP_NEW),), INIT_DEBUG_MEMBER)
    doc = json.loads(out.decode('utf-8'))
    imps = doc.get('import', [])
    require('/compat/etc/init-m1-vibrator-marble.cfg' in imps, 'Missing M1 vibrator init import')
    require('/compat/etc/init-m1-touch-wakeup-marble.cfg' in imps, 'Missing M1 touch wakeup init import')
    require(len(out) < 32768, f'hmos-init-debug.cfg exceeds 32768B cap: {len(out)}')
    return out


def patch_stage(raw: bytes) -> bytes:
    for m in (b'MARBLE_M1_VIB_DISP_STAGE_BEGIN', b'libmarble_vibrator_interface_service_2.0.z.so'):
        require(m not in raw, f'stage script already contains {m!r}')
    require(b'MARBLE_F12_TEL_FIXES_STAGE_BEGIN' in raw, 'Expected F12 stage marker')
    out = splice_once(raw, ((STAGE_ANCHOR, STAGE_M1_BLOCK + STAGE_ANCHOR),), STAGE_MEMBER)
    require(b'\r' not in out, 'CR byte in patched stage-phase4-marble.sh')
    subprocess.run(['sh', '-n'], input=out, check=True)
    return out


def patch_postboot(raw: bytes) -> bytes:
    for m in (b'start-vibrator-m1-marble.sh', b'U2_TOUCH_WAKEUP_M1_TRIGGERED'):
        require(m not in raw, f'postboot script already contains {m!r}')
    require(b'start-nfc-service-h7-marble.sh' in raw, 'Expected H7 postboot call')
    out = splice_once(raw, ((POSTBOOT_ANCHOR, POSTBOOT_M1_BLOCK + POSTBOOT_ANCHOR),), POSTBOOT_MEMBER)
    require(b'\r' not in out, 'CR byte in patched start-u2-postboot-marble.sh')
    subprocess.run(['sh', '-n'], input=out, check=True)
    return out


def validate_inputs() -> None:
    for rel, src, _mode in STAGED_REPLACE + STAGED_ADD:
        require(src.is_file(), f'Missing staged M1 artifact: {src}')
        raw = src.read_bytes()
        if rel.endswith('.json'):
            doc = json.loads(raw.decode('utf-8'))
            sas = {sa['name']: sa['libpath'] for sa in doc.get('systemability', [])}
            require(sas.get(3601) == 'libsensor_service.z.so' and sas.get(3602) == 'libmiscdevice_service.z.so',
                    f'Unexpected sensors-marble.json SAs: {sas}')
        elif rel.endswith('.cfg'):
            doc = json.loads(raw.decode('utf-8'))
            require('services' in doc and 'jobs' in doc, f'Invalid cfg: {rel}')
        elif rel.endswith('.sh'):
            require(b'\r' not in raw, f'CR byte in {rel}')
            subprocess.run(['sh', '-n'], input=raw, check=True)
        else:
            require(raw[:4] == b'\x7fELF', f'Expected ELF for {rel}')
            if LLVM_READELF.is_file():
                dyn = subprocess.check_output([str(LLVM_READELF), '-d', str(src)], text=True)
                soname = re.findall(r'\(SONAME\)\s+Library soname:\s+\[([^\]]+)\]', dyn)
                if rel.endswith('libril_vendor_qmi.z.so'):
                    require(soname == ['libril_vendor.z.so'], f'Unexpected RIL SONAME: {soname}')
                elif rel.endswith('libmarble_vibrator_interface_service_2.0.z.so'):
                    require(soname == ['libvibrator_interface_service_2.0.z.so'], f'Unexpected Vib SONAME: {soname}')
                elif rel.endswith('libdisplay_composer_vdi_impl.z.so'):
                    require(soname == ['libdisplay_composer_vdi_impl.z.so'], f'Unexpected Composer SONAME: {soname}')
                elif rel.endswith('libdisplayengine_module.so'):
                    require(soname == ['libdisplayengine_module.so'], f'Unexpected DE SONAME: {soname}')
            if LLVM_NM.is_file():
                if rel.endswith('libmarble_vibrator_interface_service_2.0.z.so'):
                    nm = subprocess.check_output([str(LLVM_NM), '-D', str(src)], text=True)
                    require(' VibratorInterfaceImplGetInstance' in nm, 'Missing VibratorInterfaceImplGetInstance')
                elif rel.endswith('libdisplay_composer_vdi_impl.z.so'):
                    nm = subprocess.check_output([str(LLVM_NM), '-D', str(src)], text=True)
                    require(' MarbleSetPccMatrix' in nm and ' MarbleSetColorModeMatrix' in nm,
                            'Missing MarbleSetPccMatrix/MarbleSetColorModeMatrix exports')


def load_oracle():
    require(ORACLE.is_file() and ORACLE.resolve() == ORACLE, f'Missing oracle: {ORACLE}')
    got_sha = hashlib.sha256(ORACLE.read_bytes()).hexdigest()
    require(got_sha == ORACLE_SHA, f'Oracle SHA256 mismatch: {got_sha}')
    spec = importlib.util.spec_from_file_location('archive_oracle_m1', ORACLE)
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
    M1_BUILD.mkdir(parents=True, exist_ok=True)
    validate_inputs()
    out_dir.mkdir(parents=True, exist_ok=True)

    archive, r = load_oracle()
    base_bytes = r.read_original(base_path)
    base_sha = r.sha(base_bytes)
    require(base_sha == BASE_SHA, f'Base SHA256 mismatch: got {base_sha}, want {BASE_SHA}')

    before_layout = r.layout(base_bytes)
    before_avb = r.verify_existing_avb(base_bytes, before_layout['payload_end'])

    hcb_kwargs = dict(
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
        usb_host=True,
        memorytracker_host=True,
    )
    expected_base_hcb = phase4.build_phase4_hdf_hcb(**hcb_kwargs, vibrator_host=False)
    new_hcb = phase4.build_phase4_hdf_hcb(**hcb_kwargs, vibrator_host=True)
    require(b'vibrator_host' in new_hcb and b'libvibrator_driver.z.so' in new_hcb and b'vibrator_interface_service' in new_hcb,
            'Generated M1 HCB missing vibrator_host declaration')

    with tempfile.TemporaryDirectory(prefix='h20_m1_', dir=str(M1_BUILD)) as tmp:
        scratch = Path(tmp)
        old, old_raw, kind, old_rd, args = r.extract_entries(
            base_path, base_bytes, before_layout, scratch / 'before'
        )
        old_manifest = archive.parse(old[r.MANIFEST][1])
        r.manifest_holds(old, old_manifest)

        require(old[HCB_MEMBER][1] == expected_base_hcb,
                'Base hdf_default_phase4.hcb does not match expected H5 15-host HCB')
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
        update_manifest_entry(new_manifest, STAGE_MEMBER, new_stage, 0o755, r.sha)

        new_postboot = patch_postboot(old[POSTBOOT_MEMBER][1])
        r.set_entry(expected, POSTBOOT_MEMBER, new_postboot)
        update_manifest_entry(new_manifest, POSTBOOT_MEMBER, new_postboot, 0o755, r.sha)

        for rel, src, mode in STAGED_REPLACE:
            member = PREFIX + rel
            require(member in old, f'Expected existing member to replace: {member}')
            blob = src.read_bytes()
            r.set_entry(expected, member, blob)
            update_manifest_entry(new_manifest, member, blob, mode, r.sha)
            modified_members.add(member)
            staged_report[rel] = {'bytes': len(blob), 'mode': oct(mode), 'sha256': r.sha(blob), 'op': 'replace'}

        next_inode = [max(header[0] for header, _ in old.values()) + 1]
        for rel, src, mode in STAGED_ADD:
            member = PREFIX + rel
            require(member not in old, f'New M1 member already exists in base image: {member}')
            blob = src.read_bytes()
            r.image_tools.put_file(expected, member, blob, mode, next_inode)
            expected[member][0][11] = len(member.encode('utf-8')) + 1
            update_manifest_entry(new_manifest, member, blob, mode, r.sha)
            modified_members.add(member)
            staged_report[rel] = {'bytes': len(blob), 'mode': oct(mode), 'sha256': r.sha(blob), 'op': 'add'}

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
        out_path = out_dir / f'vendor_boot_h20_m1_tel_vib_disp_{cand_sha[:8]}.img'
        out_path.write_bytes(cand_bytes)
        require(len(cand_bytes) == len(base_bytes) == 100663296, f'Size mismatch: {len(cand_bytes)}')

        report = {
            'batch': 'M1',
            'description': (
                'F13b Zero-Race WDA QMAPv5 (marble_modem_probe --dpm-open + imsdcm_open_wds) + '
                'Vibrator HDI v2.0 (vibrator_host host-15 + SA 3602 libmiscdevice_service) + '
                'Display Color/Eye-Comfort (SDE_DSPP_PCC_V4 + DisplayEngineWrapperOH) + '
                'AOD (LP1/LP2 + doze_brightness) + DT2W (/dev/uinput + xiaomi_touch POLLPRI)'
            ),
            'base': {'path': str(base_path), 'bytes': len(base_bytes), 'sha256': base_sha},
            'candidate': {'path': str(out_path), 'bytes': len(cand_bytes), 'sha256': cand_sha},
            'entries_before': len(old),
            'entries_after': len(got),
            'hcb_hosts_before': 15,
            'hcb_hosts_after': 16,
            'guard_members_verified': list(GUARD_MEMBERS),
            'staged': staged_report,
            'avb_before': {k: v.hex() if isinstance(v, bytes) else v for k, v in before_avb.items()},
            'avb_after': {k: v.hex() if isinstance(v, bytes) else v for k, v in after_avb.items()},
        }
        report_path = M1_BUILD / 'm1-build-report.json'
        report_path.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
        print(f'M1_BUILD_OK candidate={out_path} sha256={cand_sha} entries={len(old)}->{len(got)}')
        return out_path


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--base', type=Path, default=BASE_DEFAULT)
    ap.add_argument('--out-dir', type=Path, default=Path('/mnt/e/marble-tmp'))
    args = ap.parse_args()
    build(args.base, args.out_dir)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
