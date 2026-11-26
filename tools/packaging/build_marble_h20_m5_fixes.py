#!/usr/bin/env python3
"""Package Batch M5 (PIN 96B Auth + Vibrator Early Boot/Haptics Tuning + DT2W Auto-Mknod + Raise-to-Wake DPMS/Reset + IMS 3GPP2->3GPP SMS & HD ImsCall + Camera Full-FOV 4624x3472 Fast-NV21/CDAF + AOD SA 65840 & Reminder SA 3204 for LiveView) onto M4c vendor_boot_a."""
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
M5_BUILD = ROOT / 'work/marble-m5-build'
LLVM_READELF = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-readelf'
LLVM_NM = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-nm'

ORACLE = ROOT / 'work/marble-m15-role-storage.1v2ynS/build_m15_role_storage.py'
ORACLE_SHA = '7d4f79857a3458ffdb7c8fb86b1cfd893fd9addb02b13e7e05d7b5fa7093cfaa'

BASE_DEFAULT = Path('/mnt/e/marble-tmp/vendor_boot_h20_m4_bright_motion_anco_dcd8797f.img')
BASE_SHA = 'dcd8797f4542aa8543e4f6c34fbc545bd5719826c3c523020ca0aea3a35ddab8'

PREFIX = 'hmos-compat-marble/'
INIT_DEBUG_MEMBER = PREFIX + 'hmos-init-debug.cfg'
FOUNDATION_MEMBER = PREFIX + 'graphics/foundation-app-core.json'
STAGE_MEMBER = PREFIX + 'graphics/stage-phase4-marble.sh'
START_MEMBER = PREFIX + 'graphics/start-phase4-marble.sh'

TOUCH_CFG = ROOT / 'devices/marble/runtime/touch/init-m1-touch-wakeup-marble.cfg'
AOD_CFG = ROOT / 'devices/marble/runtime/aod/init-m5-aod-marble.cfg'
AOD_JSON = ROOT / 'devices/marble/runtime/aod/aod-marble.json'

# Immutable non-M5 ramdisk members preserved byte-for-byte, including libhmos_ram_no_tee.so (EL2 unencrypted safety)
# and the 19-host hdf_default_phase4.hcb.
GUARD_MEMBERS = (
    PREFIX + 'graphics/libhmos_ram_no_tee.so',
    PREFIX + 'graphics/hdf_default_phase4.hcb',
    PREFIX + 'graphics/init-modem-f12-marble.cfg',
    PREFIX + 'graphics/marble_modem_probe',
    PREFIX + 'graphics/start-modem-f7-marble.sh',
    PREFIX + 'graphics/display_manager_config.xml',
    PREFIX + 'graphics/libdisplay_composer_vdi_impl.z.so',
    PREFIX + 'graphics/libdisplayengine_module.so',
    PREFIX + 'graphics/libsensor_interface_service_3.1.z.so',
    PREFIX + 'graphics/sensors-marble.json',
    PREFIX + 'graphics/sensor_hdi_publisher',
    PREFIX + 'graphics/native_sensor_client_probe',
    PREFIX + 'graphics/brightness_multiple_sensor_config-marble.json',
    PREFIX + 'graphics/init-m1-vibrator-marble.cfg',
    PREFIX + 'graphics/start-vibrator-m1-marble.sh',
    PREFIX + 'graphics/native_vibrator_hdi_probe',
    PREFIX + 'graphics/libmarble_camera_host_service_1.2.z.so',
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
    PREFIX + 'graphics/init-m3-pinauth-marble.cfg',
    PREFIX + 'graphics/init-m4-anco-marble.cfg',
    PREFIX + 'graphics/start-anco-m4-marble.sh',
    PREFIX + 'graphics/init-m4-motion-marble.cfg',
    PREFIX + 'graphics/start-motion-m4-marble.sh',
    PREFIX + 'graphics/native_motion_hdi_probe',
    PREFIX + 'graphics/msdp-marble.json',
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
    ('graphics/libpin_auth_interface_service_3.0.z.so', M5_BUILD / 'pinauth/libpin_auth_interface_service_3.0.z.so', 0o644),
    ('graphics/libpin_auth_driver.z.so', M5_BUILD / 'pinauth/libpin_auth_driver.z.so', 0o644),
    ('graphics/marble_touch_wakeup', M5_BUILD / 'touch/marble_touch_wakeup', 0o755),
    ('graphics/init-m1-touch-wakeup-marble.cfg', TOUCH_CFG, 0o644),
    ('graphics/libmarble_motion_interface_service_1.1.z.so', M5_BUILD / 'motion/libmarble_motion_interface_service_1.1.z.so', 0o644),
    ('graphics/libmarble_vibrator_interface_service_2.0.z.so', M5_BUILD / 'vibrator/libmarble_vibrator_interface_service_2.0.z.so', 0o644),
    ('graphics/hmos_ims_core_service', M5_BUILD / 'telephony/hmos_ims_core_service', 0o755),
    ('graphics/libril_vendor_qmi.z.so', M5_BUILD / 'telephony/libril_vendor_qmi.z.so', 0o644),
    ('graphics/libmarble_camera_vdi_impl.z.so', M5_BUILD / 'camera/libmarble_camera_vdi_impl.z.so', 0o644),
    ('graphics/marble_camera_hw_capture', M5_BUILD / 'camera/marble_camera_hw_capture', 0o755),
)

