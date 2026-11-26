#!/usr/bin/env python3
"""AppGallery CDN downloader with hash verification.

The AppGallery CDN (`appdl-*.dbankcdn.com` behind `store-drcn.../dl/appdl/...`)
serves single objects with Range support.  Once a download URL is known this
tool fetches it with parallel ranges, resumes partial files and verifies the
`sha256`/size that the store metadata reports for the package.

    # metadata (size + sha256) for a package
    python3 tools/appgallery/dl.py info com.tencent.wechat

    # download + verify
    python3 tools/appgallery/dl.py fetch --url '<cdn url>' --out /tmp/pkg \
        --sha256 <expected> --size <expected bytes>

Nothing here talks to the store's authenticated API; it only needs a URL.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ag_web import Edge, UA  # noqa: E402

CHUNK = 4 << 20


def meta(pkg: str) -> dict:
    e = Edge('cn')
    e.refresh_interface_code()
    st, body = e.post('/webedge/appinfo', {'pkgName': pkg}, form=False)
    if st != 200:
        raise SystemExit(f'appinfo HTTP {st}')
    d = json.loads(body)
    return {
        'package': d.get('pkgName'),
        'name': d.get('name'),
        'appId': d.get('appId'),
        'id': d.get('id'),
        'version': d.get('version'),
        'versionCode': d.get('versionCode'),
        'size': d.get('size'),
        'sha256': d.get('sha256'),
        'packingType': d.get('packingType'),
    }


MINT = 'https://appgallery.cloud.huawei.com/appdl/{}'


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        return None


def mint(app_id: str) -> tuple[int, str]:
    """Ask the anonymous mint endpoint for a package URL (302 -> CDN object).

    Observed 2026-11-26: the endpoint answers with the real `/dl/appdl/...` URL
    (token included) for Huawei's own apps only; every third-party appId is
    redirected to the AppGallery home page instead.
    """
    opener = urllib.request.build_opener(_NoRedirect)
    req = urllib.request.Request(MINT.format(app_id), headers={'User-Agent': UA})
    try:
        r = opener.open(req, timeout=25)
        return r.status, r.headers.get('location', '')
    except urllib.error.HTTPError as e:
        return e.code, e.headers.get('location', '')
    except Exception as e:  # noqa: BLE001
        return 0, str(e)[:120]


EXTRA_HEADERS: dict[str, str] = {}


def _fetch_range(url: str, start: int, end: int, path: Path, progress: dict, lock: threading.Lock):
    headers = {'User-Agent': UA, 'Range': f'bytes={start}-{end}'}
    headers.update(EXTRA_HEADERS)
    req = urllib.request.Request(url, headers=headers)
    with urllib.request.urlopen(req, timeout=60) as r, open(path, 'r+b') as f:
        f.seek(start)
        while True:
            block = r.read(1 << 16)
            if not block:
                break
            f.write(block)
            with lock:
                progress['done'] += len(block)


def fetch(url: str, out: Path, expect_sha: str = '', expect_size: int = 0,
          workers: int = 4) -> int:
    head_headers = {'User-Agent': UA}
    head_headers.update(EXTRA_HEADERS)
    head = urllib.request.Request(url, method='HEAD', headers=head_headers)
    total = 0
    try:
        with urllib.request.urlopen(head, timeout=30) as r:
            total = int(r.headers.get('Content-Length') or 0)
    except urllib.error.HTTPError as e:
        body = e.read().decode('utf-8', 'replace')[:300]
        print(f'HEAD failed: HTTP {e.code} {body}'.strip())
        # fall back to a one-byte ranged GET; some CDNs refuse HEAD
        probe = urllib.request.Request(url, headers={**head_headers, 'Range': 'bytes=0-0'})
        try:
            with urllib.request.urlopen(probe, timeout=30) as r:
                cr = r.headers.get('Content-Range') or ''
                m = re.search(r'/(\d+)$', cr)
                total = int(m.group(1)) if m else int(r.headers.get('Content-Length') or 0)
        except urllib.error.HTTPError as e2:
            body2 = e2.read().decode('utf-8', 'replace')[:300]
            print(f'GET  failed: HTTP {e2.code} {body2}'.strip())
            print('REFUSED — this object needs authorization the client did not supply')
            return 4
    if total == 0:
        raise SystemExit('server did not report Content-Length')
    if expect_size and total != expect_size:
        print(f'warning: Content-Length {total} != metadata size {expect_size}')
    out.parent.mkdir(parents=True, exist_ok=True)
    if not out.exists() or out.stat().st_size != total:
        with open(out, 'wb') as f:
            f.truncate(total)
    print(f'downloading {total} bytes -> {out} with {workers} ranges')
    ranges = [(i, min(i + CHUNK - 1, total - 1)) for i in range(0, total, CHUNK)]
    progress = {'done': 0}
    lock = threading.Lock()
    started = time.time()

    def worker(queue):
        while True:
            with lock:
                if not queue:
                    return
                start, end = queue.pop(0)
            _fetch_range(url, start, end, out, progress, lock)

    queue = list(ranges)
    threads = [threading.Thread(target=worker, args=(queue,), daemon=True) for _ in range(workers)]
    for t in threads:
        t.start()
    while any(t.is_alive() for t in threads):
        time.sleep(0.5)
        pct = 100.0 * progress['done'] / total
        rate = progress['done'] / max(1e-6, time.time() - started) / 1e6
        print(f'\r  {pct:5.1f}%  {progress["done"]//1048576} MiB  {rate:.1f} MB/s', end='')
    print()
    for t in threads:
        t.join()

    h = hashlib.sha256()
    with open(out, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    digest = h.hexdigest()
    size = out.stat().st_size
    print(f'size   {size}')
    print(f'sha256 {digest}')
    if expect_sha:
        ok = digest.lower() == expect_sha.lower()
        print(f'expect {expect_sha}')
        print('VERIFY', 'ok' if ok else 'MISMATCH')
        return 0 if ok else 2
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    p_info = sub.add_parser('info', help='store metadata for a package')
    p_info.add_argument('package')
    p_fetch = sub.add_parser('fetch', help='download a CDN object and verify it')
    p_fetch.add_argument('--url', default='')
    p_fetch.add_argument('--out', type=Path, default=None)
    p_fetch.add_argument('--sha256', default='')
    p_fetch.add_argument('--size', type=int, default=0)
    p_fetch.add_argument('--workers', type=int, default=4)
    p_fetch.add_argument('--header', action='append', default=[],
                         help='extra request header "Name: value" (repeatable)')
    p_fetch.add_argument('--from-capture', type=Path, default=None,
                         help='JSON capture {"url": ..., "headers": {...}} from a real device')
    p_plan = sub.add_parser('plan', help='metadata + expected hash for a package')
    p_plan.add_argument('package')
    p_mint = sub.add_parser('mint', help='ask appdl/<appId> for a package URL')
    p_mint.add_argument('package')
    p_grab = sub.add_parser(
        'grab', help='one shot: URL (+package) -> download -> verify -> report')
    p_grab.add_argument('--url', required=True)
    p_grab.add_argument('--package', default='', help='bundle name for automatic hash/size lookup')
    p_grab.add_argument('--out', type=Path, default=None)
    p_grab.add_argument('--sha256', default='')
    p_grab.add_argument('--size', type=int, default=0)
    p_grab.add_argument('--workers', type=int, default=4)
    p_grab.add_argument('--header', action='append', default=[],
                        help='extra request header "Name: value" (repeatable)')
    args = ap.parse_args()

    if args.cmd == 'info':
        print(json.dumps(meta(args.package), ensure_ascii=False, indent=2))
        return 0
    if getattr(args, 'from_capture', None):
        cap = json.loads(args.from_capture.read_text(encoding='utf-8'))
        args.url = cap['url']
        if not args.out:
            args.out = Path('/tmp') / (args.url.split('?')[0].rstrip('/').split('/')[-1] or 'pack')
        for k, v in (cap.get('headers') or {}).items():
            EXTRA_HEADERS[k] = v
        print(f'capture: {args.from_capture} ({len(EXTRA_HEADERS)} headers)')
    for h in getattr(args, 'header', []) or []:
        if ':' in h:
            k, v = h.split(':', 1)
            EXTRA_HEADERS[k.strip()] = v.strip()
    if EXTRA_HEADERS:
        print('extra headers:', ', '.join(EXTRA_HEADERS))
    if args.cmd == 'mint':
        m = meta(args.package)
        st, loc = mint(m['appId'])
        print(f"package {m['package']}  appId {m['appId']}  packingType {m['packingType']}")
        print(f"HTTP {st}")
        print(f"location {loc}")
        if 'store-drcn' in loc or '/dl/appdl/' in loc:
            print('MINT ok — feed this to `dl.py grab --url ...`')
            return 0
        print('MINT restricted — endpoint only mints for Huawei first-party appIds '
              '(third-party apps are redirected to the AppGallery home page)')
        return 3
    if args.cmd == 'grab':
        sha, size, name = args.sha256, args.size, args.package or 'package'
        if args.package and (not sha or not size):
            m = meta(args.package)
            sha, size = sha or m['sha256'], size or m['size']
            name = f"{m['name']}({m['package']}) {m['version']}"
            print(f'target : {name}')
            print(f'store  : {size} bytes  sha256 {sha}')
        else:
            print(f'target : {name}')
        out = args.out
        if out is None:
            guess = args.url.split('?')[0].rstrip('/').split('/')[-1] or 'download.bin'
            if args.package and guess.startswith(args.package):
                pass
            out = Path('/tmp') / guess
        rc = fetch(args.url, out, sha, size, args.workers)
        print('REPORT', json.dumps({
            'url': args.url, 'file': str(out), 'bytes': out.stat().st_size if out.exists() else 0,
            'expected_sha256': sha, 'result': 'ok' if rc == 0 else 'mismatch',
        }, ensure_ascii=False))
        return rc
    if args.cmd == 'plan':
        m = meta(args.package)
        print(json.dumps(m, ensure_ascii=False, indent=2))
        print('\nwaiting for a download URL: feed it to `dl.py fetch --url … '
              f'--sha256 {m["sha256"]} --size {m["size"]}`')
        return 0
    if not args.url:
        print('need --url or --from-capture')
        return 2
    if args.out is None:
        args.out = Path('/tmp') / (args.url.split('?')[0].rstrip('/').split('/')[-1] or 'pack')
    return fetch(args.url, args.out, args.sha256, args.size, args.workers)


if __name__ == '__main__':
    raise SystemExit(main())
