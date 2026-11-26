#!/usr/bin/env python3
"""Package Batch M4 (Auto-Brightness ALS On-Change Replay + Raise-to-Wake Motion HDI v1.1/SA 2901 + DroiTong ANCO updater_sa/broker/cgroups) onto M3 vendor_boot_a."""
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
M4_BUILD = ROOT / 'work/marble-m4-build'
DONOR_FS = ROOT / 'work/donor-rom/ADY-AL10-207.0.0.111-SP10/fs'
LLVM_READELF = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-readelf'
LLVM_NM = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-nm'

sys.path.insert(0, str(ROOT / 'tools/packaging'))
import build_marble_phase4_images as phase4  # noqa: E402

ORACLE = ROOT / 'work/marble-m15-role-storage.1v2ynS/build_m15_role_storage.py'
ORACLE_SHA = '7d4f79857a3458ffdb7c8fb86b1cfd893fd9addb02b13e7e05d7b5fa7093cfaa'

BASE_DEFAULT = Path('/mnt/e/marble-tmp/vendor_boot_h20_m3_codec_pin_anco_f54237e5.img')
BASE_SHA = 'f54237e5a3838bfc925d265189a51c713db7d37cbea2e5ddc7d39268dbb3be5b'

PREFIX = 'hmos-compat-marble/'
INIT_DEBUG_MEMBER = PREFIX + 'hmos-init-debug.cfg'
INIT_PLATFORM_MEMBER = PREFIX + 'graphics/init-platform-marble.cfg'
HCB_MEMBER = PREFIX + 'graphics/hdf_default_phase4.hcb'
STAGE_MEMBER = PREFIX + 'graphics/stage-phase4-marble.sh'
START_MEMBER = PREFIX + 'graphics/start-phase4-marble.sh'
POSTBOOT_MEMBER = PREFIX + 'graphics/start-u2-postboot-marble.sh'

MOTION_CFG = ROOT / 'devices/marble/runtime/motion/init-m4-motion-marble.cfg'
MOTION_SH = ROOT / 'devices/marble/runtime/motion/start-motion-m4-marble.sh'
MSDP_JSON = ROOT / 'devices/marble/runtime/motion/msdp-marble.json'
ANCO_CFG = ROOT / 'devices/marble/runtime/anco/init-m4-anco-marble.cfg'
ANCO_SH = ROOT / 'devices/marble/runtime/anco/start-anco-m4-marble.sh'

