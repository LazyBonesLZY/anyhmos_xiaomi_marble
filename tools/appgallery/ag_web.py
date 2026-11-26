#!/usr/bin/env python3
"""AppGallery web-edge client (offline HAP download research).

Implements the request envelope the AppGallery SPA uses against
`web-drcn.hispace.dbankcloud.com/edge`:

  1. GET  /edge/webedge/getInterfaceCode          -> JWT ("interface code")
  2. every later call carries header
        interface-code: <jwt>_<epoch_ms>
  3. POST bodies use form-urlencoded, JSON or the
     nested request bean the endpoint declares.

Region hosts come from the AppGallery GRS table
(`entry.hap: resources/rawfile/grs_config/router/grs_app_global_route_config.json`):

  DR1 north.china  https://web-drcn.hispace.dbankcloud.com/edge   (STORE: store-drcn.hispace.dbankcloud.com)
  DR2 singapore    https://web-dra.hispace.dbankcloud.com/edge
  europe           https://web-dre.hispace.dbankcloud.com/edge
  russia           https://web-drru.hispace.dbankcloud.com/edge

Only read-only queries are issued; nothing is installed or written to a device.
"""
from __future__ import annotations

import argparse
import json
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

REGIONS = {
    'cn': 'https://web-drcn.hispace.dbankcloud.com/edge',
    'sg': 'https://web-dra.hispace.dbankcloud.com/edge',
    'eu': 'https://web-dre.hispace.dbankcloud.com/edge',
    'ru': 'https://web-drru.hispace.dbankcloud.com/edge',
}
STORE = {
    'cn': 'https://store-drcn.hispace.dbankcloud.com',
    'sg': 'https://store-dra.hispace.dbankcloud.com',
}
UA = ('Mozilla/5.0 (Linux; Android 12; HarmonyOS) AppleWebKit/537.36 '
      '(KHTML, like Gecko) Chrome/114.0.0.0 Mobile Safari/537.36')


class Edge:
    def __init__(self, region: str = 'cn', timeout: float = 25.0):
        self.base = REGIONS.get(region, REGIONS['cn'])
        self.timeout = timeout
        self.interface_code: str | None = None

    # -- transport ---------------------------------------------------------
    def _open(self, req: urllib.request.Request) -> tuple[int, str]:
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as r:
                return r.status, r.read().decode('utf-8', 'replace')
        except urllib.error.HTTPError as e:
            return e.code, e.read().decode('utf-8', 'replace')

    def refresh_interface_code(self) -> str:
        req = urllib.request.Request(
            self.base + '/webedge/getInterfaceCode', headers={'User-Agent': UA})
        status, body = self._open(req)
        if status != 200:
            raise RuntimeError(f'getInterfaceCode HTTP {status}: {body[:200]}')
        code = body.strip().strip('"')
        if not code or code.startswith('{'):
            raise RuntimeError(f'getInterfaceCode returned {body[:200]}')
        self.interface_code = code
        return code

    def get(self, path: str, params: dict | None = None) -> tuple[int, str]:
        if self.interface_code is None:
            self.refresh_interface_code()
        url = self.base + path
        if params:
            url += '?' + urllib.parse.urlencode(params)
        req = urllib.request.Request(url, headers={
            'User-Agent': UA,
            'interface-code': f'{self.interface_code}_{int(time.time() * 1000)}',
            'Accept': 'application/json',
        })
        return self._open(req)

    def harmony_page(self, page_id: str, page_size: int = 25) -> tuple[int, str]:
        """POST /harmony/page-detail — the page layout API (pageId = <prefix>|<key>)."""
        body = {
            'pageNum': 1,
            'pageSize': page_size,
            'pageId': page_id,
            'businessParam': {'animation': 0},
            'clientPackage': 'com.huawei.hmsapp.appgallery',
            'accountZone': 'CN',
        }
        return self.post('/harmony/page-detail', body, form=False, common=False)

    def post(self, path: str, body, form: bool = True, common: bool = True) -> tuple[int, str]:
        if self.interface_code is None:
            self.refresh_interface_code()
        if common and isinstance(body, dict):
            # The SPA's d() helper: without apsid/sid/ts/ver the gateway answers 502.
            now = int(time.time() * 1000)
            merged = {'apsid': now, 'sid': now, 'ts': now, 'ver': '1.1',
                      'serviceType': 20, 'locale': 'zh_CN', 'zone': 'CN'}
            merged.update(body)
            body = merged
        if form:
            data = urllib.parse.urlencode(body).encode()
            ctype = 'application/x-www-form-urlencoded;charset=UTF-8'
        else:
            data = json.dumps(body).encode()
            ctype = 'application/json;charset=UTF-8'
        req = urllib.request.Request(self.base + path, data=data, method='POST', headers={
            'User-Agent': UA,
            'interface-code': f'{self.interface_code}_{int(time.time() * 1000)}',
            'Content-Type': ctype,
            'Accept': 'application/json',
        })
        return self._open(req)


