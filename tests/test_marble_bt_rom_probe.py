"""Test the Marble ROM query against synthetic H4 only, not a controller."""
import errno
import shutil
import socket
import struct
import subprocess
import tempfile
import time
import unittest
from contextlib import contextmanager
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'compat/bluetooth/marble_bt_rom_probe.c'
QUERY = bytes.fromhex('01 00 fc 01 19')
COMPLETE = bytes.fromhex('04 0e 04 01 00 fc 00')
IDENTITY = struct.pack('<IHHI', 0x13, 0x1234, 0x0200, 0x400c0200)
LEGACY = bytes.fromhex('04 ff 0e 00 02') + IDENTITY
UNIFIED = bytes.fromhex('04 0e 12 01 00 fc 00 19 a5') + IDENTITY
HARNESS = r'''
#define main rom_probe_main
#include "marble_bt_rom_probe.c"
#undef main
#include <stdlib.h>
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    struct rom_version version = {0};
    if (query_version(atoi(argv[1]), atoi(argv[2]), &version) < 0) {
        printf("FAIL %d\n", errno);
        return 1;
    }
    printf("PASS %s %08" PRIx32 " %04x %04x %08" PRIx32 "\n", version.format,
           version.product, (unsigned)version.patch, (unsigned)version.rom, version.soc);
    return 0;
}
'''


@unittest.skipUnless(shutil.which('cc'), 'no host C compiler')
class MarbleBtRomProbeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        temporary = tempfile.TemporaryDirectory(prefix='marble-bt-rom-test-')
        cls.addClassCleanup(temporary.cleanup)
        cls.binary = Path(temporary.name) / 'probe-fixture'
        subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-I', str(SOURCE.parent), '-x', 'c', '-', '-o', str(cls.binary)],
                       input=HARNESS, text=True, check=True, timeout=20)

    @contextmanager
    def controller(self, milliseconds=150):
        peer, uart = socket.socketpair()
        with peer, uart, subprocess.Popen([str(self.binary), str(uart.fileno()), str(milliseconds)],
                pass_fds=(uart.fileno(),), stderr=subprocess.PIPE, stdout=subprocess.PIPE, text=True) as child:
            try:
                peer.settimeout(2)
                wire = bytearray()
                while len(wire) < len(QUERY):
                    part = peer.recv(len(QUERY) - len(wire))
                    self.assertTrue(part)
                    wire.extend(part)
                self.assertEqual(wire, QUERY)
                yield peer, child
                # Python's timeout mode polls before recv even with MSG_DONTWAIT.
                # Keep uart open here and verify that no second request was sent.
                peer.setblocking(False)
                with self.assertRaises(BlockingIOError):
                    peer.recv(1)
            finally:
                if child.poll() is None:
                    child.kill()
                child.communicate()

    def check_outcome(self, child, code, line):
        text, err = child.communicate(timeout=2)
        self.assertEqual(child.returncode, code, text + err)
        self.assertIn(line, text.splitlines())

    def test_unified_keeps_extra_byte_out_of_version_fields(self):
        with self.controller(800) as (peer, child):
            peer.sendall(bytes.fromhex('04 0e 04 01 03 0c 00') + UNIFIED[:2])
            time.sleep(0.01)
            peer.sendall(UNIFIED[2:9])
            time.sleep(0.01)
            peer.sendall(UNIFIED[9:])
            self.check_outcome(child, 0, 'PASS unified 00000013 1234 0200 400c0200')

    def test_legacy_requires_both_ordered_frames(self):
        with self.controller(800) as (peer, child):
            peer.sendall(LEGACY + COMPLETE)
            self.check_outcome(child, 0, 'PASS legacy 00000013 1234 0200 400c0200')
        for data in (LEGACY, COMPLETE, COMPLETE + LEGACY):
            with self.subTest(data=data.hex()), self.controller() as (peer, child):
                peer.sendall(data)
                self.check_outcome(child, 1, f'FAIL {errno.ETIMEDOUT}')

    def test_wrong_or_truncated_version_cannot_pass(self):
        for data, error in (
            (bytes.fromhex('04 ff 0a 00 02') + IDENTITY[:8] + COMPLETE, errno.EPROTO),
            (UNIFIED[:3] + UNIFIED[3:6] + b'\x01' + UNIFIED[7:], errno.EPROTO),
            (UNIFIED[:7] + b'\x06' + UNIFIED[8:], errno.EPROTO),
            (UNIFIED[:-4] + bytes(4), errno.EPROTO),
            (b'\xfd', errno.EPROTO),
            (UNIFIED[:-2], errno.ETIMEDOUT),
            (bytes.fromhex('04 0f 04 0c 01 00 fc'), errno.EPROTO),
        ):
            with self.subTest(data=data.hex()), self.controller() as (peer, child):
                peer.sendall(data)
                self.check_outcome(child, 1, f'FAIL {error}')
