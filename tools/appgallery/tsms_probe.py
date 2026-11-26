#!/usr/bin/env python3
"""TSMS credential probe: send a real /tsms/v2/credentials request with a
self-signed (non-Huawei) certificate chain and print the server verdict.

Background: `libcloud_app_identity_auth_api.z.so` (UCS appauth) posts a JWS to
https://tsms-drcn.platform.dbankcloud.com/tsms/v2/credentials with

    header  { alg:"ED256", cty:"anon_cert", x5c:[leaf, root], kid }
    payload { alg:6, kekAlg:2, dkAlgLen:1, akskVersion:2, pbk:<pub>, len:32, ... }
    body    { "request": "<b64url(header)>.<b64url(payload)>.<b64url(sig R||S)>" }

The server validates the chain, the JWS signature, the challenge, the security
level and appId/package before issuing a credential.  This probe exists to answer
one question with evidence: does a locally generated chain pass?  It sends own
key material only; nothing from Huawei is used.
"""
from __future__ import annotations

import argparse
import base64
import json
import os
import subprocess
import urllib.error
import urllib.request
from pathlib import Path

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature
from cryptography.x509.oid import NameOID
import datetime as dt

TSMS = 'https://tsms-drcn.platform.dbankcloud.com/tsms/v2/credentials'


def b64u(b: bytes) -> str:
    return base64.urlsafe_b64encode(b).rstrip(b'=').decode()


def make_chain(out: Path, root_cn: str = 'probe local dcm root') -> tuple[ec.EllipticCurvePrivateKey, x509.Certificate, x509.Certificate]:
    """root CA (CN=probe local root) + leaf (CN=probe anon key)."""
    out.mkdir(parents=True, exist_ok=True)
    root_key = ec.generate_private_key(ec.SECP256R1())
    leaf_key = ec.generate_private_key(ec.SECP256R1())
    now = dt.datetime.now(dt.timezone.utc)
    root_name = x509.Name([x509.NameAttribute(NameOID.COUNTRY_NAME, 'CN'),
                           x509.NameAttribute(NameOID.ORGANIZATION_NAME, 'Huawei'),
                           x509.NameAttribute(NameOID.ORGANIZATIONAL_UNIT_NAME, 'Huawei CBG'),
                           x509.NameAttribute(NameOID.COMMON_NAME, root_cn)])
    root = (x509.CertificateBuilder()
            .subject_name(root_name).issuer_name(root_name)
            .public_key(root_key.public_key())
            .serial_number(x509.random_serial_number())
            .not_valid_before(now - dt.timedelta(days=1))
            .not_valid_after(now + dt.timedelta(days=365))
            .add_extension(x509.BasicConstraints(ca=True, path_length=None), critical=True)
            .sign(root_key, hashes.SHA256()))
    leaf_name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, 'probe anon key')])
    leaf = (x509.CertificateBuilder()
            .subject_name(leaf_name).issuer_name(root_name)
            .public_key(leaf_key.public_key())
            .serial_number(x509.random_serial_number())
            .not_valid_before(now - dt.timedelta(days=1))
            .not_valid_after(now + dt.timedelta(days=365))
            .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
            .sign(root_key, hashes.SHA256()))
    (out / 'root.pem').write_bytes(root.public_bytes(serialization.Encoding.PEM))
    (out / 'leaf.pem').write_bytes(leaf.public_bytes(serialization.Encoding.PEM))
    (out / 'key.pem').write_bytes(leaf_key.private_bytes(
        serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
        serialization.NoEncryption()))
    return leaf_key, leaf, root


def build_jws(key, leaf, root, package: str, service: str, minimal: bool = False) -> str:
    """x5c entries are STANDARD base64 (JWS spec), not base64url.

    With `minimal=True` the payload stays empty so the server reaches the
    trust-anchor check first; that is the check this probe exists to observe
    (verdict: tsms.1032 "trustCa list not match: cn=<our root>").
    """
    pub = key.public_key().public_bytes(
        serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint)
    header = {
        'alg': 'ED256',
        'cty': 'anon_cert',
        'x5c': [base64.b64encode(leaf.public_bytes(serialization.Encoding.DER)).decode(),
                base64.b64encode(root.public_bytes(serialization.Encoding.DER)).decode()],
    }
    if not minimal:
        # `kid` is itself a base64 JSON blob for the server; keep it only for the
        # full request so the minimal probe reaches the trust-anchor check.
        header['kid'] = b64u(pub[:16])
    payload = {} if minimal else {
        'alg': 6, 'kekAlg': 2, 'dkAlgLen': 1, 'akskVersion': 2,
        'pbk': b64u(pub), 'len': 32,
        'packageName': package, 'serviceName': service,
        'appId': 'C27162', 'cty': 'anon_cert',
    }
    signing_input = b64u(json.dumps(header, separators=(',', ':')).encode()) + '.' + \
                    b64u(json.dumps(payload, separators=(',', ':')).encode())
    der = key.sign(signing_input.encode(), ec.ECDSA(hashes.SHA256()))
    r, s = decode_dss_signature(der)
    raw = r.to_bytes(32, 'big') + s.to_bytes(32, 'big')
    return signing_input + '.' + b64u(raw)


def post(jws: str, service: str, timeout: float = 25.0) -> tuple[int, str]:
    body = json.dumps({'request': jws}).encode()
    req = urllib.request.Request(TSMS, data=body, method='POST', headers={
        'Content-Type': 'application/json;charset=UTF-8',
        'X-Request-ID': base64.urlsafe_b64encode(os.urandom(12)).decode(),
        'X-Service-Name': service,
        'X-ApplyCredential-Terminal': '1',
    })
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read().decode('utf-8', 'replace')
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode('utf-8', 'replace')


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default='/tmp/tsms-probe')
    ap.add_argument('--package', default='com.huawei.hmsapp.appgallery')
    ap.add_argument('--service', default='com.huawei.hmsapp.appgallery')
    ap.add_argument('--reuse', action='store_true', help='reuse chain in --out')
    ap.add_argument('--root-cn', default='probe local dcm root',
                    help='CN of the generated root (trust-list probing)')
    ap.add_argument('--minimal', action='store_true',
                    help='empty JWT payload: reach the trust-anchor check first')
    args = ap.parse_args()

    out = Path(args.out)
    if args.reuse and (out / 'key.pem').exists():
        key = serialization.load_pem_private_key((out / 'key.pem').read_bytes(), None)
        leaf = x509.load_pem_x509_certificate((out / 'leaf.pem').read_bytes())
        root = x509.load_pem_x509_certificate((out / 'root.pem').read_bytes())
    else:
        key, leaf, root = make_chain(out, args.root_cn)
        print(f'chain written to {out}')

    jws = build_jws(key, leaf, root, args.package, args.service, minimal=args.minimal)
    print(f'jws len={len(jws)} head={jws[:60]}...')
    st, resp = post(jws, args.service)
    print(f'HTTP {st}')
    print(resp[:600])
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
