import importlib.util
from pathlib import Path
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('mic_merge', ROOT/'tools/packaging/merge_marble_mic_route.py')
MOD = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MOD)

class MicRouteMergeTests(unittest.TestCase):
    def test_rejects_unknown_inputs(self):
        with self.assertRaisesRegex(ValueError, 'identity mismatch'):
            MOD.compose(b'<mixer/>', b'<mixer/>')

    def test_verified_stock_path_replaces_only_the_target(self):
        folder = ROOT/'work/marble-mic-xml.ijm5fxrv'
        if not folder.is_dir(): self.skipTest('stock audio XML fixture absent')
        base = MOD.read_regular(folder/'mixer_paths_ukee_mtp.xml')
        overlay = MOD.read_regular(folder/'mixer_paths_overlay_static.xml')
        merged = MOD.compose(base, overlay)
        tree = ET.fromstring(merged)
        path, = tree.findall("./path[@name='handset-dmic-endfire']")
        values = {c.attrib['name']:c.attrib['value'] for c in path}
        self.assertEqual(values['TX DEC1 MUX'], 'SWR_MIC')
        self.assertEqual(values['TX DEC3 MUX'], 'SWR_MIC')
        self.assertEqual(values['AMIC1_MIXER Switch'], '1')
        self.assertEqual(values['AMIC5_MIXER Switch'], '1')
        self.assertNotIn('TX DMIC MUX1', values)
        for name in ('speaker','handset-mic','speaker-mic','amic1','amic5'):
            before, = ET.fromstring(base).findall(f"./path[@name='{name}']")
            after, = tree.findall(f"./path[@name='{name}']")
            self.assertEqual(ET.tostring(before), ET.tostring(after))
        with self.assertRaises(ValueError): MOD.compose(base+b' ', overlay)

    def test_actual_speaker_route_uses_verified_existing_amic5(self):
        folder = ROOT/'work/marble-mic-xml.ijm5fxrv'
        if not folder.is_dir(): self.skipTest('stock audio XML fixture absent')
        base = MOD.read_regular(folder/'mixer_paths_ukee_mtp.xml')
        overlay = MOD.read_regular(folder/'mixer_paths_overlay_static.xml')
        tree = ET.fromstring(MOD.compose(base, overlay, 'speaker-mic'))
        target, = tree.findall("./path[@name='speaker-mic']")
        self.assertEqual(len(target), 1)
        self.assertEqual(target[0].tag, 'path')
        self.assertEqual(target[0].attrib, {'name':'amic5'})
        for name in ('speaker','handset-dmic-endfire','amic5'):
            before, = ET.fromstring(base).findall(f"./path[@name='{name}']")
            after, = tree.findall(f"./path[@name='{name}']")
            self.assertEqual(ET.tostring(before), ET.tostring(after))
        with self.assertRaises(ValueError): MOD.compose(base, overlay, 'unknown')
