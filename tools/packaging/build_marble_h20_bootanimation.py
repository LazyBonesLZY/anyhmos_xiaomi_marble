#!/usr/bin/env python3
"""Build a host-only, animation-only vendor_boot delta from the exact H20 image.

Do not use the current Phase 4 source as a replacement for H20's full startup
script; its unrelated storage, WLAN, PAL and service stages differ.
"""
import argparse
import importlib.util
import subprocess
import tempfile
from hashlib import sha256
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / 'work'
BASE = WORK / 'marble-h19-yuv-native.1re8uo3y/vendor_boot_yuv_native_c687adec.img'
BASE_SHA = 'c687adec08b1b917fcbdab6fcc96caebc038aaabca43503dc1def0ceb9953079'
START_SHA = '8841ee1c180064d0150c965916e2f0f12f1ad7e50aa89890fe59cd763dfe6466'
ORACLE = WORK / 'marble-m15-role-storage.1v2ynS/build_m15_role_storage.py'
ORACLE_SHA = '7d4f79857a3458ffdb7c8fb86b1cfd893fd9addb02b13e7e05d7b5fa7093cfaa'
SOURCE = ROOT / 'devices/marble/runtime/boot/start-phase4-marble.sh'
MEMBER = 'graphics/start-phase4-marble.sh'
BEGIN = b'# BEGIN marble native boot animation\n'
END = b'# END marble native boot animation\n'
AUDIO_TO_WIFI = b'wait_sa 3011 10 || true\n\n# 6c. QCA6490 Wi-Fi'
WAKE = b'stage=wake_unlock\npset marble.debug.display.wake true\n'
HANDOFF = b'log "BOOT_COMPLETED_EMITTED"\n$B sleep 10\n\nstage=capture\n'


def require(condition, message):
    if not condition:
        raise ValueError(message)


def splice_animation(baseline, source):
    require(source.count(BEGIN) == source.count(END) == 1, 'Expected one marked animation block')
    begin, end = source.index(BEGIN), source.index(END) + len(END)
    require(begin < end and BEGIN not in baseline and END not in baseline, 'Invalid animation block order')
    block = source[begin:end]
    require(block.endswith(b'\nbootanim_start\n' + END), 'Missing original-player trigger')
    require(source.count(b'stage=wake_unlock\nbootanim_rearm_wake\n') == 1 and
            source.count(b'log "BOOT_COMPLETED_EMITTED"\n$B sleep 10\nbootanim_finish\n') == 1,
            'Missing animation handoff in source')
    pairs = (
        (AUDIO_TO_WIFI, AUDIO_TO_WIFI.replace(b'# 6c.', block + b'\n# 6c.', 1)),
        (WAKE, WAKE.replace(b'\npset ', b'\nbootanim_rearm_wake\npset ', 1)),
        (HANDOFF, HANDOFF.replace(b'\n\nstage=', b'\nbootanim_finish\n\nstage=', 1)),
    )
    result = baseline
    for old, new in pairs:
        require(result.count(old) == 1, 'H20 startup insertion point is not unique')
        result = result.replace(old, new, 1)
    reverted = result
    for old, new in reversed(pairs):
        require(reverted.count(new) == 1, 'Animation delta is not separable')
        reverted = reverted.replace(new, old, 1)
    require(reverted == baseline, 'Non-animation startup bytes changed')
    return result


def build(base_path, out):
    require(sha256(ORACLE.read_bytes()).hexdigest() == ORACLE_SHA, 'Archive verifier identity changed')
    spec = importlib.util.spec_from_file_location('h20_animation_archive_verifier', ORACLE)
    archive = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(archive)
    r = archive.load_tools()
    base, base_id = archive.read_pinned(r, base_path, BASE_SHA)
    before_layout = r.layout(base)
    before_avb = r.verify_existing_avb(base, before_layout['payload_end'])
    out.mkdir(parents=True, exist_ok=False)
    with tempfile.TemporaryDirectory(prefix='pack.', dir=out) as tmp:
        scratch = Path(tmp)
        old, old_raw, kind, old_rd, args = r.extract_entries(base_path, base, before_layout, scratch/'before')
        require(len(old) == 493, 'Unexpected H20 ramdisk member count')
        manifest = archive.parse(old[r.MANIFEST][1])
        r.manifest_holds(old, manifest)
        member = r.image_tools.COMPAT_DIR + '/' + MEMBER
        original = old[member][1]
        require(r.sha(original) == START_SHA, 'Unexpected original H20 start script')
        source = SOURCE.read_bytes()
        script = splice_animation(original, source)
        subprocess.run(['sh', '-n'], input=script, check=True)
        expected = dict(old)
        r.set_entry(expected, member, script)
        manifest[MEMBER].update(bytes=len(script), sha256=r.sha(script))
        r.set_entry(expected, r.MANIFEST, r.dump(manifest))
        r.manifest_holds(expected, manifest)
        cpio, rd, pack = (scratch / n for n in ('new.cpio', 'new.lz4', 'payload.img'))
        raw = r.image_tools.pack_cpio_newc(expected)
        cpio.write_bytes(raw)
        r.image_tools.compress_ramdisk(cpio, rd, kind)
        require(args.count(str(old_rd)) == 1, 'Ambiguous ramdisk argument')
        flags = [str(rd) if arg == str(old_rd) else arg for arg in args]
        subprocess.run(['mkbootimg', *flags, '--vendor_boot', str(pack)], check=True, capture_output=True)
        payload = pack.read_bytes()
        after_layout = r.layout(payload)
        require(after_layout['payload_end'] == len(payload) <= before_layout['payload_end'] + 4096,
                'Unexpected alignment or payload growth')
        r.check_container(base, before_layout, payload, after_layout)
        image, updated = r.image_tools.preserve_and_update_avb_footer(payload, base)
        require(updated, 'Existing footer metadata not updated')
        after_avb = r.verify_existing_avb(image, len(payload))
        require(all(after_avb[key] == before_avb[key] for key in ('vbmeta_masked', 'footer_masked')),
                'Unexpected AVB metadata change')
        verify = scratch/'roundtrip.img'
        verify.write_bytes(image)
        got, got_raw, got_kind, _, _ = r.extract_entries(verify, image, after_layout, scratch/'after')
        require(got == expected and got_raw == raw and got_kind == kind, 'Exact unpack round-trip failed')
        delta = archive.verify_raw_archive(old_raw, got_raw, {member, r.MANIFEST})
        target = out/('vendor_boot_h20_bootanimation_' + r.sha(image)[:8] + '.img')
        with target.open('xb') as f:
            f.write(image)
        report = {
            'scope': 'startup animation only, H20 base preserved', 'base': base_id, 'output': r.identity(target),
            'archive': delta, 'layout': after_layout, 'start_before_sha256': START_SHA,
            'start_after_sha256': r.sha(script), 'animation_source_sha256': r.sha(source),
            'unrelated_start_script_bytes_verified': True, 'dtb_and_bootconfig_unchanged': True,
            'render_turnip_and_asset_records_unchanged': True, 'runtime_tested': False, 'flashed': False,
        }
        (out/'report.json').write_bytes(r.dump(report))
        (out/'start-phase4-marble.effective.sh').write_bytes(script)
    return target


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument('--base', type=Path, default=BASE)
    cli.add_argument('--out', type=Path, required=True, help='New output directory; no Windows transfer copy')
    args = cli.parse_args()
    print(build(args.base.resolve(strict=True), args.out.resolve()))


if __name__ == '__main__':
    main()
