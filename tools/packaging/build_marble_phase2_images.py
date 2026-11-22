#!/usr/bin/env python3
"""Build non-destructive Xiaomi marble Phase 2 test boot & vendor_boot images.

Uses stock backed-up boot/vendor_boot images plus locally compiled Phase 1
5.10.274 kernel/modules/tests to produce:
  - boot_phase2.img        (5.10.274 Image + marble_init_entry as /init in boot ramdisk)
  - vendor_boot_phase2.img (matching 5.10.274 vendor_boot modules + /hmos-compat-marble/
                            containing hmos-probe, hmos-ipc.ko, hmos-dmabuf-sync.ko,
                            and static IPC contract tests)

Never touches super, userdata, or any connected device. Performs full round-trip
unpack/cpio verification on both output images.
"""

import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import struct
import subprocess
import tarfile
import tempfile


ROOT = Path(__file__).resolve().parents[2]
COMPAT_DIR = 'hmos-compat-marble'
BOOT_LIMIT_BYTES = 201326592        # 192 MiB (verified on marble boot_a/b)
VENDOR_BOOT_LIMIT_BYTES = 100663296 # 96 MiB (verified on marble vendor_boot_a/b)


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def parse_cpio_newc(data: bytes) -> dict:
    pos = 0
    entries = {}
    while pos + 110 <= len(data) and data[pos:pos + 6] == b'070701':
        fields = [int(data[pos + 6 + i * 8:pos + 14 + i * 8], 16) for i in range(13)]
        name_size = fields[11]
        file_size = fields[6]
        name = data[pos + 110:pos + 110 + name_size - 1].decode('utf-8')
        data_start = (pos + 110 + name_size + 3) & ~3
        if name == 'TRAILER!!!':
            return entries
        blob = data[data_start:data_start + file_size]
        entries[name] = (fields, blob)
        pos = (data_start + file_size + 3) & ~3
    raise ValueError(f'Invalid or truncated newc cpio archive at offset {pos}')


def pack_cpio_newc(entries: dict) -> bytes:
    out = bytearray()
    for name, (fields, blob) in list(entries.items()) + [('TRAILER!!!', ([0] * 13, b''))]:
        f = fields.copy()
        name_bytes = name.encode('utf-8') + b'\0'
        f[6] = len(blob)
        f[11] = len(name_bytes)
        f[12] = 0
        out.extend(b'070701' + ''.join(f'{v:08x}' for v in f).encode('ascii') + name_bytes)
        out.extend(bytes((-len(out)) % 4))
        out.extend(blob)
        out.extend(bytes((-len(out)) % 4))
    out.extend(bytes((-len(out)) % 512))
    return bytes(out)


def detect_compression(data: bytes) -> str:
    if data[:6] == b'070701':
        return 'raw'
    if data[:2] == b'\x1f\x8b':
        return 'gzip'
    if data[:4] == b'\x02\x21\x4c\x18':
        return 'lz4_legacy'
    if data[:4] == b'\x04\x22\x4d\x18':
        return 'lz4_frame'
    raise ValueError(f'Unsupported ramdisk compression magic: {data[:8].hex()}')


def decompress_ramdisk(src: Path, dst: Path) -> str:
    raw = src.read_bytes()
    kind = detect_compression(raw)
    if kind == 'raw':
        dst.write_bytes(raw)
    elif kind == 'gzip':
        dst.write_bytes(gzip.decompress(raw))
    elif kind in ('lz4_legacy', 'lz4_frame'):
        subprocess.run(['lz4', '-d', '-f', str(src), str(dst)], check=True, capture_output=True)
    return kind


def compress_ramdisk(src: Path, dst: Path, kind: str) -> None:
    if kind == 'raw':
        shutil.copy2(src, dst)
    elif kind == 'gzip':
        dst.write_bytes(gzip.compress(src.read_bytes(), compresslevel=6, mtime=0))
    elif kind == 'lz4_legacy':
        subprocess.run(['lz4', '-l', '-9', '-f', str(src), str(dst)], check=True, capture_output=True)
    elif kind == 'lz4_frame':
        subprocess.run(['lz4', '-9', '-f', str(src), str(dst)], check=True, capture_output=True)
    else:
        raise ValueError(f'Unsupported compression kind: {kind}')


