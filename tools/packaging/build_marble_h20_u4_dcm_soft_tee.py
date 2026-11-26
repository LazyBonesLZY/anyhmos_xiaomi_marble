#!/usr/bin/env python3
"""Package Batch U4 (clean-room DCM SA 65866 software-TEE + local device token)
onto the accepted U3.2b vendor_boot_a. Single-variable layer: no other member is
replaced, and all 35 prior F/U1/U2/U3.1/U3.2a/U3.2b protected members remain
byte-identical.
"""
import argparse
import copy
import importlib.util
import json
import subprocess
import sys
import tempfile
from hashlib import sha256
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / 'tools/packaging'
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import build_marble_phase4_images as p4  # noqa: F401  (archive tooling import path)

WORK = ROOT / 'work'
U4_BUILD = WORK / 'marble-dcm-build'
DCM_CFG = ROOT / 'devices/marble/runtime/dcm/init-dcm-marble.cfg'
DONOR_FS = WORK / 'donor-rom/ADY-AL10-207.0.0.111-SP10/fs'
HUKS_ARM64 = WORK / 'marble-huks-build/arm64'

BASE_DEFAULT = Path('/mnt/e/marble-tmp/vendor_boot_h20_u4_dcm_soft_tee_cbdd3cf9.img')
BASE_SHA = 'cbdd3cf9fa4105de526e85d21e2b69f41aedae5d93708d85c59cecb4bb51b713'
# Accepted baselines: U3.2b (no U4 members yet) and the current device image
# (U3.2b + an earlier U4 build). Members present are replaced, absent ones added.
ACCEPTED_BASE_SHA = {
    BASE_SHA: 'u4-installed',
    'd5299691eb38735683ff75f20576a46397e5259dc4b8f2b2efdd20733c501b6e': 'u3.2b',
}
ORACLE = WORK / 'marble-m15-role-storage.1v2ynS/build_m15_role_storage.py'
ORACLE_SHA = '7d4f79857a3458ffdb7c8fb86b1cfd893fd9addb02b13e7e05d7b5fa7093cfaa'

PREFIX = 'hmos-compat-marble/'
INIT_DEBUG_MEMBER = PREFIX + 'hmos-init-debug.cfg'
STAGE_MEMBER = PREFIX + 'graphics/stage-phase4-marble.sh'
START_PHASE4_MEMBER = PREFIX + 'graphics/start-phase4-marble.sh'

# Prior F-series / U1.7 / U2.1 / U3.1 / U3.2a / U3.2b members that must survive byte-for-byte.
GUARD_MEMBERS = (
    PREFIX + 'graphics/libhmos_audio_vdi.so',
    PREFIX + 'graphics/audio-runtime/lib64/libhmos_pal_server.so',
    PREFIX + 'graphics/init-bluetooth-marble.cfg',
    PREFIX + 'graphics/init-d-sensors-marble.cfg',
    PREFIX + 'graphics/hmos_tqftpserv',
    PREFIX + 'graphics/marble_modem_probe',
    PREFIX + 'graphics/libril_vendor_qmi.z.so',
    PREFIX + 'graphics/start-modem-f5-marble.sh',
    PREFIX + 'graphics/hmos_rmtfs',
    PREFIX + 'graphics/native_ril_hdi_loader',
    PREFIX + 'graphics/telephony-marble.json',
    PREFIX + 'graphics/libdbkeys.so',
    PREFIX + 'graphics/libhmos_sceneboard_optional_sa.so',
    PREFIX + 'graphics/init-u1-services-marble.cfg',
    PREFIX + 'graphics/native_gnss_hdi_publisher',
    PREFIX + 'graphics/libmarble_gnss_hdi_driver.z.so',
    PREFIX + 'graphics/init-u2-location-marble.cfg',
    PREFIX + 'graphics/libhmos_surface_diagnostic.so',
    PREFIX + 'graphics/vpu20_4v-fw.tar.gz',
    PREFIX + 'graphics/start-venus-marble.sh',
    PREFIX + 'graphics/start-u2-postboot-marble.sh',
    PREFIX + 'graphics/hdf_default_phase4.hcb',
    PREFIX + 'graphics/libjpeg_vdi_impl.z.so',
    PREFIX + 'graphics/libdisplayengine_module.so',
    PREFIX + 'graphics/native_venus_v4l2_probe',
    PREFIX + 'graphics/init-u3-codec-display-marble.cfg',
    PREFIX + 'graphics/libOMX_Core.z.so',
    PREFIX + 'graphics/libdisplay_buffer_vdi_impl.z.so',
    PREFIX + 'graphics/plane_layout_contract',
    PREFIX + 'graphics/native_venus_decode_test',
    PREFIX + 'graphics/native_codec_probe',
    PREFIX + 'graphics/libheifdecode_vdi_impl.z.so',
    PREFIX + 'graphics/native_heif_probe',
)

