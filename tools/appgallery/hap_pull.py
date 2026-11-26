#!/usr/bin/env python3
"""Pull a HarmonyOS package (.hap / .app) off a device or emulator over hdc.

The AppGallery CDN refuses anonymous reads of HarmonyOS packs, so the practical
route is: let a real device/emulator download it, then copy the file out.
This script automates that copy plus verification against store metadata.

    python3 tools/appgallery/hap_pull.py targets
    python3 tools/appgallery/hap_pull.py find            # search common download roots
    python3 tools/appgallery/hap_pull.py pull --out /tmp/pkgs --package com.tencent.wechat

hdc is looked up as: --hdc, $HDC, `hdc` on PATH, then the Windows SDK copy under
/mnt/e/marble-tmp.  Every invocation passes stdin=/dev/null (WSL interop hangs
otherwise, see docs/MARBLE_HANDOFF.md).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from dl import meta  # noqa: E402

HDC_CANDIDATES = (
    'hdc',
    '/mnt/e/marble-tmp/sdk-hdc-6.0.0.47/hdc.exe',
    '/mnt/c/Windows/System32/hdc.exe',
)

SEARCH_ROOTS = (
    '/data/app/el1/bundle/public',
    '/data/service/el1/public',
    '/data/service/el2',
    '/data/storage/el2',
    '/data/local/tmp',
    '/sdcard/Download',
)


def find_hdc(explicit: str | None) -> str:
    for cand in (explicit, os.environ.get('HDC'), *HDC_CANDIDATES):
        if not cand:
            continue
        path = shutil.which(cand) or (cand if Path(cand).exists() else None)
        if path:
            return path
    raise SystemExit(
        'hdc not found. Install the HarmonyOS SDK tool or pass --hdc /path/to/hdc')


def run(hdc: str, args: list[str], timeout: int = 120) -> tuple[int, str]:
    proc = subprocess.run([hdc, *args], stdin=subprocess.DEVNULL,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          timeout=timeout, text=True, errors='replace')
    return proc.returncode, proc.stdout.strip()


def targets(hdc: str) -> list[str]:
    rc, out = run(hdc, ['list', 'targets'], timeout=60)
    if rc != 0:
        print(out)
        return []
    if out.strip() in ('[Empty]', 'Empty', ''):
        return []
    return [line.strip() for line in out.splitlines() if line.strip() and not line.startswith('[')]


def search(hdc: str, roots: tuple[str, ...]) -> list[tuple[str, int]]:
    pattern = ' -o '.join(f"-name '*{ext}'" for ext in ('.hap', '.app'))
    found: dict[str, int] = {}
    for root in roots:
        rc, out = run(hdc, ['shell', f'find {root} \\( {pattern} \\) -type f 2>/dev/null'])
        if rc != 0 or not out:
            continue
        for line in out.splitlines():
            path = line.strip()
            if path.startswith('/') and path not in found:
                found[path] = 0
    # size pass
    for path in list(found):
        rc, out = run(hdc, ['shell', f'ls -l {path} 2>/dev/null'])
        parts = out.split()
        for part in parts:
            if part.isdigit() and int(part) > 1024:
                found[path] = int(part)
                break
    return sorted(found.items(), key=lambda kv: -kv[1])


def pull(hdc: str, remote: str, out_dir: Path) -> Path | None:
    out_dir.mkdir(parents=True, exist_ok=True)
    local = out_dir / Path(remote).name
    rc, out = run(hdc, ['file', 'recv', remote, str(local)], timeout=1800)
    if rc != 0 or not local.exists():
        print(f'  recv failed: {out[:200]}')
        return None
    return local


def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--hdc', default=None)
    sub = ap.add_subparsers(dest='cmd', required=True)
    sub.add_parser('targets')
    p_find = sub.add_parser('find')
    p_find.add_argument('--root', action='append', default=None,
                        help='extra search root (repeatable)')
    p_pull = sub.add_parser('pull')
    p_pull.add_argument('--out', type=Path, default=Path('/tmp/hap-pull'))
    p_pull.add_argument('--package', default='', help='bundle name for store metadata check')
    p_pull.add_argument('--remote', action='append', default=None,
                        help='explicit remote path (repeatable); default: search')
    p_pull.add_argument('--watch', type=int, default=0, metavar='SECONDS',
                        help='wait for a device to appear (poll every 5 s, give up after SECONDS)')
    p_pull.add_argument('--interval', type=float, default=5.0)
    args = ap.parse_args()

    hdc = find_hdc(args.hdc)
    print(f'hdc: {hdc}')

    if args.cmd == 'targets':
        devices = targets(hdc)
        print('devices:', devices or 'none (connect a device or start the emulator)')
        return 0 if devices else 3

    devices = targets(hdc)
    if not devices and args.cmd == 'pull' and args.watch:
        import time as _t
        deadline = _t.time() + args.watch
        print(f'waiting up to {args.watch}s for a device/emulator (start it and it will be picked up)…')
        while _t.time() < deadline:
            _t.sleep(args.interval)
            devices = targets(hdc)
            if devices:
                print('device:', devices[0])
                break
    if not devices:
        print('no device/emulator connected — nothing to search')
        return 3

    if args.cmd == 'find':
        roots = tuple(SEARCH_ROOTS) + tuple(args.root or ())
        hits = search(hdc, roots)
        if not hits:
            print('no .hap/.app found under:')
            for r in roots:
                print('  ', r)
            return 4
        for path, size in hits:
            print(f'{size:>12}  {path}')
        return 0

    out_dir: Path = args.out
    remotes = args.remote or [p for p, _ in search(hdc, tuple(SEARCH_ROOTS))]
    if not remotes:
        print('nothing to pull (use `find` first or pass --remote)')
        return 4
    expect = meta(args.package) if args.package else {}
    if expect:
        print(f"store: {expect['size']} bytes  sha256 {expect['sha256']}")
    report = []
    for remote in remotes:
        local = pull(hdc, remote, out_dir)
        if local is None:
            continue
        digest = sha256_of(local)
        row = {'remote': remote, 'file': str(local), 'bytes': local.stat().st_size,
               'sha256': digest}
        if expect:
            row['matches_store'] = (digest == expect['sha256'])
        report.append(row)
        print(json.dumps(row, ensure_ascii=False))
    if not report:
        return 5
    print(json.dumps({'pulled': len(report), 'out': str(out_dir)}, ensure_ascii=False))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
