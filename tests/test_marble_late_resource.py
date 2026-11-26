from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
SCRIPT=ROOT/'devices/marble/runtime/boot/late-resource-marble.sh'
class LateResourceTests(unittest.TestCase):
    def test_readiness_and_directory_failures_never_set_gate(self):
        for ready,parent in [(True,True),(False,True),(True,False)]:
            with self.subTest(ready=ready,parent=parent),tempfile.TemporaryDirectory() as tmp:
                d=Path(tmp);(d/'dev').mkdir();(d/'data/service/el1/public').mkdir(parents=True)
                b=d/'busybox';b.write_text('#!/bin/sh\ncase "$1" in\n'
                    'mkdir) shift;exec mkdir "$@";;\n'
                    'cat) echo '+('1' if ready else '0')+';;\n'
                    'stat) echo '+('0:0:711' if parent else '0:0:755')+';;\n'
                    'timeout) case "$*" in *"param set"*) echo SET >> "$TEST_LOG";;esac;exit '+('0' if ready else '1')+';;\n'
                    'sleep) exit 0;;\nesac\n');b.chmod(0o700)
                h=f'''B='{b}';R='{d}';export TEST_LOG='{d/'sets'}'
now() {{ date +%s; }}
log() {{ echo "$*"; }}
pget() {{ echo false; }}
wait_sa() {{ echo "WAIT $1"; }}
'''
                code=SCRIPT.read_text().replace(' + 60',' + 0')
                r=subprocess.run(['sh'],input=h+code,text=True,capture_output=True,check=True)
                if ready and parent:
                    self.assertIn('RESOURCE_LATE_TRIGGERED',r.stdout)
                    self.assertEqual((d/'sets').read_text(),'SET\n')
                else:
                    self.assertFalse((d/'sets').exists())
                    self.assertTrue('TIMEOUT' in r.stdout or 'DIRECTORY_HOLD' in r.stdout)
                r2=subprocess.run(['sh'],input=h+code,text=True,capture_output=True,check=True)
                self.assertIn('owner_exists=1',r2.stdout)
