#!/usr/bin/env python3
"""Attestation trust anchors shipped in the firmware, and chain verification.

The donor firmware carries the device-attestation roots as a plain PEM bundle:

    system/etc/security/trusted_attest_root_ca.cer
      = CN=Huawei CBG Root CA    (RSA-4096, sha256 3aa81ad7…, 1384 B DER)
      + CN=Huawei CBG Root CA G2 (EC P-384, sha256 df21a3c0…,  542 B DER)

The same two roots also appear in `system/etc/memospace_service/conf/ca.pem`,
inside `libcertificate_framework_core.z.so`, and as `resources/rawfile/…/cbg_root.pem`
in dozens of app packages.  They are **public certificates only** — the firmware
contains no private key and no keystore (verified by scanning 82 526 files), so a
chain that terminates here can be verified but never minted locally.

    python3 tools/appgallery/attest_roots.py                 # list shipped anchors
    python3 tools/appgallery/attest_roots.py --verify chain.pem
"""
from __future__ import annotations

import argparse
import hashlib
import re
import sys
from pathlib import Path

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization

DONOR = Path('work/donor-rom/ADY-AL10-207.0.0.111-SP10/fs')
BUNDLE = 'system/system/etc/security/trusted_attest_root_ca.cer'
EXTRA = (
    'system/system/etc/memospace_service/conf/ca.pem',
)

PEM = re.compile(rb'-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----', re.S)


def load_certs(path: Path) -> list[x509.Certificate]:
    raw = path.read_bytes()
    blocks = PEM.findall(raw)
    if blocks:
        return [x509.load_pem_x509_certificate(b + b'\n') for b in blocks]
    try:
        return [x509.load_der_x509_certificate(raw)]
    except Exception:
        return []


def describe(c: x509.Certificate) -> str:
    der = c.public_bytes(serialization.Encoding.DER)
    pk = c.public_key()
    kind = type(pk).__name__.replace('PublicKey', '')
    size = getattr(pk, 'key_size', '')
    curve = getattr(getattr(pk, 'curve', None), 'name', '')
    return (f'{c.subject.rfc4514_string()}\n'
            f'    sha256={hashlib.sha256(der).hexdigest()}\n'
            f'    der={len(der)}B  {kind}{" " + str(size) if size else ""}'
            f'{" (" + curve + ")" if curve else ""}')


def anchors(root: Path) -> list[x509.Certificate]:
    out: list[x509.Certificate] = []
    for rel in (BUNDLE,) + EXTRA:
        p = root / rel
        if p.exists():
            for c in load_certs(p):
                if all(c.public_bytes(serialization.Encoding.DER) !=
                       o.public_bytes(serialization.Encoding.DER) for o in out):
                    out.append(c)
    return out


def verify(chain_path: Path, anchors_list: list[x509.Certificate]) -> int:
    certs = load_certs(chain_path)
    if not certs:
        print(f'no certificate found in {chain_path}')
        return 2
    print(f'chain: {len(certs)} cert(s)')
    for i, c in enumerate(certs):
        print(f'  [{i}] {c.subject.rfc4514_string()}  <- {c.issuer.rfc4514_string()}')
    rc = 0
    for i in range(len(certs) - 1):
        leaf, issuer = certs[i], certs[i + 1]
        try:
            leaf.verify_directly_issued_by(issuer)
            print(f'  sig [{i}] ok')
        except Exception as e:
            print(f'  sig [{i}] FAILED: {e}')
            rc = 1
    last = certs[-1]
    same = [a for a in anchors_list
            if a.public_bytes(serialization.Encoding.DER) ==
            last.public_bytes(serialization.Encoding.DER)]
    if same:
        print('  anchor: MATCHES shipped attestation root')
        print(f'  {describe(same[0])}')
    else:
        print('  anchor: NOT one of the shipped attestation roots')
        print('    (a locally generated chain cannot be accepted by TSMS)')
        rc = 1
    return rc


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', type=Path, default=DONOR, help='firmware root (fs/)')
    ap.add_argument('--verify', type=Path, help='PEM chain to check against the anchors')
    args = ap.parse_args()
    a = anchors(args.root)
    if not a:
        print(f'no anchors found under {args.root}')
        return 2
    print(f'shipped attestation anchors ({args.root}):')
    for c in a:
        print('  ' + describe(c))
    if args.verify:
        print()
        return verify(args.verify, a)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
