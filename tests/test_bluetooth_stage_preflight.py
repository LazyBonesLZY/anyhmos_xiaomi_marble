from pathlib import Path
import importlib.util
import unittest
ROOT=Path(__file__).resolve().parents[1]
s=importlib.util.spec_from_file_location('btpre',ROOT/'tools/maint/bluetooth_stage_preflight.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m)
class BluetoothStagePreflightTests(unittest.TestCase):
    def test_live_numbers_not_historical_constants(self):
        r=m.plan('508:0\n','497:0\n')
        self.assertEqual(r['tty']['major'],508)
        self.assertEqual(r['power']['major'],497)
        self.assertFalse(r['power_operation_performed'])
    def test_existing_node_conflict_holds(self):
        with self.assertRaises(ValueError):m.plan('508:0','497:0',existing_tty=(507,0))
    def test_malformed_identity_rejected(self):
        for v in ('','0:0','508:0 extra','-1:0','4096:0'):
            with self.subTest(v=v),self.assertRaises(ValueError):m.device_numbers(v)
