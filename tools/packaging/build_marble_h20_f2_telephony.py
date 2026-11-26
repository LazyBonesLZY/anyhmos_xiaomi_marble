#!/usr/bin/env python3
"""Package Batch F2 clean-room QMI RIL + gated telephony SA on top of verified Batch F1 vendor_boot.

Changes ONLY vendor_boot_a ramdisk:
  - replaces graphics/hdf_default_phase4.hcb with 7-host HCB (adding gated riladapter_host preload=2)
  - updates graphics/marble_modem_probe (F2 commands)
  - adds graphics/libril_vendor_qmi.z.so, graphics/init-modem-f2-marble.cfg,
         graphics/telephony-marble.json, graphics/start-modem-f2-marble.sh
  - updates hmos-init-debug.cfg import list and graphics/stage-phase4-marble.sh
  - preserves all audio, BT, sensors, and F1 MSS files byte-for-byte identical.
"""

from __future__ import annotations

import argparse
import copy
import importlib.util
import json
import stat
import subprocess
import sys
import tempfile
from pathlib import Path

sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / 'tools/packaging'
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import build_marble_phase2_images as image_tools
import build_marble_phase4_images as p4

ORACLE = ROOT / 'work/marble-m15-role-control.MXDrsj/build_m15_role_control.py'
BASE_DEFAULT = Path('/mnt/e/marble-tmp/vendor_boot_h20_f1_modem_830aad72.img')
BASE_SHA_EXPECTED = '830aad7227a02490b5290ee0cbde0a83b2e143e9b8a07246da5a05b88fe0fb6f'
DONOR_FS = ROOT / 'work/donor-rom/ADY-AL10-207.0.0.111-SP10/fs'

PROBE_BIN = ROOT / 'work/marble-f2-build/marble_modem_probe'
RIL_LOADER_BIN = ROOT / 'work/marble-f2-build/native_ril_hdi_loader'
RIL_VENDOR_SO = ROOT / 'work/marble-f2-build/libril_vendor_qmi.z.so'
INIT_F2_CFG = ROOT / 'devices/marble/runtime/telephony/init-modem-f2-marble.cfg'
TELEPHONY_JSON = ROOT / 'devices/marble/runtime/telephony/telephony-marble.json'
START_F2_SH = ROOT / 'devices/marble/runtime/telephony/start-modem-f2-marble.sh'

SEED_MEMBER = 'hmos-compat-marble/hmos-init-debug.cfg'
MANIFEST_MEMBER = 'hmos-compat-marble/MANIFEST.json'
HCB_MEMBER = 'hmos-compat-marble/graphics/hdf_default_phase4.hcb'
STAGE_MEMBER = 'hmos-compat-marble/graphics/stage-phase4-marble.sh'
PROBE_MEMBER = 'hmos-compat-marble/graphics/marble_modem_probe'
RIL_LOADER_MEMBER = 'hmos-compat-marble/graphics/native_ril_hdi_loader'
RIL_SO_MEMBER = 'hmos-compat-marble/graphics/libril_vendor_qmi.z.so'
INIT_F2_MEMBER = 'hmos-compat-marble/graphics/init-modem-f2-marble.cfg'
TELEPHONY_MEMBER = 'hmos-compat-marble/graphics/telephony-marble.json'
START_F2_MEMBER = 'hmos-compat-marble/graphics/start-modem-f2-marble.sh'

F2_INIT_IMPORT = '/compat/etc/init-modem-f2-marble.cfg'
STAGE_ANCHOR = '$B test -x "$R/compat/bin/init"\necho MARBLE_PHASE4_RAM_COMPAT_READY'

GUARD_MEMBERS = [
    'hmos-compat-marble/graphics/libhmos_audio_vdi.so',
    'hmos-compat-marble/graphics/audio-runtime/lib64/libhmos_pal_server.so',
    'hmos-compat-marble/graphics/init-bluetooth-marble.cfg',
    'hmos-compat-marble/graphics/hmos_rmtfs',
    'hmos-compat-marble/graphics/start-modem-f1-marble.sh',
]

