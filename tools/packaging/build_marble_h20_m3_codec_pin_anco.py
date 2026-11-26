#!/usr/bin/env python3
"""Package Batch M3 (Venus Video HW Decode + Lockscreen PIN pin_auth_host host-17/SA 941 + DroiTong ANCO/RGM) onto M2 vendor_boot_a."""
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
M3_BUILD = ROOT / 'work/marble-m3-build'
M4_BUILD = ROOT / 'work/marble-m4-build'
DONOR_FS = ROOT / 'work/donor-rom/ADY-AL10-207.0.0.111-SP10/fs'
LLVM_READELF = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-readelf'
LLVM_NM = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-nm'

sys.path.insert(0, str(ROOT / 'tools/packaging'))
import build_marble_phase4_images as phase4  # noqa: E402

ORACLE = ROOT / 'work/marble-m15-role-storage.1v2ynS/build_m15_role_storage.py'
ORACLE_SHA = '7d4f79857a3458ffdb7c8fb86b1cfd893fd9addb02b13e7e05d7b5fa7093cfaa'

BASE_DEFAULT = Path('/mnt/e/marble-tmp/vendor_boot_h20_m2_cam_ims_softap_chg_531b5ebd.img')
BASE_SHA = '531b5ebdd707e231ca2cd0118a5405713fcd88dce721da059ef5c0d235f4cb6e'

PREFIX = 'hmos-compat-marble/'
INIT_DEBUG_MEMBER = PREFIX + 'hmos-init-debug.cfg'
HCB_MEMBER = PREFIX + 'graphics/hdf_default_phase4.hcb'
STAGE_MEMBER = PREFIX + 'graphics/stage-phase4-marble.sh'
START_MEMBER = PREFIX + 'graphics/start-phase4-marble.sh'
POSTBOOT_MEMBER = PREFIX + 'graphics/start-u2-postboot-marble.sh'

PINAUTH_CFG = ROOT / 'devices/marble/runtime/pinauth/init-m3-pinauth-marble.cfg'
USERIAM_JSON = ROOT / 'devices/marble/runtime/pinauth/useriam-core-marble.json'
ANCO_CFG = ROOT / 'devices/marble/runtime/anco/init-m4-anco-marble.cfg'
ANCO_SH = ROOT / 'devices/marble/runtime/anco/start-anco-m4-marble.sh'

# Immutable non-M3 ramdisk members preserved byte-for-byte, including libhmos_ram_no_tee.so (EL2 unencrypted safety).
GUARD_MEMBERS = (
    PREFIX + 'graphics/libhmos_ram_no_tee.so',
    PREFIX + 'graphics/init-modem-f12-marble.cfg',
    PREFIX + 'graphics/marble_modem_probe',
    PREFIX + 'graphics/libril_vendor_qmi.z.so',
    PREFIX + 'graphics/start-modem-f7-marble.sh',
    PREFIX + 'graphics/display_manager_config.xml',
    PREFIX + 'graphics/libdisplay_composer_vdi_impl.z.so',
    PREFIX + 'graphics/libdisplayengine_module.so',
    PREFIX + 'graphics/sensors-marble.json',
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
    PREFIX + 'graphics/libmarble_gnss_hdi_driver.z.so',
    PREFIX + 'graphics/native_gnss_hdi_publisher',
    PREFIX + 'graphics/libhmos_audio_vdi.so',
    PREFIX + 'graphics/audio-runtime/lib64/libhmos_pal_server.so',
    PREFIX + 'graphics/native_voice_audio_probe',
    PREFIX + 'graphics/telephony-marble.json',
    PREFIX + 'graphics/foundation-app-core.json',
    PREFIX + 'graphics/libhuks_hdi_service_1.1.z.so',
    PREFIX + 'graphics/libhuks_engine_core_standard.z.so',
    PREFIX + 'graphics/libdbkeys.so',
    PREFIX + 'graphics/libhmos_sceneboard_optional_sa.so',
    PREFIX + 'graphics/libmarble_nfc_interface_service_1.1.z.so',
    PREFIX + 'graphics/libmarble_nci_native_vendor.z.so',
    PREFIX + 'graphics/init-h7-nfc-service-marble.cfg',
    PREFIX + 'graphics/start-nfc-service-h7-marble.sh',
)

STAGED_REPLACE = (
    ('graphics/hmos_ims_core_service', M3_BUILD / 'hmos_ims_core_service', 0o755),
    ('graphics/libOMX_Core.z.so', M3_BUILD / 'libOMX_Core.z.so', 0o644),
    ('graphics/libuser_auth_interface_service_4.1.z.so', M3_BUILD / 'libuser_auth_interface_service_4.1.z.so', 0o644),
    ('graphics/useriam-core.json', USERIAM_JSON, 0o644),
)

