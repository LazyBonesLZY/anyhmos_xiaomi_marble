#!/usr/bin/env python3
"""Package Batch U2.1 (Native HDF location_host + LPM GNSS, surface_post fastpath, Venus VPU fw+driver) onto U1.7 vendor_boot_a."""
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

import build_marble_phase4_images as p4

WORK = ROOT / 'work'
U2_BUILD = WORK / 'marble-u2-build'
DONOR_FS = WORK / 'donor-rom/ADY-AL10-207.0.0.111-SP10/fs'
CODEC_SCRIPTS = ROOT / 'devices/marble/runtime/codec'
TEL_SCRIPTS = ROOT / 'devices/marble/runtime/telephony'

BASE_DEFAULT = Path('/mnt/e/marble-tmp/vendor_boot_h20_u1_ux_security_7689087f.img')
BASE_SHA = '7689087fb0f78cd7553bb0ff6d7072a9989279a81d2d621b81c83536079bc7eb'
ORACLE = WORK / 'marble-m15-role-storage.1v2ynS/build_m15_role_storage.py'
ORACLE_SHA = '7d4f79857a3458ffdb7c8fb86b1cfd893fd9addb02b13e7e05d7b5fa7093cfaa'

STAGE_MEMBER = 'hmos-compat-marble/graphics/stage-phase4-marble.sh'
START_PHASE4_MEMBER = 'hmos-compat-marble/graphics/start-phase4-marble.sh'
HCB_MEMBER = 'hmos-compat-marble/graphics/hdf_default_phase4.hcb'
INIT_DEBUG_MEMBER = 'hmos-compat-marble/hmos-init-debug.cfg'

# Protected members from U1.7 / F5 / Audio / BT / Sensors / HUKS that U2.1 must preserve byte-for-byte.
GUARD_MEMBERS = (
    'hmos-compat-marble/graphics/libhmos_audio_vdi.so',
    'hmos-compat-marble/graphics/audio-runtime/lib64/libhmos_pal_server.so',
    'hmos-compat-marble/graphics/init-bluetooth-marble.cfg',
    'hmos-compat-marble/graphics/init-d-sensors-marble.cfg',
    'hmos-compat-marble/graphics/hmos_tqftpserv',
    'hmos-compat-marble/graphics/marble_modem_probe',
    'hmos-compat-marble/graphics/libril_vendor_qmi.z.so',
    'hmos-compat-marble/graphics/start-modem-f5-marble.sh',
    'hmos-compat-marble/graphics/hmos_rmtfs',
    'hmos-compat-marble/graphics/native_ril_hdi_loader',
    'hmos-compat-marble/graphics/telephony-marble.json',
    'hmos-compat-marble/graphics/libhuks_hdi_service_1.1.z.so',
    'hmos-compat-marble/graphics/libhuks_engine_core_standard.z.so',
    'hmos-compat-marble/graphics/libdbkeys.so',
    'hmos-compat-marble/graphics/libhmos_sceneboard_optional_sa.so',
    'hmos-compat-marble/graphics/init-u1-services-marble.cfg',
)

# Replaced or added ramdisk members under hmos-compat-marble/.
STAGED_FILES = (
    ('graphics/native_gnss_hdi_publisher', U2_BUILD / 'native_gnss_hdi_publisher', 0o755, True),
    ('graphics/libmarble_gnss_hdi_driver.z.so', U2_BUILD / 'libmarble_gnss_hdi_driver.z.so', 0o644, False),
    ('graphics/init-u2-location-marble.cfg', TEL_SCRIPTS / 'init-u2-location-marble.cfg', 0o644, False),
    ('graphics/libhmos_surface_diagnostic.so', U2_BUILD / 'libhmos_surface_diagnostic.so', 0o644, True),
    ('graphics/vpu20_4v-fw.tar.gz', U2_BUILD / 'vpu20_4v-fw.tar.gz', 0o644, False),
    ('graphics/start-venus-marble.sh', CODEC_SCRIPTS / 'start-venus-marble.sh', 0o755, False),
    ('graphics/start-u2-postboot-marble.sh', TEL_SCRIPTS / 'start-u2-postboot-marble.sh', 0o755, False),
)

STAGE_ANCHOR = b'$B test -x "$R/compat/bin/init"\necho MARBLE_PHASE4_RAM_COMPAT_READY'