STAGE_F2_BLOCK = """# Batch F2: stage gated QMI RIL vendor lib, telephony profile/init, and vendorlib.path param override.
$B mkdir -p /compat/bin "$R/compat/bin" "$R/compat/lib64" "$R/compat/etc"
$B cp /graphics/libril_vendor_qmi.z.so "$R/compat/lib64/libril_vendor_qmi.z.so"
$B chmod 0644 "$R/compat/lib64/libril_vendor_qmi.z.so"
$B cp /graphics/native_ril_hdi_loader "$R/compat/bin/native_ril_hdi_loader"
$B chmod 0755 "$R/compat/bin/native_ril_hdi_loader"
$B cp /graphics/init-modem-f2-marble.cfg "$R/compat/etc/init-modem-f2-marble.cfg"
$B cp /graphics/telephony-marble.json "$R/compat/etc/telephony-marble.json"
$B chmod 0644 "$R/compat/etc/init-modem-f2-marble.cfg" "$R/compat/etc/telephony-marble.json"
$B cp /graphics/start-modem-f2-marble.sh /compat/bin/start-modem-f2-marble.sh
$B cp /graphics/start-modem-f2-marble.sh "$R/compat/bin/start-modem-f2-marble.sh"
$B chmod 0755 /compat/bin/start-modem-f2-marble.sh "$R/compat/bin/start-modem-f2-marble.sh"
ril_para_src="$R/chip_prod/etc/ril_param/common.para"
$B mkdir -p "$R/chip_prod/etc/ril_param"
printf 'const.sys.radio.vendorlib.path=/vendor/lib64/libril_vendor.z.so\\n' > "$R/compat/etc/ril_common.para"
$B chmod 0644 "$R/compat/etc/ril_common.para"
$B cp "$R/compat/etc/ril_common.para" "$ril_para_src"
$B chmod 0644 "$ril_para_src"
if $B test -f "$R/system/etc/param/telephony.para"; then
 $B chmod 0644 "$R/system/etc/param/telephony.para"
 if ! $B grep -q '^const\\.sys\\.radio\\.vendorlib\\.path=' "$R/system/etc/param/telephony.para"; then
  printf '\\nconst.sys.radio.vendorlib.path=/vendor/lib64/libril_vendor.z.so\\n' >> "$R/system/etc/param/telephony.para"
 fi
fi
"""


def _load_oracle():
    spec = importlib.util.spec_from_file_location('arc', ORACLE)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def patch_init_seed(seed_bytes: bytes) -> bytes:
    cfg = json.loads(seed_bytes.decode('utf-8'))
    imports = cfg.get('import', [])
    if not isinstance(imports, list):
        raise ValueError('hmos-init-debug.cfg missing import list')
    if F2_INIT_IMPORT not in imports:
        imports.append(F2_INIT_IMPORT)
    return (json.dumps(cfg, indent=2, ensure_ascii=True) + '\n').encode('utf-8')


def patch_stage_script(stage_bytes: bytes) -> bytes:
    text = stage_bytes.decode('utf-8')
    if 'Batch F2:' in text:
        raise ValueError('stage-phase4-marble.sh already contains Batch F2 block')
    if text.count(STAGE_ANCHOR) != 1:
        raise ValueError('Expected exactly one stage-phase4-marble.sh tail anchor')
    patched = text.replace(STAGE_ANCHOR, STAGE_F2_BLOCK + STAGE_ANCHOR, 1)
    return patched.encode('utf-8')


