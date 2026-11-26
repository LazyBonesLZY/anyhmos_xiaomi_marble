#!/usr/bin/env python3
"""Package Batch M6 (Genuine OpenHarmony Kernel code_sign + ECDSA P-256/P-384 fs-verity + key_enable + non-destructive ext4 RO_COMPAT_VERITY) onto M5 boot_a & vendor_boot_a."""
from __future__ import annotations

import argparse
import copy
import hashlib
import importlib.util
import json
import subprocess
import sys
import tempfile
from pathlib import Path

sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[2]
SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

from build_marble_phase2_images import (
    BOOT_LIMIT_BYTES,
    attach_boot_v4_signature,
    preserve_and_update_avb_footer,
    replace_arg_value,
    unpack_with_args,
)

M6_BUILD = ROOT / 'work/marble-m6-build'
KERNEL_IMAGE_DEFAULT = ROOT / 'work/marble-kernel-work/kernel-out/arch/arm64/boot/Image'
KERNEL_SYMVERS_DEFAULT = ROOT / 'work/marble-kernel-work/kernel-out/vmlinux.symvers'
KERNEL_RELEASE_DEFAULT = ROOT / 'work/marble-kernel-work/kernel-out/include/config/kernel.release'
VENDOR_DLKM_DIR_DEFAULT = ROOT / 'work/marble-kernel-work/vendor_dlkm_modules'

ORACLE = ROOT / 'work/marble-m15-role-storage.1v2ynS/build_m15_role_storage.py'
ORACLE_SHA = '7d4f79857a3458ffdb7c8fb86b1cfd893fd9addb02b13e7e05d7b5fa7093cfaa'

BOOT_BASE_DEFAULT = ROOT / 'work/marble-phase4-m15.BTQ5GO/boot_phase4.img'
BOOT_BASE_SHA = 'c83a8468955ca6584df29b6e5b7cba27c4b27ae5563a43401566bad301c9cae8'

VB_BASE_DEFAULT = Path('/mnt/e/marble-tmp/vendor_boot_h20_m5_fixes_c4b6a64f.img')
VB_BASE_SHA = 'c4b6a64f857fac5f144662b7240ef147c0687a77f87c0e60b6fac6fbf23a2f07'

PREFIX = 'hmos-compat-marble/'
INIT_DEBUG_MEMBER = PREFIX + 'hmos-init-debug.cfg'
DONOR_PATCHES_MEMBER = PREFIX + 'graphics/apply-donor-patches-phase4.sh'
USERDATA_MEMBER = PREFIX + 'graphics/init-userdata-ext4.sh'
STAGE_MEMBER = PREFIX + 'graphics/stage-phase4-marble.sh'
START_MEMBER = PREFIX + 'graphics/start-phase4-marble.sh'

CODESIGN_CFG = ROOT / 'devices/marble/runtime/codesign/init-m6-codesign-marble.cfg'
EXT4_VERITY_BIN = M6_BUILD / 'marble_ext4_verity_enable'

GUARD_MEMBERS = (
    PREFIX + 'graphics/libhmos_ram_no_tee.so',
    PREFIX + 'graphics/hdf_default_phase4.hcb',
    PREFIX + 'graphics/foundation-app-core.json',
    PREFIX + 'graphics/libpin_auth_interface_service_3.0.z.so',
    PREFIX + 'graphics/libpin_auth_driver.z.so',
    PREFIX + 'graphics/marble_touch_wakeup',
    PREFIX + 'graphics/init-m1-touch-wakeup-marble.cfg',
    PREFIX + 'graphics/libmarble_motion_interface_service_1.1.z.so',
    PREFIX + 'graphics/libmarble_vibrator_interface_service_2.0.z.so',
    PREFIX + 'graphics/hmos_ims_core_service',
    PREFIX + 'graphics/libril_vendor_qmi.z.so',
    PREFIX + 'graphics/libmarble_camera_vdi_impl.z.so',
    PREFIX + 'graphics/marble_camera_hw_capture',
    PREFIX + 'graphics/init-m5-aod-marble.cfg',
    PREFIX + 'graphics/aod-marble.json',
    PREFIX + 'hmos-ipc.ko',
    PREFIX + 'hmos-dmabuf-sync.ko',
)