STAGE_U2_BLOCK = (
    b'# === MARBLE_U2_LOC_MEDIA_STAGE_BEGIN ===\n'
    b'if $B test -f /graphics/libmarble_gnss_hdi_driver.z.so; then\n'
    b' $B cp /graphics/libmarble_gnss_hdi_driver.z.so "$R/compat/lib64/libmarble_gnss_hdi_driver.z.so"\n'
    b' $B chmod 0644 "$R/compat/lib64/libmarble_gnss_hdi_driver.z.so"\n'
    b'fi\n'
    b'if $B test -f /graphics/init-u2-location-marble.cfg; then\n'
    b' $B cp /graphics/init-u2-location-marble.cfg "$R/compat/etc/init-u2-location-marble.cfg"\n'
    b' $B chmod 0644 "$R/compat/etc/init-u2-location-marble.cfg"\n'
    b'fi\n'
    b'$B mkdir -p "$R/data/local/tmp/gnss-hdi"\n'
    b'$B chown 1022:1021 "$R/data/local/tmp/gnss-hdi" 2>/dev/null || true\n'
    b'$B chmod 0775 "$R/data/local/tmp/gnss-hdi" 2>/dev/null || true\n'
    b'$B rm -f "$R/data/local/tmp/gnss-hdi/status.log" 2>/dev/null || true\n'
    b'$B mkdir -p "$R/system/etc/param"\n'
    b'$B cat <<\'EOF\' > "$R/system/etc/param/marble_location.para"\n'
    b'const.location.gps.support=true\n'
    b'const.location.nlp_service_name=com.huawei.hmos.location\n'
    b'const.location.nlp_ability_name=LocationServiceExtensionAbility\n'
    b'const.location.geocode_service_name=com.huawei.hmos.location\n'
    b'const.location.geocode_ability_name=GeocodeServiceExtensionAbility\n'
    b'const.location.supl_mode=2\n'
    b'const.location.agnss_server_addr=supl.platform.dbankcloud.cn\n'
    b'const.location.agnss_server_port=7275\n'
    b'const.location.settings_bundle_name=com.huawei.hmos.security.privacycenter\n'
    b'EOF\n'
    b'$B chmod 0644 "$R/system/etc/param/marble_location.para"\n'
    b'if $B test -x /graphics/start-venus-marble.sh; then\n'
    b' $B cp /graphics/start-venus-marble.sh /compat/bin/start-venus-marble.sh\n'
    b' $B cp /graphics/start-venus-marble.sh "$R/compat/bin/start-venus-marble.sh"\n'
    b' $B chmod 0755 /compat/bin/start-venus-marble.sh "$R/compat/bin/start-venus-marble.sh"\n'
    b'fi\n'
    b'if $B test -x /graphics/start-u2-postboot-marble.sh; then\n'
    b' $B cp /graphics/start-u2-postboot-marble.sh /compat/bin/start-u2-postboot-marble.sh\n'
    b' $B cp /graphics/start-u2-postboot-marble.sh "$R/compat/bin/start-u2-postboot-marble.sh"\n'
    b' $B chmod 0755 /compat/bin/start-u2-postboot-marble.sh "$R/compat/bin/start-u2-postboot-marble.sh"\n'
    b'fi\n'
    b'u2_vpu_cnt=0\n'
    b'if $B test -f /graphics/vpu20_4v-fw.tar.gz; then\n'
    b' $B mkdir -p /compat/firmware "$R/compat/firmware"\n'
    b' $B tar -xzf /graphics/vpu20_4v-fw.tar.gz -C /compat/firmware\n'
    b' $B tar -xzf /graphics/vpu20_4v-fw.tar.gz -C "$R/compat/firmware"\n'
    b' $B chmod 0644 /compat/firmware/vpu20_4v.* "$R/compat/firmware/vpu20_4v."* 2>/dev/null || true\n'
    b' u2_vpu_cnt=$($B ls -1 /compat/firmware/vpu20_4v.* 2>/dev/null | $B wc -l | $B tr -d " \\t\\r\\n" || echo 0)\n'
    b'fi\n'
    b'echo "MARBLE_U2_STAGE_READY vpu_fw_cnt=$u2_vpu_cnt"\n'
    b'# === MARBLE_U2_LOC_MEDIA_STAGE_END ===\n'
)

START_END_ANCHOR = b'stage=complete\nlog "PHASE4_ORCHESTRATOR_DONE procs=$($B ps | $B wc -l)"'
START_POSTBOOT_BLOCK = (
    b'stage=u2_postboot\n'
    b'if $B test -x /compat/bin/start-u2-postboot-marble.sh; then\n'
    b' $B setsid $B sh /compat/bin/start-u2-postboot-marble.sh > "$R/data/local/tmp/marble-u2-postboot.log" 2>&1 </dev/null &\n'
    b' log "U2_POSTBOOT_LAUNCHED=1"\n'
    b'fi\n'
)