def unpack_with_args(image: Path, out_dir: Path) -> list:
    out_dir.mkdir(parents=True, exist_ok=True)
    res = subprocess.run(
        ['unpack_bootimg', '--boot_img', str(image), '--out', str(out_dir), '--format=mkbootimg', '-0'],
        check=True,
        capture_output=True,
    )
    raw = res.stdout
    if raw.endswith(b'\0'):
        raw = raw[:-1]
    return [arg.decode('utf-8') for arg in raw.split(b'\0')] if raw else []


def replace_arg_value(args: list, flag: str, new_value: str) -> list:
    updated = list(args)
    for i, tok in enumerate(updated):
        if tok == flag and i + 1 < len(updated):
            updated[i + 1] = new_value
            return updated
    raise ValueError(f'Expected flag {flag} not found in unpack_bootimg args: {args}')


def compile_phase2_binaries(build_dir: Path, cc: str = 'aarch64-linux-gnu-gcc') -> dict:
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
            '-DHMOS_PROBE_ONLY=1',
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


def make_entry(inode: int, mode: int, data: bytes) -> tuple:
    nlink = 2 if stat.S_ISDIR(mode) else 1
    return ([inode, mode, 0, 0, nlink, 0, len(data), 0, 0, 0, 0, 0, 0], data)


def ensure_dir(entries: dict, path: str, next_inode: list) -> None:
    parts = path.strip('/').split('/')
    cur = ''
    for part in parts:
        cur = f'{cur}/{part}' if cur else part
        if cur not in entries:
            entries[cur] = make_entry(next_inode[0], stat.S_IFDIR | 0o755, b'')
            next_inode[0] += 1


def put_file(entries: dict, path: str, data: bytes, mode: int, next_inode: list) -> None:
    if '/' in path:
        ensure_dir(entries, path.rsplit('/', 1)[0], next_inode)
    if path in entries:
        fields = entries[path][0].copy()
        fields[1] = stat.S_IFREG | mode
        fields[2] = 0
        fields[3] = 0
        fields[4] = 1
        fields[6] = len(data)
        entries[path] = (fields, data)
    else:
        entries[path] = make_entry(next_inode[0], stat.S_IFREG | mode, data)
        next_inode[0] += 1


def extract_tar_modules(tar_path: Path) -> dict:
    mods = {}
    with tarfile.open(tar_path, 'r:*') as tf:
        for member in tf.getmembers():
            if member.isfile() and member.name.endswith('.ko'):
                name = Path(member.name).name
                mods[name] = tf.extractfile(member).read()
    return mods


def extract_full_single_vendor_ramdisk(stock_vendor_boot: Path, dst: Path) -> bool:
    """Recover full single vendor_ramdisk when table[0].size was left stale by AnyKernel3."""
    raw = stock_vendor_boot.read_bytes()
    if len(raw) < 2128 or raw[:8] != b'VNDRBOOT':
        return False
    ver, page = struct.unpack_from('<II', raw, 8)
    rd_size = struct.unpack_from('<I', raw, 24)[0]
    hs = struct.unpack_from('<I', raw, 2096)[0]
    ne = struct.unpack_from('<I', raw, 2116)[0]
    if ver < 4 or ne != 1 or rd_size == 0:
        return False
    rs = (hs + page - 1) // page * page
    if rs + rd_size > len(raw):
        return False
    dst.write_bytes(raw[rs:rs + rd_size])
    return True


def attach_boot_v4_signature(boot_payload: bytes, sig_path: Path) -> bytes:
    """Preserve the 4096-byte GKI v4 boot_signature extracted from stock boot.img."""
    if not sig_path.is_file():
        return boot_payload
    sig = sig_path.read_bytes()
    if not sig or len(boot_payload) < 1584 or boot_payload[:8] != b'ANDROID!':
        return boot_payload
    out = bytearray(boot_payload)
    if len(out) % 4096:
        out.extend(bytes(4096 - (len(out) % 4096)))
    struct.pack_into('<I', out, 1580, len(sig))
    out.extend(sig)
    return bytes(out)