STAGED_ADD = (
    ('graphics/init-m6-codesign-marble.cfg', CODESIGN_CFG, 0o644),
    ('graphics/marble_ext4_verity_enable', EXT4_VERITY_BIN, 0o755),
)

INIT_IMP_OLD = b'    "/compat/etc/init-m5-aod-marble.cfg"\n  ],'
INIT_IMP_NEW = (
    b'    "/compat/etc/init-m5-aod-marble.cfg",\n'
    b'    "/compat/etc/init-m6-codesign-marble.cfg"\n  ],'
)

DONOR_CODESIGN_PATCH_OLD = (
    b'apply_one "$R/system/lib64/libcode_sign_utils.z.so" '
    b'"$R/compat/lib64/libcode_sign_utils.z.so" "0644" '
    b'"2dd39556056de2d5eb2d8d2696316c616a8645c71c3a26702b0c0b351dd86158" '
    b'"61ccb5aa63c462e02a199c221059dcd41c873463b556bd1a5c1d83763c323e30" '
    b'"1" 69472 "CACAUg=="\n'
)
DONOR_CODESIGN_PATCH_NEW = (
    b'# MARBLE_M6: legacy libcode_sign_utils.z.so fs-verity bypass removed; '
    b'authentic donor library active\n'
)

USERDATA_MOUNT_ANCHOR = (
    b'$B umount "$R/data"\n'
    b'$B mount -t ext4 -o rw,nosuid,nodev,noatime "$U" "$R/data" || fail \'ext4 mount failed\''
)
USERDATA_M6_VERITY_NEW = (
    b'$B umount "$R/data"\n'
    b'# === MARBLE_M6_USERDATA_VERITY_BEGIN ===\n'
    b'if $B test -x /graphics/marble_ext4_verity_enable; then\n'
    b' v_rc=0\n'
    b' /graphics/marble_ext4_verity_enable "$U" || v_rc=$?\n'
    b' if [ "$v_rc" -eq 20 ]; then\n'
    b'  echo "HMOS_USERDATA_VERITY_REPLAY_JOURNAL=1"\n'
    b'  if $B mount -t ext4 -o rw,nosuid,nodev,noatime "$U" "$R/data"; then\n'
    b'   $B sync\n'
    b'   $B umount "$R/data"\n'
    b'   /graphics/marble_ext4_verity_enable "$U" || echo "HMOS_USERDATA_VERITY_WARN rc=$?"\n'
    b'  else\n'
    b'   echo "HMOS_USERDATA_VERITY_REPLAY_MOUNT_WARN=1"\n'
    b'  fi\n'
    b' elif [ "$v_rc" -ne 0 ]; then\n'
    b'  echo "HMOS_USERDATA_VERITY_WARN rc=$v_rc"\n'
    b' fi\n'
    b'fi\n'
    b'# === MARBLE_M6_USERDATA_VERITY_END ===\n'
    b'$B mount -t ext4 -o rw,nosuid,nodev,noatime "$U" "$R/data" || fail \'ext4 mount failed\''
)

STAGE_ANCHOR = b'$B test -x "$R/compat/bin/init"\necho MARBLE_PHASE4_RAM_COMPAT_READY'
STAGE_M6_BLOCK = (
    b'# === MARBLE_M6_CODESIGN_STAGE_BEGIN ===\n'
    b'$B mkdir -p "$R/compat/etc"\n'
    b'if $B test -f /graphics/init-m6-codesign-marble.cfg; then\n'
    b' $B cp /graphics/init-m6-codesign-marble.cfg "$R/compat/etc/init-m6-codesign-marble.cfg"\n'
    b' $B chmod 0644 "$R/compat/etc/init-m6-codesign-marble.cfg"\n'
    b'fi\n'
    b'mk_char_sys /sys/class/misc/code_sign "$R/dev/code_sign" 0 3060 0660\n'
    b'echo "MARBLE_M6_CODESIGN_STAGED=1 dev=$([ -c "$R/dev/code_sign" ] && echo ok || echo missing)"\n'
    b'# === MARBLE_M6_CODESIGN_STAGE_END ===\n'
)