def require(cond, msg):
    if not cond:
        raise ValueError(msg)


def splice_once(data: bytes, pairs: tuple[tuple[bytes, bytes], ...], label: str) -> bytes:
    out = data
    for old, new in pairs:
        require(out.count(old) == 1, f'{label}: insertion anchor not unique (count={out.count(old)}): {old[:80]!r}')
        out = out.replace(old, new, 1)
    reverted = out
    for old, new in reversed(pairs):
        require(reverted.count(new) == 1, f'{label}: delta not separable')
        reverted = reverted.replace(new, old, 1)
    require(reverted == data, f'{label}: non-target bytes changed')
    return out


def patch_init_debug(raw: bytes) -> bytes:
    require(b'init-u2-location-marble.cfg' not in raw, 'init-debug already imports init-u2-location-marble.cfg')
    old_imp = b'    "/compat/etc/init-u1-services-marble.cfg"\n  ],'
    new_imp = b'    "/compat/etc/init-u1-services-marble.cfg",\n    "/compat/etc/init-u2-location-marble.cfg"\n  ],'
    out = splice_once(raw, ((old_imp, new_imp),), INIT_DEBUG_MEMBER)
    doc = json.loads(out.decode('utf-8'))
    require('/compat/etc/init-u2-location-marble.cfg' in doc['import'], 'U2 location init import missing after JSON parse')
    require(len(out) < 32768, f'hmos-init-debug.cfg exceeds 32768-byte init cap: {len(out)}')
    return out


def patch_stage(raw: bytes) -> bytes:
    for marker in (
        b'MARBLE_U2_LOC_MEDIA_STAGE_BEGIN',
        b'libmarble_gnss_hdi_driver.z.so',
        b'init-u2-location-marble.cfg',
        b'marble_location.para',
        b'start-venus-marble.sh',
        b'start-u2-postboot-marble.sh',
        b'vpu20_4v-fw.tar.gz',
    ):
        require(marker not in raw, f'stage script already contains {marker!r}')
    out = splice_once(raw, ((STAGE_ANCHOR, STAGE_U2_BLOCK + STAGE_ANCHOR),), STAGE_MEMBER)
    subprocess.run(['sh', '-n'], input=out, check=True)
    return out


def patch_start_phase4(raw: bytes) -> bytes:
    for marker in (b'U2_GNSS_EARLY_REQUESTED', b'U2_POSTBOOT_LAUNCHED', b'start-u2-postboot-marble.sh', b'marble.debug.start.gnss'):
        require(marker not in raw, f'start-phase4 already contains {marker!r}')
    out = splice_once(
        raw,
        ((START_END_ANCHOR, START_POSTBOOT_BLOCK + START_END_ANCHOR),),
        START_PHASE4_MEMBER,
    )
    subprocess.run(['sh', '-n'], input=out, check=True)
    return out


def validate_inputs() -> None:
    for rel, src, _mode, _replaces in STAGED_FILES:
        require(src.is_file(), f'Missing staged U2 artifact: {src}')
        raw = src.read_bytes()
        if rel.endswith('.sh'):
            subprocess.run(['sh', '-n'], input=raw, check=True)
        elif rel.endswith('.cfg'):
            json.loads(raw.decode('utf-8'))
        elif rel.endswith('.tar.gz'):
            require(raw[:2] == b'\x1f\x8b', f'Expected gzip tarball for {rel}')
        else:
            require(raw[:4] == b'\x7fELF', f'Expected ELF binary for {rel}')