# Immutable non-M4 ramdisk members preserved byte-for-byte, including libhmos_ram_no_tee.so (EL2 unencrypted safety).
GUARD_MEMBERS = (
    PREFIX + 'graphics/libhmos_ram_no_tee.so',
    PREFIX + 'graphics/init-modem-f12-marble.cfg',
    PREFIX + 'graphics/hmos_ims_core_service',
    PREFIX + 'graphics/marble_modem_probe',
    PREFIX + 'graphics/libril_vendor_qmi.z.so',
    PREFIX + 'graphics/start-modem-f7-marble.sh',
    PREFIX + 'graphics/display_manager_config.xml',
    PREFIX + 'graphics/libdisplay_composer_vdi_impl.z.so',
    PREFIX + 'graphics/libdisplayengine_module.so',
    PREFIX + 'graphics/sensors-marble.json',
    PREFIX + 'graphics/sensor_hdi_publisher',
    PREFIX + 'graphics/native_sensor_client_probe',
    PREFIX + 'graphics/brightness_multiple_sensor_config-marble.json',
    PREFIX + 'graphics/libmarble_vibrator_interface_service_2.0.z.so',
    PREFIX + 'graphics/native_vibrator_hdi_probe',
    PREFIX + 'graphics/init-m1-vibrator-marble.cfg',
    PREFIX + 'graphics/start-vibrator-m1-marble.sh',
    PREFIX + 'graphics/marble_touch_wakeup',
    PREFIX + 'graphics/init-m1-touch-wakeup-marble.cfg',
    PREFIX + 'graphics/libmarble_camera_host_service_1.2.z.so',
    PREFIX + 'graphics/libmarble_camera_vdi_impl.z.so',
    PREFIX + 'graphics/marble_camera_hw_capture',
    PREFIX + 'graphics/native_camera_hdi_probe',
    PREFIX + 'graphics/ohcamera_fwk_probe',
    PREFIX + 'graphics/start-camera-g2-marble.sh',
    PREFIX + 'graphics/native_battery_hdi_publisher_v131',
    PREFIX + 'graphics/native_thermal_launcher_marble',
    PREFIX + 'graphics/marble-mi-thermal.tar.gz',
    PREFIX + 'graphics/init-m4-mi-thermal-marble.cfg',
    PREFIX + 'graphics/start-mi-thermal-marble.sh',
    PREFIX + 'graphics/init-m4-softap-marble.cfg',
    PREFIX + 'graphics/hostapd-marble.conf',
    PREFIX + 'graphics/start-softap-m4-marble.sh',
    PREFIX + 'graphics/libOMX_Core.z.so',
    PREFIX + 'graphics/libuser_auth_interface_service_4.1.z.so',
    PREFIX + 'graphics/useriam-core.json',
    PREFIX + 'graphics/libpin_auth_driver.z.so',
    PREFIX + 'graphics/libpin_auth_interface_service_3.0.z.so',
    PREFIX + 'graphics/init-m3-pinauth-marble.cfg',
    PREFIX + 'graphics/libmarble_gnss_hdi_driver.z.so',
    PREFIX + 'graphics/native_gnss_hdi_publisher',
    PREFIX + 'graphics/libhmos_audio_vdi.so',
    PREFIX + 'graphics/audio-runtime/lib64/libhmos_pal_server.so',
    PREFIX + 'graphics/libhuks_hdi_service_1.1.z.so',
    PREFIX + 'graphics/libhuks_engine_core_standard.z.so',
    PREFIX + 'graphics/libdbkeys.so',
    PREFIX + 'graphics/libmarble_nfc_interface_service_1.1.z.so',
    PREFIX + 'graphics/libmarble_nci_native_vendor.z.so',
)

STAGED_REPLACE = (
    ('graphics/libsensor_interface_service_3.1.z.so', M4_BUILD / 'sensors/libsensor_interface_service_3.1.z.so', 0o644),
    ('graphics/init-m4-anco-marble.cfg', ANCO_CFG, 0o644),
    ('graphics/start-anco-m4-marble.sh', ANCO_SH, 0o755),
)

STAGED_ADD = (
    ('graphics/libmarble_motion_interface_service_1.1.z.so', M4_BUILD / 'motion/libmarble_motion_interface_service_1.1.z.so', 0o644),
    ('graphics/native_motion_hdi_probe', M4_BUILD / 'motion/native_motion_hdi_probe', 0o755),
    ('graphics/msdp-marble.json', MSDP_JSON, 0o644),
    ('graphics/init-m4-motion-marble.cfg', MOTION_CFG, 0o644),
    ('graphics/start-motion-m4-marble.sh', MOTION_SH, 0o755),
)

INIT_IMP_OLD = b'    "/compat/etc/init-m4-anco-marble.cfg"\n  ],'
INIT_IMP_NEW = (
    b'    "/compat/etc/init-m4-anco-marble.cfg",\n'
    b'    "/compat/etc/init-m4-motion-marble.cfg"\n  ],'
)