STAGED_ADD = (
    ('graphics/init-m5-aod-marble.cfg', AOD_CFG, 0o644),
    ('graphics/aod-marble.json', AOD_JSON, 0o644),
)

INIT_IMP_OLD = b'    "/compat/etc/init-m4-motion-marble.cfg"\n  ],'
INIT_IMP_NEW = (
    b'    "/compat/etc/init-m4-motion-marble.cfg",\n'
    b'    "/compat/etc/init-m5-aod-marble.cfg"\n  ],'
)

STAGE_ANCHOR = b'$B test -x "$R/compat/bin/init"\necho MARBLE_PHASE4_RAM_COMPAT_READY'
STAGE_M5_BLOCK = (
    b'# === MARBLE_M5_AOD_STAGE_BEGIN ===\n'
    b'$B mkdir -p "$R/compat/etc" "$R/system/profile"\n'
    b'if $B test -f /graphics/init-m5-aod-marble.cfg; then\n'
    b' $B cp /graphics/init-m5-aod-marble.cfg "$R/compat/etc/init-m5-aod-marble.cfg"\n'
    b' $B chmod 0644 "$R/compat/etc/init-m5-aod-marble.cfg"\n'
    b'fi\n'
    b'if $B test -f /graphics/aod-marble.json; then\n'
    b' $B cp /graphics/aod-marble.json "$R/compat/etc/aod-marble.json"\n'
    b' $B cp /graphics/aod-marble.json "$R/system/profile/aod_service.json"\n'
    b' $B chmod 0644 "$R/compat/etc/aod-marble.json" "$R/system/profile/aod_service.json"\n'
    b'fi\n'
    b'echo "MARBLE_M5_AOD_STAGED=1"\n'
    b'# === MARBLE_M5_AOD_STAGE_END ===\n'
)

START_SENSORS_ANCHOR = b'if $B test -f /graphics/start-sensors-marble.sh; then'
START_M5_VIBRATOR_EARLY_BLOCK = (
    b'# === MARBLE_M5_VIBRATOR_EARLY_BEGIN ===\n'
    b'vib_skip=$(pget marble.debug.skip.vibrator)\n'
    b'vib_skip_list=$($B cat "$R/data/local/tmp/marble-skip-stages" 2>/dev/null | $B tr -d \' \\t\\r\\n\' || true)\n'
    b'case ",$vib_skip_list," in\n'
    b' *,vibrator,*|*,haptics,*|*,vibrator_hdi,*) vib_skip=true ;;\n'
    b'esac\n'
    b'if [ "$vib_skip" = true ] || [ "$vib_skip" = 1 ]; then\n'
    b' log "M5_VIBRATOR_EARLY_SKIP=requested"\n'
    b'elif $B test -x /compat/bin/start-vibrator-m1-marble.sh; then\n'
    b' $B timeout 12 $B sh /compat/bin/start-vibrator-m1-marble.sh > "$R/data/local/tmp/marble-vibrator-startup.log" 2>&1 </dev/null || true\n'
    b' $B sleep 1\n'
    b' log "M5_VIBRATOR_EARLY_DONE=1"\n'
    b'fi\n'
    b'# === MARBLE_M5_VIBRATOR_EARLY_END ===\n'
)