def preserve_and_update_avb_footer(payload: bytes, stock_raw: bytes) -> tuple:
    """Update AVB0 hash descriptor and full-partition AVBf footer when present in stock image.

    Prevents stale AVBf footers at the end of boot_a (192 MiB) or vendor_boot_a (96 MiB)
    from pointing into overwritten payload offsets during Qualcomm ABL AvbSlotVerify.
    """
    if len(stock_raw) < 64 or stock_raw[-64:-60] != b'AVBf':
        return payload, False
    part_size = len(stock_raw)
    footer = bytearray(stock_raw[-64:])
    magic, vmaj, vmin, old_orig, old_vb_off, vb_size = struct.unpack('>4sIIQQQ', footer[:36])
    if old_vb_off + vb_size > part_size:
        return payload, False
    vb = bytearray(stock_raw[old_vb_off:old_vb_off + vb_size])
    if vb[:4] != b'AVB0':
        return payload, False
    tag, _ = struct.unpack_from('>QQ', vb, 256)
    if tag == 2:
        part_name_len, salt_len, digest_len = struct.unpack_from('>III', vb, 256 + 16 + 8 + 32)
        var_off = 256 + 132
        salt = bytes(vb[var_off + part_name_len:var_off + part_name_len + salt_len])
        new_digest = hashlib.sha256(salt + payload).digest()
        struct.pack_into('>Q', vb, 256 + 16, len(payload))
        vb[var_off + part_name_len + salt_len:var_off + part_name_len + salt_len + digest_len] = new_digest
    new_orig = len(payload)
    struct.pack_into('>QQQ', footer, 12, new_orig, new_orig, vb_size)
    out = bytearray(payload)
    out.extend(vb)
    if len(out) + 64 > part_size:
        raise ValueError(f'Payload ({len(payload)}) + VBMeta ({vb_size}) exceeds partition size ({part_size})')
    out.extend(bytes(part_size - len(out) - 64))
    out.extend(footer)
    return bytes(out), True


def extract_ko_modinfo(ko_bytes: bytes) -> tuple:
    """Extract (depends_list, softdep_list) from a compiled .ko ELF's .modinfo section."""
    if len(ko_bytes) < 64 or ko_bytes[:4] != b'\x7fELF' or ko_bytes[4] != 2 or ko_bytes[5] != 1:
        return [], []
    e_shoff = struct.unpack_from('<Q', ko_bytes, 40)[0]
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from('<HHH', ko_bytes, 58)
    if e_shoff == 0 or e_shentsize < 64 or e_shnum == 0 or e_shstrndx >= e_shnum:
        return [], []
    if e_shoff + e_shnum * e_shentsize > len(ko_bytes):
        return [], []

    def shdr(idx):
        off = e_shoff + idx * e_shentsize
        return struct.unpack_from('<IIQQQQ', ko_bytes, off)

    _, _, _, _, shstr_off, shstr_size = shdr(e_shstrndx)
    shstr = ko_bytes[shstr_off:shstr_off + shstr_size]
    modinfo_blob = b''
    for idx in range(e_shnum):
        sh_name, _, _, _, sh_off, sh_size = shdr(idx)
        end = shstr.find(b'\0', sh_name)
        name = shstr[sh_name:end if end != -1 else len(shstr)]
        if name == b'.modinfo':
            modinfo_blob = ko_bytes[sh_off:sh_off + sh_size]
            break

    depends = []
    softdeps = []
    for entry in modinfo_blob.split(b'\0'):
        if entry.startswith(b'depends='):
            val = entry[8:].decode('utf-8', errors='replace').strip()
            for item in val.split(','):
                item = item.strip()
                if item:
                    depends.append(item)
        elif entry.startswith(b'softdep='):
            val = entry[8:].decode('utf-8', errors='replace').strip()
            if val:
                softdeps.append(val)
    return depends, softdeps


