#!/usr/bin/env python3
"""Build or validate Marble's QRTR, codec and Bluetooth inputs on the host only."""
import argparse
import hashlib
import json
import os
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
INPUTS = {
    'qmi_probe': ('compat/telephony/qmi_probe.c', 0o755),
    'native_codec_probe': ('compat/codec/native_codec_probe.c', 0o755),
    'hmos_bt_bridge_marble': ('compat/bluetooth/marble_bt_bridge.c', 0o755),
    'libhmos_bt_bridge_marble.so': ('compat/bluetooth/marble_bt_bridge.c', 0o644),
}


def digest(value):
    return hashlib.sha256(value).hexdigest()


def inspect_artifact(path):
    if path.is_symlink() or not path.is_file():
        raise ValueError(f'Expected a regular diagnostics artifact: {path}')
    blob = path.read_bytes()
    if len(blob) < 64 or blob[:6] != b'\x7fELF\x02\x01':
        raise ValueError(f'Expected a little-endian ELF64 diagnostics artifact: {path}')
    kind, machine = struct.unpack_from('<HH', blob, 16)
    if machine != 183 or kind not in (2, 3) or (path.suffix == '.so' and kind != 3):
        raise ValueError(f'Expected an AArch64 executable or shared object: {path}')
    return blob


def load_diagnostics(directory):
    """Return measured bytes for packaging, with no fallback to old work paths."""
    report = directory / 'diagnostics.json'
    if report.is_symlink() or not report.is_file():
        raise ValueError(f'Missing diagnostics build record: {report}; build into a fresh directory first')
    artifacts = json.loads(report.read_bytes())['artifacts']
    if not isinstance(artifacts, dict) or set(artifacts) != set(INPUTS):
        raise ValueError(f'Unexpected diagnostics artifact set: {report}')
    payload = {}
    for name, (source, mode) in INPUTS.items():
        item = artifacts[name]
        if item['source'] != source or item['source_sha256'] != digest((ROOT / source).read_bytes()):
            raise ValueError(f'Stale diagnostics source for {name}; rebuild from current sources')
        blob = inspect_artifact(directory / name)
        if item['bytes'] != len(blob) or item['sha256'] != digest(blob):
            raise ValueError(f'Diagnostics binary does not match its build record: {name}')
        payload[f'graphics/{name}'] = (blob, mode)
    return payload


def build_diagnostics(out):
    hashes = {source: digest((ROOT / source).read_bytes()) for source, _ in INPUTS.values()}
    out.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', CODEC_PROBE_OUT=str(out))
    env.setdefault('HMOS_SDK', str(ROOT / 'work/ohos-sdk/installed/native'))
    commands = [
        ['sh', 'compat/telephony/build-qmi-probe.sh', str(out / 'qmi_probe')],
        ['sh', 'compat/codec/build-native-codec-probe.sh'],
        [sys.executable, '-B', 'tools/build/build_current.py', '--out', str(out),
         'bluetooth-bridge-marble', 'bluetooth-bridge-marble-lib'],
    ]
    for command in commands:
        subprocess.run(command, cwd=ROOT, env=env, check=True)
    if any(digest((ROOT / source).read_bytes()) != sha for source, sha in hashes.items()):
        raise ValueError('Diagnostics sources changed during build; start a fresh build')
    records = {}
    for name, (source, _mode) in INPUTS.items():
        blob = inspect_artifact(out / name)
        records[name] = {'source': source, 'source_sha256': hashes[source],
                         'bytes': len(blob), 'sha256': digest(blob)}
    with (out / 'diagnostics.json').open('x', encoding='utf-8') as f:
        json.dump({'artifacts': records, 'device_executed': False, 'flashed': False}, f, indent=2)
        f.write('\n')
    load_diagnostics(out)
    return out / 'diagnostics.json'


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument('--out', required=True, type=Path, help='New output directory')
    args = cli.parse_args()
    print(build_diagnostics(args.out.resolve()))


if __name__ == '__main__':
    main()