START_INPUT_OLD = (
    b'stage=hardware_input\n'
    b'for ev in /sys/class/input/event*; do\n'
    b' [ -r "$ev/dev" ] || continue\n'
    b' base=${ev##*/}\n'
    b' nums=$($B cat "$ev/dev")\n'
    b' $B test -e "$R/dev/input/$base" || $B mknod "$R/dev/input/$base" c "${nums%:*}" "${nums#*:}"\n'
    b' $B chown 6696:6696 "$R/dev/input/$base"\n'
    b' $B chmod 0660 "$R/dev/input/$base"\n'
    b' ev_name=$($B cat "$ev/device/name" 2>/dev/null || echo unknown)\n'
    b' log "INPUT_READY node=$base dev=$nums name=$ev_name"\n'
    b'done'
)

START_INPUT_NEW = (
    b'stage=hardware_input\n'
    b'# === MARBLE_M5_TOUCH_WAKEUP_EARLY_BEGIN ===\n'
    b'dt2w_skip=$(pget marble.debug.skip.touch_wakeup)\n'
    b'dt2w_skip_list=$($B cat "$R/data/local/tmp/marble-skip-stages" 2>/dev/null | $B tr -d \' \\t\\r\\n\' || true)\n'
    b'case ",$dt2w_skip_list," in\n'
    b' *,touch_wakeup,*|*,dt2w,*) dt2w_skip=true ;;\n'
    b'esac\n'
    b'if [ "$dt2w_skip" != true ] && [ "$dt2w_skip" != 1 ]; then\n'
    b' pset marble.touch_wakeup.ready true\n'
    b' tw_i=0\n'
    b' while [ "$tw_i" -lt 10 ]; do\n'
    b'  if $B grep -q "^marble-touch-wakeup$" /sys/class/input/event*/device/name 2>/dev/null; then\n'
    b'   break\n'
    b'  fi\n'
    b'  tw_i=$((tw_i + 1))\n'
    b'  $B sleep 0.2\n'
    b' done\n'
    b' log "M5_TOUCH_WAKEUP_EARLY_DONE wait_steps=$tw_i"\n'
    b'fi\n'
    b'# === MARBLE_M5_TOUCH_WAKEUP_EARLY_END ===\n'
    b'for ev in /sys/class/input/event*; do\n'
    b' [ -r "$ev/dev" ] || continue\n'
    b' base=${ev##*/}\n'
    b' nums=$($B cat "$ev/dev")\n'
    b' ev_name=$($B cat "$ev/device/name" 2>/dev/null || echo unknown)\n'
    b' ev_lc=$(echo "$ev_name" | $B tr \'A-Z\' \'a-z\')\n'
    b' $B test -e "$R/dev/input/$base" || $B mknod "$R/dev/input/$base" c "${nums%:*}" "${nums#*:}"\n'
    b' case "$ev_lc" in\n'
    b'  *haptics*|*vibrator*) $B chown 3034:3034 "$R/dev/input/$base" ;;\n'
    b'  *) $B chown 6696:6696 "$R/dev/input/$base" ;;\n'
    b' esac\n'
    b' $B chmod 0660 "$R/dev/input/$base"\n'
    b' log "INPUT_READY node=$base dev=$nums name=$ev_name"\n'
    b'done'
)

