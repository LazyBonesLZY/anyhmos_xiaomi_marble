from pathlib import Path
import importlib.util
import unittest
ROOT=Path(__file__).resolve().parents[1]
s=importlib.util.spec_from_file_location('yuv_failure',ROOT/'tools/maint/analyze_yuv_import_failure.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m)
class YuvImportFailureTests(unittest.TestCase):
    def test_valid_fd_does_not_mean_valid_gpu_import(self):
        text='''HMOS_IMPORT_V4 tid=1 call=3 valid=0 vk=0
HMOS_HANDLE_V4 fd=7 fstat=0 width=960 height=720 stride=1024 format=24
NativeBufferUtils: vkCreateImage failed
'''
        r=m.summarize(text)
        self.assertTrue(r['failed_yuv_imports'][0]['fd_valid'])
        self.assertFalse(r['failed_yuv_imports'][0]['backend_valid'])
        self.assertTrue(r['vk_create_image_failure_observed'])
        self.assertFalse(r['decoder_failure_proven'])
    def test_rgb_not_reported_as_yuv_failure(self):
        r=m.summarize('HMOS_IMPORT_V4 valid=0\nHMOS_HANDLE_V4 format=12 width=10 height=10 stride=256 fstat=0')
        self.assertEqual(r['failed_yuv_imports'],[])
