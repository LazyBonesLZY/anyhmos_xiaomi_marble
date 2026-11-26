from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
SCRIPT=ROOT/'devices/marble/runtime/boot/late-softbus-marble.sh'

class LateSoftbusTests(unittest.TestCase):
    def test_readiness_negative_and_positive(self):
        for ready in (True,False):
            with self.subTest(ready=ready), tempfile.TemporaryDirectory() as tmp:
                d=Path(tmp);(d/'dev').mkdir()
                b=d/'busybox'
                b.write_text('#!/bin/sh\n'
                    'case "$1" in\n'
                    ' mkdir) shift; exec mkdir "$@";;\n'
                    ' cat) echo '+('1' if ready else '0')+';;\n'
                    ' timeout) case "$*" in *"param set"*) echo SET >> "$TEST_LOG";; esac; exit '+('0' if ready else '1')+';;\n'
                    ' sleep) exit 0;;\n'
                    'esac\n')
                b.chmod(0o700)
                harness=f'''B='{b}';R='{d}';export TEST_LOG='{d/'sets'}'
now() {{ date +%s; }}
log() {{ echo "$*"; }}
pget() {{ echo false; }}
wait_sa() {{ echo "WAIT $1"; }}
'''
                code=SCRIPT.read_text().replace(' + 60',' + 0')
                r=subprocess.run(['sh'],input=harness+code,text=True,capture_output=True,check=True)
                if ready:
                    self.assertIn('SOFTBUS_LATE_TRIGGERED',r.stdout)
                    self.assertEqual((d/'sets').read_text(),'SET\n')
                else:
                    self.assertIn('SOFTBUS_LATE_TIMEOUT',r.stdout)
                    self.assertFalse((d/'sets').exists())
                # Reuse the boot owner: no second start.
                r2=subprocess.run(['sh'],input=harness+code,text=True,capture_output=True,check=True)
                self.assertIn('owner_exists=1',r2.stdout)
