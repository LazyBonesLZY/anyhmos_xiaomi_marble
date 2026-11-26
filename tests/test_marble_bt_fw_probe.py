"""Host unit tests for Marble QCA6490 baud switch, TLV/NVM patching, and HCI checks."""
from __future__ import annotations

import shutil
import socket
import struct
import subprocess
import tempfile
import threading
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'compat/bluetooth/marble_bt_fw_probe.c'

HARNESS = r'''
#define main fw_probe_main
#include "marble_bt_fw_probe.c"
#undef main

static int fake_switch(int fd, uint32_t baud, void *ctx)
{
    (void)fd;
    uint32_t *recorded = ctx;
    *recorded = baud;
    printf("HOST_SWITCHED=%u\n", baud);
    return 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc < 3) return 2;
    const char *mode = argv[1];
    int fd = atoi(argv[2]);
    if (!strcmp(mode, "baud")) {
        uint32_t baud = (uint32_t)strtoul(argv[3], NULL, 10);
        bool unified = atoi(argv[4]) != 0;
        uint32_t switched = 0;
        if (set_baud_rate_proto(fd, baud, unified, 500, fake_switch, &switched) != 0) {
            printf("BAUD_FAIL errno=%d\n", errno);
            return 1;
        }
        printf("BAUD_PASS switched=%u\n", switched);
        return 0;
    }
    if (!strcmp(mode, "patch_dl")) {
        uint8_t buf[600];
        memset(buf, 0xaa, sizeof(buf));
        uint32_t tlv_len = (uint32_t)(sizeof(buf) - 4);
        buf[0] = QCA_TLV_TYPE_PATCH;
        buf[1] = (uint8_t)(tlv_len & 0xff);
        buf[2] = (uint8_t)((tlv_len >> 8) & 0xff);
        buf[3] = (uint8_t)((tlv_len >> 16) & 0xff);
        buf[14] = (uint8_t)atoi(argv[3]); /* download_mode */
        buf[16] = 0x13; buf[17] = 0x00;
        buf[18] = 0x01; buf[19] = 0x02;
        buf[20] = 0x81; buf[21] = 0x5e;
        struct rom_version v = { .product = 0x13, .rom = 0x0201, .soc = 0x400c1211, .unified = true };
        uint8_t dm = 0; uint16_t pv = 0;
        if (validate_rampatch(buf, sizeof(buf), &v, &dm, &pv) != 0) {
            printf("VALIDATE_FAIL errno=%d\n", errno);
            return 1;
        }
        unsigned segs = 0;
        if (download_tlv_stream(fd, buf, sizeof(buf), QCA_TLV_TYPE_PATCH, dm, true, 500, &segs) != 0) {
            printf("DL_FAIL errno=%d\n", errno);
            return 1;
        }
        printf("DL_PASS segs=%u pv=0x%04x\n", segs, (unsigned)pv);
        return 0;
    }
    if (!strcmp(mode, "nvm_patch")) {
        uint8_t buf[4 + 12 + 6 + 12 + 11 + 12 + 120];
        memset(buf, 0, sizeof(buf));
        uint32_t tl = (uint32_t)(sizeof(buf) - 4);
        buf[0] = QCA_TLV_TYPE_NVM;
        buf[1] = (uint8_t)(tl & 0xff);
        buf[2] = (uint8_t)((tl >> 8) & 0xff);
        buf[3] = (uint8_t)((tl >> 16) & 0xff);
        size_t p = 4;
        /* Tag 17 */
        buf[p] = 17; buf[p+2] = 6; buf[p+12] = 0x95; buf[p+13] = 0x0e;
        p += 12 + 6;
        /* Tag 27 */
        buf[p] = 27; buf[p+2] = 11; buf[p+12] = 0x01; buf[p+13] = 0x01;
        p += 12 + 11;
        /* Tag 35 */
        buf[p] = 35; buf[p+2] = 120; buf[p+12+0x76] = (uint8_t)atoi(argv[3]);
        struct nvm_patch_info info;
        if (parse_and_patch_nvm(buf, sizeof(buf), 0x11, true, &info) != 0) {
            printf("NVM_FAIL errno=%d\n", errno);
            return 1;
        }
        printf("NVM_PASS t17=%02x%02x->%02x%02x t27=%02x%02x->%02x%02x ipa=%d\n",
               info.tag17_old[0], info.tag17_old[1], info.tag17_new[0], info.tag17_new[1],
               info.tag27_old[0], info.tag27_old[1], info.tag27_new[0], info.tag27_new[1],
               info.ipa_supported ? 1 : 0);
        return 0;
    }
    if (!strcmp(mode, "board_and_hci")) {
        uint8_t b0 = 0, b1 = 0; bool valid = false;
        if (query_board_id(fd, true, 500, &b0, &b1, &valid) != 0) {
            printf("BID_FAIL errno=%d\n", errno); return 1;
        }
        if (hci_reset_strict(fd, 500) != 0) {
            printf("RESET_FAIL errno=%d\n", errno); return 1;
        }
        struct local_version lv = {0};
        if (hci_read_local_version(fd, 500, &lv) != 0) {
            printf("VER_FAIL errno=%d\n", errno); return 1;
        }
        uint8_t bd[6] = {0};
        if (hci_read_bd_addr(fd, 500, bd) != 0) {
            printf("BD_FAIL errno=%d\n", errno); return 1;
        }
        printf("HCI_PASS bid=%d:%02x:%02x ver=%02x:%04x:%04x bd=%02x:%02x:%02x:%02x:%02x:%02x\n",
               valid ? 1 : 0, b0, b1, lv.hci_ver, lv.manufacturer, lv.lmp_subver,
               bd[5], bd[4], bd[3], bd[2], bd[1], bd[0]);
        return 0;
    }
    return 2;
}
'''