START_AOD_ANCHOR = b'pset marble.debug.start.ime true'
START_M5_AOD_BLOCK = (
    b'\n# === MARBLE_M5_AOD_START_BEGIN ===\n'
    b'aod_skip=$(pget marble.debug.skip.aod)\n'
    b'aod_skip_list=$($B cat "$R/data/local/tmp/marble-skip-stages" 2>/dev/null | $B tr -d \' \\t\\r\\n\' || true)\n'
    b'case ",$aod_skip_list," in\n'
    b' *,aod,*|*,aod_service,*) aod_skip=true ;;\n'
    b'esac\n'
    b'if [ "$aod_skip" = true ] || [ "$aod_skip" = 1 ]; then\n'
    b' log "M5_AOD_SKIP=requested"\n'
    b'else\n'
    b' pset marble.debug.start.aod true\n'
    b' wait_sa 65840 10 || true\n'
    b' log "M5_AOD_TRIGGERED=1"\n'
    b'fi\n'
    b'# === MARBLE_M5_AOD_START_END ==='
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
    require(b'init-m5-aod-marble.cfg' not in raw, 'hmos-init-debug.cfg already contains init-m5-aod-marble.cfg')
    out = splice_once(raw, ((INIT_IMP_OLD, INIT_IMP_NEW),), INIT_DEBUG_MEMBER)
    doc = json.loads(out.decode('utf-8'))
    require('/compat/etc/init-m5-aod-marble.cfg' in doc.get('import', []), 'Missing M5 AOD init import')
    require(len(out) < 32768, f'hmos-init-debug.cfg exceeds 32768B cap: {len(out)}')
    return out


def patch_foundation_reminder(raw: bytes) -> bytes:
    doc = json.loads(raw.decode('utf-8'))
    sas = doc.get('systemability', [])
    existing = {sa.get('name'): sa for sa in sas}
    require(3203 in existing, 'foundation-app-core.json missing SA 3203 (libans.z.so)')
    if 3204 in existing:
        existing[3204]['libpath'] = 'libreminder.z.so'
        existing[3204]['run-on-create'] = True
    else:
        sas.append({
            'name': 3204,
            'libpath': 'libreminder.z.so',
            'run-on-create': True,
            'distributed': False,
            'dump_level': 1,
            'min_hdi_proxy_version': [],
        })
    out = (json.dumps(doc, indent=2) + '\n').encode('utf-8')
    check = json.loads(out.decode('utf-8'))
    sa3204 = next((s for s in check['systemability'] if s['name'] == 3204), None)
    require(sa3204 is not None and sa3204['libpath'] == 'libreminder.z.so' and sa3204['run-on-create'] is True,
            'Patched foundation-app-core.json missing SA 3204 run-on-create=true')
    return out


def patch_stage(raw: bytes) -> bytes:
    for m in (b'MARBLE_M5_AOD_STAGE_BEGIN', b'init-m5-aod-marble.cfg'):
        require(m not in raw, f'stage script already contains {m!r}')
    require(b'MARBLE_M4_MOTION_BRIGHT_STAGE_BEGIN' in raw, 'Expected M4 stage marker')
    out = splice_once(raw, ((STAGE_ANCHOR, STAGE_M5_BLOCK + STAGE_ANCHOR),), STAGE_MEMBER)
    require(b'\r' not in out, 'CR byte in patched stage-phase4-marble.sh')
    subprocess.run(['sh', '-n'], input=out, check=True)
    return out


def patch_start(raw: bytes) -> bytes:
    for m in (b'MARBLE_M5_VIBRATOR_EARLY_BEGIN', b'MARBLE_M5_TOUCH_WAKEUP_EARLY_BEGIN', b'MARBLE_M5_AOD_START_BEGIN'):
        require(m not in raw, f'start script already contains {m!r}')
    out = splice_once(
        raw,
        (
            (START_SENSORS_ANCHOR, START_M5_VIBRATOR_EARLY_BLOCK + START_SENSORS_ANCHOR),
            (START_INPUT_OLD, START_INPUT_NEW),
            (START_AOD_ANCHOR, START_AOD_ANCHOR + START_M5_AOD_BLOCK),
        ),
        START_MEMBER,
    )
    require(b'\r' not in out, 'CR byte in patched start-phase4-marble.sh')
    subprocess.run(['sh', '-n'], input=out, check=True)
    return out


def validate_inputs() -> None:
    for rel, src, _mode in STAGED_REPLACE + STAGED_ADD:
        require(src.is_file(), f'Missing staged M5 artifact: {src}')
        raw = src.read_bytes()
        if rel.endswith('.json'):
            doc = json.loads(raw.decode('utf-8'))
            sas = {sa['name']: sa['libpath'] for sa in doc.get('systemability', [])}
            require(sas.get(65840) == 'libaod_service.z.so', f'Unexpected aod-marble.json SAs: {sas}')
        elif rel.endswith('.cfg'):
            doc = json.loads(raw.decode('utf-8'))
            require('services' in doc and 'jobs' in doc, f'Invalid cfg: {rel}')
            require(len(raw) < 32768, f'cfg exceeds 32768B cap: {rel} ({len(raw)})')
            svcs = {s['name']: s for s in doc['services']}
            if rel.endswith('init-m5-aod-marble.cfg'):
                require('aod_service' in svcs and svcs['aod_service']['uid'] == 'aod', 'Invalid aod_service cfg')
            elif rel.endswith('init-m1-touch-wakeup-marble.cfg'):
                path = svcs['marble_touch_wakeup']['path']
                require('--power-key' in path and '--only-when-off' in path, f'Unexpected touch_wakeup args: {path}')
                caps = set(svcs['marble_touch_wakeup'].get('caps', []))
                require({'MKNOD', 'CHOWN', 'FOWNER'}.issubset(caps), f'Missing touch_wakeup caps: {caps}')
        else:
            require(raw[:4] == b'\x7fELF', f'Expected ELF for {rel}')
            if LLVM_READELF.is_file():
                dyn = subprocess.check_output([str(LLVM_READELF), '-d', str(src)], text=True)
                soname = re.findall(r'\(SONAME\)\s+Library soname:\s+\[([^\]]+)\]', dyn)
                expected_sonames = {
                    'graphics/libpin_auth_interface_service_3.0.z.so': 'libpin_auth_interface_service_3.0.z.so',
                    'graphics/libpin_auth_driver.z.so': 'libpin_auth_driver.z.so',
                    'graphics/libmarble_motion_interface_service_1.1.z.so': 'libmotion_interface_service_1.1.z.so',
                    'graphics/libmarble_vibrator_interface_service_2.0.z.so': 'libvibrator_interface_service_2.0.z.so',
                    'graphics/libril_vendor_qmi.z.so': 'libril_vendor.z.so',
                    'graphics/libmarble_camera_vdi_impl.z.so': 'libmarble_camera_vdi_impl.z.so',
                }
                if rel in expected_sonames:
                    require(soname == [expected_sonames[rel]], f'Unexpected SONAME for {rel}: {soname}')
            if LLVM_NM.is_file():
                nm_checks = {
                    'graphics/libpin_auth_interface_service_3.0.z.so': 'PinAuthInterfaceImplGetInstance',
                    'graphics/libmarble_motion_interface_service_1.1.z.so': 'MotionInterfaceImplGetInstance',
                    'graphics/libmarble_vibrator_interface_service_2.0.z.so': 'VibratorInterfaceImplGetInstance',
                    'graphics/libril_vendor_qmi.z.so': 'RilInitOps',
                }
                if rel in nm_checks:
                    nm = subprocess.check_output([str(LLVM_NM), '-D', str(src)], text=True)
                    require(f' {nm_checks[rel]}' in nm, f'Missing {nm_checks[rel]} in {rel}')


def load_oracle():
    require(ORACLE.is_file() and ORACLE.resolve() == ORACLE, f'Missing oracle: {ORACLE}')
    got_sha = hashlib.sha256(ORACLE.read_bytes()).hexdigest()
    require(got_sha == ORACLE_SHA, f'Oracle SHA256 mismatch: {got_sha}')
    spec = importlib.util.spec_from_file_location('archive_oracle_m5', ORACLE)
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
    M5_BUILD.mkdir(parents=True, exist_ok=True)
    validate_inputs()
    out_dir.mkdir(parents=True, exist_ok=True)

    archive, r = load_oracle()
    base_bytes = r.read_original(base_path)
    base_sha = r.sha(base_bytes)
    require(base_sha == BASE_SHA, f'Base SHA256 mismatch: got {base_sha}, want {BASE_SHA}')

    before_layout = r.layout(base_bytes)
    before_avb = r.verify_existing_avb(base_bytes, before_layout['payload_end'])

    with tempfile.TemporaryDirectory(prefix='h20_m5_', dir=str(M5_BUILD)) as tmp:
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
        modified_members = {
            r.MANIFEST,
            INIT_DEBUG_MEMBER,
            FOUNDATION_MEMBER,
            STAGE_MEMBER,
            START_MEMBER,
        }
        staged_report = {}

        new_init_debug = patch_init_debug(old[INIT_DEBUG_MEMBER][1])
        r.set_entry(expected, INIT_DEBUG_MEMBER, new_init_debug)
        if 'hmos-init-debug.cfg' in new_manifest:
            new_manifest['hmos-init-debug.cfg'].update(bytes=len(new_init_debug), sha256=r.sha(new_init_debug))

        new_foundation = patch_foundation_reminder(old[FOUNDATION_MEMBER][1])
        r.set_entry(expected, FOUNDATION_MEMBER, new_foundation)
        update_manifest_entry(new_manifest, FOUNDATION_MEMBER, new_foundation, 0o644, r.sha)

        new_stage = patch_stage(old[STAGE_MEMBER][1])
        r.set_entry(expected, STAGE_MEMBER, new_stage)
        update_manifest_entry(new_manifest, STAGE_MEMBER, new_stage, 0o755, r.sha)

        new_start = patch_start(old[START_MEMBER][1])
        r.set_entry(expected, START_MEMBER, new_start)
        update_manifest_entry(new_manifest, START_MEMBER, new_start, 0o755, r.sha)

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
            require(member not in old, f'New M5 member already exists in base image: {member}')
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
        out_path = out_dir / f'vendor_boot_h20_m5_fixes_{cand_sha[:8]}.img'
        out_path.write_bytes(cand_bytes)
        require(len(cand_bytes) == len(base_bytes) == 100663296, f'Size mismatch: {len(cand_bytes)}')
        report = {
            'batch': 'M5',
            'description': (
                'PIN 96B auth fix + Vibrator early boot/haptics event6 ownership & LRA tuning + '
                'DT2W auto-mknod/--power-key/--only-when-off + Raise-to-Wake DPMS screen-off & repeat-reset + '
                'IMS 3GPP2->3GPP SMS transcoding & HD ImsCall QMI_VOICE + '
                'Camera OV64B40 full-FOV 4624x3472 direct NV21 & GT9764 CDAF + '
                'AOD SA 65840 & Reminder SA 3204 for LiveView refresh'
            ),
            'base': {'path': str(base_path), 'bytes': len(base_bytes), 'sha256': base_sha},
            'candidate': {'path': str(out_path), 'bytes': len(cand_bytes), 'sha256': cand_sha},
            'entries_before': len(old),
            'entries_after': len(got),
            'hcb_hosts_before': 19,
            'hcb_hosts_after': 19,
            'staged': staged_report,
            'avb_before': {k: v.hex() if isinstance(v, bytes) else v for k, v in before_avb.items()},
            'avb_after': {k: v.hex() if isinstance(v, bytes) else v for k, v in after_avb.items()},
        }
        (M5_BUILD / 'm5-build-report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
        print(f'M5_BUILD_OK candidate={out_path} sha256={cand_sha} entries={len(old)}->{len(got)}')
        return out_path


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument('--base', type=Path, default=BASE_DEFAULT)
    ap.add_argument('--out-dir', type=Path, default=Path('/mnt/e/marble-tmp'))
    args = ap.parse_args()
    build(args.base, args.out_dir)


if __name__ == '__main__':
    main()