def _validate_inputs() -> dict[str, tuple[bytes, int]]:
    for path in (BASE_DEFAULT, PROBE_BIN, RIL_LOADER_BIN, RIL_VENDOR_SO, INIT_F2_CFG, TELEPHONY_JSON, START_F2_SH, ORACLE):
        if path.is_symlink() or not path.is_file():
            raise FileNotFoundError(f'Missing regular Batch F2 input: {path}')
    json.loads(INIT_F2_CFG.read_text(encoding='utf-8'))
    json.loads(TELEPHONY_JSON.read_text(encoding='utf-8'))
    subprocess.run(['sh', '-n', str(START_F2_SH)], check=True)
    return {
        PROBE_MEMBER: (PROBE_BIN.read_bytes(), 0o755),
        RIL_LOADER_MEMBER: (RIL_LOADER_BIN.read_bytes(), 0o755),
        RIL_SO_MEMBER: (RIL_VENDOR_SO.read_bytes(), 0o644),
        INIT_F2_MEMBER: (INIT_F2_CFG.read_bytes(), 0o644),
        TELEPHONY_MEMBER: (TELEPHONY_JSON.read_bytes(), 0o644),
        START_F2_MEMBER: (START_F2_SH.read_bytes(), 0o755),
    }


def _manifest_record(blob: bytes, mode: int) -> dict:
    return {'bytes': len(blob), 'mode': oct(mode), 'sha256': image_tools.sha256_bytes(blob)}


