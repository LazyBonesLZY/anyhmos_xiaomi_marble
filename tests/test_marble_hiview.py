"""Contracts for the marble hiview / hisysevent wiring.

The device previously logged, on every boot and for every process::

    E C02D08/samgr/HISYSEVENT_TRANSPORT: /dev/unix/socket/hisysevent write
    failed, errno=2, msg=No such file or directory

`/dev/unix/socket` was never created because no service declared the
`hisysevent` sockets.  The donor ADY-AL10 ROM ships `hiview` with two
AF_UNIX datagram sockets; declaring the same service on marble lets init
create `/dev/unix/socket/hisysevent{,_fast}` and start the broker.
"""

import json
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
INIT_DIR = ROOT / 'devices/marble/runtime/init'
PLATFORM_CFG = INIT_DIR / 'init-platform-marble.cfg'
PHASE4_CFG = INIT_DIR / 'hmos-init-phase4.cfg'


def _load(path):
    return json.loads(path.read_text(encoding='utf-8'))


def _service(cfg, name):
    for service in cfg.get('services', []):
        if service.get('name') == name:
            return service
    return None


class MarbleHiviewTests(unittest.TestCase):
    def test_hiview_service_declares_both_hisysevent_sockets(self):
        cfg = _load(PLATFORM_CFG)
        service = _service(cfg, 'hiview')
        self.assertIsNotNone(service, 'hiview service missing from platform cfg')
        self.assertEqual(service['path'], ['/system/bin/hiview'])
        self.assertEqual(service['uid'], 'hiview')
        self.assertEqual(service['secon'], 'u:r:hiview:s0')
        # hiview itself is kept as documentation only: it needs device nodes this
        # kernel does not provide and would crash-loop, so the compiled sink owns
        # the sockets instead.  The service must stay disabled and non-repeating.
        self.assertEqual(service['start-mode'], 'condition')
        self.assertEqual(service['disabled'], 1)
        self.assertEqual(service.get('once'), 1)
        self.assertEqual(service.get('sandbox'), 0)
        sockets = {s['name']: s for s in service.get('socket', [])}
        self.assertEqual(set(sockets), {'hisysevent', 'hisysevent_fast'})
        for name, spec in sockets.items():
            self.assertEqual(spec['type'], 'SOCK_DGRAM', name)
            self.assertEqual(spec['permissions'], '0662', name)
            self.assertEqual(spec['uid'], 'hiview', name)
            self.assertEqual(spec['gid'], 'system', name)

    def test_hiview_job_creates_the_socket_directory(self):
        cfg = _load(PLATFORM_CFG)
        jobs = {job['name']: job for job in cfg.get('jobs', [])}
        self.assertIn('services:hiview', jobs)
        cmds = jobs['services:hiview']['cmds']
        self.assertIn('mkdir /dev/unix 0777 root root', cmds)
        self.assertIn('mkdir /dev/unix/socket 0777 root root', cmds)
        # init runs this job right before starting the hiview service.
        self.assertTrue(
            any(isinstance(cmd, str) and cmd.startswith('mkdir /data/log/hiview')
                for cmd in cmds),
            cmds,
        )

    def test_platform_cfg_is_imported_by_the_phase4_root_cfg(self):
        root = _load(PHASE4_CFG)
        self.assertIn('/compat/etc/init-platform-marble.cfg', root.get('import', []))

    def test_no_duplicate_service_names_inside_the_loaded_cfg_set(self):
        root = _load(PHASE4_CFG)
        loaded = [PHASE4_CFG]
        for imported in root.get('import', []):
            candidate = INIT_DIR / Path(imported).name
            if candidate.exists():
                loaded.append(candidate)
        self.assertIn(PLATFORM_CFG, loaded)
        seen = {}
        for path in loaded:
            for service in _load(path).get('services', []):
                name = service.get('name')
                self.assertNotIn(
                    name, seen,
                    f'service {name!r} defined twice: {seen.get(name)} and {path.name}',
                )
                seen[name] = path.name


if __name__ == '__main__':
    unittest.main()