STAGE_ANCHOR = b'$B test -x "$R/compat/bin/init"\necho MARBLE_PHASE4_RAM_COMPAT_READY'
STAGE_M4_BLOCK = (
    b'# === MARBLE_M4_MOTION_BRIGHT_STAGE_BEGIN ===\n'
    b'$B mkdir -p /compat/bin "$R/compat/bin" "$R/compat/lib64" "$R/compat/etc" "$R/data/log/brightness_wrapper"\n'
    b'$B chown 5528:5528 "$R/data/log/brightness_wrapper" 2>/dev/null || true\n'
    b'$B chmod 0770 "$R/data/log/brightness_wrapper" 2>/dev/null || true\n'
    b'if $B test -f /graphics/libmarble_motion_interface_service_1.1.z.so; then\n'
    b' $B cp /graphics/libmarble_motion_interface_service_1.1.z.so "$R/compat/lib64/libmarble_motion_interface_service_1.1.z.so"\n'
    b' $B chmod 0644 "$R/compat/lib64/libmarble_motion_interface_service_1.1.z.so"\n'
    b' if $B test -f "$R/vendor/lib64/libmotion_interface_service_1.1.z.so"; then\n'
    b'  if ! $B grep -q " $R/vendor/lib64/libmotion_interface_service_1.1.z.so " /proc/mounts 2>/dev/null; then\n'
    b'   $B mount --bind "$R/compat/lib64/libmarble_motion_interface_service_1.1.z.so" "$R/vendor/lib64/libmotion_interface_service_1.1.z.so" 2>/dev/null || true\n'
    b'  fi\n'
    b' fi\n'
    b'fi\n'
    b'if $B test -f /graphics/msdp-marble.json; then\n'
    b' $B cp /graphics/msdp-marble.json "$R/system/profile/msdp.json"\n'
    b' $B chmod 0644 "$R/system/profile/msdp.json"\n'
    b'fi\n'
    b'if $B test -f /graphics/init-m4-motion-marble.cfg; then\n'
    b' $B cp /graphics/init-m4-motion-marble.cfg "$R/compat/etc/init-m4-motion-marble.cfg"\n'
    b' $B chmod 0644 "$R/compat/etc/init-m4-motion-marble.cfg"\n'
    b'fi\n'
    b'for m4bin in native_motion_hdi_probe start-motion-m4-marble.sh; do\n'
    b' if $B test -f "/graphics/$m4bin"; then\n'
    b'  $B cp "/graphics/$m4bin" "/compat/bin/$m4bin"\n'
    b'  $B cp "/graphics/$m4bin" "$R/compat/bin/$m4bin"\n'
    b'  $B chmod 0755 "/compat/bin/$m4bin" "$R/compat/bin/$m4bin"\n'
    b' fi\n'
    b'done\n'
    b'echo "MARBLE_M4_MOTION_BRIGHT_STAGED=1"\n'
    b'# === MARBLE_M4_MOTION_BRIGHT_STAGE_END ===\n'
)

START_ANCHOR = b'pset marble.debug.start.uideps true'
START_M4_BLOCK = (
    b'# === MARBLE_M4_MOTION_START_BEGIN ===\n'
    b'motion_skip=$(pget marble.debug.skip.motion)\n'
    b'motion_skip_list=$($B cat "$R/data/local/tmp/marble-skip-stages" 2>/dev/null | $B tr -d \' \\t\\r\\n\' || true)\n'
    b'case ",$motion_skip_list," in\n'
    b' *,motion,*|*,motion_hdi,*|*,pickup,*) motion_skip=true ;;\n'
    b'esac\n'
    b'if [ "$motion_skip" = true ] || [ "$motion_skip" = 1 ]; then\n'
    b' log "M4_MOTION_SKIP=requested"\n'
    b'else\n'
    b' pset marble.motion.ready true\n'
    b' log "M4_MOTION_TRIGGERED=1"\n'
    b'fi\n'
    b'# === MARBLE_M4_MOTION_START_END ===\n'
)