START_INSTALLS_ANCHOR = b'pset marble.debug.start.installs true'
START_M6_CODESIGN_BLOCK = (
    b'# === MARBLE_M6_CODESIGN_START_BEGIN ===\n'
    b'cs_skip=$(pget marble.debug.skip.codesign)\n'
    b'cs_skip_list=$($B cat "$R/data/local/tmp/marble-skip-stages" 2>/dev/null | $B tr -d \' \\t\\r\\n\' || true)\n'
    b'case ",$cs_skip_list," in\n'
    b' *,codesign,*|*,key_enable,*) cs_skip=true ;;\n'
    b'esac\n'
    b'if [ "$cs_skip" = true ] || [ "$cs_skip" = 1 ]; then\n'
    b' log "M6_CODESIGN_SKIP=requested"\n'
    b'else\n'
    b' $B mkdir -p "$R/data/service/el1/public/profiles/release" "$R/data/service/el1/public/profiles/debug"\n'
    b' $B chown 3060:3060 "$R/data/service/el1/public/profiles" "$R/data/service/el1/public/profiles/release" "$R/data/service/el1/public/profiles/debug"\n'
    b' $B chmod 0755 "$R/data/service/el1/public/profiles" "$R/data/service/el1/public/profiles/release" "$R/data/service/el1/public/profiles/debug"\n'
    b' if $B test -w /proc/sys/fs/verity/require_signatures; then\n'
    b'  echo 1 > /proc/sys/fs/verity/require_signatures 2>/dev/null || true\n'
    b' fi\n'
    b' if $B test -w /proc/sys/fs/verity/developer_mode; then\n'
    b'  if [ "$(pget const.security.developermode.state)" = true ]; then\n'
    b'   echo 1 > /proc/sys/fs/verity/developer_mode 2>/dev/null || true\n'
    b'  else\n'
    b'   echo 0 > /proc/sys/fs/verity/developer_mode 2>/dev/null || true\n'
    b'  fi\n'
    b' fi\n'
    b' pset marble.debug.start.codesign true\n'
    b' cs_i=0\n'
    b' while [ "$cs_i" -lt 15 ]; do\n'
    b'  $B grep -q \'fs_verity_key\' /proc/keys 2>/dev/null && break\n'
    b'  cs_i=$((cs_i + 1))\n'
    b'  $B sleep 0.3\n'
    b' done\n'
    b' log "M6_CODESIGN_READY keys=$($B grep -c \'fs_verity_key\' /proc/keys 2>/dev/null || echo 0) dev=$([ -c "$R/dev/code_sign" ] && echo ok || echo missing) wait_steps=$cs_i"\n'
    b'fi\n'
    b'# === MARBLE_M6_CODESIGN_START_END ===\n'
)


def require(cond: bool, msg: str) -> None:
    if not cond:
        raise ValueError(msg)


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


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
    require(b'init-m6-codesign-marble.cfg' not in raw, 'hmos-init-debug.cfg already has init-m6-codesign-marble.cfg')
    out = splice_once(raw, ((INIT_IMP_OLD, INIT_IMP_NEW),), INIT_DEBUG_MEMBER)
    doc = json.loads(out.decode('utf-8'))
    require('/compat/etc/init-m6-codesign-marble.cfg' in doc.get('import', []), 'Missing M6 codesign import')
    require(len(out) < 32768, f'hmos-init-debug.cfg exceeds 32768B cap: {len(out)}')
    return out