STAGED_ADD = (
    ('graphics/libpin_auth_driver.z.so', M3_BUILD / 'pinauth/libpin_auth_driver.z.so', 0o644),
    ('graphics/libpin_auth_interface_service_3.0.z.so', M3_BUILD / 'pinauth/libpin_auth_interface_service_3.0.z.so', 0o644),
    ('graphics/init-m3-pinauth-marble.cfg', PINAUTH_CFG, 0o644),
    ('graphics/init-m4-anco-marble.cfg', ANCO_CFG, 0o644),
    ('graphics/start-anco-m4-marble.sh', ANCO_SH, 0o755),
)

INIT_IMP_OLD = b'    "/compat/etc/init-m4-softap-marble.cfg"\n  ],'
INIT_IMP_NEW = (
    b'    "/compat/etc/init-m4-softap-marble.cfg",\n'
    b'    "/compat/etc/init-m3-pinauth-marble.cfg",\n'
    b'    "/compat/etc/init-m4-anco-marble.cfg"\n  ],'
)

STAGE_ANCHOR = b'$B test -x "$R/compat/bin/init"\necho MARBLE_PHASE4_RAM_COMPAT_READY'
STAGE_M3_BLOCK = (
    b'# === MARBLE_M3_PINAUTH_ANCO_STAGE_BEGIN ===\n'
    b'$B mkdir -p /compat/bin "$R/compat/bin" "$R/compat/lib64" "$R/compat/etc"\n'
    b'for pinlib in libpin_auth_driver.z.so libpin_auth_interface_service_3.0.z.so; do\n'
    b' if $B test -f "/graphics/$pinlib"; then\n'
    b'  $B cp "/graphics/$pinlib" "$R/compat/lib64/$pinlib"\n'
    b'  $B chmod 0644 "$R/compat/lib64/$pinlib"\n'
    b'  if $B test -f "$R/vendor/lib64/$pinlib"; then\n'
    b'   if ! $B grep -q " $R/vendor/lib64/$pinlib " /proc/mounts 2>/dev/null; then\n'
    b'    $B mount --bind "$R/compat/lib64/$pinlib" "$R/vendor/lib64/$pinlib" 2>/dev/null || true\n'
    b'   fi\n'
    b'  fi\n'
    b' fi\n'
    b'done\n'
    b'for m3etc in init-m3-pinauth-marble.cfg init-m4-anco-marble.cfg; do\n'
    b' if $B test -f "/graphics/$m3etc"; then\n'
    b'  $B cp "/graphics/$m3etc" "$R/compat/etc/$m3etc"\n'
    b'  $B chmod 0644 "$R/compat/etc/$m3etc"\n'
    b' fi\n'
    b'done\n'
    b'if $B test -f /graphics/start-anco-m4-marble.sh; then\n'
    b' $B cp /graphics/start-anco-m4-marble.sh /compat/bin/start-anco-m4-marble.sh\n'
    b' $B cp /graphics/start-anco-m4-marble.sh "$R/compat/bin/start-anco-m4-marble.sh"\n'
    b' $B chmod 0755 /compat/bin/start-anco-m4-marble.sh "$R/compat/bin/start-anco-m4-marble.sh"\n'
    b'fi\n'
    b'echo "MARBLE_M3_PINAUTH_ANCO_STAGED=1"\n'
    b'# === MARBLE_M3_PINAUTH_ANCO_STAGE_END ===\n'
)

START_ANCHOR = b'stage=services\nfor gate in storage kvdata useriam account appspawn theme wallpaper ui diagnostics deviceproviders b1; do'
START_M3_BLOCK = (
    b'# === MARBLE_M3_PINAUTH_START_BEGIN ===\n'
    b'pinauth_skip=$(pget marble.debug.skip.pinauth)\n'
    b'pinauth_skip_list=$($B cat "$R/data/local/tmp/marble-skip-stages" 2>/dev/null | $B tr -d \' \\t\\r\\n\' || true)\n'
    b'case ",$pinauth_skip_list," in\n'
    b' *,pinauth,*|*,pin_auth,*) pinauth_skip=true ;;\n'
    b'esac\n'
    b'if [ "$pinauth_skip" = true ] || [ "$pinauth_skip" = 1 ]; then\n'
    b' log "M3_PINAUTH_SKIP=requested"\n'
    b'else\n'
    b' pset marble.pinauth.ready true\n'
    b' log "M3_PINAUTH_TRIGGERED=1"\n'
    b'fi\n'
    b'# === MARBLE_M3_PINAUTH_START_END ===\n'
)