def prune_module_metadata(entries: dict, valid_ko_names: set) -> None:
    """Prune removed modules from modules.load* and regenerate modules.dep/softdep from actual .ko .modinfo."""
    for load_key in ('lib/modules/modules.load', 'lib/modules/modules.load.recovery'):
        if load_key in entries:
            fields, blob = entries[load_key]
            kept = [
                line for line in blob.decode('utf-8', errors='replace').splitlines()
                if line.strip() and Path(line.strip()).name in valid_ko_names
            ]
            new_blob = ('\n'.join(kept) + '\n').encode('utf-8')
            f = fields.copy()
            f[6] = len(new_blob)
            entries[load_key] = (f, new_blob)

    norm_to_ko = {name[:-3].replace('_', '-'): name for name in valid_ko_names if name.endswith('.ko')}
    direct_deps = {}
    softdep_lines = []
    has_elf_modinfo = False

    for ko_name in sorted(valid_ko_names):
        key = 'lib/modules/' + ko_name
        blob = entries[key][1] if key in entries else b''
        deps, sdeps = extract_ko_modinfo(blob)
        if deps or sdeps or (len(blob) >= 4 and blob[:4] == b'\x7fELF'):
            has_elf_modinfo = True
        resolved = []
        for dep in deps:
            target = norm_to_ko.get(dep.replace('_', '-'))
            if target and target != ko_name and target not in resolved:
                resolved.append(target)
        direct_deps[ko_name] = resolved
        mod_ident = ko_name[:-3].replace('-', '_')
        for sd in sdeps:
            softdep_lines.append(f'softdep {mod_ident} {sd}')

    dep_key = 'lib/modules/modules.dep'
    if dep_key in entries:
        fields, old_blob = entries[dep_key]
        if has_elf_modinfo:
            # Compute transitive dependency closure (matching depmod order: direct deps first)
            def closure(mod_name, seen):
                out = []
                for d in direct_deps.get(mod_name, []):
                    if d in seen:
                        continue
                    seen.add(d)
                    out.append(d)
                    out.extend(closure(d, seen))
                return out

            new_lines = []
            for ko_name in sorted(valid_ko_names):
                all_deps = closure(ko_name, {ko_name})
                if all_deps:
                    deps_str = ' '.join(f'/lib/modules/{d}' for d in all_deps)
                    new_lines.append(f'/lib/modules/{ko_name}: {deps_str}')
                else:
                    new_lines.append(f'/lib/modules/{ko_name}:')
        else:
            # Fallback for synthetic non-ELF test fixtures
            new_lines = []
            for line in old_blob.decode('utf-8', errors='replace').splitlines():
                if ':' not in line:
                    continue
                mod_part, deps_part = line.split(':', 1)
                mod_name = Path(mod_part.strip()).name
                if mod_name not in valid_ko_names:
                    continue
                kept_deps = [
                    d for d in deps_part.split()
                    if Path(d.strip()).name in valid_ko_names
                ]
                if kept_deps:
                    new_lines.append(f'{mod_part.strip()}: {" ".join(kept_deps)}')
                else:
                    new_lines.append(f'{mod_part.strip()}:')
        new_blob = ('\n'.join(new_lines) + '\n').encode('utf-8')
        f = fields.copy()
        f[6] = len(new_blob)
        entries[dep_key] = (f, new_blob)

    softdep_key = 'lib/modules/modules.softdep'
    if softdep_key in entries and has_elf_modinfo and softdep_lines:
        fields, _ = entries[softdep_key]
        new_blob = ('# Soft dependencies extracted from compiled modules.\n' + '\n'.join(softdep_lines) + '\n').encode('utf-8')
        f = fields.copy()
        f[6] = len(new_blob)
        entries[softdep_key] = (f, new_blob)