def patch_donor_patches(raw: bytes) -> bytes:
    require(b'MARBLE_M6: legacy libcode_sign_utils.z.so' not in raw, 'apply-donor-patches-phase4.sh already patched')
    out = splice_once(raw, ((DONOR_CODESIGN_PATCH_OLD, DONOR_CODESIGN_PATCH_NEW),), DONOR_PATCHES_MEMBER)
    require(b'CACAUg==' not in out, 'Legacy libcode_sign_utils.z.so bypass still present')
    require(b'\r' not in out, 'CR byte in patched apply-donor-patches-phase4.sh')
    subprocess.run(['sh', '-n'], input=out, check=True)
    return out


def patch_userdata(raw: bytes) -> bytes:
    require(b'MARBLE_M6_USERDATA_VERITY_BEGIN' not in raw, 'init-userdata-ext4.sh already patched')
    out = splice_once(raw, ((USERDATA_MOUNT_ANCHOR, USERDATA_M6_VERITY_NEW),), USERDATA_MEMBER)
    require(b'\r' not in out, 'CR byte in patched init-userdata-ext4.sh')
    subprocess.run(['sh', '-n'], input=out, check=True)
    return out


def patch_stage(raw: bytes) -> bytes:
    for m in (b'MARBLE_M6_CODESIGN_STAGE_BEGIN', b'init-m6-codesign-marble.cfg', b'/dev/code_sign'):
        require(m not in raw, f'stage script already contains {m!r}')
    require(b'MARBLE_M5_AOD_STAGE_BEGIN' in raw, 'Expected M5 stage marker')
    out = splice_once(raw, ((STAGE_ANCHOR, STAGE_M6_BLOCK + STAGE_ANCHOR),), STAGE_MEMBER)
    require(b'\r' not in out, 'CR byte in patched stage-phase4-marble.sh')
    subprocess.run(['sh', '-n'], input=out, check=True)
    return out


def patch_start(raw: bytes) -> bytes:
    require(b'MARBLE_M6_CODESIGN_START_BEGIN' not in raw, 'start script already patched for M6')
    require(b'MARBLE_M5_AOD_START_BEGIN' in raw, 'Expected M5 start marker')
    out = splice_once(
        raw,
        ((START_INSTALLS_ANCHOR, START_M6_CODESIGN_BLOCK + START_INSTALLS_ANCHOR),),
        START_MEMBER,
    )
    require(b'\r' not in out, 'CR byte in patched start-phase4-marble.sh')
    subprocess.run(['sh', '-n'], input=out, check=True)
    return out


def load_oracle():
    require(ORACLE.is_file() and ORACLE.resolve() == ORACLE, f'Missing oracle: {ORACLE}')
    got_sha = hashlib.sha256(ORACLE.read_bytes()).hexdigest()
    require(got_sha == ORACLE_SHA, f'Oracle SHA256 mismatch: {got_sha}')
    spec = importlib.util.spec_from_file_location('archive_oracle_m6', ORACLE)
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