class StoreClient:
    """store-drcn client protocol.

    Envelope discovered 2026-11-26:
      POST https://store-drcn.hispace.dbankcloud.com/hwmarket/harmony/client
           ?apsid=<s>&sid=<s>&ts=<s>&ver=1.1&serviceType=20&encodingCode=0200
           &clientId=108377959&method=client.<Name>
      Content-Type: application/json  (JSON body; the WAF rejects JSON unless
      `method` travels in the query string)
    Body is FLAT (no wrapper object). Mapped methods so far:
      client.getPageDetail              -> needs pageSize/pageNum (+ leagueAppId)
      client.queryClientHarmonyAppInfo  -> needs bundleName + queryScene
      client.fetchHarmonyFiles          -> 630102 "TSMS verify parameters blank."
    """

    BASE = 'https://store-drcn.hispace.dbankcloud.com/hwmarket/harmony/client'
    MAPPED = ('client.getPageDetail', 'client.queryClientHarmonyAppInfo', 'client.fetchHarmonyFiles')

    def __init__(self, timeout: float = 25.0):
        self.timeout = timeout

    def call(self, method: str, body: dict | None = None) -> tuple[int, str]:
        now = int(time.time())
        q = urllib.parse.urlencode({
            'apsid': now, 'sid': now, 'ts': now, 'ver': '1.1', 'serviceType': 20,
            'encodingCode': '0200', 'clientId': 108377959, 'method': method})
        req = urllib.request.Request(
            self.BASE + '?' + q,
            data=json.dumps(body or {}, separators=(',', ':')).encode(),
            method='POST',
            headers={'User-Agent': UA, 'Content-Type': 'application/json;charset=UTF-8'})
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as r:
                return r.status, r.read().decode('utf-8', 'replace')
        except urllib.error.HTTPError as e:
            return e.code, e.read().decode('utf-8', 'replace')


