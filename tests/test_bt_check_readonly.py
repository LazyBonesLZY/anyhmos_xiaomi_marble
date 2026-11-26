from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
class BtReadonlyCheckTests(unittest.TestCase):
    def test_check_never_creates_socket_parent(self):
        with tempfile.TemporaryDirectory() as tmp:
            d=Path(tmp);binary=d/'bt'
            subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror',str(ROOT/'compat/bluetooth/marble_bt_bridge.c'),'-o',str(binary)],check=True)
            fw=d/'fw';fw.mkdir();(fw/'hpbtfw20.tlv').write_bytes(b'fixture');(fw/'hpnv20.bin').write_bytes(b'fixture')
            tty=d/'tty';power=d/'power';tty.touch();power.touch()
            missing=d/'missing'
            args=[str(binary),'--check','--tty',str(tty),'--power',str(power),'--firmware-dir',str(fw),'--socket',str(missing/'h4')]
            r=subprocess.run(args,capture_output=True,text=True)
            self.assertNotEqual(r.returncode,0)
            self.assertFalse(missing.exists())
            ready=d/'ready';ready.mkdir()
            args[-1]=str(ready/'h4')
            r=subprocess.run(args,capture_output=True,text=True,check=True)
            self.assertFalse((ready/'h4').exists())
            self.assertIn('CHECK_RESULT=ok',r.stderr+r.stdout)