def build(base_path: Path, out: Path) -> Path:
    require(sha256(ORACLE.read_bytes()).hexdigest() == ORACLE_SHA, 'Archive verifier identity changed')
    spec = importlib.util.spec_from_file_location('h20_u2_archive_verifier', ORACLE)
    archive = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(archive)
    r = archive.load_tools()

    base = r.read_original(base_path)
    require(r.sha(base) == BASE_SHA, f'Base SHA mismatch: got {r.sha(base)}, want {BASE_SHA}')
    base_id = r.identity(base_path)
    validate_inputs()
    require(DONOR_FS.is_dir(), f'Missing ADY donor fs directory: {DONOR_FS}')

    hcb7 = p4.build_phase4_hdf_hcb(
        wifi_chip_donor=DONOR_FS,
        wpa_on_demand_donor=DONOR_FS,
        a2dp_donor=DONOR_FS,
        ril_donor=DONOR_FS,
    )
    hcb8 = p4.build_phase4_hdf_hcb(
        wifi_chip_donor=DONOR_FS,
        wpa_on_demand_donor=DONOR_FS,
        a2dp_donor=DONOR_FS,
        ril_donor=DONOR_FS,
        location_host=True,
    )

    before_layout = r.layout(base)
    before_avb = r.verify_existing_avb(base, before_layout['payload_end'])
    out.mkdir(parents=True, exist_ok=True)
    U2_BUILD.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix='pack_u2.', dir=out) as tmp:
        scratch = Path(tmp)
        old, old_raw, kind, old_rd, args = r.extract_entries(base_path, base, before_layout, scratch / 'before')
        for gm in GUARD_MEMBERS:
            require(gm in old, f'Guard member missing in base image: {gm}')
        require(old[HCB_MEMBER][1] == hcb7, 'Base 7-host HCB mismatch')
        manifest = archive.parse(old[r.MANIFEST][1])
        r.manifest_holds(old, manifest)

        patched_members = {
            INIT_DEBUG_MEMBER: ('hmos-init-debug.cfg', patch_init_debug(old[INIT_DEBUG_MEMBER][1])),
            HCB_MEMBER: ('graphics/hdf_default_phase4.hcb', hcb8),
            STAGE_MEMBER: ('graphics/stage-phase4-marble.sh', patch_stage(old[STAGE_MEMBER][1])),
            START_PHASE4_MEMBER: ('graphics/start-phase4-marble.sh', patch_start_phase4(old[START_PHASE4_MEMBER][1])),
        }

        u2_cfg_obj = json.loads((TEL_SCRIPTS / 'init-u2-location-marble.cfg').read_text(encoding='utf-8'))
        u2_service_names = {s['name'] for s in u2_cfg_obj['services']}
        for member, (_, raw_member) in old.items():
            if member.endswith('.cfg'):
                m_bytes = patched_members[member][1] if member in patched_members else raw_member
                obj = json.loads(m_bytes.decode('utf-8'))
                existing_names = {s['name'] for s in obj.get('services', [])}
                overlap = existing_names & u2_service_names
                require(not overlap, f'Duplicate service name(s) in {member}: {sorted(overlap)}')

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
        for rel, src, mode, replaces in STAGED_FILES:
            full = f'{r.image_tools.COMPAT_DIR}/{rel}'
            blob = src.read_bytes()
            if replaces:
                require(full in expected, f'Expected existing member to replace: {full}')
                r.set_entry(expected, full, blob)
                modified_members.add(full)
                new_manifest[rel].update(bytes=len(blob), mode=oct(mode), sha256=r.sha(blob))
            else:
                require(full not in expected, f'Duplicate ramdisk member: {full}')
                r.image_tools.put_file(expected, full, blob, mode, next_inode)
                expected[full][0][11] = len(full.encode('utf-8')) + 1
                new_manifest[rel] = {'bytes': len(blob), 'mode': oct(mode), 'sha256': r.sha(blob)}
            staged_info[rel] = {'bytes': len(blob), 'mode': oct(mode), 'sha256': r.sha(blob), 'replaced': replaces}

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
        require(after_layout['payload_end'] == len(payload) <= before_layout['payload_end'] + 4 * 1024 * 1024,
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
        require(got == expected and got_raw == raw and got_kind == kind, 'Exact unpack round-trip failed')

        for gm in GUARD_MEMBERS:
            require(got[gm][1] == old[gm][1], f'Protected member altered: {gm}')

        old_spans, old_trailer = archive.record_spans(old_raw)
        got_spans, got_trailer = archive.record_spans(got_raw)
        require(got_trailer == old_trailer, 'CPIO trailer changed')
        unchanged = set(old) - modified_members
        require(all(got_raw[slice(*got_spans[n])] == old_raw[slice(*old_spans[n])] for n in unchanged),
                'Unrelated CPIO record changed')

    cand_sha = r.sha(image)
    out_img = out / f'vendor_boot_h20_u2_loc_media_{cand_sha[:8]}.img'
    out_img.write_bytes(image)
    report = {
        'base': base_id,
        'candidate': r.identity(out_img),
        'entries_before': len(old),
        'entries_after': len(expected),
        'guard_members_verified': list(GUARD_MEMBERS),
        'staged': staged_info,
    }
    report_path = U2_BUILD / 'u2-build-report.json'
    report_path.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f'CANDIDATE_IMAGE={out_img}')
    print(f'CANDIDATE_SHA256={cand_sha}')
    print(f'CANDIDATE_SHA8={cand_sha[:8]}')
    print(f'BUILD_REPORT={report_path}')
    return out_img


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--base', type=Path, default=BASE_DEFAULT)
    p.add_argument('--out', type=Path, default=Path('/mnt/e/marble-tmp'))
    args = p.parse_args()
    build(args.base, args.out)


if __name__ == '__main__':
    main()