def audit_kabi(symvers_path: Path, release: str, vb_entries: dict, dlkm_dir: Path, scratch: Path) -> dict:
    """Verify EVERY .ko module in vendor_boot AND vendor_dlkm matches the new kernel vmlinux.symvers & release."""
    vmlinux_syms = {}
    for line in symvers_path.read_text(encoding='utf-8').splitlines():
        parts = line.split()
        if len(parts) >= 2:
            vmlinux_syms[parts[1]] = int(parts[0], 16)

    ko_dir = scratch / 'vb_kos'
    ko_dir.mkdir(parents=True, exist_ok=True)
    ko_files: list[tuple[str, Path]] = []
    for member, (_hdr, blob) in sorted(vb_entries.items()):
        if member.endswith('.ko'):
            dst = ko_dir / member.replace('/', '__')
            dst.write_bytes(blob)
            ko_files.append((f'vendor_boot:{member}', dst))
    if dlkm_dir.is_dir():
        for ko in sorted(dlkm_dir.glob('*.ko')):
            ko_files.append((f'vendor_dlkm:{ko.name}', ko))

    mismatches = []
    vermagic_bad = []
    checked_syms = 0
    for label, path in ko_files:
        vm = subprocess.check_output(['modinfo', '-F', 'vermagic', str(path)], text=True).strip()
        if not vm.startswith(release + ' '):
            vermagic_bad.append((label, vm))
        mv = subprocess.check_output(['modprobe', '--dump-modversions', str(path)], text=True)
        for line in mv.splitlines():
            parts = line.split()
            if len(parts) < 2:
                continue
            crc, sym = int(parts[0], 16), parts[1]
            if sym in vmlinux_syms:
                checked_syms += 1
                if vmlinux_syms[sym] != crc:
                    mismatches.append((label, sym, hex(crc), hex(vmlinux_syms[sym])))

    require(not vermagic_bad, f'Kernel release/vermagic mismatch ({len(vermagic_bad)}): {vermagic_bad[:5]}')
    require(not mismatches, f'KABI CRC mismatch against vmlinux.symvers ({len(mismatches)}): {mismatches[:10]}')
    return {
        'release': release,
        'modules_checked': len(ko_files),
        'vmlinux_symbol_links_checked': checked_syms,
        'crc_mismatches': 0,
        'vermagic_mismatches': 0,
    }


def validate_inputs(kernel_image: Path, symvers_path: Path, release_path: Path) -> str:
    require(kernel_image.is_file() and kernel_image.stat().st_size > 20 * 1024 * 1024, f'Invalid kernel Image: {kernel_image}')
    require(symvers_path.is_file(), f'Missing vmlinux.symvers: {symvers_path}')
    require(release_path.is_file(), f'Missing kernel.release: {release_path}')
    raw_img = kernel_image.read_bytes()
    for marker in (b'code_sign', b'ecdsa-nist-p256', b'ecdsa-nist-p384', b'developer_mode'):
        require(marker in raw_img, f'Kernel Image missing expected M6 symbol/string: {marker!r}')
    sym_txt = symvers_path.read_text(encoding='utf-8')
    require('fsverity_ioctl_enable_code_sign' in sym_txt, 'vmlinux.symvers missing fsverity_ioctl_enable_code_sign')
    require('hmos_ipc_api_v1' in sym_txt, 'vmlinux.symvers missing hmos_ipc_api_v1')
    require(CODESIGN_CFG.is_file(), f'Missing {CODESIGN_CFG}')
    cfg_doc = json.loads(CODESIGN_CFG.read_text(encoding='utf-8'))
    svcs = {s['name']: s for s in cfg_doc.get('services', [])}
    require('key_enable' in svcs and svcs['key_enable']['path'] == ['/system/bin/key_enable'], 'Invalid key_enable cfg')
    require(EXT4_VERITY_BIN.is_file() and EXT4_VERITY_BIN.read_bytes()[:4] == b'\x7fELF', f'Invalid {EXT4_VERITY_BIN}')
    return release_path.read_text(encoding='utf-8').strip()


