"""Exercise the real Reset gate using synthetic H4 frames and a local socketpair."""
from contextlib import contextmanager
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import unittest


BRIDGE = Path(__file__).resolve().parents[1] / 'compat/bluetooth/marble_bt_bridge.c'
RESET = bytes.fromhex('01 03 0c 00')
RESET_OK = bytes.fromhex('04 0e 04 01 03 0c 00')
OTHER_COMPLETE = bytes.fromhex('04 0e 04 01 01 10 00')
RESET_STATUS_EVENT = bytes.fromhex('04 0f 04 00 01 03 0c')
# A byte pattern that looks exactly like Reset completion inside a vendor event.
VENDOR_EVENT = bytes((0x04, 0xff, len(RESET_OK))) + RESET_OK

FIXTURE = r'''
#define HMOS_BT_BRIDGE_NO_MAIN
#include "marble_bt_bridge.c"

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    struct hmos_bt_config cfg;
    hmos_bt_bridge_defaults(&cfg);
    cfg.timeout_ms = atoi(argv[2]);
    /* The socket-path check is our sentinel: it runs only after valid Reset.
     * Deliberately invalid so these tests never create a listener. */
    cfg.socket_path = "no-listener";
    return hmos_bt_reset_then_serve(atoi(argv[1]), &cfg) == 0 ? 0 : 1;
}
'''


class BtHciResetTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        temp = tempfile.TemporaryDirectory(prefix='marble-bt-reset-')
        cls.addClassCleanup(temp.cleanup)
        cls.fixture = Path(temp.name) / 'fixture'
        subprocess.run(
            ['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-I', str(BRIDGE.parent),
             '-x', 'c', '-', '-o', str(cls.fixture)],
            input=FIXTURE, text=True, check=True,
        )

    @contextmanager
    def controller(self, timeout_ms):
        host, bridge = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
        with host, bridge, subprocess.Popen(
            [str(self.fixture), str(bridge.fileno()), str(timeout_ms)],
            pass_fds=(bridge.fileno(),), stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
            text=True,
        ) as child:
            try:
                host.settimeout(3)
                sent = b''
                while len(sent) < len(RESET):
                    part = host.recv(len(RESET) - len(sent))
                    self.assertTrue(part, 'fixture closed before sending Reset')
                    sent += part
                self.assertEqual(sent, RESET)
                yield host, bridge, child
            finally:
                if child.poll() is None:
                    child.kill()
                child.communicate()

    def expect_gate_open(self, child):
        _, log = child.communicate(timeout=3)
        self.assertEqual(child.returncode, 1, log)  # sentinel rejects the deliberately invalid path
        self.assertIn('HMOS_BT HCI_RESET_OK\n', log)
        self.assertIn('HMOS_BT SOCKET_ERROR reason=bad_path', log)
        self.assertNotIn('BRINGUP_FAILED stage=hci_reset', log)

    def expect_gate_closed(self, child, reason):
        _, log = child.communicate(timeout=3)
        self.assertEqual(child.returncode, 1, log)
        self.assertIn('BRINGUP_FAILED stage=hci_reset reason=' + reason, log)
        self.assertNotIn('HCI_RESET_OK', log)
        self.assertNotIn('SOCKET_ERROR', log)
        self.assertNotIn('BRIDGE_LISTENING', log)

    def test_complete_matching_reset_enters_socket_path(self):
        with self.controller(1500) as (peer, _, child):
            peer.sendall(RESET_OK)
            self.expect_gate_open(child)

    def test_fragmented_reset_after_unrelated_events_preserves_next_frame(self):
        following = bytes.fromhex('04 10 01 01')
        with self.controller(1500) as (peer, bridge, child):
            peer.sendall(VENDOR_EVENT + OTHER_COMPLETE + RESET_STATUS_EVENT + RESET_OK[:1])
            time.sleep(0.02)
            peer.sendall(RESET_OK[1:5])
            time.sleep(0.02)
            peer.sendall(RESET_OK[5:] + following)
            self.expect_gate_open(child)
            self.assertEqual(bridge.recv(1024, socket.MSG_DONTWAIT), following)

    def test_unrelated_events_alone_never_enter_socket_path(self):
        for frame in (VENDOR_EVENT, OTHER_COMPLETE, RESET_STATUS_EVENT):
            with self.subTest(frame=frame.hex()), self.controller(100) as (peer, _, child):
                peer.sendall(frame)
                self.expect_gate_closed(child, 'no_matching_command_complete')

    def test_matching_opcode_with_error_status_fails(self):
        with self.controller(1500) as (peer, _, child):
            peer.sendall(RESET_OK[:-1] + b'\x0c' + RESET_OK)
            self.expect_gate_closed(child, 'controller_status status=0x0c')

    def test_matching_completion_requires_exact_parameter_length(self):
        for frame in (bytes.fromhex('04 0e 03 01 03 0c'), bytes.fromhex('04 0e 05 01 03 0c 00 00')):
            with self.subTest(frame=frame.hex()), self.controller(1500) as (peer, _, child):
                peer.sendall(frame)
                self.expect_gate_closed(child, 'bad_completion_length')

    def test_truncated_matching_completion_times_out(self):
        with self.controller(100) as (peer, _, child):
            peer.sendall(RESET_OK[:-1])
            self.expect_gate_closed(child, 'no_matching_command_complete')

    def test_non_event_does_not_enter_socket_path(self):
        with self.controller(1500) as (peer, _, child):
            peer.sendall(b'\x02' + RESET_OK)
            self.expect_gate_closed(child, 'not_event')

    def test_controller_eof_does_not_enter_socket_path(self):
        with self.controller(1500) as (peer, _, child):
            peer.shutdown(socket.SHUT_WR)
            self.expect_gate_closed(child, 'event_read')


if __name__ == '__main__':
    unittest.main()
