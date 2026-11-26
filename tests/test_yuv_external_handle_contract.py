from pathlib import Path
import importlib.util
import unittest
ROOT=Path(__file__).resolve().parents[1]
s=importlib.util.spec_from_file_location('yuv_contract',ROOT/'tools/maint/yuv_external_handle_contract.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m)
class YuvHandleContractTests(unittest.TestCase):
    def test_yuv_opaque_fd_mismatch_and_native_positive(self):
        self.assertFalse(m.compatible(0,24,1))
        self.assertTrue(m.compatible(0,24,0x8000))
        self.assertTrue(m.compatible(0,24,0x2000))
    def test_rgb_path_not_globally_changed(self):
        self.assertTrue(m.compatible(37,0,1))
        self.assertFalse(m.compatible(37,24,1))
