from pathlib import Path
import importlib.util
import unittest
import xml.etree.ElementTree as ET
ROOT=Path(__file__).resolve().parents[1]
s=importlib.util.spec_from_file_location('mono_policy',ROOT/'tools/packaging/mono_builtin_input_policy.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m)
class MonoPolicyTests(unittest.TestCase):
    def test_only_builtin_input_routes_are_narrowed(self):
        p=ROOT/'work/marble-pcm-policy-bidir.puyqqxkx/audio_policy_config.xml'
        if not p.is_file():self.skipTest('policy fixture absent')
        old=ET.fromstring(p.read_bytes());new=ET.fromstring(m.compose(p.read_bytes()))
        a=old.find("./adapters/adapter[@name='primary']");b=new.find("./adapters/adapter[@name='primary']")
        targets=[]
        for pipe in b.findall('./pipes/pipe'):
            for prop in pipe.findall('./streamProps/streamProp'):
                if 'Builtin_Mic_In' in [v.strip() for v in prop.attrib.get('supportDevices','').split(',')]:targets.append((pipe.attrib['name'],prop.attrib))
        self.assertEqual(len(targets),1)
        self.assertEqual(targets[0][0],'primary_input')
        self.assertEqual(targets[0][1]['channelLayout'],'CH_LAYOUT_MONO')
        self.assertEqual(targets[0][1]['sampleRates'],'48000')
        self.assertEqual(targets[0][1]['bufferSize'],'1920')
        for pipe in a.findall('./pipes/pipe'):
            if pipe.attrib['role']=='output':
                other=b.find(f"./pipes/pipe[@name='{pipe.attrib['name']}']")
                self.assertEqual(ET.tostring(pipe),ET.tostring(other))
    def test_unknown_policy_rejected(self):
        with self.assertRaises(ValueError):m.compose(b'<policy/>')
