#!/usr/bin/env python3
"""Package single-increment H20+D SLPI/sensor/auto-brightness overlay onto H20+B2 vendor_boot_a."""
import argparse
import copy
import importlib.util
import json
import subprocess
import tempfile
from hashlib import sha256
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / 'work'
BASE_DEFAULT = Path('/mnt/e/marble-tmp/vendor_boot_h20_b2_270d36fa.img')
BASE_SHA = '270d36fa9005c16cee0079a9806471fecd676dab45395702f08963e38fd7b552'
ORACLE = WORK / 'marble-m15-role-storage.1v2ynS/build_m15_role_storage.py'
ORACLE_SHA = '7d4f79857a3458ffdb7c8fb86b1cfd893fd9addb02b13e7e05d7b5fa7093cfaa'

D_BUILD = WORK / 'marble-d-build/sensors-out'
D_STOCK_TAR = WORK / 'marble-sensors-stock/marble-sensors-root.tar.gz'
D_CFG = ROOT / 'devices/marble/runtime/init/init-d-sensors-marble.cfg'
D_SCRIPTS = ROOT / 'devices/marble/runtime/sensors'

SEED_MEMBER = 'hmos-compat-marble/hmos-init-debug.cfg'
STAGE_MEMBER = 'hmos-compat-marble/graphics/stage-phase4-marble.sh'
START_MEMBER = 'hmos-compat-marble/graphics/start-phase4-marble.sh'
READY_PARAM = 'bootevent.hmos_sensor_hdi.ready'

ADDED_FILES = (
    ('graphics/init-d-sensors-marble.cfg', D_CFG, 0o644),
    ('graphics/sensors-marble.json', D_SCRIPTS / 'sensors-marble.json', 0o644),
    ('graphics/brightness_multiple_sensor_config-marble.json',
     D_SCRIPTS / 'brightness_multiple_sensor_config-marble.json', 0o644),
    ('graphics/start-sensors-marble.sh', D_SCRIPTS / 'start-sensors-marble.sh', 0o755),
    ('graphics/prepare-private-persist.sh', D_SCRIPTS / 'prepare-private-persist.sh', 0o755),
    ('graphics/stage-fastrpc-nodes.sh', D_SCRIPTS / 'stage-fastrpc-nodes.sh', 0o755),
    ('graphics/sensor_hdi_publisher', D_BUILD / 'sensor_hdi_publisher', 0o755),
    ('graphics/native_sensorspd_launcher', D_BUILD / 'native_sensorspd_launcher', 0o755),
    ('graphics/native_sensor_client_probe', D_BUILD / 'native_sensor_client_probe', 0o755),
    ('graphics/libsensor_interface_service_3.1.z.so', D_BUILD / 'libsensor_interface_service_3.1.z.so', 0o644),
    ('graphics/libsensor_utils.z.so', D_BUILD / 'libsensor_utils.z.so', 0o644),
    ('graphics/marble-sensors-root.tar.gz', D_STOCK_TAR, 0o644),
)

PARCEL_PATCHES = (
    (0x10c68, bytes.fromhex('81020291'), bytes.fromhex('81e20191')),
    (0x10c90, bytes.fromhex('81e20191'), bytes.fromhex('81f20191')),
    (0x10cb8, bytes.fromhex('81f20191'), bytes.fromhex('81020291')),
)