STAGED_FILES = (
    ('graphics/hmos_dcm_service', U4_BUILD / 'hmos_dcm_service', 0o755, False),
    ('graphics/hmos_dcm_token_probe', U4_BUILD / 'hmos_dcm_token_probe', 0o755, False),
    ('graphics/init-dcm-marble.cfg', DCM_CFG, 0o644, False),
    ('graphics/libhuks_hdi_service_1.1.z.so',
     HUKS_ARM64 / 'libhuks_hdi_service_1.1.z.so', 0o644, True),
    ('graphics/libhuks_engine_core_standard.z.so',
     HUKS_ARM64 / 'libhuks_engine_core_standard.z.so', 0o644, True),
)

OPT_SA_U4_OLD = b'65862 65866 65920 65930'
OPT_SA_U4_NEW = b'65862 65920 65930'

STAGE_ANCHOR = b'$B test -x "$R/compat/bin/init"\necho MARBLE_PHASE4_RAM_COMPAT_READY'

STAGE_U4_BLOCK = (
    b'# === MARBLE_U4_DCM_SOFT_TEE_STAGE_BEGIN ===\n'
    b'$B mkdir -p /compat/bin "$R/compat/bin" "$R/compat/etc"\n'
    b'u4_bins=0\n'
    b'for u4_bin in hmos_dcm_service hmos_dcm_token_probe; do\n'
    b' if $B test -f "/graphics/$u4_bin"; then\n'
    b'  $B cp "/graphics/$u4_bin" "/compat/bin/$u4_bin"\n'
    b'  $B cp "/graphics/$u4_bin" "$R/compat/bin/$u4_bin"\n'
    b'  $B chmod 0755 "/compat/bin/$u4_bin" "$R/compat/bin/$u4_bin"\n'
    b'  u4_bins=$((u4_bins + 1))\n'
    b' fi\n'
    b'done\n'
    b'if $B test -f /graphics/init-dcm-marble.cfg; then\n'
    b' $B cp /graphics/init-dcm-marble.cfg "$R/compat/etc/init-dcm-marble.cfg"\n'
    b' $B chmod 0644 "$R/compat/etc/init-dcm-marble.cfg"\n'
    b'fi\n'
    b'$B mkdir -p "$R/data/service/el1/public/device_cert_manager_server"\n'
    b'$B chown 3562:3562 "$R/data/service/el1/public/device_cert_manager_server" 2>/dev/null || true\n'
    b'$B chmod 0771 "$R/data/service/el1/public/device_cert_manager_server" 2>/dev/null || true\n'
    b'u4_sn=$($B awk -F\'"\' \'/^androidboot[.]serialno = /{print $2; exit}\' /proc/bootconfig 2>/dev/null)\n'
    b'[ -n "$u4_sn" ] || u4_sn=$($B sed -n \'s/.*androidboot[.]serialno=\\([^ ]*\\).*/\\1/p\' /proc/cmdline 2>/dev/null | $B head -n 1)\n'
    b'u4_model=$($B awk -F\'"\' \'/^androidboot[.]hardware[.]sku = /{print $2; exit}\' /proc/bootconfig 2>/dev/null)\n'
    b'[ -n "$u4_model" ] || u4_model=marble\n'
    b'$B mkdir -p "$R/compat/etc"\n'
    b"$B printf '{\"sn\":\"%s\",\"model\":\"%s\",\"udid\":\"\"}\\n' \"$u4_sn\" \"$u4_model\" > \"$R/compat/etc/dcm-device.json\"\n"
    b'$B chown 3562:3562 "$R/compat/etc/dcm-device.json" 2>/dev/null || true\n'
    b'$B chmod 0600 "$R/compat/etc/dcm-device.json"\n'
    b'echo "MARBLE_U4_DCM_STAGE_READY bins=$u4_bins sn_len=${#u4_sn}"\n'
    b'# === MARBLE_U4_DCM_SOFT_TEE_STAGE_END ===\n'
)