def build_phase2_images(
    stock_boot: Path,
    stock_vendor_boot: Path,
    artifacts_dir: Path,
    out_dir: Path,
    busybox: Path = None,
    replace_dtb: bool = False,
    cc: str = 'aarch64-linux-gnu-gcc',
) -> dict:
    out_dir.mkdir(parents=True, exist_ok=True)
    work = out_dir / 'work'
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)

    kernel_image = artifacts_dir / 'Image'
    dtb_image = artifacts_dir / 'devicetree/ukee.dtb'
    ipc_ko = artifacts_dir / 'modules/hmos_ipc.ko'
    dmabuf_ko = artifacts_dir / 'modules/hmos_dmabuf_sync.ko'
    vb_mods_tar = artifacts_dir / 'vendor_boot_modules.tar.gz'
    vd_mods_tar = artifacts_dir / 'vendor_dlkm_modules.tar.gz'
    tests = {
        'token_contract': artifacts_dir / 'tests/token_contract',
        'binder_nested': artifacts_dir / 'tests/binder_nested',
        'binder_oneway_pid': artifacts_dir / 'tests/binder_oneway_pid',
    }
    for req in (stock_boot, stock_vendor_boot, kernel_image, ipc_ko, dmabuf_ko, vb_mods_tar, *tests.values()):
        if not req.is_file():
            raise FileNotFoundError(f'Missing required Phase 2 input: {req}')
    if replace_dtb and not dtb_image.is_file():
        raise FileNotFoundError(f'Missing compiled DTB: {dtb_image}')

    bins = compile_phase2_binaries(work / 'bin', cc=cc)

    # --- 1. Unpack & repack boot.img (5.10.274 Image + marble_init_entry as /init) ---
    boot_unpack = work / 'boot-unpack'
    boot_args = unpack_with_args(stock_boot, boot_unpack)
    stock_ramdisk = boot_unpack / 'ramdisk'
    if not stock_ramdisk.is_file():
        raise ValueError('Stock boot image has no generic ramdisk; marble requires boot ramdisk')
    boot_cpio_in = work / 'boot-ramdisk.cpio'
    boot_comp = decompress_ramdisk(stock_ramdisk, boot_cpio_in)
    boot_entries = parse_cpio_newc(boot_cpio_in.read_bytes())
    boot_inode = [max((f[0] for f, _ in boot_entries.values()), default=10) + 1]
    original_init_blob = boot_entries.get('init.real', boot_entries.get('init', (None, None)))[1]
    put_file(boot_entries, 'init', bins['init_entry'].read_bytes(), 0o755, boot_inode)

    boot_cpio_out = work / 'boot-ramdisk-phase2.cpio'
    boot_cpio_out.write_bytes(pack_cpio_newc(boot_entries))
    boot_ramdisk_out = work / 'boot-ramdisk-phase2.img'
    compress_ramdisk(boot_cpio_out, boot_ramdisk_out, boot_comp)

    boot_repack_args = replace_arg_value(boot_args, '--kernel', str(kernel_image))
    boot_repack_args = replace_arg_value(boot_repack_args, '--ramdisk', str(boot_ramdisk_out))
    boot_out = out_dir / 'boot_phase2.img'
    subprocess.run(['mkbootimg', *boot_repack_args, '--output', str(boot_out)], check=True)
    boot_payload = attach_boot_v4_signature(boot_out.read_bytes(), boot_unpack / 'boot_signature')
    boot_final, boot_avb_updated = preserve_and_update_avb_footer(boot_payload, stock_boot.read_bytes())
    boot_out.write_bytes(boot_final)
    if boot_out.stat().st_size > BOOT_LIMIT_BYTES:
        raise ValueError(f'boot_phase2.img exceeds 192 MiB limit: {boot_out.stat().st_size}')

    # --- 2. Unpack & repack vendor_boot.img (matching 5.10.274 modules + /hmos-compat-marble/) ---
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
            # Handle stale vendor_ramdisk_table[0].size < header.vendor_ramdisk_size
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
    if vd_mods_tar.is_file():
        for k, v in extract_tar_modules(vd_mods_tar).items():
            built_mods.setdefault(k, v)

    # Replace existing .ko files under lib/modules/ with matching 5.10.274 builds,
    # and remove any unmatched old kernel modules so no vermagic mismatch remains.
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

    # Ensure all compiled vendor_boot modules (including USB, touch, hdcp) exist in /lib/modules/
    added_usb_mods = []
    for mod, blob in sorted(vb_only_mods.items()):
        rel = mod_prefix + mod
        if rel not in target_entries:
            put_file(target_entries, rel, blob, 0o644, vb_inode)
            added_usb_mods.append(mod)

    valid_ko_names = {
        Path(name).name for name in target_entries
        if name.startswith(mod_prefix) and name.endswith('.ko')
    }
    prune_module_metadata(target_entries, valid_ko_names)

    # Inject /hmos-compat-marble/ payload
    payload_files = {
        'hmos-probe': (bins['hmos_probe'].read_bytes(), 0o755),
        'hmos-ipc.ko': (ipc_ko.read_bytes(), 0o644),
        'hmos-dmabuf-sync.ko': (dmabuf_ko.read_bytes(), 0o644),
        'token_contract': (tests['token_contract'].read_bytes(), 0o755),
        'binder_nested': (tests['binder_nested'].read_bytes(), 0o755),
        'binder_oneway_pid': (tests['binder_oneway_pid'].read_bytes(), 0o755),
    }
    if original_init_blob is not None:
        payload_files['init.android.original'] = (original_init_blob, 0o755)
    if busybox is not None:
        if not busybox.is_file():
            raise FileNotFoundError(f'BusyBox binary not found: {busybox}')
        payload_files['busybox'] = (busybox.read_bytes(), 0o755)

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

    vb_cpio_out = work / (target_rd.name + '-phase2.cpio')
    vb_cpio_out.write_bytes(pack_cpio_newc(target_entries))
    vb_rd_out = work / (target_rd.name + '-phase2.img')
    compress_ramdisk(vb_cpio_out, vb_rd_out, target_rd_comp)

    vb_repack_args = list(vb_args)
    replaced_rd_arg = False
    for i, tok in enumerate(vb_repack_args):
        if tok == str(target_rd):
            vb_repack_args[i] = str(vb_rd_out)
            replaced_rd_arg = True
    if not replaced_rd_arg:
        raise ValueError(f'Could not locate {target_rd} in vendor_boot mkbootimg args')
    if replace_dtb:
        vb_repack_args = replace_arg_value(vb_repack_args, '--dtb', str(dtb_image))

    vb_out = out_dir / 'vendor_boot_phase2.img'
    subprocess.run(['mkbootimg', *vb_repack_args, '--vendor_boot', str(vb_out)], check=True)
    vb_payload = vb_out.read_bytes()
    vb_final, vb_avb_updated = preserve_and_update_avb_footer(vb_payload, stock_vendor_boot.read_bytes())
    vb_out.write_bytes(vb_final)
    if vb_out.stat().st_size > VENDOR_BOOT_LIMIT_BYTES:
        raise ValueError(f'vendor_boot_phase2.img exceeds 96 MiB limit: {vb_out.stat().st_size}')

    # --- 3. Full round-trip verification of both output images ---
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
        'boot_phase2': {
            'path': str(boot_out),
            'bytes': boot_out.stat().st_size,
            'payload_bytes': len(boot_payload),
            'limit_bytes': BOOT_LIMIT_BYTES,
            'ramdisk_compression': boot_comp,
            'avb_footer_updated': boot_avb_updated,
            'sha256': sha256_bytes(boot_out.read_bytes()),
        },
        'vendor_boot_phase2': {
            'path': str(vb_out),
            'bytes': vb_out.stat().st_size,
            'payload_bytes': len(vb_payload),
            'limit_bytes': VENDOR_BOOT_LIMIT_BYTES,
            'target_ramdisk': target_rd.name,
            'ramdisk_compression': target_rd_comp,
            'dtb_source': str(dtb_image) if replace_dtb else 'stock',
            'replaced_modules_count': len(replaced_mods),
            'added_usb_modules': added_usb_mods,
            'removed_stale_modules': removed_stale_mods,
            'avb_footer_updated': vb_avb_updated,
            'sha256': sha256_bytes(vb_out.read_bytes()),
        },
        'compat_manifest': manifest,
        'roundtrip_verified': True,
    }
    (out_dir / 'phase2-build-report.json').write_text(
        json.dumps(report, indent=2) + '\n', encoding='utf-8', newline='\n'
    )
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stock-boot', type=Path, required=True, help='Backed-up marble boot_a/b.img')
    parser.add_argument('--stock-vendor-boot', type=Path, required=True, help='Backed-up marble vendor_boot_a/b.img')
    parser.add_argument('--artifacts', type=Path, default=ROOT / 'work/marble-kernel-artifacts',
                        help='Phase 1 kernel build artifacts directory')
    parser.add_argument('--out', type=Path, default=ROOT / 'work/marble-phase2-out',
                        help='Output directory for Phase 2 test images')
    parser.add_argument('--busybox', type=Path, help='Optional static AArch64 busybox binary for USB cmd worker')
    parser.add_argument('--replace-dtb', action='store_true',
                        help='Replace stock vendor_boot DTB table with compiled ukee.dtb')
    parser.add_argument('--cc', default='aarch64-linux-gnu-gcc', help='AArch64 C cross compiler')
    args = parser.parse_args(argv)
    report = build_phase2_images(
        stock_boot=args.stock_boot,
        stock_vendor_boot=args.stock_vendor_boot,
        artifacts_dir=args.artifacts,
        out_dir=args.out,
        busybox=args.busybox,
        replace_dtb=args.replace_dtb,
        cc=args.cc,
    )
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