OPTIONAL_SA_B2 = (
    '4005 4016 1903 1905 1907 2802 3008 3203 3601 4300 5204 6001 8250 302 3506 3526 8003 10120 '
    '65862 65866 65936 65952 65962 65974 65993 66006 66048 66106 66134 '
    '66208 66234 66250 66288 66358 66532 66588 70633'
)
OPTIONAL_SA_D = (
    '4005 4016 1903 1905 1907 3008 3203 4300 5204 6001 8250 302 3506 3526 8003 10120 '
    '65862 65866 65936 65952 65962 65974 65993 66006 66048 66106 66134 '
    '66208 66234 66250 66288 66358 66532 66588 70633'
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


def patch_sensor_parcel(original: bytes) -> bytes:
    result = bytearray(original)
    for offset, before, after in PARCEL_PATCHES:
        require(original[offset:offset + 4] == before, f'Sensor parcel mismatch at {hex(offset)}')
        result[offset:offset + 4] = after
    return bytes(result)


def patch_seed(seed_bytes: bytes) -> bytes:
    old_anchor = b'    "/compat/etc/init-b2-apps-marble.cfg"\n  ],'
    new_anchor = (
        b'    "/compat/etc/init-b2-apps-marble.cfg",\n'
        b'    "/compat/etc/init-d-sensors-marble.cfg"\n'
        b'  ],'
    )
    require(b'init-d-sensors-marble.cfg' not in seed_bytes, 'Seed already imports init-d-sensors-marble.cfg')
    patched = splice_once(seed_bytes, ((old_anchor, new_anchor),), 'hmos-init-debug.cfg')
    doc = json.loads(patched)
    require('/compat/etc/init-d-sensors-marble.cfg' in doc['import'], 'Import missing after JSON parse')
    require(len(patched) < 32768, f'hmos-init-debug.cfg exceeds 32768-byte init cap: {len(patched)}')
    return patched


def patch_stage(stage_bytes: bytes) -> bytes:
    for marker in (
        b'init-d-sensors-marble.cfg',
        b'libsensor_utils.z.so',
        b'brightness_multiple_sensor_config-marble.json',
        b'marble_sensor.para.dac',
    ):
        require(marker not in stage_bytes, f'stage script already contains {marker!r}')
    sensor_stage = (
        b'# Batch D: Sensor HDI (V3_1 SSC bridge), parcel fix, auto-brightness config, param ACLs.\n'
        b'$B mkdir -p "$R/compat/bin" "$R/compat/lib64" "$R/compat/etc/display/brightness_config" "$R/compat/sensor-param"\n'
        b'$B cp /graphics/sensor_hdi_publisher "$R/compat/bin/native_sensor_hdi_publisher"\n'
        b'$B cp /graphics/native_sensorspd_launcher "$R/compat/bin/native_sensorspd_launcher"\n'
        b'$B cp /graphics/native_sensor_client_probe "$R/compat/bin/native_sensor_client_probe"\n'
        b'$B cp /graphics/libsensor_interface_service_3.1.z.so "$R/compat/lib64/libsensor_interface_service_3.1.z.so"\n'
        b'$B cp /graphics/libsensor_utils.z.so "$R/compat/lib64/libsensor_utils.z.so"\n'
        b'$B cp /graphics/sensors-marble.json "$R/compat/etc/sensors-marble.json"\n'
        b'$B cp /graphics/brightness_multiple_sensor_config-marble.json "$R/compat/etc/display/brightness_config/brightness_multiple_sensor_config.json"\n'
        b'$B chmod 0755 "$R/compat/bin/native_sensor_hdi_publisher" "$R/compat/bin/native_sensorspd_launcher" "$R/compat/bin/native_sensor_client_probe"\n'
        b'$B chmod 0644 "$R/compat/lib64/libsensor_interface_service_3.1.z.so" "$R/compat/lib64/libsensor_utils.z.so" '
        b'"$R/compat/etc/sensors-marble.json" "$R/compat/etc/display/brightness_config/brightness_multiple_sensor_config.json"\n'
        b'if $B test -f "$R/system/lib64/platformsdk/libsensor_utils.z.so"; then\n'
        b' $B mount --bind "$R/compat/lib64/libsensor_utils.z.so" "$R/system/lib64/platformsdk/libsensor_utils.z.so"\n'
        b'fi\n'
        b'if $B test -f "$R/sys_prod/etc/display/brightness_config/brightness_multiple_sensor_config.json"; then\n'
        b' $B mount --bind "$R/compat/etc/display/brightness_config/brightness_multiple_sensor_config.json" '
        b'"$R/sys_prod/etc/display/brightness_config/brightness_multiple_sensor_config.json"\n'
        b'fi\n'
        b'$B cp -a "$R/system/etc/param/." "$R/compat/sensor-param/"\n'
        + f"echo '{READY_PARAM} = sensor_host:sensor_host:0775:bool' > \"$R/compat/sensor-param/marble_sensor.para.dac\"\n".encode('utf-8')
        + b'$B chmod 0644 "$R/compat/sensor-param/marble_sensor.para.dac"\n'
        b'$B mount --bind "$R/compat/sensor-param" "$R/system/etc/param"\n'
        b'$B test -x "$R/compat/bin/init"\n'
    )
    pairs = (
        (
            b'for f in /compat/original-firmware/image/adsp2.* /compat/original-firmware/image/*.jsn; do\n',
            b'for f in /compat/original-firmware/image/adsp2.* /compat/original-firmware/image/slpi2.* /compat/original-firmware/image/*.jsn; do\n',
        ),
        (
            b' init-b2-apps-marble.cfg \\\n'
            b' index_insert_service-marble.json \\\n',
            b' init-b2-apps-marble.cfg \\\n'
            b' init-d-sensors-marble.cfg \\\n'
            b' index_insert_service-marble.json \\\n',
        ),
        (
            f"printf '%s\\n' '{OPTIONAL_SA_B2}' > \"$R/compat/etc/sceneboard-optional-sa.list\"\n".encode('utf-8'),
            f"printf '%s\\n' '{OPTIONAL_SA_D}' > \"$R/compat/etc/sceneboard-optional-sa.list\"\n".encode('utf-8'),
        ),
        (
            b'$B test -x "$R/compat/bin/init"\n',
            sensor_stage,
        ),
    )
    patched = splice_once(stage_bytes, pairs, 'stage-phase4-marble.sh')
    subprocess.run(['sh', '-n'], input=patched, check=True)
    return patched


def patch_start(start_bytes: bytes) -> bytes:
    require(b'start-sensors-marble.sh' not in start_bytes, 'start script already launches start-sensors-marble.sh')
    require(b'bootevent.hmos_sensor_hdi.ready' not in start_bytes, 'start script already waits for sensor HDI ready')
    pairs = (
        (
            b' log "WIFI_BG_STARTED pid=$wifi_bg_pid"\nfi\n',
            b' log "WIFI_BG_STARTED pid=$wifi_bg_pid"\nfi\n'
            b'if $B test -f /graphics/start-sensors-marble.sh; then\n'
            b' $B setsid $B sh /graphics/start-sensors-marble.sh > "$R/data/local/tmp/marble-sensors-startup.log" 2>&1 </dev/null &\n'
            b' log "SENSORS_BG_STARTED pid=$!"\n'
            b'fi\n',
        ),
        (
            b'pset marble.debug.start.hardware true\n',
            b'wait_param bootevent.hmos_sensor_hdi.ready true 20 || true\n'
            b'wait_sa 3601 15 || true\n'
            b'pset marble.debug.start.hardware true\n',
        ),
    )
    patched = splice_once(start_bytes, pairs, 'start-phase4-marble.sh')
    subprocess.run(['sh', '-n'], input=patched, check=True)
    return patched


def validate_inputs() -> None:
    donor_utils = ROOT / 'work/donor-rom/ADY-AL10-207.0.0.111-SP10/fs/system/system/lib64/platformsdk/libsensor_utils.z.so'
    for rel, src, _mode in ADDED_FILES:
        require(src.is_file(), f'Missing staged D artifact: {src}')
        raw_src = src.read_bytes()
        if rel.endswith('.json') or rel.endswith('.cfg'):
            json.loads(raw_src)
        elif rel.endswith('.sh'):
            subprocess.run(['sh', '-n'], input=raw_src, check=True)
        elif rel.endswith('.tar.gz'):
            require(raw_src[:2] == b'\x1f\x8b', f'Expected gzip archive for {rel}')
        elif rel == 'graphics/libsensor_utils.z.so' and donor_utils.is_file():
            require(raw_src == patch_sensor_parcel(donor_utils.read_bytes()),
                    'libsensor_utils.z.so does not match exact donor + 3-word parcel patch')
        else:
            require(raw_src[:4] == b'\x7fELF', f'Expected ELF binary for {rel}')
    cfg = json.loads(D_CFG.read_bytes())
    svc_names = {s['name'] for s in cfg['services']}
    require(svc_names == {'sensor_host', 'sensors'}, f'Unexpected init-d services: {svc_names}')
    bright = json.loads((D_SCRIPTS / 'brightness_multiple_sensor_config-marble.json').read_bytes())
    require(bright.get('isSupportBackSensor') is False, 'isSupportBackSensor must be False')


def build(base_path: Path, out: Path) -> Path:
    require(sha256(ORACLE.read_bytes()).hexdigest() == ORACLE_SHA, 'Archive verifier identity changed')
    spec = importlib.util.spec_from_file_location('h20_d_archive_verifier', ORACLE)
    archive = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(archive)
    r = archive.load_tools()

    base = r.read_original(base_path)
    require(r.sha(base) == BASE_SHA, f'Base SHA mismatch: got {r.sha(base)}, want {BASE_SHA}')
    base_id = r.identity(base_path)
    validate_inputs()

    before_layout = r.layout(base)
    before_avb = r.verify_existing_avb(base, before_layout['payload_end'])
    out.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix='pack.', dir=out) as tmp:
        scratch = Path(tmp)
        old, old_raw, kind, old_rd, args = r.extract_entries(base_path, base, before_layout, scratch / 'before')
        require(len(old) == 507, f'Unexpected 270d36fa ramdisk member count: {len(old)}')
        manifest = archive.parse(old[r.MANIFEST][1])
        r.manifest_holds(old, manifest)

        new_seed = patch_seed(old[SEED_MEMBER][1])
        new_stage = patch_stage(old[STAGE_MEMBER][1])
        new_start = patch_start(old[START_MEMBER][1])

        expected = dict(old)
        next_inode = [max(fields[0] for fields, _ in old.values()) + 1]
        new_manifest = copy.deepcopy(manifest)

        r.set_entry(expected, SEED_MEMBER, new_seed)
        r.set_entry(expected, STAGE_MEMBER, new_stage)
        r.set_entry(expected, START_MEMBER, new_start)
        new_manifest['hmos-init-debug.cfg'].update(bytes=len(new_seed), sha256=r.sha(new_seed))
        new_manifest['graphics/stage-phase4-marble.sh'].update(bytes=len(new_stage), sha256=r.sha(new_stage))
        new_manifest['graphics/start-phase4-marble.sh'].update(bytes=len(new_start), sha256=r.sha(new_start))

        added_info = {}
        for rel, src, mode in ADDED_FILES:
            full = f'{r.image_tools.COMPAT_DIR}/{rel}'
            require(full not in expected, f'Duplicate ramdisk member: {full}')
            blob = src.read_bytes()
            r.image_tools.put_file(expected, full, blob, mode, next_inode)
            expected[full][0][11] = len(full.encode('utf-8')) + 1
            new_manifest[rel] = {'bytes': len(blob), 'mode': oct(mode), 'sha256': r.sha(blob)}
            added_info[rel] = {'bytes': len(blob), 'mode': oct(mode), 'sha256': r.sha(blob)}

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

        old_spans, old_trailer = archive.record_spans(old_raw)
        got_spans, got_trailer = archive.record_spans(got_raw)
        require(got_trailer == old_trailer, 'CPIO trailer changed')
        modified = {SEED_MEMBER, STAGE_MEMBER, START_MEMBER, r.MANIFEST}
        unchanged = set(old) - modified
        require(all(got_raw[slice(*got_spans[n])] == old_raw[slice(*old_spans[n])] for n in unchanged),
                'Unrelated CPIO record changed')

    cand_sha = r.sha(image)
    out_img = out / f'vendor_boot_h20_d_sensors_{cand_sha[:8]}.img'
    out_img.write_bytes(image)
    report = {
        'base': base_id,
        'candidate': r.identity(out_img),
        'entries_before': len(old),
        'entries_after': len(expected),
        'unchanged_entries': len(unchanged),
        'modified_entries': sorted(modified),
        'added_entries': added_info,
        'seed_bytes': len(new_seed),
        'payload_bytes_before': before_layout['payload_end'],
        'payload_bytes_after': after_layout['payload_end'],
    }
    (out / 'd-build-report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report, indent=2))
    return out_img


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--base', type=Path, default=BASE_DEFAULT)
    ap.add_argument('--out', type=Path, required=True)
    args = ap.parse_args()
    build(args.base, args.out)


if __name__ == '__main__':
    main()