START_U4_ANCHOR = b'# === MARBLE_U3_MEDIA_DISPLAY_START_END ===\n'
START_U4_BLOCK = (
    b'# === MARBLE_U4_DCM_SOFT_TEE_START_BEGIN ===\n'
    b'stage=u4_dcm\n'
    b'u4_skip=$(pget marble.debug.skip.u4_dcm)\n'
    b'u4_skip_list=$($B cat "$R/data/local/tmp/marble-skip-stages" 2>/dev/null | $B tr -d \' \\t\\r\\n\' || true)\n'
    b'case ",$u4_skip_list," in\n'
    b' *,u4_dcm,*) u4_skip=true ;;\n'
    b'esac\n'
    b'if [ "$u4_skip" = true ] || [ "$u4_skip" = 1 ]; then\n'
    b' log "U4_DCM_SKIP=requested"\n'
    b'else\n'
    b' pset marble.debug.start.u4_dcm true\n'
    b' log "U4_DCM_TRIGGERED=1"\n'
    b'fi\n'
    b'# === MARBLE_U4_DCM_SOFT_TEE_START_END ===\n'
)


def require(cond, msg):
    if not cond:
        raise ValueError(msg)


def splice_once(data: bytes, pairs: tuple[tuple[bytes, bytes], ...], label: str) -> bytes:
    out = data
    for old, new in pairs:
        cnt = out.count(old)
        require(cnt == 1, f'{label}: insertion anchor not unique (count={cnt}): {old[:80]!r}')
        out = out.replace(old, new, 1)
    reverted = out
    for old, new in reversed(pairs):
        cnt = reverted.count(new)
        require(cnt == 1, f'{label}: delta not separable (count={cnt})')
        reverted = reverted.replace(new, old, 1)
    require(reverted == data, f'{label}: non-target bytes changed')
    return out


def patch_init_debug(raw: bytes) -> bytes:
    if b'init-dcm-marble.cfg' in raw:
        doc = json.loads(raw.decode('utf-8'))
        require('/compat/etc/init-dcm-marble.cfg' in doc.get('import', []),
                'U4 DCM init import missing in baseline')
        return raw
    old_imp = b'    "/compat/etc/init-u3-codec-display-marble.cfg"\n  ],'
    new_imp = (
        b'    "/compat/etc/init-u3-codec-display-marble.cfg",\n'
        b'    "/compat/etc/init-dcm-marble.cfg"\n  ],'
    )
    out = splice_once(raw, ((old_imp, new_imp),), INIT_DEBUG_MEMBER)
    doc = json.loads(out.decode('utf-8'))
    require('/compat/etc/init-dcm-marble.cfg' in doc['import'],
            'U4 DCM init import missing after JSON parse')
    require(len(out) < 32768, f'hmos-init-debug.cfg exceeds 32768-byte init cap: {len(out)}')
    return out


def patch_stage(raw: bytes) -> bytes:
    if b'MARBLE_U4_DCM_SOFT_TEE_STAGE_BEGIN' in raw:
        return raw
    for marker in (
        b'MARBLE_U4_DCM_SOFT_TEE_STAGE_BEGIN',
        b'hmos_dcm_service',
        b'hmos_dcm_token_probe',
        b'init-dcm-marble.cfg',
    ):
        require(marker not in raw, f'stage script already contains {marker!r}')
    out = splice_once(
        raw,
        (
            (OPT_SA_U4_OLD, OPT_SA_U4_NEW),
            (STAGE_ANCHOR, STAGE_U4_BLOCK + STAGE_ANCHOR),
        ),
        STAGE_MEMBER,
    )
    require(b'\r' not in out, 'CR byte in patched stage-phase4-marble.sh')
    subprocess.run(['sh', '-n'], input=out, check=True)
    return out


def patch_start_phase4(raw: bytes) -> bytes:
    if b'MARBLE_U4_DCM_SOFT_TEE_START_BEGIN' in raw:
        return raw
    for marker in (
        b'MARBLE_U4_DCM_SOFT_TEE_START_BEGIN',
        b'marble.debug.start.u4_dcm',
        b'marble.debug.skip.u4_dcm',
    ):
        require(marker not in raw, f'start script already contains {marker!r}')
    out = splice_once(
        raw,
        ((START_U4_ANCHOR, START_U4_ANCHOR + START_U4_BLOCK),),
        START_PHASE4_MEMBER,
    )
    require(b'\r' not in out, 'CR byte in patched start-phase4-marble.sh')
    subprocess.run(['sh', '-n'], input=out, check=True)
    return out


