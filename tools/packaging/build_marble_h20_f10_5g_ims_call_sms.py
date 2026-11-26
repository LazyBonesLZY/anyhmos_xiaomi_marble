#!/usr/bin/env python3
"""Package Batch F10 (5G/5A ENDC+DCNR+EFL + QRTR 0x302 IMSDCM VoLTE/VoNR/IMS-SMS + QMI WMS/VOICE + SA 4005/4006) onto H7 vendor_boot_a."""
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
F10_BUILD = ROOT / 'work/marble-f10-build'
LLVM_READELF = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-readelf'
LLVM_NM = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-nm'

TEL_F10_JSON = ROOT / 'devices/marble/runtime/telephony/telephony-f10-marble.json'

ORACLE = ROOT / 'work/marble-m15-role-storage.1v2ynS/build_m15_role_storage.py'
ORACLE_SHA = '7d4f79857a3458ffdb7c8fb86b1cfd893fd9addb02b13e7e05d7b5fa7093cfaa'

BASE_DEFAULT = Path('/mnt/e/marble-tmp/vendor_boot_h20_h7_nfc_service_663bdf7f.img')
BASE_SHA = '663bdf7fa2eab5148b052b6ab61d911920136166bcbe3cebbeb4fe2f2b6259d6'

PREFIX = 'hmos-compat-marble/'
STAGE_MEMBER = PREFIX + 'graphics/stage-phase4-marble.sh'
FOUNDATION_MEMBER = PREFIX + 'graphics/foundation-app-core.json'
TELEPHONY_MEMBER = PREFIX + 'graphics/telephony-marble.json'
RIL_MEMBER = PREFIX + 'graphics/libril_vendor_qmi.z.so'