POSTBOOT_ANCHOR = b'say "U2_GNSS_MODE=hdf_location_host"'
POSTBOOT_M3_BLOCK = (
    b'# Step O: Batch M3 DroiTong / ANCO container runtime (binderfs + /mnt/isula/var + RGM SAs).\n'
    b'if skipped anco || skipped rgm || skipped droitong; then\n'
    b'  say "U2_ANCO_M3_SKIP=requested"\n'
    b'elif [ -x /compat/bin/start-anco-m4-marble.sh ]; then\n'
    b'  $B sh /compat/bin/start-anco-m4-marble.sh || say "U2_ANCO_M3_RC=$?"\n'
    b'else\n'
    b'  say "U2_ANCO_M3_SKIP=script_missing"\n'
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
    for m in (b'init-m3-pinauth-marble.cfg', b'init-m4-anco-marble.cfg'):
        require(m not in raw, f'hmos-init-debug.cfg already contains {m!r}')
    out = splice_once(raw, ((INIT_IMP_OLD, INIT_IMP_NEW),), INIT_DEBUG_MEMBER)
    doc = json.loads(out.decode('utf-8'))
    imps = doc.get('import', [])
    require('/compat/etc/init-m3-pinauth-marble.cfg' in imps, 'Missing M3 pinauth init import')
    require('/compat/etc/init-m4-anco-marble.cfg' in imps, 'Missing M3 anco init import')
    require(len(out) < 32768, f'hmos-init-debug.cfg exceeds 32768B cap: {len(out)}')
    return out


def patch_stage(raw: bytes) -> bytes:
    for m in (b'MARBLE_M3_PINAUTH_ANCO_STAGE_BEGIN', b'libpin_auth_interface_service_3.0.z.so'):
        require(m not in raw, f'stage script already contains {m!r}')
    require(b'MARBLE_M2_CAM_THERMAL_SOFTAP_STAGE_BEGIN' in raw, 'Expected M2 stage marker')
    out = splice_once(raw, ((STAGE_ANCHOR, STAGE_M3_BLOCK + STAGE_ANCHOR),), STAGE_MEMBER)
    require(b'\r' not in out, 'CR byte in patched stage-phase4-marble.sh')
    subprocess.run(['sh', '-n'], input=out, check=True)
    return out


def patch_start(raw: bytes) -> bytes:
    for m in (b'MARBLE_M3_PINAUTH_START_BEGIN', b'marble.pinauth.ready'):
        require(m not in raw, f'start script already contains {m!r}')
    out = splice_once(raw, ((START_ANCHOR, START_M3_BLOCK + START_ANCHOR),), START_MEMBER)
    require(b'\r' not in out, 'CR byte in patched start-phase4-marble.sh')
    subprocess.run(['sh', '-n'], input=out, check=True)
    return out


def patch_postboot(raw: bytes) -> bytes:
    for m in (b'start-anco-m4-marble.sh', b'U2_ANCO_M3_SKIP'):
        require(m not in raw, f'postboot script already contains {m!r}')
    require(b'start-softap-m4-marble.sh' in raw, 'Expected M2 postboot call')
    out = splice_once(raw, ((POSTBOOT_ANCHOR, POSTBOOT_M3_BLOCK + POSTBOOT_ANCHOR),), POSTBOOT_MEMBER)
    require(b'\r' not in out, 'CR byte in patched start-u2-postboot-marble.sh')
    subprocess.run(['sh', '-n'], input=out, check=True)
    return out


def validate_inputs() -> None:
    for rel, src, _mode in STAGED_REPLACE + STAGED_ADD:
        require(src.is_file(), f'Missing staged M3 artifact: {src}')
        raw = src.read_bytes()
        if rel.endswith('.json'):
            doc = json.loads(raw.decode('utf-8'))
            sas = {sa['name']: sa['libpath'] for sa in doc.get('systemability', [])}
            require(sas.get(941) == 'libpinauthservice.z.so' and sas.get(901) == 'libuserauthservice.z.so',
                    f'Unexpected useriam-core.json SAs: {sas}')
        elif rel.endswith('.cfg'):
            doc = json.loads(raw.decode('utf-8'))
            require('services' in doc and 'jobs' in doc, f'Invalid cfg: {rel}')
            if rel.endswith('init-m3-pinauth-marble.cfg'):
                svcs = {s['name']: s for s in doc['services']}
                require(svcs['pin_auth_host']['path'][2] == '17', 'pin_auth_host must use host index -i 17')
        elif rel.endswith('.sh'):
            require(b'\r' not in raw, f'CR byte in {rel}')
            subprocess.run(['sh', '-n'], input=raw, check=True)
        else:
            require(raw[:4] == b'\x7fELF', f'Expected ELF for {rel}')
            if LLVM_READELF.is_file():
                dyn = subprocess.check_output([str(LLVM_READELF), '-d', str(src)], text=True)
                soname = re.findall(r'\(SONAME\)\s+Library soname:\s+\[([^\]]+)\]', dyn)
                if rel.endswith('libOMX_Core.z.so'):
                    require(soname == ['libOMX_Core.z.so'], f'Unexpected OMX SONAME: {soname}')
                elif rel.endswith('libuser_auth_interface_service_4.1.z.so'):
                    require(soname == ['libuser_auth_interface_service_4.1.z.so'], f'Unexpected UserAuth SONAME: {soname}')
                elif rel.endswith('libpin_auth_interface_service_3.0.z.so'):
                    require(soname == ['libpin_auth_interface_service_3.0.z.so'], f'Unexpected PinAuth SONAME: {soname}')
            if LLVM_NM.is_file():
                if rel.endswith('libpin_auth_interface_service_3.0.z.so'):
                    nm = subprocess.check_output([str(LLVM_NM), '-D', str(src)], text=True)
                    require(' PinAuthInterfaceImplGetInstance' in nm, 'Missing PinAuthInterfaceImplGetInstance')
                elif rel.endswith('libuser_auth_interface_service_4.1.z.so'):
                    nm = subprocess.check_output([str(LLVM_NM), '-D', str(src)], text=True)
                    require(' UserAuthInterfaceImplGetInstance' in nm, 'Missing UserAuthInterfaceImplGetInstance')


def load_oracle():
    require(ORACLE.is_file() and ORACLE.resolve() == ORACLE, f'Missing oracle: {ORACLE}')
    got_sha = hashlib.sha256(ORACLE.read_bytes()).hexdigest()
    require(got_sha == ORACLE_SHA, f'Oracle SHA256 mismatch: {got_sha}')
    spec = importlib.util.spec_from_file_location('archive_oracle_m3', ORACLE)
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
    M3_BUILD.mkdir(parents=True, exist_ok=True)
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
    )
    expected_base_hcb = phase4.build_phase4_hdf_hcb(**hcb_kwargs, pin_auth_host=False)
    new_hcb = phase4.build_phase4_hdf_hcb(**hcb_kwargs, pin_auth_host=True)
    require(b'pin_auth_host' in new_hcb and b'libpin_auth_driver.z.so' in new_hcb and b'pin_auth_interface_service' in new_hcb,
            'Generated M3 HCB missing pin_auth_host declaration')

    with tempfile.TemporaryDirectory(prefix='h20_m3_', dir=str(M3_BUILD)) as tmp:
        scratch = Path(tmp)
        old, old_raw, kind, old_rd, args = r.extract_entries(
            base_path, base_bytes, before_layout, scratch / 'before'
        )
        old_manifest = archive.parse(old[r.MANIFEST][1])
        r.manifest_holds(old, old_manifest)

        require(old[HCB_MEMBER][1] == expected_base_hcb,
                'Base hdf_default_phase4.hcb does not match expected M2 17-host HCB')
        for member in GUARD_MEMBERS:
            require(member in old, f'Missing guarded member in base image: {member}')

        expected = dict(old)
        new_manifest = copy.deepcopy(old_manifest)
        modified_members = {r.MANIFEST, HCB_MEMBER, INIT_DEBUG_MEMBER, STAGE_MEMBER, START_MEMBER, POSTBOOT_MEMBER}
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
            require(member not in old, f'New M3 member already exists in base image: {member}')
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
        out_path = out_dir / f'vendor_boot_h20_m3_codec_pin_anco_{cand_sha[:8]}.img'
        out_path.write_bytes(cand_bytes)
        require(len(cand_bytes) == len(base_bytes) == 100663296, f'Size mismatch: {len(cand_bytes)}')
        report = {
            'batch': 'M3',
            'description': 'Venus Video HW Decode (DYNAMIC_HANDLE + seek CSD) + Lockscreen PIN (adapter41 + pure-userspace pin_auth_host host-17 + SA 941, EL2 unencrypted) + DroiTong ANCO/RGM',
            'base': {'path': str(base_path), 'bytes': len(base_bytes), 'sha256': base_sha},
            'candidate': {'path': str(out_path), 'bytes': len(cand_bytes), 'sha256': cand_sha},
            'entries_before': len(old),
            'entries_after': len(got),
            'hcb_hosts_before': 17,
            'hcb_hosts_after': 18,
            'staged': staged_report,
            'avb_before': {k: v.hex() if isinstance(v, bytes) else v for k, v in before_avb.items()},
            'avb_after': {k: v.hex() if isinstance(v, bytes) else v for k, v in after_avb.items()},
        }
        (M3_BUILD / 'm3-build-report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
        print(f'M3_BUILD_OK candidate={out_path} sha256={cand_sha} entries={len(old)}->{len(got)}')
        return out_path


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument('--base', type=Path, default=BASE_DEFAULT)
    ap.add_argument('--out-dir', type=Path, default=Path('/mnt/e/marble-tmp'))
    args = ap.parse_args()
    build(args.base, args.out_dir)


if __name__ == '__main__':
    main()