def validate_inputs() -> None:
    for rel, src, _mode, _replaces in STAGED_FILES:
        require(src.is_file(), f'Missing staged U4 artifact: {src}')
        raw = src.read_bytes()
        if rel.endswith('.cfg'):
            json.loads(raw.decode('utf-8'))
        elif rel.endswith('.so'):
            require(raw[:4] == b'\x7fELF', f'Expected ELF shared object for {rel}')
        else:
            require(raw[:4] == b'\x7fELF', f'Expected ELF binary for {rel}')


def assert_no_duplicate_services(entries: dict) -> None:
    dcm_name = 'hmos_dcm_service'
    for member, (_, blob) in entries.items():
        # The DCM cfg itself defines the service and is replaced by this batch;
        # only other cfgs may not define the same name.
        if not member.endswith('.cfg') or member.endswith('init-dcm-marble.cfg'):
            continue
        try:
            doc = json.loads(blob.decode('utf-8', errors='ignore'))
        except json.JSONDecodeError:
            continue
        names = {svc.get('name') for svc in doc.get('services', []) if isinstance(svc, dict)}
        require(dcm_name not in names, f'Duplicate service definition {dcm_name} already in {member}')


def load_oracle():
    require(ORACLE.is_file(), f'Missing oracle: {ORACLE}')
    require(sha256(ORACLE.read_bytes()).hexdigest() == ORACLE_SHA, 'Archive verifier identity changed')
    spec = importlib.util.spec_from_file_location('h20_u4_archive_verifier', ORACLE)
    archive = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(archive)
    return archive, archive.load_tools()