# All 84 prior immutable ramdisk members from H7 (zero HCB delta, zero audio delta, all H1-H7 NFC, G1-G5 camera, U1-U4, F1-F7 preserved byte-for-byte).
GUARD_MEMBERS = (
    PREFIX + 'hmos-init-debug.cfg',
    PREFIX + 'graphics/start-u2-postboot-marble.sh',
    PREFIX + 'graphics/hdf_default_phase4.hcb',
    PREFIX + 'graphics/libhmos_audio_vdi.so',
    PREFIX + 'graphics/audio-runtime/lib64/libhmos_pal_server.so',
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
    PREFIX + 'graphics/native_venc_probe',
    PREFIX + 'graphics/libvulkan_freedreno.so.gz',
    PREFIX + 'graphics/zink-runtime-marble.tar.gz',
    PREFIX + 'graphics/libdisplay_composer_vdi_impl.z.so',
    PREFIX + 'graphics/start-phase4-marble.sh',
    PREFIX + 'graphics/marble_modem_probe',
    PREFIX + 'graphics/start-modem-f7-marble.sh',
    PREFIX + 'graphics/carrier-cn-marble.tar.gz',
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

STAGED_FILES = (
    ('graphics/libril_vendor_qmi.z.so', F10_BUILD / 'libril_vendor_qmi.z.so', 0o644),
    ('graphics/telephony-marble.json', TEL_F10_JSON, 0o644),
)

OPT_SA_OLD = (
    b"printf '%s\\n' '4005 4016 1903 1905 1907 3006 4300 5204 6001 8250 302 3526 8003 10120 "
    b"65862 65920 65930 65936 65952 66006 66048 66106 66208 66234 66250 66288 66358 66406 "
    b"66532 66588 70633 70634' > \"$R/compat/etc/sceneboard-optional-sa.list\""
)
OPT_SA_NEW = (
    b"printf '%s\\n' '4016 1903 1905 1907 3006 4300 5204 6001 8250 302 3526 8003 10120 "
    b"65862 65920 65930 65936 65952 66006 66048 66106 66208 66234 66250 66288 66358 66406 "
    b"66532 66588 70633 70634' > \"$R/compat/etc/sceneboard-optional-sa.list\""
)

STAGE_ANCHOR = b'$B test -x "$R/compat/bin/init"\necho MARBLE_PHASE4_RAM_COMPAT_READY'
STAGE_F10_BLOCK = (
    b'# === MARBLE_F10_5G_IMS_CALL_SMS_STAGE_BEGIN ===\n'
    b'# Update U1 bind-mounted sys_prod/etc/param/hw_defaults.para IN PLACE (preserving inode) for 5G/5A EFL display.\n'
    b'f10_dst="$R/compat/etc/u1-sysprod-hw-defaults.para"\n'
    b'if $B test -f "$f10_dst"; then\n'
    b' for f10_kv in "const.telephony.efl_forbidden=false" "const.telephony.show_efl_mobile_type=true"; do\n'
    b'  f10_k="${f10_kv%%=*}"\n'
    b'  f10_v="${f10_kv#*=}"\n'
    b'  $B awk -v k="$f10_k" -v v="$f10_v" \'BEGIN{seen=0; rk=k; gsub(/[.]/, "[.]", rk); pat="^[ \\t]*" rk "[ \\t]*="} $0 ~ pat{print k "=" v; seen=1; next} {print} END{if(!seen) print k "=" v}\' "$f10_dst" > "$f10_dst.f10tmp"\n'
    b'  $B cat "$f10_dst.f10tmp" > "$f10_dst"\n'
    b'  $B rm -f "$f10_dst.f10tmp"\n'
    b' done\n'
    b' $B chmod 0644 "$f10_dst"\n'
    b'elif $B test -f "$R/sys_prod/etc/param/hw_defaults.para"; then\n'
    b' u1_patch_para "$R/sys_prod/etc/param/hw_defaults.para" "$f10_dst" \\\n'
    b'  "const.telephony.efl_forbidden=false" \\\n'
    b'  "const.telephony.show_efl_mobile_type=true"\n'
    b'fi\n'
    b'echo "MARBLE_F10_5G_IMS_CALL_SMS_STAGED=1"\n'
    b'# === MARBLE_F10_5G_IMS_CALL_SMS_STAGE_END ===\n'
)

SA_4005_ENTRY = {
    'name': 4005,
    'libpath': 'libtel_call_manager.z.so',
    'run-on-create': True,
    'distributed': False,
    'dump_level': 1,
    'min_hdi_proxy_version': [],
}


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
    for marker in (b'MARBLE_F10_5G_IMS_CALL_SMS_STAGE_BEGIN', b'const.telephony.efl_forbidden=false'):
        require(marker not in raw, f'stage script already contains {marker!r}')
    require(b'MARBLE_H7_NFC_SERVICE_STAGE_BEGIN' in raw, 'Expected H7 stage marker in base stage script')
    out = splice_once(
        raw,
        (
            (OPT_SA_OLD, OPT_SA_NEW),
            (STAGE_ANCHOR, STAGE_F10_BLOCK + STAGE_ANCHOR),
        ),
        STAGE_MEMBER,
    )
    require(b'\r' not in out, 'CR byte in patched stage-phase4-marble.sh')
    subprocess.run(['sh', '-n'], input=out, check=True)
    return out


def patch_foundation_json(raw: bytes) -> bytes:
    doc = json.loads(raw.decode('utf-8'))
    require(doc.get('process') == 'foundation', f'Unexpected foundation process: {doc.get("process")!r}')
    sas = doc.get('systemability', [])
    require(isinstance(sas, list) and len(sas) >= 10, f'Unexpected foundation SA list: {len(sas)}')
    require(not any(s.get('name') == 4005 for s in sas), 'foundation-app-core.json already contains SA 4005')
    new_doc = copy.deepcopy(doc)
    new_doc['systemability'].append(copy.deepcopy(SA_4005_ENTRY))
    out = (json.dumps(new_doc, indent=2) + '\n').encode('utf-8')
    parsed = json.loads(out.decode('utf-8'))
    require([s.get('name') for s in parsed['systemability'][:-1]] == [s.get('name') for s in sas],
            'Prior foundation SAs altered')
    require(parsed['systemability'][-1] == SA_4005_ENTRY, 'SA 4005 entry mismatch')
    return out


def validate_inputs() -> None:
    for rel, src, _mode in STAGED_FILES:
        require(src.is_file(), f'Missing staged F10 artifact: {src}')
        raw = src.read_bytes()
        if rel.endswith('libril_vendor_qmi.z.so'):
            require(raw[:4] == b'\x7fELF', f'Expected ELF shared library for {rel}')
            if LLVM_READELF.is_file():
                dyn = subprocess.check_output([str(LLVM_READELF), '-d', str(src)], text=True)
                needed = re.findall(r'\(NEEDED\)\s+Shared library:\s+\[([^\]]+)\]', dyn)
                soname = re.findall(r'\(SONAME\)\s+Library soname:\s+\[([^\]]+)\]', dyn)
                require(needed == ['libc.so'], f'Unexpected NEEDED for {rel}: {needed}')
                require(soname == ['libril_vendor.z.so'], f'Unexpected SONAME for {rel}: {soname}')
            if LLVM_NM.is_file():
                syms = subprocess.check_output([str(LLVM_NM), '-D', '--defined-only', str(src)], text=True)
                require('RilInitOps' in syms, f'Missing RilInitOps export in {rel}')
        elif rel.endswith('telephony-marble.json'):
            doc = json.loads(raw.decode('utf-8'))
            require(doc.get('process') == 'telephony', 'Invalid telephony process')
            by_name = {s['name']: s for s in doc.get('systemability', [])}
            for req_sa in (4010, 4007, 4006, 4008, 66250):
                require(req_sa in by_name, f'Missing SA {req_sa} in telephony-f10-marble.json')
            require(by_name[4006].get('libpath') == 'libtel_cellular_call.z.so', 'Invalid SA 4006 libpath')
            require(by_name[4006].get('depend') == [4010], 'Invalid SA 4006 depend')


def load_oracle():
    require(ORACLE.is_file() and ORACLE.resolve() == ORACLE, f'Missing oracle: {ORACLE}')
    got_sha = hashlib.sha256(ORACLE.read_bytes()).hexdigest()
    require(got_sha == ORACLE_SHA, f'Oracle SHA256 mismatch: {got_sha}')
    spec = importlib.util.spec_from_file_location('archive_oracle_f10', ORACLE)
    arc = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(arc)
    return arc, arc.load_tools()


def update_manifest_entry(manifest: dict, member: str, blob: bytes, mode: int | None, sha_fn) -> None:
    require(member.startswith(PREFIX), f'Invalid manifest member prefix: {member}')
    rel = member[len(PREFIX):]
    if rel in manifest:
        manifest[rel].update(bytes=len(blob), sha256=sha_fn(blob))
        if mode is not None:
            manifest[rel]['mode'] = oct(mode)
    else:
        manifest[rel] = {'bytes': len(blob), 'mode': oct(mode if mode is not None else 0o644), 'sha256': sha_fn(blob)}


def build(base_path: Path, out_dir: Path) -> Path:
    require(base_path.is_file(), f'Missing base image: {base_path}')
    F10_BUILD.mkdir(parents=True, exist_ok=True)
    validate_inputs()
    out_dir.mkdir(parents=True, exist_ok=True)

    archive, r = load_oracle()
    base_bytes = r.read_original(base_path)
    base_sha = r.sha(base_bytes)
    require(base_sha == BASE_SHA, f'Base SHA256 mismatch: got {base_sha}, want {BASE_SHA}')

    before_layout = r.layout(base_bytes)
    before_avb = r.verify_existing_avb(base_bytes, before_layout['payload_end'])

    with tempfile.TemporaryDirectory(prefix='h20_f10_', dir=str(F10_BUILD)) as tmp:
        scratch = Path(tmp)
        old, old_raw, kind, old_rd, args = r.extract_entries(
            base_path, base_bytes, before_layout, scratch / 'before'
        )
        old_manifest = archive.parse(old[r.MANIFEST][1])
        r.manifest_holds(old, old_manifest)

        for member in GUARD_MEMBERS:
            require(member in old, f'Missing guarded member in H7 base image: {member}')
        for req_m in (STAGE_MEMBER, FOUNDATION_MEMBER, TELEPHONY_MEMBER, RIL_MEMBER):
            require(req_m in old, f'Missing target member in H7 base image: {req_m}')

        expected = dict(old)
        new_manifest = copy.deepcopy(old_manifest)
        modified_members = {r.MANIFEST, STAGE_MEMBER, FOUNDATION_MEMBER, TELEPHONY_MEMBER, RIL_MEMBER}
        staged_report = {}

        new_stage = patch_stage(old[STAGE_MEMBER][1])
        r.set_entry(expected, STAGE_MEMBER, new_stage)
        update_manifest_entry(new_manifest, STAGE_MEMBER, new_stage, None, r.sha)

        new_foundation = patch_foundation_json(old[FOUNDATION_MEMBER][1])
        r.set_entry(expected, FOUNDATION_MEMBER, new_foundation)
        update_manifest_entry(new_manifest, FOUNDATION_MEMBER, new_foundation, None, r.sha)

        for rel, src, mode in STAGED_FILES:
            member = PREFIX + rel
            blob = src.read_bytes()
            require(member in expected, f'Expected existing member to replace: {member}')
            r.set_entry(expected, member, blob)
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
        out_path = out_dir / f'vendor_boot_h20_f10_5g_ims_call_sms_{cand_sha[:8]}.img'
        out_path.write_bytes(cand_bytes)

        report = {
            'batch': 'F10',
            'description': (
                '5G/5A ENDC+DCNR+EFL display + QRTR 0x302 IMSDCM VoLTE/VoNR/IMS-SMS + '
                'per-slot QMI WMS (0x004c) SMS + QMI VOICE (0x0044) Call + SA 4005/4006 layered onto H7 NFC'
            ),
            'base': {'path': str(base_path), 'bytes': len(base_bytes), 'sha256': base_sha},
            'candidate': {'path': str(out_path), 'bytes': len(cand_bytes), 'sha256': cand_sha},
            'entries_before': len(old),
            'entries_after': len(got),
            'guard_members_verified': list(GUARD_MEMBERS),
            'staged': staged_report,
            'avb_before': {k: (v.hex() if isinstance(v, bytes) else v) for k, v in before_avb.items()},
            'avb_after': {k: (v.hex() if isinstance(v, bytes) else v) for k, v in after_avb.items()},
        }
        (F10_BUILD / 'f10-build-report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
        print(f'F10_BUILD_OK path={out_path} sha256={cand_sha} entries={len(old)}->{len(got)}')
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
