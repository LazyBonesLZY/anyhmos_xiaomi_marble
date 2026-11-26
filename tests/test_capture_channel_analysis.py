from pathlib import Path
import importlib.util
import unittest
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('channels',ROOT/'tools/maint/analyze_capture_channels.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
class ChannelAnalysisTests(unittest.TestCase):
    def test_identical_channels_are_not_independent_mics(self):
        r=m.summarize('MIC_CHANNEL id=0 pairs=2 nonzero=2 peak=2 sum=0 squares=8\nMIC_CHANNEL id=1 pairs=2 nonzero=2 peak=2 sum=0 squares=8\nMIC_PAIR equal=2 pairs=2 cross=8')
        self.assertTrue(r['identical_channels_observed'])
        self.assertFalse(r['two_independent_microphones_proven'])
        self.assertEqual(r['channels'][0]['ac_rms_s16'],2)
    def test_missing_statistics_rejected(self):
        with self.assertRaises(ValueError):m.summarize('PAL_CAPTURE_PROBE frames=24000')