def pretty(status: int, body: str) -> str:
    try:
        return f'[{status}] ' + json.dumps(json.loads(body), ensure_ascii=False)[:4000]
    except Exception:
        return f'[{status}] {body[:4000]}'


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description='AppGallery web-edge probe')
    ap.add_argument('--region', default='cn', choices=sorted(REGIONS))
    ap.add_argument('cmd', choices=[
        'interface-code', 'download-url', 'appinfo', 'detail', 'appdetail',
        'page-detail', 'tab-detail', 'store-method', 'store-appinfo', 'raw'],
        help='endpoint to exercise')
    ap.add_argument('--appid', default='', help='AppGallery app id (Cxxxx)')
    ap.add_argument('--pkg', default='', help='bundle/package name, e.g. com.tencent.wechat')
    ap.add_argument('--path', default='', help='path for "raw"')
    ap.add_argument('--method', default='GET', help='GET or POST for "raw"')
    ap.add_argument('--json', default='{}', help='JSON or form body for "raw" POST')
    ap.add_argument('--save', default='', help='save the raw response JSON here')
    ap.add_argument('--store-method', default='client.getPageDetail', help='method for store-* commands')
    ap.add_argument('--body', default='{}', help='JSON body for store-* commands')
    args = ap.parse_args(argv)

    e = Edge(args.region)
    code = e.refresh_interface_code()
    print(f'interface-code len={len(code)} head={code[:24]}...')

    if args.cmd == 'interface-code':
        print(pretty(200, json.dumps({'interfaceCode': code})))
    elif args.cmd == 'download-url':
        st, body = e.get('/webAppLink/getAppDownloadUrl',
                         {'appid': args.appid, 'pkgName': args.pkg})
        print(pretty(st, body))
    elif args.cmd == 'appinfo':
        # /appinfo/query returns "queryOpenAppInfos is error"; /webedge/appinfo works.
        st, body = e.post('/webedge/appinfo', {'pkgName': args.pkg}, form=False)
        print(pretty(st, body))
    elif args.cmd == 'detail':
        uri = f'app|{args.appid}' if args.appid else f'pkgName|{args.pkg}'
        st, body = e.get('/uowap/index', {
            'method': 'internal.getTabDetail',
            'serviceType': 20,
            'reqPageNum': 1,
            'uri': uri,
            'maxResults': 25,
            'locale': 'zh_CN',
            'countryCode': 'CN',
        })
        print(pretty(st, body))
    elif args.cmd == 'appdetail':
        key = args.appid
        if not key and args.pkg:
            # webedge/appinfo resolves pkgName -> appId (Cxxxx); the page API needs the appId.
            st0, body0 = e.post('/webedge/appinfo', {'pkgName': args.pkg}, form=False)
            try:
                key = json.loads(body0).get('appId') or ''
            except Exception:
                key = ''
            print(f'resolved {args.pkg} -> appId={key}')
        key = key or args.pkg
        st, body = e.harmony_page(f'webAgAppDetail|{key}')
        if st == 200 and 'refs_app' in body:
            try:
                d = json.loads(body)
                ref = d['pages'][0]['data']['card']['data'][0]['list'][0]['refs_app']
                keep = ('appId', 'packageName', 'name', 'version', 'versionCode', 'sha256',
                        'id', 'ctype', 'detailId', 'minSdkLevel', 'adaptInfo')
                print(json.dumps({k: ref.get(k) for k in keep}, ensure_ascii=False, indent=2))
                out = Path(args.save) if args.save else None
                if out:
                    out.write_text(body, encoding='utf-8')
                    print(f'saved {out} ({len(body)} bytes)')
                return 0
            except Exception:
                pass
        print(pretty(st, body))
    elif args.cmd == 'page-detail':
        st, body = e.post('/harmony/client-page-detail', {
            'buildPage': {
                'pageDetailReq': {
                    'pageSize': 10,
                    'pageNum': 1,
                    'pkgName': args.pkg,
                }
            }
        }, form=False)
        print(pretty(st, body))
    elif args.cmd == 'tab-detail':
        st, body = e.post('/index/gettabdetail', {
            'clientTabDetailReq': {
                'pkgName': args.pkg,
                'appId': args.appid,
                'pageNum': 1,
                'pageSize': 10,
            }
        }, form=False)
        print(pretty(st, body))
    elif args.cmd in ('store-method', 'store-appinfo'):
        sc = StoreClient()
        method = args.store_method if args.cmd == 'store-method' else 'client.queryClientHarmonyAppInfo'
        if args.cmd == 'store-appinfo':
            body = {'bundleName': args.pkg, 'queryScene': 0}
        else:
            body = json.loads(args.body)
        st, resp = sc.call(method, body)
        print(pretty(st, resp))
    elif args.cmd == 'raw':
        if args.method.upper() == 'POST':
            try:
                body = json.loads(args.json)
                st, text = e.post(args.path, body, form=not isinstance(body, dict) or False)
            except json.JSONDecodeError:
                st, text = e.post(args.path, dict(urllib.parse.parse_qsl(args.json)))
            print(pretty(st, text))
        else:
            st, text = e.get(args.path, dict(urllib.parse.parse_qsl(args.json)))
            print(pretty(st, text))
    return 0


if __name__ == '__main__':
    sys.exit(main())
