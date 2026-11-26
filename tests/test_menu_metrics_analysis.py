import importlib.util
from pathlib import Path
import unittest
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('menu_metrics',ROOT/'tools/maint/analyze_menu_metrics.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
class MenuMetricsTests(unittest.TestCase):
    def test_carried_idle_gap_is_not_menu_latency(self):
        text='''MEASURE_BEGIN uptime=10.0
MEASURE_BATCH t_ns=13000000000 submit frames=60 span_ms=3000 avg_us fence=1 import=1 props=1 commit=6000 cleanup=1 max_us=12000 over25ms=0
MEASURE_BATCH t_ns=13000000001 SUBMIT_PACING frames=60 entry_max_us=16000000 fence_error=0 clock_errors=0
MEASURE_END uptime=16.0
'''
        r=m.summarize(text)
        self.assertTrue(r['batches'][0]['fully_within_action'])
        self.assertTrue(r['pacing'][0]['contains_pre_action_or_idle_gap'])
        self.assertFalse(r['page_latency_measured'])
        self.assertEqual(r['batches'][0]['service_over25ms'],0)
    def test_rejects_missing_boundary(self):
        with self.assertRaises(ValueError):m.summarize('submit frames=1')
