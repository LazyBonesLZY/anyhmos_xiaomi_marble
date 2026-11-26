"""Check the host-side Phase 4 diagnostics handoff without executing a target binary."""
import hashlib
import json
import struct
import tempfile
import unittest
from pathlib import Path

from tools.packaging import marble_diagnostics as diag


class MarbleDiagnosticsTests(unittest.TestCase):
    def setUp(self):
        scratch = tempfile.TemporaryDirectory(prefix='marble-diagnostics-')
        self.addCleanup(scratch.cleanup)
        self.directory = Path(scratch.name)
        self.report_path = self.directory / 'diagnostics.json'
        self.report = {'artifacts': {}}
        self.blobs = {}
        for name, (source, _) in diag.INPUTS.items():
            # The packager checks an ELF identity header, not runnability.
            blob = bytearray(64)
            blob[:6] = b'\x7fELF\x02\x01'
            struct.pack_into('<HH', blob, 16, 3 if name.endswith('.so') else 2, 183)
            data = bytes(blob) + name.encode('ascii')
            self.blobs[name] = data
            (self.directory / name).write_bytes(data)
            self.report['artifacts'][name] = {
                'source': source,
                'source_sha256': hashlib.sha256((diag.ROOT / source).read_bytes()).hexdigest(),
                'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest(),
            }
        self.write_report()

    def write_report(self):
        self.report_path.write_text(json.dumps(self.report), encoding='utf-8')

    def test_returns_measured_bytes_and_modes_for_packaging(self):
        payload = diag.load_diagnostics(self.directory)
        self.assertEqual(payload, {f'graphics/{name}': (self.blobs[name], mode)
                                  for name, (_, mode) in diag.INPUTS.items()})
        (self.directory / 'qmi_probe').write_bytes(b'changed after verification')
        self.assertEqual(payload['graphics/qmi_probe'][0], self.blobs['qmi_probe'])

    def test_missing_record_has_no_historical_fallback(self):
        self.report_path.unlink()
        with self.assertRaisesRegex(ValueError, 'Missing diagnostics build record'):
            diag.load_diagnostics(self.directory)

    def test_rejects_stale_source_even_if_binary_hash_matches(self):
        self.report['artifacts']['native_codec_probe']['source_sha256'] = '0' * 64
        self.write_report()
        with self.assertRaisesRegex(ValueError, 'Stale diagnostics source for native_codec_probe'):
            diag.load_diagnostics(self.directory)

    def test_rejects_binary_replacement_without_a_matching_record(self):
        (self.directory / 'hmos_bt_bridge_marble').write_bytes(self.blobs['hmos_bt_bridge_marble'] + b'changed')
        with self.assertRaisesRegex(ValueError, 'does not match its build record'):
            diag.load_diagnostics(self.directory)

    def test_rejects_wrong_architecture_even_with_a_matching_record(self):
        data = bytearray(self.blobs['qmi_probe'])
        struct.pack_into('<H', data, 18, 62)  # x86-64
        (self.directory / 'qmi_probe').write_bytes(data)
        self.report['artifacts']['qmi_probe']['sha256'] = hashlib.sha256(data).hexdigest()
        self.write_report()
        with self.assertRaisesRegex(ValueError, 'Expected an AArch64'):
            diag.load_diagnostics(self.directory)

    def test_record_and_artifact_set_are_exact(self):
        self.report['artifacts']['extra'] = {}
        self.write_report()
        with self.assertRaisesRegex(ValueError, 'Unexpected diagnostics artifact set'):
            diag.load_diagnostics(self.directory)
