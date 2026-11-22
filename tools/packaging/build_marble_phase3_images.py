#!/usr/bin/env python3
"""Build Xiaomi marble Phase 3 images (vendor_dlkm, 9 GiB super, boot, vendor_boot).

Builds locally without touching any connected device:
  1. vendor_dlkm.img         (EROFS containing 5.10.274-Bouquet-anyhmos vendor_dlkm modules
                              + ELF-extracted modules.dep/modules.softdep/modules.load)
  2. super_marble_phase3.img (9 GiB Android sparse LibLP v10 dynamic partition image:
                              system_a, sys_prod_a, vendor_a, preload_a, vendor_dlkm_a + empty _b)
  3. boot_phase3.img         (5.10.274 Image + marble_init_entry + GKI v4 boot_signature + AVBf)
  4. vendor_boot_phase3.img  (5.10.274 vendor_boot modules + ELF-extracted dep metadata +
                              hmos-probe [HMOS_MARBLE_PHASE3] + Phase 3 runtime configs/scripts
                              + guarded donor patch script + minimal HCB + AVBf)
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import struct
import subprocess
import sys

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

from build_marble_phase2_images import (
    BOOT_LIMIT_BYTES,
    COMPAT_DIR,
    ROOT,
    VENDOR_BOOT_LIMIT_BYTES,
    attach_boot_v4_signature,
    compress_ramdisk,
    decompress_ramdisk,
    extract_full_single_vendor_ramdisk,
    extract_ko_modinfo,
    extract_tar_modules,
    pack_cpio_newc,
    parse_cpio_newc,
    preserve_and_update_avb_footer,
    prune_module_metadata,
    put_file,
    replace_arg_value,
    sha256_bytes,
    unpack_with_args,
)
from liblp_sparse import build_super_sparse_image, sha256_file


MARBLE_SUPER_BYTES = 9663676416  # 9.00 GiB exact (verified on marble /dev/block/sda32)

# Standalone Phase 3 donor binary recipes from patches/donor/ady-al10-111-sp10/.
# Excludes Phase 4 graphics (librender_service_base, libvulkan) and db-key shim
# recipes (librelational_store_crypt, libkv_store_crypt, libdistributeddatasvcfwk)
# until libdbkeys.so / Turnip Vulkan are built in Phase 4.
PHASE3_DONOR_RECIPES = [
    'init.json',
    'libcode_sign_utils.z.so.json',
    'libcli_tool_client.z.so.json',
    'libtime_system_ability.z.so.json',
    'libmemmgrservice.z.so.json',
    'libwifi_sdk.z.so.json',
    'libaudio_policy_client.z.so.json',
    'libaudio_policy_service.z.so.json',
    'libnet_tether_manager_if.z.so.json',
    'libsensor_client.z.so.json',
    'libsps_privacy_indicator_service_intf.z.so.json',
    'libdevicestatus_service.z.so.json',
]


def build_minimal_hdf_hcb() -> bytes:
    """Build a minimal valid OpenHarmony HCB with root { device_info { match_attr = "hdf_manager"; } }.

    Matches the exact HbcHeader + bytecode layout observed in donor ADY-AL10 hdf_default.hcb
    while omitting all Huawei Kirin hardware hosts.
    """
    # PARSEROP_CONFTERM (0x02) + "match_attr\0" + PARSEROP_STRING (0x14) + "hdf_manager\0"
    term = b'\x02match_attr\x00\x14hdf_manager\x00'
    # PARSEROP_CONFNODE (0x01) + "device_info\0" + uint32(len(term)) + term
    dev_info = b'\x01device_info\x00' + struct.pack('<I', len(term)) + term
    # PARSEROP_CONFNODE (0x01) + "root\0" + uint32(len(dev_info)) + dev_info
    root_node = b'\x01root\x00' + struct.pack('<I', len(dev_info)) + dev_info
    # HbcHeader: magic=0xA00AA00A, verMajor=0, verMinor=7, checkSum=0, totalSize=len(root_node)
    header = struct.pack('<IIIII', 0xA00AA00A, 0, 7, 0, len(root_node))
    return header + root_node


def generate_phase3_donor_patch_script(recipes_dir: Path, recipe_names: list[str] | None = None) -> str:
    """Generate a deterministic, SHA-256-guarded BusyBox shell script for Phase 3 donor patches."""
    names = recipe_names if recipe_names is not None else PHASE3_DONOR_RECIPES
    lines = [
        '#!/bin/sh',
        '# Auto-generated guarded donor patcher for Xiaomi marble Phase 3 (RAM-only).',
        'set -eu',
        'B=/busybox',
        'R=/hmos-root',
        '',
        'apply_one() {',
        ' src="$1"; dst="$2"; mode="$3"; in_sha="$4"; out_sha="$5"; bind_over="$6"; shift 6',
        ' sum=$($B sha256sum "$src"); sum=${sum%% *}',
        ' if [ "$sum" = "$out_sha" ]; then',
        '  echo "PATCH_ALREADY_APPLIED $dst"',
        '  return 0',
        ' fi',
        ' if [ "$sum" != "$in_sha" ]; then',
        '  echo "PATCH_INPUT_MISMATCH $src got=$sum want=$in_sha" >&2',
        '  exit 65',
        ' fi',
        ' $B cp "$src" "$dst.new"',
        ' while [ "$#" -ge 2 ]; do',
        '  off="$1"; b64="$2"; shift 2',
        '  printf "%s" "$b64" | $B base64 -d | $B dd of="$dst.new" bs=1 seek="$off" conv=notrunc 2>/dev/null',
        ' done',
        ' got=$($B sha256sum "$dst.new"); got=${got%% *}',
        ' if [ "$got" != "$out_sha" ]; then',
        '  $B rm -f "$dst.new"',
        '  echo "PATCH_OUTPUT_MISMATCH $dst got=$got want=$out_sha" >&2',
        '  exit 66',
        ' fi',
        ' $B chmod "$mode" "$dst.new"',
        ' $B mv "$dst.new" "$dst"',
        ' if [ "$bind_over" = "1" ]; then',
        '  $B mount --bind "$dst" "$src"',
        ' fi',
        ' echo "PATCH_STAGED $dst"',
        '}',
        '',
    ]

    for recipe_name in names:
        recipe_path = recipes_dir / recipe_name
        spec = json.loads(recipe_path.read_text(encoding='utf-8'))
        rel = spec['path']
        if not rel.startswith('/'):
            raise ValueError(f'Expected absolute donor path in {recipe_path}: {rel}')
        in_sha = spec['input_sha256']
        out_sha = spec['output_sha256']
        changes = spec['changes']
        if not changes:
            raise ValueError(f'No changes in {recipe_path}')

        base = Path(rel).name
        if rel == '/system/bin/init':
            dst = '$R/compat/bin/init'
            mode = '0755'
            bind_over = '0'
        else:
            dst = f'$R/compat/lib64/{base}'
            mode = '0644'
            bind_over = '1'

        args = [f'"$R{rel}"', f'"{dst}"', f'"{mode}"', f'"{in_sha}"', f'"{out_sha}"', f'"{bind_over}"']
        for ch in changes:
            after_b = bytes.fromhex(ch['after'])
            before_b = bytes.fromhex(ch['before'])
            if len(after_b) != len(before_b) or ch['offset'] < 0:
                raise ValueError(f'Invalid change in {recipe_path}')
            b64 = base64.b64encode(after_b).decode('ascii')
            args.append(str(ch['offset']))
            args.append(f'"{b64}"')
        lines.append('apply_one ' + ' '.join(args))

    lines.append('echo MARBLE_PHASE3_DONOR_PATCHES_ALL_OK')
    return '\n'.join(lines) + '\n'


def build_vendor_dlkm_erofs(vd_mods_tar: Path, out_img: Path, work_dir: Path) -> dict:
    """Build marble vendor_dlkm.img (EROFS) with ELF-extracted modules.dep/softdep/load."""
    mods = extract_tar_modules(vd_mods_tar)
    if not mods:
        raise ValueError(f'No .ko modules found in {vd_mods_tar}')

    stage = work_dir / 'vendor_dlkm_root'
    if stage.exists():
        shutil.rmtree(stage)
    mod_dir = stage / 'lib' / 'modules'
    mod_dir.mkdir(parents=True)

    norm_to_ko = {name[:-3].replace('_', '-'): name for name in mods if name.endswith('.ko')}
    direct_deps = {}
    softdep_lines = []
    for ko_name in sorted(mods):
        blob = mods[ko_name]
        (mod_dir / ko_name).write_bytes(blob)
        os.chmod(mod_dir / ko_name, 0o644)
        deps, sdeps = extract_ko_modinfo(blob)
        resolved = []
        for dep in deps:
            target = norm_to_ko.get(dep.replace('_', '-'))
            if target and target != ko_name and target not in resolved:
                resolved.append(target)
        direct_deps[ko_name] = resolved
        mod_ident = ko_name[:-3].replace('-', '_')
        for sd in sdeps:
            softdep_lines.append(f'softdep {mod_ident} {sd}')

    def closure(mod_name: str, seen: set[str]) -> list[str]:
        out = []
        for d in direct_deps.get(mod_name, []):
            if d in seen:
                continue
            seen.add(d)
            out.append(d)
            out.extend(closure(d, seen))
        return out

    dep_lines = []
    for ko_name in sorted(mods):
        all_deps = closure(ko_name, {ko_name})
        if all_deps:
            deps_str = ' '.join(f'/vendor_dlkm/lib/modules/{d}' for d in all_deps)
            dep_lines.append(f'/vendor_dlkm/lib/modules/{ko_name}: {deps_str}')
        else:
            dep_lines.append(f'/vendor_dlkm/lib/modules/{ko_name}:')

    (mod_dir / 'modules.dep').write_text('\n'.join(dep_lines) + '\n', encoding='utf-8', newline='\n')
    (mod_dir / 'modules.softdep').write_text(
        '# Soft dependencies extracted from compiled modules.\n' + '\n'.join(softdep_lines) + '\n',
        encoding='utf-8',
        newline='\n',
    )
    (mod_dir / 'modules.load').write_text('\n'.join(sorted(mods)) + '\n', encoding='utf-8', newline='\n')
    (mod_dir / 'modules.alias').write_text('# Aliases placeholder\n', encoding='utf-8', newline='\n')

    out_img.parent.mkdir(parents=True, exist_ok=True)
    if out_img.exists():
        out_img.unlink()
    subprocess.run(
        ['mkfs.erofs', '--all-root', '-T', '0', str(out_img), str(stage)],
        check=True,
        capture_output=True,
    )
    raw_head = out_img.read_bytes()[:2048]
    if raw_head[1024:1028] != b'\xe2\xe1\xf5\xe0':
        raise ValueError(f'Generated vendor_dlkm.img missing EROFS magic: {out_img}')

    return {
        'path': str(out_img),
        'bytes': out_img.stat().st_size,
        'module_count': len(mods),
        'sha256': sha256_file(out_img),
    }


def compile_phase3_binaries(build_dir: Path, cc: str = 'aarch64-linux-gnu-gcc') -> dict:
    build_dir.mkdir(parents=True, exist_ok=True)
    init_bin = build_dir / 'marble_init_entry'
    probe_bin = build_dir / 'hmos-probe'

    subprocess.run(
        [
            cc, '-nostdlib', '-static', '-Os', '-fno-stack-protector', '-fno-builtin',
            '-Wall', '-Wextra', '-Werror',
            '-Wl,-e,_start,-z,noexecstack,--build-id=none',
            str(ROOT / 'devices/marble/boot/marble_init_entry.c'),
            '-o', str(init_bin),
        ],
        check=True,
    )
    subprocess.run(
        [
            cc, '-static', '-O2', '-Wall', '-Wextra', '-Werror',
            '-DHMOS_MARBLE_PHASE3=1',
            str(ROOT / 'compat/bootstrap/hmos_probe_v121.c'),
            '-o', str(probe_bin),
        ],
        check=True,
    )
    strip = cc.replace('-gcc', '-strip')
    if shutil.which(strip):
        subprocess.run([strip, str(init_bin)], check=True)
        subprocess.run([strip, str(probe_bin)], check=True)
    return {'init_entry': init_bin, 'hmos_probe': probe_bin}


def build_phase3_images(
    stock_boot: Path,
    stock_vendor_boot: Path,
    artifacts_dir: Path,
    donor_dir: Path | None,
    recipes_dir: Path,
    busybox: Path,
    out_dir: Path,
    *,
    skip_super: bool = False,
    super_bytes: int = MARBLE_SUPER_BYTES,
    cc: str = 'aarch64-linux-gnu-gcc',
) -> dict:
    out_dir.mkdir(parents=True, exist_ok=True)
    work = out_dir / 'work'
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)

    kernel_image = artifacts_dir / 'Image'
    ipc_ko = artifacts_dir / 'modules/hmos_ipc.ko'
    dmabuf_ko = artifacts_dir / 'modules/hmos_dmabuf_sync.ko'
    vb_mods_tar = artifacts_dir / 'vendor_boot_modules.tar.gz'
    vd_mods_tar = artifacts_dir / 'vendor_dlkm_modules.tar.gz'
    tests = {
        'token_contract': artifacts_dir / 'tests/token_contract',
        'binder_nested': artifacts_dir / 'tests/binder_nested',
        'binder_oneway_pid': artifacts_dir / 'tests/binder_oneway_pid',
    }
    runtime_init_cfg = ROOT / 'devices/marble/runtime/init/hmos-init-phase3.cfg'
    runtime_userdata_sh = ROOT / 'devices/marble/runtime/boot/init-userdata-ext4.sh'
    runtime_stage_sh = ROOT / 'devices/marble/runtime/boot/stage-phase3-marble.sh'
    runtime_start_sh = ROOT / 'devices/marble/runtime/boot/start-phase3-marble.sh'

    for req in (
        stock_boot,
        stock_vendor_boot,
        kernel_image,
        ipc_ko,
        dmabuf_ko,
        vb_mods_tar,
        vd_mods_tar,
        busybox,
        runtime_init_cfg,
        runtime_userdata_sh,
        runtime_stage_sh,
        runtime_start_sh,
        *tests.values(),
    ):
        if not req.is_file():
            raise FileNotFoundError(f'Missing required Phase 3 input: {req}')

    # Validate runtime init JSON and fit within hmos_probe's 32 KiB cfg buffer.
    init_cfg_bytes = runtime_init_cfg.read_bytes()
    json.loads(init_cfg_bytes.decode('utf-8'))
    if len(init_cfg_bytes) >= 32768:
        raise ValueError(f'hmos-init-phase3.cfg exceeds 32768-byte probe buffer: {len(init_cfg_bytes)}')

    # Generate and syntax-check guarded donor patch script.
    patch_script = generate_phase3_donor_patch_script(recipes_dir)
    patch_script_path = work / 'apply-donor-patches-phase3.sh'
    patch_script_path.write_text(patch_script, encoding='utf-8', newline='\n')
    subprocess.run(['bash', '-n', str(patch_script_path)], check=True)
    minimal_hcb = build_minimal_hdf_hcb()

    # --- 1. Build vendor_dlkm.img (EROFS) ---
    vd_out = out_dir / 'vendor_dlkm.img'
    vd_info = build_vendor_dlkm_erofs(vd_mods_tar, vd_out, work)

    # --- 2. Build 9 GiB Android Sparse LibLP v10 super_marble_phase3.img ---
    super_info = None
    if not skip_super:
        if donor_dir is None:
            raise ValueError('--donor is required unless --skip-super is specified')
        donor_sources = [
            ('system', donor_dir / 'extracted/base/system.img'),
            ('sys_prod', donor_dir / 'extracted/base/sys_prod.img'),
            ('vendor', donor_dir / 'extracted/base/vendor.img'),
            ('preload', donor_dir / 'extracted/preload/preload.img'),
            ('vendor_dlkm', vd_out),
        ]
        for pname, ppath in donor_sources:
            if not ppath.is_file():
                raise FileNotFoundError(f'Missing super partition input {pname}: {ppath}')
        super_out = out_dir / 'super_marble_phase3.img'
        super_info = build_super_sparse_image(
            super_out,
            super_bytes,
            donor_sources,
            verify_readback=True,
        )

    # --- 3. Compile Phase 3 marble_init_entry + hmos-probe ---
    bins = compile_phase3_binaries(work / 'bin', cc=cc)

    # --- 4. Repack boot_phase3.img ---
    boot_unpack = work / 'boot-unpack'
    boot_args = unpack_with_args(stock_boot, boot_unpack)
    stock_ramdisk = boot_unpack / 'ramdisk'
    if not stock_ramdisk.is_file():
        raise ValueError('Stock boot image has no generic ramdisk')
    boot_cpio_in = work / 'boot-ramdisk.cpio'
    boot_comp = decompress_ramdisk(stock_ramdisk, boot_cpio_in)
    boot_entries = parse_cpio_newc(boot_cpio_in.read_bytes())
    boot_inode = [max((f[0] for f, _ in boot_entries.values()), default=10) + 1]
    original_init_blob = boot_entries.get('init.real', boot_entries.get('init', (None, None)))[1]
    put_file(boot_entries, 'init', bins['init_entry'].read_bytes(), 0o755, boot_inode)

    boot_cpio_out = work / 'boot-ramdisk-phase3.cpio'
    boot_cpio_out.write_bytes(pack_cpio_newc(boot_entries))
    boot_ramdisk_out = work / 'boot-ramdisk-phase3.img'
    compress_ramdisk(boot_cpio_out, boot_ramdisk_out, boot_comp)

    boot_repack_args = replace_arg_value(boot_args, '--kernel', str(kernel_image))
    boot_repack_args = replace_arg_value(boot_repack_args, '--ramdisk', str(boot_ramdisk_out))
    boot_out = out_dir / 'boot_phase3.img'
    subprocess.run(['mkbootimg', *boot_repack_args, '--output', str(boot_out)], check=True)
    boot_payload = attach_boot_v4_signature(boot_out.read_bytes(), boot_unpack / 'boot_signature')
    boot_final, boot_avb_updated = preserve_and_update_avb_footer(boot_payload, stock_boot.read_bytes())
    boot_out.write_bytes(boot_final)
    if boot_out.stat().st_size > BOOT_LIMIT_BYTES:
        raise ValueError(f'boot_phase3.img exceeds 192 MiB limit: {boot_out.stat().st_size}')

    # --- 5. Repack vendor_boot_phase3.img ---
    vb_unpack = work / 'vendor_boot-unpack'
    vb_args = unpack_with_args(stock_vendor_boot, vb_unpack)
    vb_ramdisks = sorted(p for p in vb_unpack.glob('vendor_ramdisk*') if p.is_file())
    if not vb_ramdisks:
        raise ValueError('No vendor_ramdisk found in stock vendor_boot image')

    target_rd = None
    target_rd_comp = None
    target_entries = None
    for rd in vb_ramdisks:
        cpio_path = work / (rd.name + '.cpio')
        try:
            comp = decompress_ramdisk(rd, cpio_path)
        except subprocess.CalledProcessError:
            if len(vb_ramdisks) == 1 and extract_full_single_vendor_ramdisk(stock_vendor_boot, rd):
                comp = decompress_ramdisk(rd, cpio_path)
            else:
                raise
        parsed = parse_cpio_newc(cpio_path.read_bytes())
        if any(name.endswith('modules.load') for name in parsed):
            target_rd = rd
            target_rd_comp = comp
            target_entries = parsed
            break
    if target_rd is None:
        target_rd = vb_ramdisks[0]
        cpio_path = work / (target_rd.name + '.cpio')
        target_rd_comp = decompress_ramdisk(target_rd, cpio_path)
        target_entries = parse_cpio_newc(cpio_path.read_bytes())

    vb_inode = [max((f[0] for f, _ in target_entries.values()), default=100) + 1]
    vb_only_mods = extract_tar_modules(vb_mods_tar)
    built_mods = dict(vb_only_mods)
    for k, v in extract_tar_modules(vd_mods_tar).items():
        built_mods.setdefault(k, v)

    replaced_mods = []
    removed_stale_mods = []
    mod_prefix = 'lib/modules/'
    for name in list(target_entries):
        if name.startswith(mod_prefix) and name.endswith('.ko'):
            base = Path(name).name
            if base in built_mods:
                put_file(target_entries, name, built_mods[base], 0o644, vb_inode)
                replaced_mods.append(base)
            else:
                del target_entries[name]
                removed_stale_mods.append(base)

    added_vb_mods = []
    for mod, blob in sorted(vb_only_mods.items()):
        rel = mod_prefix + mod
        if rel not in target_entries:
            put_file(target_entries, rel, blob, 0o644, vb_inode)
            added_vb_mods.append(mod)

    valid_ko_names = {
        Path(name).name for name in target_entries
        if name.startswith(mod_prefix) and name.endswith('.ko')
    }
    prune_module_metadata(target_entries, valid_ko_names)

    # Inject /hmos-compat-marble/ Phase 3 payload
    payload_files = {
        'hmos-probe': (bins['hmos_probe'].read_bytes(), 0o755),
        'hmos-ipc.ko': (ipc_ko.read_bytes(), 0o644),
        'hmos-dmabuf-sync.ko': (dmabuf_ko.read_bytes(), 0o644),
        'token_contract': (tests['token_contract'].read_bytes(), 0o755),
        'binder_nested': (tests['binder_nested'].read_bytes(), 0o755),
        'binder_oneway_pid': (tests['binder_oneway_pid'].read_bytes(), 0o755),
        'busybox': (busybox.read_bytes(), 0o755),
        'hmos-init-debug.cfg': (init_cfg_bytes, 0o644),
        'hmos-native-hilog': (b'1\n', 0o644),
        'graphics/init-userdata-ext4.sh': (runtime_userdata_sh.read_bytes(), 0o755),
        'graphics/stage-phase3-marble.sh': (runtime_stage_sh.read_bytes(), 0o755),
        'graphics/start-phase3-marble.sh': (runtime_start_sh.read_bytes(), 0o755),
        'graphics/apply-donor-patches-phase3.sh': (patch_script_path.read_bytes(), 0o755),
        'graphics/hdf_default_minimal.hcb': (minimal_hcb, 0o644),
    }
    if original_init_blob is not None:
        payload_files['init.android.original'] = (original_init_blob, 0o755)

    manifest = {}
    for rel_name, (blob, mode) in payload_files.items():
        put_file(target_entries, f'{COMPAT_DIR}/{rel_name}', blob, mode, vb_inode)
        manifest[rel_name] = {'bytes': len(blob), 'mode': oct(mode), 'sha256': sha256_bytes(blob)}
    put_file(
        target_entries,
        f'{COMPAT_DIR}/MANIFEST.json',
        (json.dumps(manifest, indent=2) + '\n').encode('utf-8'),
        0o644,
        vb_inode,
    )

    vb_cpio_out = work / (target_rd.name + '-phase3.cpio')
    vb_cpio_out.write_bytes(pack_cpio_newc(target_entries))
    vb_rd_out = work / (target_rd.name + '-phase3.img')
    compress_ramdisk(vb_cpio_out, vb_rd_out, target_rd_comp)

    vb_repack_args = list(vb_args)
    replaced_rd_arg = False
    for i, tok in enumerate(vb_repack_args):
        if tok == str(target_rd):
            vb_repack_args[i] = str(vb_rd_out)
            replaced_rd_arg = True
    if not replaced_rd_arg:
        raise ValueError(f'Could not locate {target_rd} in vendor_boot mkbootimg args')

    vb_out = out_dir / 'vendor_boot_phase3.img'
    subprocess.run(['mkbootimg', *vb_repack_args, '--vendor_boot', str(vb_out)], check=True)
    vb_payload = vb_out.read_bytes()
    vb_final, vb_avb_updated = preserve_and_update_avb_footer(vb_payload, stock_vendor_boot.read_bytes())
    vb_out.write_bytes(vb_final)
    if vb_out.stat().st_size > VENDOR_BOOT_LIMIT_BYTES:
        raise ValueError(f'vendor_boot_phase3.img exceeds 96 MiB limit: {vb_out.stat().st_size}')

    # --- 6. Round-trip verification of boot & vendor_boot ---
    verify_boot = work / 'verify-boot'
    unpack_with_args(boot_out, verify_boot)
    assert (verify_boot / 'kernel').read_bytes() == kernel_image.read_bytes()
    v_boot_cpio = work / 'verify-boot.cpio'
    decompress_ramdisk(verify_boot / 'ramdisk', v_boot_cpio)
    v_boot_entries = parse_cpio_newc(v_boot_cpio.read_bytes())
    assert v_boot_entries['init'][1] == bins['init_entry'].read_bytes()

    verify_vb = work / 'verify-vb'
    unpack_with_args(vb_out, verify_vb)
    v_vb_rd = verify_vb / target_rd.name
    v_vb_cpio = work / 'verify-vb.cpio'
    decompress_ramdisk(v_vb_rd, v_vb_cpio)
    v_vb_entries = parse_cpio_newc(v_vb_cpio.read_bytes())
    for rel_name, (blob, _) in payload_files.items():
        assert v_vb_entries[f'{COMPAT_DIR}/{rel_name}'][1] == blob

    report = {
        'vendor_dlkm': vd_info,
        'super_marble_phase3': super_info,
        'boot_phase3': {
            'path': str(boot_out),
            'bytes': boot_out.stat().st_size,
            'payload_bytes': len(boot_payload),
            'limit_bytes': BOOT_LIMIT_BYTES,
            'ramdisk_compression': boot_comp,
            'avb_footer_updated': boot_avb_updated,
            'sha256': sha256_file(boot_out),
        },
        'vendor_boot_phase3': {
            'path': str(vb_out),
            'bytes': vb_out.stat().st_size,
            'payload_bytes': len(vb_payload),
            'limit_bytes': VENDOR_BOOT_LIMIT_BYTES,
            'target_ramdisk': target_rd.name,
            'ramdisk_compression': target_rd_comp,
            'replaced_modules_count': len(replaced_mods),
            'added_modules_count': len(added_vb_mods),
            'removed_stale_modules': removed_stale_mods,
            'avb_footer_updated': vb_avb_updated,
            'sha256': sha256_file(vb_out),
        },
        'phase3_donor_recipes': PHASE3_DONOR_RECIPES,
        'compat_manifest': manifest,
        'roundtrip_verified': True,
    }
    (out_dir / 'phase3-build-report.json').write_text(
        json.dumps(report, indent=2) + '\n', encoding='utf-8', newline='\n'
    )
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stock-boot', type=Path, default=ROOT / 'work/marble-backup/stock/boot_a.img')
    parser.add_argument('--stock-vendor-boot', type=Path, default=ROOT / 'work/marble-backup/stock/vendor_boot_a.img')
    parser.add_argument('--artifacts', type=Path, default=ROOT / 'work/marble-kernel-artifacts')
    parser.add_argument('--donor', type=Path, default=ROOT / 'work/donor-rom/ADY-AL10-207.0.0.111-SP10')
    parser.add_argument('--recipes', type=Path, default=ROOT / 'patches/donor/ady-al10-111-sp10')
    parser.add_argument('--busybox', type=Path, default=ROOT / 'work/marble-backup/stock/busybox-aarch64')
    parser.add_argument('--out', type=Path, default=ROOT / 'work/marble-phase3-out')
    parser.add_argument('--skip-super', action='store_true', help='Skip building 9 GiB super.img')
    parser.add_argument('--cc', default='aarch64-linux-gnu-gcc')
    args = parser.parse_args(argv)
    report = build_phase3_images(
        stock_boot=args.stock_boot,
        stock_vendor_boot=args.stock_vendor_boot,
        artifacts_dir=args.artifacts,
        donor_dir=None if args.skip_super else args.donor,
        recipes_dir=args.recipes,
        busybox=args.busybox,
        out_dir=args.out,
        skip_super=args.skip_super,
        cc=args.cc,
    )
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