def build_boot(boot_base: Path, kernel_image: Path, out_dir: Path, scratch: Path) -> tuple[Path, dict]:
    base_bytes = boot_base.read_bytes()
    base_sha = sha256_bytes(base_bytes)
    require(base_sha == BOOT_BASE_SHA, f'boot base SHA mismatch: got {base_sha}, want {BOOT_BASE_SHA}')

    unpack_dir = scratch / 'boot_unpack'
    boot_args = unpack_with_args(boot_base, unpack_dir)
    orig_ramdisk = (unpack_dir / 'ramdisk').read_bytes()
    new_kernel = kernel_image.read_bytes()

    repack_args = replace_arg_value(boot_args, '--kernel', str(kernel_image))
    raw_out = scratch / 'boot_m6_raw.img'
    subprocess.run(['mkbootimg', *repack_args, '--output', str(raw_out)], check=True)
    payload = attach_boot_v4_signature(raw_out.read_bytes(), unpack_dir / 'boot_signature')
    final_bytes, avb_updated = preserve_and_update_avb_footer(payload, base_bytes)
    require(avb_updated, 'boot AVB footer not updated')
    require(len(final_bytes) == len(base_bytes) == BOOT_LIMIT_BYTES, f'boot size mismatch: {len(final_bytes)}')

    # Roundtrip verify
    rt_img = scratch / 'boot_rt.img'
    rt_img.write_bytes(final_bytes)
    rt_dir = scratch / 'boot_rt_unpack'
    unpack_with_args(rt_img, rt_dir)
    require((rt_dir / 'ramdisk').read_bytes() == orig_ramdisk, 'boot ramdisk drifted on repack')
    require((rt_dir / 'kernel').read_bytes() == new_kernel, 'boot kernel drifted on repack')

    cand_sha = sha256_bytes(final_bytes)
    out_path = out_dir / f'boot_h20_m6_codesign_{cand_sha[:8]}.img'
    out_path.write_bytes(final_bytes)
    return out_path, {
        'base': {'path': str(boot_base), 'bytes': len(base_bytes), 'sha256': base_sha},
        'candidate': {'path': str(out_path), 'bytes': len(final_bytes), 'sha256': cand_sha},
        'kernel_image_bytes': len(new_kernel),
        'kernel_image_sha256': sha256_bytes(new_kernel),
        'ramdisk_sha256': sha256_bytes(orig_ramdisk),
    }