def recv_exact(sock: socket.socket, n: int) -> bytes:
    buf = bytearray()
    while len(buf) < n:
        part = sock.recv(n - len(buf))
        if not part:
            raise EOFError(f'short read {len(buf)}/{n}')
        buf.extend(part)
    return bytes(buf)


def recv_hci_cmd(sock: socket.socket) -> tuple[int, bytes]:
    hdr = recv_exact(sock, 4)
    assert hdr[0] == 0x01, hdr
    opcode = hdr[1] | (hdr[2] << 8)
    plen = hdr[3]
    return opcode, recv_exact(sock, plen)


@unittest.skipUnless(shutil.which('cc'), 'no host C compiler')
class MarbleBtFwProbeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='marble-bt-fw-test-')
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.bin = Path(cls.tmp.name) / 'fw-probe-fixture'
        subprocess.run(
            ['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
             '-I', str(SOURCE.parent), '-x', 'c', '-', '-o', str(cls.bin)],
            input=HARNESS, text=True, check=True, timeout=20,
        )

    def run_fixture(self, args: list[str], responder):
        peer, uart = socket.socketpair()
        err_box: list[BaseException] = []
        def worker():
            try:
                peer.settimeout(2.0)
                responder(peer)
            except BaseException as exc:
                err_box.append(exc)
        t = threading.Thread(target=worker, daemon=True)
        t.start()
        try:
            proc = subprocess.run(
                [str(self.bin), args[0], str(uart.fileno()), *args[1:]],
                pass_fds=(uart.fileno(),), capture_output=True, text=True, timeout=5,
            )
        finally:
            uart.close()
            t.join(timeout=2)
            peer.close()
        if err_box:
            raise err_box[0]
        return proc

    def test_baud_switch_unified_pass_and_bad_status_fail(self):
        def ok_resp(peer):
            op, params = recv_hci_cmd(peer)
            self.assertEqual((op, params), (0xfc48, b'\x11'))
            peer.sendall(bytes.fromhex('04 0e 04 01 48 fc 01'))
        proc = self.run_fixture(['baud', '3200000', '1'], ok_resp)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn('BAUD_PASS switched=3200000', proc.stdout)

        for bad in (b'\x00', b'\x02'):
            def fail_resp(peer, status=bad):
                op, params = recv_hci_cmd(peer)
                self.assertEqual((op, params), (0xfc48, b'\x11'))
                peer.sendall(bytes.fromhex('04 0e 04 01 48 fc') + status)
            proc = self.run_fixture(['baud', '3200000', '1'], fail_resp)
            self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)

    def test_rampatch_skip_vse_cc_acks_only_last_segment_with_subcmd_1e(self):
        seen = []
        def resp(peer):
            # 600 bytes -> 243 + 243 + 114 = 3 segments
            for idx in range(3):
                op, params = recv_hci_cmd(peer)
                self.assertEqual(op, 0xfc00)
                self.assertEqual(params[0], 0x1e)
                self.assertEqual(params[1], len(params) - 2)
                seen.append(params[1])
                if idx == 2:
                    peer.sendall(bytes.fromhex('04 0e 05 01 00 fc 00 1e'))
        proc = self.run_fixture(['patch_dl', '3'], resp)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(seen, [243, 243, 114])
        self.assertIn('DL_PASS segs=3 pv=0x5e81', proc.stdout)

    def test_nvm_patching_disables_sibs_sets_baud_and_detects_ipa(self):
        for tag35_byte, expect_ipa in ((0x01, 0), (0x05, 1)):
            proc = subprocess.run(
                [str(self.bin), 'nvm_patch', '0', str(tag35_byte)],
                capture_output=True, text=True, check=True, timeout=5,
            )
            self.assertIn(f'NVM_PASS t17=950e->1511 t27=0101->0100 ipa={expect_ipa}', proc.stdout)

    def test_board_id_reset_local_version_and_bd_addr(self):
        def resp(peer):
            op, params = recv_hci_cmd(peer)
            self.assertEqual((op, params), (0xfc00, b'\x23'))
            peer.sendall(bytes.fromhex('04 0e 08 01 00 fc 00 23 02 00 5a'))
            op, params = recv_hci_cmd(peer)
            self.assertEqual((op, params), (0x0c03, b''))
            peer.sendall(bytes.fromhex('04 0e 04 01 03 0c 00'))
            op, params = recv_hci_cmd(peer)
            self.assertEqual((op, params), (0x1001, b''))
            peer.sendall(bytes.fromhex('04 0e 0c 01 01 10 00 0b') + struct.pack('<HBHH', 0x5e81, 0x0b, 0x001d, 0x0201))
            op, params = recv_hci_cmd(peer)
            self.assertEqual((op, params), (0x1009, b''))
            peer.sendall(bytes.fromhex('04 0e 0a 01 09 10 00 11 22 33 44 55 66'))
        proc = self.run_fixture(['board_and_hci'], resp)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn('HCI_PASS bid=1:00:5a ver=0b:001d:0201 bd=66:55:44:33:22:11', proc.stdout)


if __name__ == '__main__':
    unittest.main()