POSTBOOT_ANCHOR = b'say "U2_GNSS_MODE=hdf_location_host"'
POSTBOOT_M4_BLOCK = (
    b'# Step P: Batch M4 Raise-to-Wake Motion HDI v1.1 (motion_host host-18) verification.\n'
    b'if skipped motion || skipped motion_hdi || skipped pickup; then\n'
    b'  say "U2_MOTION_M4_SKIP=requested"\n'
    b'elif [ -x /compat/bin/start-motion-m4-marble.sh ]; then\n'
    b'  $B sh /compat/bin/start-motion-m4-marble.sh || say "U2_MOTION_M4_RC=$?"\n'
    b'else\n'
    b'  say "U2_MOTION_M4_SKIP=script_missing"\n'
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
    require(b'init-m4-motion-marble.cfg' not in raw, 'hmos-init-debug.cfg already contains init-m4-motion-marble.cfg')
    out = splice_once(raw, ((INIT_IMP_OLD, INIT_IMP_NEW),), INIT_DEBUG_MEMBER)
    doc = json.loads(out.decode('utf-8'))
    require('/compat/etc/init-m4-motion-marble.cfg' in doc.get('import', []), 'Missing M4 motion init import')
    require(len(out) < 32768, f'hmos-init-debug.cfg exceeds 32768B cap: {len(out)}')
    return out


def patch_init_platform_msdp(raw: bytes) -> bytes:
    doc = json.loads(raw.decode('utf-8'))
    found = False
    for svc in doc.get('services', []):
        if svc.get('name') == 'msdp':
            found = True
            perms = svc.setdefault('permission', [])
            for perm in (
                'ohos.permission.GYROSCOPE',
                'ohos.permission.ACTIVITY_MOTION',
                'ohos.permission.POWER_OPTIMIZATION',
            ):
                if perm not in perms:
                    perms.append(perm)
    require(found, 'msdp service not found in init-platform-marble.cfg')
    out = (json.dumps(doc, indent=2) + '\n').encode('utf-8')
    # Verify JSON valid and contains added perms.
    check = json.loads(out.decode('utf-8'))
    msdp = next(s for s in check['services'] if s['name'] == 'msdp')
    require('ohos.permission.ACTIVITY_MOTION' in msdp['permission'], 'Missing ACTIVITY_MOTION in patched msdp')
    require('ohos.permission.GYROSCOPE' in msdp['permission'], 'Missing GYROSCOPE in patched msdp')
    return out


def patch_stage(raw: bytes) -> bytes:
    for m in (b'MARBLE_M4_MOTION_BRIGHT_STAGE_BEGIN', b'libmarble_motion_interface_service_1.1.z.so'):
        require(m not in raw, f'stage script already contains {m!r}')
    require(b'MARBLE_M3_PINAUTH_ANCO_STAGE_BEGIN' in raw, 'Expected M3 stage marker')
    out = splice_once(raw, ((STAGE_ANCHOR, STAGE_M4_BLOCK + STAGE_ANCHOR),), STAGE_MEMBER)
    require(b'\r' not in out, 'CR byte in patched stage-phase4-marble.sh')
    subprocess.run(['sh', '-n'], input=out, check=True)
    return out


def patch_start(raw: bytes) -> bytes:
    for m in (b'MARBLE_M4_MOTION_START_BEGIN', b'marble.motion.ready'):
        require(m not in raw, f'start script already contains {m!r}')
    out = splice_once(raw, ((START_ANCHOR, START_M4_BLOCK + START_ANCHOR),), START_MEMBER)
    require(b'\r' not in out, 'CR byte in patched start-phase4-marble.sh')
    subprocess.run(['sh', '-n'], input=out, check=True)
    return out


def patch_postboot(raw: bytes) -> bytes:
    for m in (b'start-motion-m4-marble.sh', b'U2_MOTION_M4_SKIP'):
        require(m not in raw, f'postboot script already contains {m!r}')
    require(b'start-anco-m4-marble.sh' in raw, 'Expected M3 postboot call')
    out = splice_once(raw, ((POSTBOOT_ANCHOR, POSTBOOT_M4_BLOCK + POSTBOOT_ANCHOR),), POSTBOOT_MEMBER)
    require(b'\r' not in out, 'CR byte in patched start-u2-postboot-marble.sh')
    subprocess.run(['sh', '-n'], input=out, check=True)
    return out


def validate_inputs() -> None:
    for rel, src, _mode in STAGED_REPLACE + STAGED_ADD:
        require(src.is_file(), f'Missing staged M4 artifact: {src}')
        raw = src.read_bytes()
        if rel.endswith('.json'):
            doc = json.loads(raw.decode('utf-8'))
            sas = {sa['name']: sa['libpath'] for sa in doc.get('systemability', [])}
            require(
                sas.get(2901) == 'libmotion_service.z.so'
                and sas.get(2902) == 'libdevicestatus_service.z.so'
                and sas.get(2907) == 'libuser_status_service.z.so',
                f'Unexpected msdp-marble.json SAs: {sas}',
            )
        elif rel.endswith('.cfg'):
            doc = json.loads(raw.decode('utf-8'))
            require('services' in doc and 'jobs' in doc, f'Invalid cfg: {rel}')
            require(len(raw) < 32768, f'cfg exceeds 32768B cap: {rel} ({len(raw)})')
            if rel.endswith('init-m4-motion-marble.cfg'):
                svcs = {s['name']: s for s in doc['services']}
                require(svcs['motion_host']['path'][2] == '18', 'motion_host must use host index -i 18')
            elif rel.endswith('init-m4-anco-marble.cfg'):
                svcs = {s['name']: s for s in doc['services']}
                require('updater_sa' in svcs and 'broker' in svcs and 'bms_broker' in svcs,
                        'init-m4-anco-marble.cfg missing updater_sa/broker/bms_broker')
        elif rel.endswith('.sh'):
            require(b'\r' not in raw, f'CR byte in {rel}')
            subprocess.run(['sh', '-n'], input=raw, check=True)
        else:
            require(raw[:4] == b'\x7fELF', f'Expected ELF for {rel}')
            if LLVM_READELF.is_file():
                dyn = subprocess.check_output([str(LLVM_READELF), '-d', str(src)], text=True)
                soname = re.findall(r'\(SONAME\)\s+Library soname:\s+\[([^\]]+)\]', dyn)
                if rel.endswith('libmarble_motion_interface_service_1.1.z.so'):
                    require(soname == ['libmotion_interface_service_1.1.z.so'], f'Unexpected Motion SONAME: {soname}')
                elif rel.endswith('libsensor_interface_service_3.1.z.so'):
                    require(soname == ['libsensor_interface_service_3.1.z.so'], f'Unexpected Sensor SONAME: {soname}')
            if LLVM_NM.is_file():
                if rel.endswith('libmarble_motion_interface_service_1.1.z.so'):
                    nm = subprocess.check_output([str(LLVM_NM), '-D', str(src)], text=True)
                    require(' MotionInterfaceImplGetInstance' in nm, 'Missing MotionInterfaceImplGetInstance')
                elif rel.endswith('libsensor_interface_service_3.1.z.so'):
                    nm = subprocess.check_output([str(LLVM_NM), '-D', str(src)], text=True)
                    require(' SensorInterfaceImplGetInstance' in nm, 'Missing SensorInterfaceImplGetInstance')


def load_oracle():
    require(ORACLE.is_file() and ORACLE.resolve() == ORACLE, f'Missing oracle: {ORACLE}')
    got_sha = hashlib.sha256(ORACLE.read_bytes()).hexdigest()
    require(got_sha == ORACLE_SHA, f'Oracle SHA256 mismatch: {got_sha}')
    spec = importlib.util.spec_from_file_location('archive_oracle_m4', ORACLE)
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
    M4_BUILD.mkdir(parents=True, exist_ok=True)
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
        vibrator_host=True,
        hostapd_donor=DONOR_FS,
        pin_auth_host=True,
    )
    expected_base_hcb = phase4.build_phase4_hdf_hcb(**hcb_kwargs, motion_host=False)
    new_hcb = phase4.build_phase4_hdf_hcb(**hcb_kwargs, motion_host=True)
    require(b'motion_host' in new_hcb and b'libmotion_driver.z.so' in new_hcb and b'motion_interface_service' in new_hcb,
            'Generated M4 HCB missing motion_host declaration')

    with tempfile.TemporaryDirectory(prefix='h20_m4_', dir=str(M4_BUILD)) as tmp:
        scratch = Path(tmp)
        old, old_raw, kind, old_rd, args = r.extract_entries(
            base_path, base_bytes, before_layout, scratch / 'before'
        )
        old_manifest = archive.parse(old[r.MANIFEST][1])
        r.manifest_holds(old, old_manifest)

        require(old[HCB_MEMBER][1] == expected_base_hcb,
                'Base hdf_default_phase4.hcb does not match expected M3 18-host HCB')
        for member in GUARD_MEMBERS:
            require(member in old, f'Missing guarded member in base image: {member}')

        expected = dict(old)
        new_manifest = copy.deepcopy(old_manifest)
        modified_members = {
            r.MANIFEST,
            HCB_MEMBER,
            INIT_DEBUG_MEMBER,
            INIT_PLATFORM_MEMBER,
            STAGE_MEMBER,
            START_MEMBER,
            POSTBOOT_MEMBER,
        }
        staged_report = {}

        r.set_entry(expected, HCB_MEMBER, new_hcb)
        update_manifest_entry(new_manifest, HCB_MEMBER, new_hcb, 0o644, r.sha)

        new_init_debug = patch_init_debug(old[INIT_DEBUG_MEMBER][1])
        r.set_entry(expected, INIT_DEBUG_MEMBER, new_init_debug)
        if 'hmos-init-debug.cfg' in new_manifest:
            new_manifest['hmos-init-debug.cfg'].update(bytes=len(new_init_debug), sha256=r.sha(new_init_debug))

        new_init_platform = patch_init_platform_msdp(old[INIT_PLATFORM_MEMBER][1])
        r.set_entry(expected, INIT_PLATFORM_MEMBER, new_init_platform)
        update_manifest_entry(new_manifest, INIT_PLATFORM_MEMBER, new_init_platform, 0o644, r.sha)

        new_stage = patch_stage(old[STAGE_MEMBER][1])
        r.set_entry(expected, STAGE_MEMBER, new_stage)
        update_manifest_entry(new_manifest, STAGE_MEMBER, new_stage, 0o755, r.sha)

        new_start = patch_start(old[START_MEMBER][1])
        r.set_entry(expected, START_MEMBER, new_start)
        update_manifest_entry(new_manifest, START_MEMBER, new_start, 0o755, r.sha)

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
            require(member not in old, f'New M4 member already exists in base image: {member}')
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

        changed = {k for k in got if k not in old or got[k][1] != old[k][1]}
        removed = set(old) - set(got)
        require(not removed, f'Unexpected removed members: {sorted(removed)}')
        require(changed == modified_members, f'Unexpected modified member set diff: {sorted(changed ^ modified_members)}')

        old_spans, old_trailer = archive.record_spans(old_raw)
        got_spans, got_trailer = archive.record_spans(got_raw)
        require(got_trailer == old_trailer, 'CPIO trailer changed')
        unchanged = set(old) - modified_members
        require(
            all(got_raw[slice(*got_spans[n])] == old_raw[slice(*old_spans[n])] for n in unchanged),
            'Unrelated CPIO record changed',
        )

        cand_sha = r.sha(cand_bytes)
        out_path = out_dir / f'vendor_boot_h20_m4_bright_motion_anco_{cand_sha[:8]}.img'
        out_path.write_bytes(cand_bytes)
        require(len(cand_bytes) == len(base_bytes) == 100663296, f'Size mismatch: {len(cand_bytes)}')
        report = {
            'batch': 'M4',
            'description': 'Auto-Brightness ALS (tcs3701 on-change mode=2/minDelay=0/lastEvent replay) + Raise-to-Wake (motion_host host-18 HDI v1.1 + SA 2901 msdp) + DroiTong ANCO (updater_sa SA 3006 + broker SA 66818 + cgroups + proper ondemand)',
            'base': {'path': str(base_path), 'bytes': len(base_bytes), 'sha256': base_sha},
            'candidate': {'path': str(out_path), 'bytes': len(cand_bytes), 'sha256': cand_sha},
            'entries_before': len(old),
            'entries_after': len(got),
            'hcb_hosts_before': 18,
            'hcb_hosts_after': 19,
            'staged': staged_report,
            'avb_before': {k: v.hex() if isinstance(v, bytes) else v for k, v in before_avb.items()},
            'avb_after': {k: v.hex() if isinstance(v, bytes) else v for k, v in after_avb.items()},
        }
        (M4_BUILD / 'm4-build-report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
        print(f'M4_BUILD_OK candidate={out_path} sha256={cand_sha} entries={len(old)}->{len(got)}')
        return out_path


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument('--base', type=Path, default=BASE_DEFAULT)
    ap.add_argument('--out-dir', type=Path, default=Path('/mnt/e/marble-tmp'))
    args = ap.parse_args()
    build(args.base, args.out_dir)


if __name__ == '__main__':
    main()