def build(base_img: Path, out_dir: Path, *, write_work_report: bool = False) -> Path:
    r = _load_oracle()
    out_dir.mkdir(parents=True, exist_ok=True)
    if base_img.is_symlink() or not base_img.is_file():
        raise FileNotFoundError(f'Missing base vendor_boot image: {base_img}')
    base_bytes = r.read_original(base_img)
    base_layout = r.layout(base_bytes)
    r.verify_existing_avb(base_bytes, base_layout['payload_end'])

    f2_inputs = _validate_inputs()
    hcb7 = p4.build_phase4_hdf_hcb(
        wifi_chip_donor=DONOR_FS,
        wpa_on_demand_donor=DONOR_FS,
        a2dp_donor=DONOR_FS,
        ril_donor=DONOR_FS,
    )

    with tempfile.TemporaryDirectory(prefix='f2build.', dir=out_dir) as tmp:
        scratch = Path(tmp)
        old_entries, old_cpio, compression, orig_rd, args = r.extract_entries(
            base_img, base_bytes, base_layout, scratch / 'base'
        )
        manifest = json.loads(old_entries[MANIFEST_MEMBER][1].decode('utf-8'))
        r.manifest_holds(old_entries, manifest)

        next_inode = [max(fields[0] for fields, _ in old_entries.values()) + 1]
        new_entries = dict(old_entries)

        new_seed = patch_init_seed(old_entries[SEED_MEMBER][1])
        new_stage = patch_stage_script(old_entries[STAGE_MEMBER][1])
        with tempfile.NamedTemporaryFile('w', dir=scratch, suffix='.sh', delete=False) as tf:
            tf.write(new_stage.decode('utf-8'))
            stage_tmp = tf.name
        subprocess.run(['sh', '-n', stage_tmp], check=True)

        modified_targets = {
            SEED_MEMBER: (new_seed, stat.S_IMODE(old_entries[SEED_MEMBER][0][1])),
            HCB_MEMBER: (hcb7, stat.S_IMODE(old_entries[HCB_MEMBER][0][1])),
            STAGE_MEMBER: (new_stage, stat.S_IMODE(old_entries[STAGE_MEMBER][0][1])),
            PROBE_MEMBER: f2_inputs[PROBE_MEMBER],
        }
        added_targets = {
            RIL_LOADER_MEMBER: f2_inputs[RIL_LOADER_MEMBER],
            RIL_SO_MEMBER: f2_inputs[RIL_SO_MEMBER],
            INIT_F2_MEMBER: f2_inputs[INIT_F2_MEMBER],
            TELEPHONY_MEMBER: f2_inputs[TELEPHONY_MEMBER],
            START_F2_MEMBER: f2_inputs[START_F2_MEMBER],
        }

        next_manifest = copy.deepcopy(manifest)
        for member, (blob, mode) in {**modified_targets, **added_targets}.items():
            image_tools.put_file(new_entries, member, blob, mode, next_inode)
            rel = member.removeprefix('hmos-compat-marble/')
            next_manifest[rel] = _manifest_record(blob, mode)

        manifest_bytes = r.dump(next_manifest)
        manifest_mode = stat.S_IMODE(old_entries[MANIFEST_MEMBER][0][1])
        image_tools.put_file(new_entries, MANIFEST_MEMBER, manifest_bytes, manifest_mode, next_inode)
        r.manifest_holds(new_entries, next_manifest)

        for g in GUARD_MEMBERS:
            if new_entries[g][1] != old_entries[g][1]:
                raise ValueError(f'Guard member unexpectedly changed: {g}')

        next_cpio = image_tools.pack_cpio_newc(new_entries)
        cpio_path = scratch / 'cand.cpio'
        cpio_path.write_bytes(next_cpio)
        cand_rd = scratch / 'cand.lz4'
        image_tools.compress_ramdisk(cpio_path, cand_rd, compression)

        repack_args = [str(cand_rd) if tok == str(orig_rd) else tok for tok in args]
        raw_payload_path = scratch / 'cand.payload'
        subprocess.run(['mkbootimg', *repack_args, '--vendor_boot', str(raw_payload_path)], check=True)
        payload = raw_payload_path.read_bytes()
        cand_layout = r.layout(payload)
        r.check_container(base_bytes, base_layout, payload, cand_layout)

        final_img, avb_updated = image_tools.preserve_and_update_avb_footer(payload, base_bytes)
        r.verify_existing_avb(final_img, cand_layout['payload_end'])

        img_sha = r.sha(final_img)
        out_img = out_dir / f'vendor_boot_h20_f2_telephony_{img_sha[:8]}.img'
        out_img.write_bytes(final_img)

        # Round-trip verify written image
        v_layout = r.layout(final_img)
        v_entries, _, _, _, _ = r.extract_entries(out_img, final_img, v_layout, scratch / 'verify')
        if len(v_entries) != len(new_entries):
            raise ValueError('Round-trip cpio entry count mismatch')

        unchanged = sum(
            1 for k, (_, b) in old_entries.items() if k in v_entries and v_entries[k][1] == b
        )
        modified = sorted(
            k for k, (_, b) in old_entries.items() if k in v_entries and v_entries[k][1] != b
        )
        added_report = {
            k.removeprefix('hmos-compat-marble/'): _manifest_record(v_entries[k][1], mode)
            for k, (_, mode) in added_targets.items()
        }
        report = {
            'base': {'path': str(base_img), 'bytes': len(base_bytes), 'sha256': r.sha(base_bytes)},
            'candidate': {'path': str(out_img), 'bytes': len(final_img), 'sha256': img_sha},
            'avb_footer_updated': avb_updated,
            'entries_before': len(old_entries),
            'entries_after': len(v_entries),
            'unchanged_entries': unchanged,
            'modified_entries': modified,
            'added_entries': added_report,
            'guards_unchanged': {g: r.sha(v_entries[g][1]) for g in GUARD_MEMBERS},
            'hdf_hcb7_sha256': r.sha(hcb7),
            'payload_bytes_before': base_layout['payload_end'],
            'payload_bytes_after': cand_layout['payload_end'],
        }
        report_bytes = r.dump(report)
        (out_dir / 'f2-build-report.json').write_bytes(report_bytes)
        if write_work_report:
            (ROOT / 'work/marble-f2-build/f2-build-report.json').write_bytes(report_bytes)
        return out_img


def main(argv=None) -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', type=Path, default=BASE_DEFAULT)
    parser.add_argument('--out', type=Path, default=Path('/mnt/e/marble-tmp'))
    args = parser.parse_args(argv)
    out_img = build(args.base, args.out, write_work_report=True)
    print(f'F2_BUILT={out_img}')


if __name__ == '__main__':
    main()