def build(base_path: Path, out: Path) -> Path:
    archive, r = load_oracle()
    base = r.read_original(base_path)
    base_sha = r.sha(base)
    require(base_sha in ACCEPTED_BASE_SHA,
            f'Base SHA mismatch: got {base_sha}, want one of {sorted(ACCEPTED_BASE_SHA)}')
    base_id = r.identity(base_path)
    validate_inputs()
    require(DONOR_FS.is_dir(), f'Missing ADY donor fs directory: {DONOR_FS}')

    before_layout = r.layout(base)
    before_avb = r.verify_existing_avb(base, before_layout['payload_end'])
    out.mkdir(parents=True, exist_ok=True)
    U4_BUILD.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix='pack_u4.', dir=out) as tmp:
        scratch = Path(tmp)
        old, old_raw, kind, old_rd, args = r.extract_entries(
            base_path, base, before_layout, scratch / 'before')
        for gm in GUARD_MEMBERS:
            require(gm in old, f'Guard member missing in base image: {gm}')

        manifest = archive.parse(old[r.MANIFEST][1])
        r.manifest_holds(old, manifest)

        patched_members = {
            INIT_DEBUG_MEMBER: ('hmos-init-debug.cfg', patch_init_debug(old[INIT_DEBUG_MEMBER][1])),
            STAGE_MEMBER: ('graphics/stage-phase4-marble.sh', patch_stage(old[STAGE_MEMBER][1])),
            START_PHASE4_MEMBER: ('graphics/start-phase4-marble.sh', patch_start_phase4(old[START_PHASE4_MEMBER][1])),
        }

        expected = dict(old)
        next_inode = [max(fields[0] for fields, _ in old.values()) + 1]
        new_manifest = copy.deepcopy(manifest)

        modified_members = {r.MANIFEST}
        for full_member, (m_key, new_bytes) in patched_members.items():
            r.set_entry(expected, full_member, new_bytes)
            modified_members.add(full_member)
            if m_key in new_manifest:
                new_manifest[m_key].update(bytes=len(new_bytes), sha256=r.sha(new_bytes))

        staged_info = {}
        dcm_cfg = json.loads(DCM_CFG.read_text(encoding='utf-8'))
        require(dcm_cfg['services'][0]['path'] == ['/compat/bin/hmos_dcm_service'],
                'DCM init path must be /compat/bin/hmos_dcm_service')
        for rel, src, mode, replaces in STAGED_FILES:
            full = f'{r.image_tools.COMPAT_DIR}/{rel}'
            blob = src.read_bytes()
            # The same packager must accept both a baseline without U4
            # (U3.2b) and a baseline that already carries U4 (current device):
            # replace when present, add when absent.
            replaced = full in expected
            if replaced:
                r.set_entry(expected, full, blob)
                modified_members.add(full)
                if rel in new_manifest:
                    new_manifest[rel].update(bytes=len(blob), mode=oct(mode), sha256=r.sha(blob))
                else:
                    new_manifest[rel] = {'bytes': len(blob), 'mode': oct(mode),
                                         'sha256': r.sha(blob)}
            else:
                r.image_tools.put_file(expected, full, blob, mode, next_inode)
                expected[full][0][11] = len(full.encode('utf-8')) + 1
                new_manifest[rel] = {'bytes': len(blob), 'mode': oct(mode), 'sha256': r.sha(blob)}
            staged_info[rel] = {
                'bytes': len(blob), 'mode': oct(mode), 'sha256': r.sha(blob), 'replaced': replaced,
            }
        assert_no_duplicate_services(old)

        r.set_entry(expected, r.MANIFEST, r.dump(new_manifest))
        r.manifest_holds(expected, new_manifest)

        cpio, rd, pack = (scratch / n for n in ('new.cpio', 'new.lz4', 'payload.img'))
        raw = r.image_tools.pack_cpio_newc(expected)
        cpio.write_bytes(raw)
        r.image_tools.compress_ramdisk(cpio, rd, kind)
        require(args.count(str(old_rd)) == 1, 'Ambiguous ramdisk argument')
        flags = [str(rd) if arg == str(old_rd) else arg for arg in args]
        subprocess.run(['mkbootimg', *flags, '--vendor_boot', str(pack)],
                       check=True, capture_output=True)
        payload = pack.read_bytes()
        after_layout = r.layout(payload)
        require(after_layout['payload_end'] == len(payload) <=
                before_layout['payload_end'] + 4 * 1024 * 1024,
                f'Unexpected payload growth: {before_layout["payload_end"]} -> {after_layout["payload_end"]}')
        r.check_container(base, before_layout, payload, after_layout)
        image, updated = r.image_tools.preserve_and_update_avb_footer(payload, base)
        require(updated, 'Existing AVB footer metadata not updated')
        after_avb = r.verify_existing_avb(image, len(payload))
        require(all(after_avb[k] == before_avb[k] for k in ('vbmeta_masked', 'footer_masked')),
                'Unexpected AVB metadata change')

        verify = scratch / 'roundtrip.img'
        verify.write_bytes(image)
        got, got_raw, got_kind, _, _ = r.extract_entries(verify, image, after_layout, scratch / 'after')
        require(got == expected and got_raw == raw and got_kind == kind,
                'Exact unpack round-trip failed')

        for gm in GUARD_MEMBERS:
            require(got[gm][1] == old[gm][1], f'Protected member altered: {gm}')

        old_spans, old_trailer = archive.record_spans(old_raw)
        got_spans, got_trailer = archive.record_spans(got_raw)
        require(got_trailer == old_trailer, 'CPIO trailer changed')
        unchanged = set(old) - modified_members
        require(all(got_raw[slice(*got_spans[n])] == old_raw[slice(*old_spans[n])] for n in unchanged),
                'Unrelated CPIO record changed')

    cand_sha = r.sha(image)
    out_img = out / f'vendor_boot_h20_u4_dcm_soft_tee_{cand_sha[:8]}.img'
    out_img.write_bytes(image)
    report = {
        'base': base_id,
        'candidate': r.identity(out_img),
        'entries_before': len(old),
        'entries_after': len(expected),
        'guard_members_verified': list(GUARD_MEMBERS),
        'staged': staged_info,
        'dcm_token': 'local HMAC-SHA256 over serial/model/UDID-derived payload; not a Huawei attestation',
    }
    report_path = U4_BUILD / 'u4-build-report.json'
    report_path.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f'CANDIDATE_IMAGE={out_img}')
    print(f'CANDIDATE_SHA256={cand_sha}')
    print(f'CANDIDATE_SHA8={cand_sha[:8]}')
    print(f'BUILD_REPORT={report_path}')
    return out_img


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', type=Path, default=BASE_DEFAULT)
    parser.add_argument('--out', type=Path, default=Path('/mnt/e/marble-tmp'))
    args = parser.parse_args()
    build(args.base, args.out)


if __name__ == '__main__':
    main()
