#!/usr/bin/env python3
"""Build single-increment Batch B1 (Platform Foundations + Boot Speed) vendor_boot delta on top of 0fc0cf63.

Preserves all unrelated H20 + bootanimation + Bluetooth ramdisk members, DTB,
bootconfig, and AVB footer structure, adding only:
  1. Persistent loopback `lo` (127.0.0.1/8, ::1/128) UP in stage-phase4-marble.sh
  2. `hmos_hisysevent_sink` daemon started early after hilogd to eliminate errno=2 storms
  3. Exact donor init definitions for `oaid_service` (6101), `deviceauth_service` (4701),
     and `locationhub` (2801-2805/4353) via imported `/compat/etc/init-b1-platform-marble.cfg`
  4. Expanded `libhmos_sceneboard_optional_sa.so` (128 slots) & updated allowlist of
     genuinely absent optional SAs causing 10-20s app stalls
  5. `/sys_prod/variant/region_comm/china` in `/compat/etc/cust/baselayer.cfg`
  6. Parallelized boot orchestration (`SA 1301` concurrent with graphics/audio;
     Wi-Fi + Bluetooth backgrounded concurrently with `stage=scene`; bounded polls
     replacing blind sleeps)
"""
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
BASE_DEFAULT = Path('/mnt/e/marble-tmp/vendor_boot_h20_bluetooth_0fc0cf63.img')
BASE_SHA = '0fc0cf63b146ca23775ec26900f20cd098c816ece47491b8b0d3683037bc65a3'
ORACLE = WORK / 'marble-m15-role-storage.1v2ynS/build_m15_role_storage.py'
ORACLE_SHA = '7d4f79857a3458ffdb7c8fb86b1cfd893fd9addb02b13e7e05d7b5fa7093cfaa'

B1_BUILD = WORK / 'marble-b1-build'
B1_CFG = ROOT / 'devices/marble/runtime/init/init-b1-platform-marble.cfg'

SEED_MEMBER = 'hmos-compat-marble/hmos-init-debug.cfg'
STAGE_MEMBER = 'hmos-compat-marble/graphics/stage-phase4-marble.sh'
START_MEMBER = 'hmos-compat-marble/graphics/start-phase4-marble.sh'
OPTIONAL_SA_MEMBER = 'hmos-compat-marble/graphics/libhmos_sceneboard_optional_sa.so'

ADDED_FILES = (
    ('graphics/hmos_hisysevent_sink', B1_BUILD / 'hmos_hisysevent_sink', 0o755),
    ('graphics/init-b1-platform-marble.cfg', B1_CFG, 0o644),
)

REPLACED_FILES = (
    ('graphics/libhmos_sceneboard_optional_sa.so', B1_BUILD / 'libhmos_sceneboard_optional_sa.so', 0o644),
)

