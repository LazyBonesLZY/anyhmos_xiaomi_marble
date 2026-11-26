import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BUNDLE = 'org.example.marble.hapfixture.a91e4f62'

class BundleFixtureCliTests(unittest.TestCase):
    def test_recorder_uses_foreground_user_not_singleton_user(self):
        s=(ROOT/'compat/services/native_bundle_cli.c').read_text()
        self.assertIn('system_core && strcmp(name, "SoundRecorder") ? "0" : "100"',s)
        self.assertIn('strcmp(name, "ArkWebCoreLegacy")',s)
        self.assertIn('strcmp(name, "SoundRecorder")',s)

    def test_fixed_bundle_and_exec_arguments(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            request, log, argv, hap = (d/n for n in ('request','log','argv.json','fixture.hap'))
            mock = d/'bm'
            mock.write_text('#!/usr/bin/env python3\nimport json,sys\n'
                            f'open({str(argv)!r},"w").write(json.dumps(sys.argv[1:]))\n')
            mock.chmod(0o700)
            hap.write_bytes(b'test archive identity is verified by packaging, not this CLI')
            hap.chmod(0o444)
            binary=d/'cli'
            defs=[f'-D{k}="{v}"' for k,v in (
                ('HMOS_BUNDLE_REQUEST',request),('HMOS_BUNDLE_LOG',log),
                ('HMOS_BM_PATH',mock),('HMOS_FIXTURE_HAP',hap))]
            subprocess.run(['cc','-D_DEFAULT_SOURCE','-std=c17','-O2','-Wall','-Wextra','-Werror',
                            *defs,str(ROOT/'compat/services/native_bundle_cli.c'),'-o',str(binary)],check=True)
            def run(text):
                request.write_text(text)
                if argv.exists(): argv.unlink()
                return subprocess.run([str(binary)],capture_output=True).returncode
            self.assertEqual(run('uninstall-fixture '+BUNDLE),0)
            self.assertEqual(json.loads(argv.read_text()),['uninstall','-n',BUNDLE,'-u','100'])
            self.assertEqual(run('uninstall-fixture com.huawei.hmos.clock'),4)
            self.assertFalse(argv.exists())
            self.assertEqual(run('install-fixture OTHER'),4)
            self.assertEqual(run('uninstall-fixture '+BUNDLE+' extra'),3)
            self.assertEqual(run('dump com.huawei.hmos.clock'),0)
            self.assertEqual(json.loads(argv.read_text()),['dump','-n','com.huawei.hmos.clock','-u','100'])
            if os.getuid()==0:
                self.assertEqual(run('install-fixture '+BUNDLE),0)
                self.assertEqual(json.loads(argv.read_text()),['install','-p',str(hap),'-u','100'])
            else:
                self.assertEqual(run('install-fixture '+BUNDLE),4)
            hap.unlink()
            hap.symlink_to(request)
            self.assertEqual(run('install-fixture '+BUNDLE),4)
            self.assertFalse(argv.exists())