def build(
    boot_base: Path,
    vb_base: Path,
    kernel_image: Path,
    symvers_path: Path,
    release_path: Path,
    dlkm_dir: Path,
    out_dir: Path,
) -> tuple[Path, Path]:
    M6_BUILD.mkdir(parents=True, exist_ok=True)
    out_dir.mkdir(parents=True, exist_ok=True)
    release = validate_inputs(kernel_image, symvers_path, release_path)

    archive, r = load_oracle()
    base_bytes = r.read_original(vb_base)
    base_sha = r.sha(base_bytes)
    require(base_sha == VB_BASE_SHA, f'vendor_boot base SHA mismatch: got {base_sha}, want {VB_BASE_SHA}')

    before_layout = r.layout(base_bytes)
    before_avb = r.verify_existing_avb(base_bytes, before_layout['payload_end'])

    with tempfile.TemporaryDirectory(prefix='h20_m6_', dir=str(M6_BUILD)) as tmp:
        scratch = Path(tmp)
        old, old_raw, kind, old_rd, args = r.extract_entries(
            vb_base, base_bytes, before_layout, scratch / 'before'
        )
        old_manifest = archive.parse(old[r.MANIFEST][1])
        r.manifest_holds(old, old_manifest)

        # Mandatory KABI check BEFORE creating any flashable image!
        kabi_report = audit_kabi(symvers_path, release, old, dlkm_dir, scratch)

        for member in GUARD_MEMBERS:
            require(member in old, f'Missing guarded member in base image: {member}')

        expected = dict(old)
        new_manifest = copy.deepcopy(old_manifest)
        modified_members = {
            r.MANIFEST,
            INIT_DEBUG_MEMBER,
            DONOR_PATCHES_MEMBER,
            USERDATA_MEMBER,
            STAGE_MEMBER,
            START_MEMBER,
        }
        staged_report = {}

        new_init_debug = patch_init_debug(old[INIT_DEBUG_MEMBER][1])
        r.set_entry(expected, INIT_DEBUG_MEMBER, new_init_debug)
        if 'hmos-init-debug.cfg' in new_manifest:
            new_manifest['hmos-init-debug.cfg'].update(bytes=len(new_init_debug), sha256=r.sha(new_init_debug))

        new_donor_patches = patch_donor_patches(old[DONOR_PATCHES_MEMBER][1])
        r.set_entry(expected, DONOR_PATCHES_MEMBER, new_donor_patches)
        update_manifest_entry(new_manifest, DONOR_PATCHES_MEMBER, new_donor_patches, 0o755, r.sha)

        new_userdata = patch_userdata(old[USERDATA_MEMBER][1])
        r.set_entry(expected, USERDATA_MEMBER, new_userdata)
        update_manifest_entry(new_manifest, USERDATA_MEMBER, new_userdata, 0o755, r.sha)

        new_stage = patch_stage(old[STAGE_MEMBER][1])
        r.set_entry(expected, STAGE_MEMBER, new_stage)
        update_manifest_entry(new_manifest, STAGE_MEMBER, new_stage, 0o755, r.sha)

        new_start = patch_start(old[START_MEMBER][1])
        r.set_entry(expected, START_MEMBER, new_start)
        update_manifest_entry(new_manifest, START_MEMBER, new_start, 0o755, r.sha)

        next_inode = [max(header[0] for header, _ in old.values()) + 1]
        for rel, src, mode in STAGED_ADD:
            member = PREFIX + rel
            require(member not in old, f'New M6 member already exists in base image: {member}')
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

        vb_sha = r.sha(cand_bytes)
        vb_out = out_dir / f'vendor_boot_h20_m6_codesign_{vb_sha[:8]}.img'
        vb_out.write_bytes(cand_bytes)
        require(len(cand_bytes) == len(base_bytes) == 100663296, f'Size mismatch: {len(cand_bytes)}')

        boot_out, boot_report = build_boot(boot_base, kernel_image, out_dir, scratch)

        report = {
            'batch': 'M6',
            'description': (
                'Genuine OpenHarmony kernel code_sign (/dev/code_sign, FS_IOC_ENABLE_CODE_SIGN, '
                'fs-verity code_sign_descriptor & PKCS#7 cert-chain + ownerid verification, '
                'ECDSA NIST P-256/P-384 kernel crypto) + donor key_enable startup + '
                'non-destructive ext4 RO_COMPAT_VERITY superblock enabler'
            ),
            'kabi_audit': kabi_report,
            'boot': boot_report,
            'vendor_boot': {
                'base': {'path': str(vb_base), 'bytes': len(base_bytes), 'sha256': base_sha},
                'candidate': {'path': str(vb_out), 'bytes': len(cand_bytes), 'sha256': vb_sha},
                'entries_before': len(old),
                'entries_after': len(got),
                'hcb_hosts_before': 19,
                'hcb_hosts_after': 19,
                'staged': staged_report,
            },
        }
        (M6_BUILD / 'm6-build-report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
        print(f'M6_BUILD_OK boot={boot_out} ({boot_report["candidate"]["sha256"][:8]}) '
              f'vendor_boot={vb_out} ({vb_sha[:8]}) kabi_modules={kabi_report["modules_checked"]} crc_mismatches=0')
        return boot_out, vb_out


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument('--boot-base', type=Path, default=BOOT_BASE_DEFAULT)
    ap.add_argument('--vb-base', type=Path, default=VB_BASE_DEFAULT)
    ap.add_argument('--kernel-image', type=Path, default=KERNEL_IMAGE_DEFAULT)
    ap.add_argument('--symvers', type=Path, default=KERNEL_SYMVERS_DEFAULT)
    ap.add_argument('--release', type=Path, default=KERNEL_RELEASE_DEFAULT)
    ap.add_argument('--dlkm-dir', type=Path, default=VENDOR_DLKM_DIR_DEFAULT)
    ap.add_argument('--out-dir', type=Path, default=Path('/mnt/e/marble-tmp'))
    args = ap.parse_args()
    build(args.boot_base, args.vb_base, args.kernel_image, args.symvers, args.release, args.dlkm_dir, args.out_dir)


if __name__ == '__main__':
    main()