OPTIONAL_SA_NEW = (
    '4005 4016 1903 1905 1907 2802 3008 3203 3601 4300 5204 6001 8250 302 3506 3526 8003 10120 '
    '65862 65866 65926 65936 65952 65962 65974 65993 66006 66048 66074 66106 66134 '
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


def patch_seed(seed_bytes: bytes) -> bytes:
    old_anchor = b'    "/compat/etc/init-bluetooth-marble.cfg"\n  ],'
    new_anchor = (
        b'    "/compat/etc/init-bluetooth-marble.cfg",\n'
        b'    "/compat/etc/init-b1-platform-marble.cfg"\n'
        b'  ],'
    )
    require(b'init-b1-platform-marble.cfg' not in seed_bytes, 'Seed already imports init-b1-platform-marble.cfg')
    patched = splice_once(seed_bytes, ((old_anchor, new_anchor),), 'hmos-init-debug.cfg')
    doc = json.loads(patched)
    require('/compat/etc/init-b1-platform-marble.cfg' in doc['import'], 'Import missing after JSON parse')
    require(len(patched) < 32768, f'hmos-init-debug.cfg exceeds 32768-byte init cap: {len(patched)}')
    return patched


def patch_stage(stage_bytes: bytes) -> bytes:
    pairs = (
        (
            b'echo MARBLE_PHASE4_STAGE_BEGIN\n',
            b'echo MARBLE_PHASE4_STAGE_BEGIN\n'
            b'# Bring up loopback (127.0.0.1/8, ::1/128) for local proxies, ArkWeb, and IPC.\n'
            b'$B ip addr add 127.0.0.1/8 dev lo 2>/dev/null || true\n'
            b'$B ip addr add ::1/128 dev lo 2>/dev/null || true\n'
            b'$B ip link set lo up 2>/dev/null || true\n'
            b'echo "MARBLE_LO_UP=$($B ip -4 addr show dev lo 2>/dev/null | $B grep -c \'127\\.0\\.0\\.1\' || echo 0)"\n',
        ),
        (
            b' start-wifi-marble.sh; do\n',
            b' hmos_hisysevent_sink \\\n'
            b' start-wifi-marble.sh; do\n',
        ),
        (
            b' init-bluetooth-marble.cfg \\\n',
            b' init-bluetooth-marble.cfg \\\n'
            b' init-b1-platform-marble.cfg \\\n',
        ),
        (
            b"printf '%s\\n' '4005 1903 1905 1907 2802 3008 3203 3601 65862 65926 65962 65974 66048 66106 66134 66250 66288 70633' > \"$R/compat/etc/sceneboard-optional-sa.list\"\n",
            f"printf '%s\\n' '{OPTIONAL_SA_NEW}' > \"$R/compat/etc/sceneboard-optional-sa.list\"\n".encode('utf-8'),
        ),
        (
            b'bl_src="$R/system/etc/cust/baselayer.cfg"\n'
            b'if $B test -f "$bl_src" && $B test -d "$R/preload/ADY-AL10/all/cn"; then\n'
            b' $B cp "$bl_src" "$R/compat/etc/cust/baselayer.cfg"\n'
            b" printf '\\n# marble compatibility: mounted ADY-AL10 preload policy\\n/preload/ADY-AL10/all/cn\\n' >> \"$R/compat/etc/cust/baselayer.cfg\"\n",
            b'bl_src="$R/system/etc/cust/baselayer.cfg"\n'
            b'if $B test -f "$bl_src" && $B test -d "$R/preload/ADY-AL10/all/cn"; then\n'
            b' $B cp "$bl_src" "$R/compat/etc/cust/baselayer.cfg"\n'
            b' if $B test -d "$R/sys_prod/variant/region_comm/china"; then\n'
            b"  printf '\\n# marble compatibility: ADY-AL10 china region_comm layer (themes/fonts/wallpapers/params)\\n/sys_prod/variant/region_comm/china\\n' >> \"$R/compat/etc/cust/baselayer.cfg\"\n"
            b' fi\n'
            b" printf '\\n# marble compatibility: mounted ADY-AL10 preload policy\\n/preload/ADY-AL10/all/cn\\n' >> \"$R/compat/etc/cust/baselayer.cfg\"\n",
        ),
    )
    patched = splice_once(stage_bytes, pairs, 'stage-phase4-marble.sh')
    subprocess.run(['sh', '-n'], input=patched, check=True)
    return patched


def patch_start(start_bytes: bytes) -> bytes:
    pairs = (
        (
            b'# Start continuous background hilog capture on ext4 /data.\n'
            b'$B setsid $B timeout 900 $B chroot "$R" /system/bin/hilog -L I,W,E,F > "$R/data/local/tmp/marble-p4-hilog.txt" 2>&1 </dev/null &\n',
            b'# Start hisysevent datagram sink before system services to eliminate errno=2 storms.\n'
            b'if $B test -x "$R/compat/bin/hmos_hisysevent_sink"; then\n'
            b' $B mkdir -p "$R/dev/unix/socket"\n'
            b' $B chmod 0755 "$R/dev/unix" "$R/dev/unix/socket" 2>/dev/null || true\n'
            b' $B setsid $B chroot "$R" /compat/bin/hmos_hisysevent_sink -q --socket-dir /dev/unix/socket --stats /data/local/tmp/hisysevent-sink.log > "$R/data/local/tmp/hisysevent-sink.out" 2>&1 </dev/null &\n'
            b' log "HISYSEVENT_SINK_STARTED=1"\n'
            b'fi\n\n'
            b'# Start continuous background hilog capture on ext4 /data.\n'
            b'$B setsid $B timeout 900 $B chroot "$R" /system/bin/hilog -L I,W,E,F > "$R/data/local/tmp/marble-p4-hilog.txt" 2>&1 </dev/null &\n',
        ),
        (
            b'# 4. Core framework & account & appspawn & useriam & UI support services.\n'
            b'stage=services\n'
            b'for gate in storage kvdata useriam account appspawn theme wallpaper ui diagnostics deviceproviders; do\n'
            b' pset "marble.debug.start.$gate" true\n'
            b'done\n'
            b'wait_sa 1301 60 || true\n',
            b'# 4. Core framework & account & appspawn & useriam & UI support services.\n'
            b'# Launch kvdata (SA 1301), B1 services (oaid/deviceauth/location), and QCA6490 Wi-Fi\n'
            b'# kernel/firmware bring-up concurrently with graphics/input/audio so both overlap\n'
            b'# cleanly and finish BEFORE stage=scene (zero race with foundation/SceneBoard).\n'
            b'stage=services\n'
            b'for gate in storage kvdata useriam account appspawn theme wallpaper ui diagnostics deviceproviders b1; do\n'
            b' pset "marble.debug.start.$gate" true\n'
            b'done\n'
            b'wifi_bg_pid=""\n'
            b'if $B test -x "$R/compat/bin/start-wifi-marble.sh"; then\n'
            b' $B timeout 50 $B sh "$R/compat/bin/start-wifi-marble.sh" > "$R/data/local/tmp/marble-wifi-startup.log" 2>&1 </dev/null &\n'
            b' wifi_bg_pid=$!\n'
            b' log "WIFI_BG_STARTED pid=$wifi_bg_pid"\n'
            b'fi\n',
        ),
        (
            b'# 6c. QCA6490 Wi-Fi (CNSS2 + MHI + wlan0 + wifi_host/wpa_host/wifi_manager) & Netstack.\n'
            b'stage=wifi_netstack\n'
            b'if $B test -x "$R/compat/bin/start-wifi-marble.sh"; then\n'
            b' if $B timeout 50 $B sh "$R/compat/bin/start-wifi-marble.sh" > "$R/data/local/tmp/marble-wifi-startup.log" 2>&1; then\n'
            b'  log "WIFI_READY=1"\n'
            b' else\n'
            b'  log "WIFI_START_WARN rc=$?"\n'
            b' fi\n'
            b'fi\n'
            b'pset marble.debug.start.netstack true\n'
            b'for id in 1120 1124 1126 1158 1151; do\n'
            b' wait_sa "$id" 12 || true\n'
            b'done\n\n'
            b'# 6d. QCA6490 Bluetooth (UART H4 bridge + OHOS HDI + bluetooth_service SA 1130).\n'
            b'stage=bluetooth\n'
            b'if $B test -x "$R/compat/bin/start-bluetooth-marble.sh"; then\n'
            b' if $B timeout 25 $B sh "$R/compat/bin/start-bluetooth-marble.sh" > "$R/data/local/tmp/marble-bluetooth-startup.log" 2>&1; then\n'
            b'  log "BLUETOOTH_READY=1"\n'
            b' else\n'
            b'  log "BLUETOOTH_START_WARN rc=$?"\n'
            b' fi\n'
            b'fi\n'
            b'wait_sa 1130 12 || true\n'
            b'wait_sa 1191 10 || true\n\n'
            b'# 7. Foundation (BMS + AMS + WMS + ScreenLock -> launches SceneBoard!).\n',
            b'# 6c. Join parallel QCA6490 Wi-Fi bring-up and bring up Netstack BEFORE SceneBoard.\n'
            b'stage=wifi_netstack\n'
            b'if [ -n "$wifi_bg_pid" ]; then\n'
            b' if wait "$wifi_bg_pid"; then\n'
            b'  log "WIFI_READY=1"\n'
            b' else\n'
            b'  log "WIFI_START_WARN rc=$?"\n'
            b' fi\n'
            b'fi\n'
            b'pset marble.debug.start.netstack true\n'
            b'for id in 1120 1124 1126 1158 1151; do\n'
            b' wait_sa "$id" 15 || true\n'
            b'done\n\n'
            b'# 6d. QCA6490 Bluetooth (UART H4 bridge + OHOS HDI + bluetooth_service SA 1130).\n'
            b'stage=bluetooth\n'
            b'if $B test -x "$R/compat/bin/start-bluetooth-marble.sh"; then\n'
            b' if $B timeout 25 $B sh "$R/compat/bin/start-bluetooth-marble.sh" > "$R/data/local/tmp/marble-bluetooth-startup.log" 2>&1; then\n'
            b'  log "BLUETOOTH_READY=1"\n'
            b' else\n'
            b'  log "BLUETOOTH_START_WARN rc=$?"\n'
            b' fi\n'
            b'fi\n'
            b'wait_sa 1130 12 || true\n'
            b'wait_sa 1191 10 || true\n\n'
            b'# Sync distributeddata (SA 1301) and oaid_service (SA 6101) before Foundation/SceneBoard.\n'
            b'stage=services_sync\n'
            b'wait_sa 1301 30 || true\n'
            b'wait_sa 6101 8 || true\n\n'
            b'# 7. Foundation (BMS + AMS + WMS + ScreenLock -> launches SceneBoard!).\n',
        ),
        (
            b'pset marble.debug.display.wake true\n'
            b'$B sleep 2\n'
            b'pset marble.debug.screen.unlock true\n'
            b'$B sleep 4\n',
            b'pset marble.debug.display.wake true\n'
            b'$B sleep 1\n'
            b'pset marble.debug.screen.unlock true\n'
            b'u_w=0\n'
            b'while [ "$u_w" -lt 10 ]; do\n'
            b' $B grep -q "SCREENLOCK_VERIFIED" "$R/data/local/tmp/hmos-capture/unlock-request.txt" 2>/dev/null && break\n'
            b' $B sleep 0.5\n'
            b' u_w=$((u_w + 1))\n'
            b'done\n',
        ),
        (
            b'pset bootevent.boot.completed true\n'
            b'log "BOOT_COMPLETED_EMITTED"\n'
            b'$B sleep 10\n'
            b'bootanim_finish\n',
            b'pset bootevent.boot.completed true\n'
            b'log "BOOT_COMPLETED_EMITTED"\n'
            b'ba_w=0\n'
            b'while [ "$ba_w" -lt 16 ]; do\n'
            b' [ "$(bootanim_get bootevent.bootanimation.finished || true)" = true ] && break\n'
            b' $B sleep 0.5\n'
            b' ba_w=$((ba_w + 1))\n'
            b'done\n'
            b'bootanim_finish\n',
        ),
        (
            b' deadline=$(($(now) + 60))\n'
            b' while :;\n'
            b' do\n' if False else
            b' deadline=$(($(now) + 60))\n'
            b' while :; do\n'
            b'  ready=1\n'
            b'  for id in 1301 401 4606 1120; do\n',
            b' deadline=$(($(now) + 95))\n'
            b' while :; do\n'
            b'  ready=1\n'
            b'  for id in 1301 401 4606 1120; do\n',
        ),
    )
    patched = splice_once(start_bytes, pairs, 'start-phase4-marble.sh')
    subprocess.run(['sh', '-n'], input=patched, check=True)
    return patched


def build(base_path: Path, out: Path) -> Path:
    require(sha256(ORACLE.read_bytes()).hexdigest() == ORACLE_SHA, 'Archive verifier identity changed')
    spec = importlib.util.spec_from_file_location('h20_b1_archive_verifier', ORACLE)
    archive = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(archive)
    r = archive.load_tools()

    base = r.read_original(base_path)
    require(r.sha(base) == BASE_SHA, f'Base SHA mismatch: got {r.sha(base)}, want {BASE_SHA}')
    base_id = r.identity(base_path)

    for rel, src, _mode in ADDED_FILES + REPLACED_FILES:
        require(src.is_file(), f'Missing staged B1 artifact: {src}')
        raw_src = src.read_bytes()
        if rel.endswith('.json') or rel.endswith('.cfg'):
            json.loads(raw_src)
        elif rel.endswith('.sh'):
            subprocess.run(['sh', '-n'], input=raw_src, check=True)
        else:
            require(raw_src[:4] == b'\x7fELF', f'Expected ELF binary for {rel}')

    before_layout = r.layout(base)
    before_avb = r.verify_existing_avb(base, before_layout['payload_end'])
    out.mkdir(parents=True, exist_ok=False)

    with tempfile.TemporaryDirectory(prefix='pack.', dir=out) as tmp:
        scratch = Path(tmp)
        old, old_raw, kind, old_rd, args = r.extract_entries(base_path, base, before_layout, scratch / 'before')
        require(len(old) == 501, f'Unexpected 0fc0cf63 ramdisk member count: {len(old)}')
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

        replaced_full = set()
        for rel, src, mode in REPLACED_FILES:
            full = f'{r.image_tools.COMPAT_DIR}/{rel}'
            require(full in expected, f'Expected existing ramdisk member to replace: {full}')
            blob = src.read_bytes()
            r.set_entry(expected, full, blob)
            new_manifest[rel].update(bytes=len(blob), sha256=r.sha(blob))
            replaced_full.add(full)

        added_full = set()
        added_info = {}
        for rel, src, mode in ADDED_FILES:
            full = f'{r.image_tools.COMPAT_DIR}/{rel}'
            require(full not in expected, f'Duplicate ramdisk member: {full}')
            blob = src.read_bytes()
            r.image_tools.put_file(expected, full, blob, mode, next_inode)
            expected[full][0][11] = len(full.encode('utf-8')) + 1
            new_manifest[rel] = {'bytes': len(blob), 'mode': oct(mode), 'sha256': r.sha(blob)}
            added_full.add(full)
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
        require(after_layout['payload_end'] == len(payload) <= before_layout['payload_end'] + 262144,
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
        modified = {SEED_MEMBER, STAGE_MEMBER, START_MEMBER, r.MANIFEST} | replaced_full
        unchanged = set(old) - modified
        require(all(got_raw[slice(*got_spans[n])] == old_raw[slice(*old_spans[n])] for n in unchanged),
                'Unrelated CPIO record changed')

    cand_sha = r.sha(image)
    out_img = out / f'vendor_boot_h20_b1_{cand_sha[:8]}.img'
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
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report, indent=2))
    return out_img


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--base', type=Path, default=BASE_DEFAULT)
    ap.add_argument('--out', type=Path, required=True)
    args = ap.parse_args()
    build(args.base, args.out)


if __name__ == '__main__':
    main()
