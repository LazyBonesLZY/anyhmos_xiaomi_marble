#!/usr/bin/env python3
"""Assemble the marble audio runtime (bin + lib64) from already-extracted donors.

This script never downloads anything and never touches a device.  It walks the
DT_NEEDED closure of the PAL entry points, copies each object from the donor
directories into ``compat/audio/runtime/{bin,lib64}``, then writes
``SHA256SUMS`` and ``manifest.json`` and fails loudly when a dependency cannot
be resolved.

Donor priority (first match wins, and the reason is recorded per file):

1. ``compat/audio/runtime/lib64/libaudioroute.so`` - locally built shim
   (built by ``tools/build/build_current.py audioroute``); never taken from a
   donor because the stock vendor does not ship one that matches our Bionic.
2. ``work/marble-audio-dump/lib64`` - A15 dump of the marble vendor image; the
   set is self consistent and is preferred for every vendor library.
3. ``work/marble-bionic/lib64`` - Bionic runtime extracted from the marble
   com.android.runtime.apex plus the vendor system/lib64.
4. ``work/marble-audio-vendor/lib64`` (and ``bin``) - stock vendor_boot tar;
   used ONLY for objects the dump does not contain (different build, must not be
   mixed for objects that exist in the dump).

Usage:
    python3 compat/audio/runtime/assemble_runtime.py            # assemble
    python3 compat/audio/runtime/assemble_runtime.py --check    # verify only
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
RUNTIME = ROOT / 'compat/audio/runtime'
# Build outputs of tools/build/build_current.py; never a donor.
BUILD = ROOT / 'work/marble-audio-build'
AUDIOROUTE = BUILD / 'libaudioroute.so'

DUMP = ROOT / 'work/marble-audio-dump/lib64'
BIONIC = ROOT / 'work/marble-bionic/lib64'
VENDOR_LIB = ROOT / 'work/marble-audio-vendor/lib64'
VENDOR_BIN = ROOT / 'work/marble-audio-vendor/bin'
LINKER = ROOT / 'work/marble-bionic/bin/linker64'

# Roots: whatever PAL loads first.  Everything else is discovered from NEEDED.
LIB_ROOTS = (
    'libar-pal.so',
    'libagm.so',
    'libagmclient.so',
    'libagmdevice.so',
    'libagmmixer.so',
    'libagm_compress_plugin.so',
    'libagm_mixer_plugin.so',
    'libagm_pcm_plugin.so',
)
BIN_ROOTS = ('agmplay',)

# Objects the loader itself provides; never shipped as files.
LOADER_PROVIDED = {'ld-android.so'}
# Symbols the dynamic linker itself exports (ld-android.so == bin/linker64).
LOADER_SYMBOL_PREFIXES = ('__loader_',)
# Symbols weak-linked by the vendor objects; unresolved is allowed by Bionic.
ALLOW_UNRESOLVED_IF_WEAK = True

SOURCES = (
    ('self-built', BUILD),
    ('dump', DUMP),
    ('bionic', BIONIC),
    ('vendor', VENDOR_LIB),
    ('vendor', VENDOR_BIN),
)


def _elf_dynamic(path: Path):
    """Return (needed, soname) for a little-endian AArch64 ELF."""
    data = memoryview(path.read_bytes())
    if data[:4] != b'\x7fELF' or data[4] != 2 or data[5] != 1:
        raise ValueError(f'not a 64-bit little-endian ELF: {path}')
    e_machine = struct.unpack_from('<H', data, 0x12)[0]
    if e_machine != 183:
        raise ValueError(f'not AArch64 (e_machine={e_machine}): {path}')
    phoff, shoff = struct.unpack_from('<QQ', data, 0x20)
    phentsize, phnum = struct.unpack_from('<HH', data, 0x36)
    needed, soname, dyn = [], None, None
    for i in range(phnum):
        base = phoff + i * phentsize
        p_type = struct.unpack_from('<I', data, base)[0]
        if p_type == 2:  # PT_DYNAMIC
            dyn = struct.unpack_from('<QQ', data, base + 8)
    if dyn is None:
        return needed, soname
    offset, size = dyn
    strtab = None
    entries = []
    for off in range(offset, offset + size, 16):
        tag, val = struct.unpack_from('<qQ', data, off)
        if tag == 0:
            break
        entries.append((tag, val))
    for tag, val in entries:
        if tag == 5:  # DT_STRTAB (virtual address)
            strtab = _vaddr_to_offset(data, phoff, phentsize, phnum, val)
    if strtab is None:
        return needed, soname
    for tag, val in entries:
        if tag == 1:  # DT_NEEDED
            needed.append(_cstr(data, strtab + val))
        elif tag == 14:  # DT_SONAME
            soname = _cstr(data, strtab + val)
    return needed, soname


def _vaddr_to_offset(data, phoff, phentsize, phnum, vaddr):
    for i in range(phnum):
        base = phoff + i * phentsize
        p_type = struct.unpack_from('<I', data, base)[0]
        if p_type != 1:  # PT_LOAD
            continue
        p_offset, p_vaddr = struct.unpack_from('<QQ', data, base + 8)
        p_filesz = struct.unpack_from('<Q', data, base + 32)[0]
        if p_vaddr <= vaddr < p_vaddr + p_filesz:
            return p_offset + (vaddr - p_vaddr)
    raise ValueError('DT_STRTAB is not backed by a PT_LOAD segment')


def _cstr(data, offset):
    end = bytes(data[offset:]).index(b'\0')
    return bytes(data[offset:offset + end]).decode('utf-8', 'replace')


def dynamic_exports(path: Path) -> dict[str, str]:
    """Map exported symbol name -> binding for a dynamic object."""
    data = memoryview(path.read_bytes())
    e_shoff = struct.unpack_from('<Q', data, 0x28)[0]
    e_shentsize, e_shnum = struct.unpack_from('<HH', data, 0x3A)
    sections = []
    for i in range(e_shnum):
        base = e_shoff + i * e_shentsize
        sh_type = struct.unpack_from('<I', data, base + 4)[0]
        sh_offset, sh_size = struct.unpack_from('<QQ', data, base + 0x18)
        sh_link, = struct.unpack_from('<I', data, base + 0x28)
        sh_entsize, = struct.unpack_from('<Q', data, base + 0x38)
        sections.append((sh_type, sh_offset, sh_size, sh_link, sh_entsize))
    out: dict[str, str] = {}
    for sh_type, sh_offset, sh_size, sh_link, sh_entsize in sections:
        if sh_type not in (11,):  # SHT_DYNSYM
            continue
        str_off = sections[sh_link][1]
        for off in range(sh_offset, sh_offset + sh_size, sh_entsize or 24):
            st_name, st_info, _other, st_shndx = struct.unpack_from('<IBBH', data, off)
            if st_shndx == 0 or st_name == 0:
                continue
            bind = st_info >> 4
            out.setdefault(_cstr(data, str_off + st_name),
                           {0: 'LOCAL', 1: 'GLOBAL', 2: 'WEAK'}.get(bind, 'OTHER'))
    return out


def dynamic_imports(path: Path) -> dict[str, str]:
    data = memoryview(path.read_bytes())
    e_shoff = struct.unpack_from('<Q', data, 0x28)[0]
    e_shentsize, e_shnum = struct.unpack_from('<HH', data, 0x3A)
    sections = []
    for i in range(e_shnum):
        base = e_shoff + i * e_shentsize
        sh_type = struct.unpack_from('<I', data, base + 4)[0]
        sh_offset, sh_size = struct.unpack_from('<QQ', data, base + 0x18)
        sh_link, = struct.unpack_from('<I', data, base + 0x28)
        sh_entsize, = struct.unpack_from('<Q', data, base + 0x38)
        sections.append((sh_type, sh_offset, sh_size, sh_link, sh_entsize))
    out: dict[str, str] = {}
    for sh_type, sh_offset, sh_size, sh_link, sh_entsize in sections:
        if sh_type != 11:
            continue
        str_off = sections[sh_link][1]
        for off in range(sh_offset, sh_offset + sh_size, sh_entsize or 24):
            st_name, st_info, _other, st_shndx = struct.unpack_from('<IBBH', data, off)
            if st_shndx != 0 or st_name == 0:
                continue
            bind = st_info >> 4
            out.setdefault(_cstr(data, str_off + st_name),
                           {0: 'LOCAL', 1: 'GLOBAL', 2: 'WEAK'}.get(bind, 'OTHER'))
    return out


def soname_of(path: Path):
    return _elf_dynamic(path)[1]


def needed_of(path: Path):
    return _elf_dynamic(path)[0]


def resolve(name: str):
    """Return (source_kind, path) for a DT_NEEDED / binary name."""
    for kind, directory in SOURCES:
        candidate = directory / name
        if candidate.is_file():
            return kind, candidate
    return None, None


def plan():
    """Compute the closure.

    Returns (files, unresolved, reasons) where files maps target relative path
    to (kind, source path).
    """
    files: dict[str, tuple[str, Path]] = {}
    reasons: dict[str, str] = {}
    queue: list[str] = [f'lib64/{n}' for n in LIB_ROOTS] + [f'bin/{n}' for n in BIN_ROOTS]
    if not AUDIOROUTE.is_file():
        raise SystemExit(
            'libaudioroute.so is not built yet: run '
            '`python3 tools/build/build_current.py --out work/marble-audio-build audioroute`')
    for target in queue:
        kind, source = resolve(target.split('/', 1)[1])
        if source is None:
            raise SystemExit(f'missing root object {target}')
        files[target] = (kind, source)
        reasons[target] = f'{kind}:{source.relative_to(ROOT).as_posix()}'

    pending = list(files)
    while pending:
        target = pending.pop()
        source = files[target][1]
        for dependency in needed_of(source):
            # every DT_NEEDED of a shipped object is a shared library
            dep_target = f'lib64/{dependency}'
            if dependency in LOADER_PROVIDED:
                continue
            if dep_target not in files:
                kind, dep_source = resolve(dependency)
                if dep_source is None:
                    raise SystemExit(f'unresolved dependency {dependency!r} required by {target}')
                files[dep_target] = (kind, dep_source)
                reasons[dep_target] = f'{kind}:{dep_source.relative_to(ROOT).as_posix()}'
                pending.append(dep_target)
    return files, reasons


def closure_report(files) -> list[str]:
    """Check that every imported symbol is exported by the shipped set."""
    exported: dict[str, str] = {}
    for target, (_kind, source) in files.items():
        if not target.endswith('.so'):
            continue
        for symbol, bind in dynamic_exports(source).items():
            if bind != 'LOCAL':
                exported.setdefault(symbol, target)
    missing = []
    for target, (_kind, source) in sorted(files.items()):
        for symbol, bind in sorted(dynamic_imports(source).items()):
            if symbol in exported:
                continue
            if symbol.startswith(LOADER_SYMBOL_PREFIXES):
                continue
            if ALLOW_UNRESOLVED_IF_WEAK and bind == 'WEAK':
                continue
            missing.append(f'{target}: {symbol} ({bind})')
    return missing


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open('rb') as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b''):
            digest.update(chunk)
    return digest.hexdigest()


def assemble(destination: Path = RUNTIME) -> dict:
    files, reasons = plan()
    if LINKER.is_file() and 'bin/linker64' not in files:
        files['bin/linker64'] = ('bionic', LINKER)
        reasons['bin/linker64'] = f'bionic:{LINKER.relative_to(ROOT).as_posix()}'
    missing = closure_report(files)
    if missing:
        raise SystemExit('symbol coverage failed:\n  ' + '\n  '.join(missing))

    for relative in files:
        (destination / relative).parent.mkdir(parents=True, exist_ok=True)
    # Drop stale objects so the directory always equals the planned closure.
    for existing in sorted((destination / 'lib64').glob('*')) + sorted((destination / 'bin').glob('*')):
        if existing.is_file() and existing.relative_to(destination).as_posix() not in files:
            existing.unlink()

    entries = []
    for relative, (kind, source) in sorted(files.items()):
        target = destination / relative
        shutil.copyfile(source, target)
        target.chmod(0o755)
        entries.append(dict(
            path=relative,
            sha256=sha256(target),
            bytes=target.stat().st_size,
            source=kind,
            source_path=source.relative_to(ROOT).as_posix(),
            soname=soname_of(target) if relative.endswith('.so') else None,
            needed=needed_of(target),
        ))
    checks = ''.join(f"{e['sha256']}  {e['path']}\n" for e in entries)
    (destination / 'SHA256SUMS').write_text(checks, encoding='utf-8', newline='\n')
    manifest = dict(
        format_version=1,
        hash_algorithm='SHA256',
        purpose='marble (SM7475/ukee) PAL/AGM user-space runtime for the HarmonyOS NEXT port',
        roots=dict(bin=list(BIN_ROOTS), lib64=list(LIB_ROOTS)),
        loaders=dict(linker='bin/linker64'),
        loader_provided=sorted(LOADER_PROVIDED),
        donor_sets=[
            dict(name='dump', path='work/marble-audio-dump/lib64',
                 what='marble Android 15 device dump (PAL/AGM/osal/AGMIPC client), '
                      'self-consistent set',
                 identity=dict(
                     file_list='work/marble-dump-all_files.txt',
                     dump_paths=['system/system/lib64/', 'system/system/apex/'],
                     rom_branch=None,
                     note='the ROM branch/build id of the dump is not recorded in this '
                          'workspace (only the file list survived), so it is left unset '
                          'instead of being guessed')),
            dict(name='bionic', path='work/marble-bionic',
                 what='marble Bionic runtime: bin/linker64 from system/bin, lib64 from '
                      'com.android.runtime.apex + vendor system/lib64 (A15)',
                 identity=dict(
                     apex='system/system/apex/com.android.runtime.apex',
                     apex_sha256='b98d530fdea732d2394d367e9b21c2759272bbcb968c6dc4000bb5066ad36a08',
                     apex_payload_sha256='8eb8d3351289e8774b0db563ee1eec77b091150d0ece50b63603ff2ae3d8050b',
                     linker_source='system/bin/linker64')),
            dict(name='vendor', path='work/marble-audio-vendor',
                 what='stock vendor_boot tar extract, different build; used only for objects the '
                      'dump does not contain',
                 identity=dict(
                     tar='work/marble-stock-vendor/marble-stock-vendor.tar.gz',
                     mixing_policy='never mixed per object: a file present in the dump or in '
                                   'bionic is always taken from there')),
            dict(name='self-built', path='work/marble-audio-build',
                 what='AOSP audio_route.c + opaque-pointer shim, NDK r27d, built by '
                      'tools/build/build_current.py audioroute (source in compat/audio/audioroute)'),
        ],
        self_built=dict(
            libaudioroute_so=dict(
                source='compat/audio/audioroute/{audio_route.c,shim.c}',
                export_map='compat/audio/audioroute/exports.map',
                ndk='r27d (27.3.13750724)',
                target='aarch64-linux-android33',
                command='python3 tools/build/build_current.py --out OUT audioroute',
            ),
        ),
        files=entries,
    )
    (destination / 'manifest.json').write_text(
        json.dumps(manifest, indent=2) + '\n', encoding='utf-8', newline='\n')
    return manifest


def check(destination: Path = RUNTIME) -> dict:
    manifest = json.loads((destination / 'manifest.json').read_text(encoding='utf-8'))
    entries = {e['path']: e for e in manifest['files']}
    actual = sorted(p.relative_to(destination).as_posix()
                    for p in destination.rglob('*') if p.is_file())
    keep = {'SHA256SUMS', 'manifest.json', 'assemble_runtime.py', '.gitignore'}
    unexpected = [p for p in actual if p not in entries and p not in keep
                  and '__pycache__' not in p]
    assert not unexpected, f'unexpected files: {unexpected}'
    for relative, entry in entries.items():
        path = destination / relative
        assert path.is_file(), f'missing {relative}'
        assert path.stat().st_size == entry['bytes'], f'size mismatch {relative}'
        assert sha256(path) == entry['sha256'], f'sha256 mismatch {relative}'
    sums = {}
    for line in (destination / 'SHA256SUMS').read_text(encoding='utf-8').splitlines():
        digest, name = line.split('  ', 1)
        sums[name] = digest
    assert sums == {e['path']: e['sha256'] for e in manifest['files']}, 'SHA256SUMS differs from manifest'
    files = {relative: (entry['source'], destination / relative)
             for relative, entry in entries.items()}
    missing = closure_report(files)
    assert not missing, 'symbol coverage failed: ' + '; '.join(missing)
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--out', type=Path, default=RUNTIME)
    parser.add_argument('--check', action='store_true', help='verify the existing directory only')
    args = parser.parse_args()
    if args.check:
        manifest = check(args.out)
        print(json.dumps(dict(files=len(manifest['files']),
                              bytes=sum(e['bytes'] for e in manifest['files']),
                              check='ok'), indent=2))
        return 0
    manifest = assemble(args.out)
    print(json.dumps(dict(files=len(manifest['files']),
                          bytes=sum(e['bytes'] for e in manifest['files']),
                          out=str(args.out)), indent=2))
    return 0


if __name__ == '__main__':
    sys.exit(main())
