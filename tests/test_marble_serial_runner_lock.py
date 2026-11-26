"""Host-only COM9 runner serialization test; never opens a device."""
import importlib.util
from pathlib import Path
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]

class SerialRunnerLockTests(unittest.TestCase):
    def test_owned_calls_do_not_overlap(self):
        src=ROOT/'work/marble-dual-20261123/serial_local.py'
        if not src.is_file(): self.skipTest('serial runner not present')
        spec=importlib.util.spec_from_file_location('serial_runner_test',src)
        mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod)
        with tempfile.TemporaryDirectory() as tmp:
            mod.WIN=Path(tmp)
            active=0
            peak=0
            guard=threading.Lock()
            results=[]
            def fake(script,**kwargs):
                nonlocal active,peak
                with guard: active+=1;peak=max(peak,active)
                time.sleep(0.03)
                with guard: active-=1
                return script
            mod._run_locked=fake
            def call(i): results.append(mod.run(str(i),label='test'))
            threads=[threading.Thread(target=call,args=(i,)) for i in range(3)]
            for t in threads:t.start()
            for t in threads:t.join(timeout=2)
            self.assertTrue(all(not t.is_alive() for t in threads))
            self.assertEqual(peak,1)
            self.assertEqual(sorted(results),['0','1','2'])
