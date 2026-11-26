#!/usr/bin/env python3
"""Exercise the probe with synthetic datagrams and stubbed socket syscalls."""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# Numeric constants/offsets below are independent fixture values from the
# marble kernel's include/uapi/linux/qrtr.h and net/qrtr/ns.c.
HARNESS = r'''
#define _POSIX_C_SOURCE 200809L
#include <poll.h>
/* Declare the stub without glibc's write-only buffer attribute for poll(). */
int fake_poll(struct pollfd *, nfds_t, int);
#define socket fake_socket
#define getsockname fake_getsockname
#define bind fake_bind
#define poll fake_poll
#define sendto fake_sendto
#define recvmsg fake_recvmsg
#define close fake_close
#define main probe_main
#include "compat/telephony/qmi_probe.c"
#undef main
#include <assert.h>

_Static_assert(QRTR_PORT_CTRL == 0xfffffffeU, "control port ABI");
_Static_assert(sizeof(struct qrtr_ctrl_pkt) == 20, "control datagram ABI");
_Static_assert(sizeof(struct sockaddr_qrtr) == 12, "address ABI");
_Static_assert(offsetof(struct sockaddr_qrtr, sq_node) == 4, "node offset");
_Static_assert(offsetof(struct sockaddr_qrtr, sq_port) == 8, "port offset");
_Static_assert(QRTR_TYPE_NEW_SERVER == 4 && QRTR_TYPE_DEL_SERVER == 5 &&
               QRTR_TYPE_NEW_LOOKUP == 10 && QRTR_TYPE_DEL_LOOKUP == 11, "command ABI");

#define LOCAL 37U
#define PEER 0x25abU
#define PORT 0x5336U
#define INST 0x3101U
#define CTL 0xfffffffeU
#define FD 987

static const char *scenario;
static unsigned sent, queried, opened, closed, total_queries;
static uint32_t service;

struct event {
    uint8_t bytes[24];
    size_t len;
    uint32_t node, port;
    unsigned short family;
    socklen_t addrlen;
    int flags;
};

static struct event events[80];
static size_t next_event, event_count;

static struct event *emit(uint32_t node, uint32_t port, size_t len)
{
    struct event *e = &events[event_count++];
    assert(event_count <= sizeof(events) / sizeof(events[0]));
    memset(e, 0, sizeof(*e));
    e->len = len;
    e->node = node;
    e->port = port;
    e->family = AF_QIPCRTR;
    e->addrlen = 12;
    return e;
}

static void fixture_u32(uint8_t *out, uint32_t val)
{
    for (unsigned i = 0; i < 4; i++)
        out[i] = (uint8_t)(val >> (8 * i));
}

static struct event *ctrl(uint32_t cmd, uint32_t id, uint32_t instance,
                          uint32_t node, uint32_t port)
{
    struct event *e = emit(LOCAL, CTL, 20);
    fixture_u32(e->bytes, cmd);
    fixture_u32(e->bytes + 4, id);
    fixture_u32(e->bytes + 8, instance);
    fixture_u32(e->bytes + 12, node);
    fixture_u32(e->bytes + 16, port);
    return e;
}

static struct event *reply(const uint8_t *req, uint32_t node, uint32_t port)
{
    struct event *e = emit(node, port, 19);
    e->bytes[0] = 2;
    memcpy(e->bytes + 1, req + 1, 4);
    e->bytes[5] = 12;
    e->bytes[7] = 2;
    e->bytes[8] = 4;
    e->bytes[14] = 0x10;
    e->bytes[15] = 2;
    e->bytes[17] = 0x88; /* arbitrary opaque service payload */
    e->bytes[18] = 0x99;
    return e;
}

int fake_socket(int family, int type, int protocol)
{
    assert(family == AF_QIPCRTR && type == SOCK_DGRAM && protocol == 0);
    assert(opened == closed);
    if (!strcmp(scenario, "unavailable")) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    if (!strcmp(scenario, "multi")) {
        static const uint32_t order[] = {1, 2, 3, 11, 0x24, 11, 3};
        assert(opened < sizeof(order) / sizeof(order[0]));
        service = order[opened];
    }
    opened++;
    sent = queried = 0;
    next_event = event_count = 0;
    return FD;
}

int fake_getsockname(int fd, struct sockaddr *addr, socklen_t *len)
{
    const struct sockaddr_qrtr local = {.sq_family = AF_QIPCRTR, .sq_node = LOCAL};
    assert(fd == FD && *len == sizeof(local));
    memcpy(addr, &local, sizeof(local));
    return 0;
}

int fake_bind(int fd, const struct sockaddr *addr, socklen_t len)
{
    const struct sockaddr_qrtr *sq = (const struct sockaddr_qrtr *)addr;
    assert(fd == FD && len == sizeof(*sq) && sq->sq_family == AF_QIPCRTR);
    assert(sq->sq_node == LOCAL && sq->sq_port == 0);
    return 0;
}

ssize_t fake_sendto(int fd, const void *buf, size_t len, int flags,
                    const struct sockaddr *addr, socklen_t addrlen)
{
    const uint8_t *req = buf;
    const struct sockaddr_qrtr *sq = (const struct sockaddr_qrtr *)addr;
    uint8_t expected[20] = {0};

    assert(fd == FD && flags == MSG_DONTWAIT && addrlen == sizeof(*sq));
    assert(sq->sq_family == AF_QIPCRTR);
    if (sq->sq_port != CTL) {
        assert(sent == 1 && !queried && sq->sq_node == PEER && sq->sq_port == PORT);
        assert(service == 3 || service == 11);
        queried++;
        total_queries++;
        assert(len == 7 && req[0] == 0 && req[1] == 1 && req[2] == 0 && req[4] == 0);
        assert(req[3] == (service == 3 ? 0x24 : 0x2f) && req[5] == 0 && req[6] == 0);
        if (!strcmp(scenario, "wrong-qmi-endpoint")) {
            reply(req, PEER + 1, PORT);
            reply(req, PEER, PORT + 1);
            reply(req, PEER, PORT)->family = AF_INET;
            reply(req, PEER, PORT)->addrlen--;
        } else if (!strcmp(scenario, "withdraw-during-query")) {
            ctrl(5, service, INST, PEER, PORT);
            reply(req, PEER, PORT);
        } else if (!strcmp(scenario, "ambiguous-during-query")) {
            ctrl(4, service, INST, PEER + 1, PORT);
            reply(req, PEER, PORT);
        } else if (!strcmp(scenario, "bad-qmi-length")) {
            reply(req, PEER, PORT)->bytes[5]++;
        } else if (!strcmp(scenario, "bad-qmi-tlv")) {
            reply(req, PEER, PORT)->bytes[15]++;
        } else if (!strcmp(scenario, "truncated-qmi")) {
            reply(req, PEER, PORT)->flags = MSG_TRUNC;
        } else if (!strcmp(scenario, "qmi-rejected")) {
            struct event *e = reply(req, PEER, PORT);
            e->bytes[10] = 1;
            e->bytes[12] = 0x2a;
        } else {
            reply(req, PEER, PORT)->bytes[1]++;
            reply(req, PEER, PORT)->bytes[3]++;
            reply(req, PEER, PORT)->bytes[0] = 4;
            reply(req, PEER, PORT);
        }
        return (ssize_t)len;
    }

    assert(sq->sq_node == LOCAL && len == sizeof(expected));
    fixture_u32(expected, sent ? 11 : 10);
    fixture_u32(expected + 4, service);
    assert(!memcmp(req, expected, sizeof(expected)));
    if (sent++)
        return (ssize_t)len;

    if (!strcmp(scenario, "empty")) {
        ctrl(4, 0, 0, 0, 0);
        return (ssize_t)len;
    }
    if (!strcmp(scenario, "invalid-address")) {
        ctrl(4, service, INST, 0xffffffffU, PORT);
    } else if (!strcmp(scenario, "invalid-port")) {
        ctrl(4, service, INST, PEER, CTL);
    } else if (!strcmp(scenario, "unrelated")) {
        ctrl(4, service + 1, INST, PEER, PORT);
    } else if (!strcmp(scenario, "wrong-control-sender")) {
        ctrl(4, service, INST, PEER, PORT)->node = PEER;
        ctrl(4, service, INST, PEER, PORT)->port--;
        ctrl(4, service, INST, PEER, PORT)->family = AF_INET;
        ctrl(4, service, INST, PEER, PORT)->addrlen--;
    } else if (!strcmp(scenario, "old-enum")) {
        ctrl(3, service, INST, PEER, PORT);
    } else if (!strcmp(scenario, "deleted-only")) {
        ctrl(5, service, INST, PEER, PORT);
    } else if (!strcmp(scenario, "local-only")) {
        ctrl(4, service, INST, LOCAL, PORT);
    } else if (!strcmp(scenario, "overflow")) {
        for (unsigned i = 0; i < 40; i++)
            ctrl(4, service, INST, PEER, PORT + i);
    } else {
        ctrl(4, service, INST, PEER, PORT);
    }
    if (!strcmp(scenario, "ambiguous") || !strcmp(scenario, "deletes-other"))
        ctrl(4, service, INST + 0x100, PEER + 1, PORT + 1);
    if (!strcmp(scenario, "local-plus-remote"))
        ctrl(4, service, INST, LOCAL, PORT);
    if (!strcmp(scenario, "conflicting-instance"))
        ctrl(4, service, INST + 0x100, PEER, PORT);
    if (!strcmp(scenario, "deleted"))
        ctrl(5, service, INST, PEER, PORT);
    if (!strcmp(scenario, "deletes-other"))
        ctrl(5, service, INST + 0x100, PEER + 1, PORT + 1);
    if (!strcmp(scenario, "duplicates"))
        ctrl(4, service, INST, PEER, PORT);
    if (!strcmp(scenario, "no-eof"))
        return (ssize_t)len;
    if (!strcmp(scenario, "bad-eof-type"))
        ctrl(5, 0, 0, 0, 0);
    else if (!strcmp(scenario, "bad-eof-fields"))
        ctrl(4, 0, INST, 0, 0);
    else if (!strcmp(scenario, "foreign-eof"))
        ctrl(4, 0, 0, 0, 0)->node = PEER;
    else if (!strcmp(scenario, "short-eof"))
        ctrl(4, 0, 0, 0, 0)->len--;
    else if (!strcmp(scenario, "long-eof"))
        ctrl(4, 0, 0, 0, 0)->len++;
    else if (!strcmp(scenario, "truncated-eof"))
        ctrl(4, 0, 0, 0, 0)->flags = MSG_TRUNC;
    else
        ctrl(4, 0, 0, 0, 0);
    if (!strcmp(scenario, "deleted-after-eof"))
        ctrl(5, service, INST, PEER, PORT);
    return (ssize_t)len;
}

int fake_poll(struct pollfd *fds, nfds_t count, int timeout)
{
    assert(count == 1 && fds[0].fd == FD && fds[0].events == POLLIN && timeout > 0);
    fds[0].revents = next_event < event_count ? POLLIN : 0;
    return !!fds[0].revents;
}

ssize_t fake_recvmsg(int fd, struct msghdr *msg, int flags)
{
    assert(fd == FD && flags == MSG_DONTWAIT && msg->msg_iovlen == 1);
    if (next_event == event_count) {
        errno = EAGAIN;
        return -1;
    }
    const struct event *e = &events[next_event++];
    struct sockaddr_qrtr src = {.sq_family = e->family, .sq_node = e->node, .sq_port = e->port};
    size_t size = e->len;

    assert(msg->msg_namelen == sizeof(src));
    memcpy(msg->msg_name, &src, sizeof(src));
    msg->msg_namelen = e->addrlen;
    msg->msg_flags = e->flags;
    if (size > msg->msg_iov[0].iov_len) {
        size = msg->msg_iov[0].iov_len;
        msg->msg_flags |= MSG_TRUNC;
    }
    memcpy(msg->msg_iov[0].iov_base, e->bytes, size);
    return (ssize_t)size;
}

int fake_close(int fd)
{
    assert(fd == FD && opened == closed + 1);
    closed++;
    return 0;
}

int main(int argc, char **argv)
{
    int rc;
    assert(argc >= 2);
    scenario = argv[1];
    service = argc == 3 ? 3 : 11;
    if (argc == 3) {
        rc = probe_main(argc - 1, argv + 1);
        if (!strcmp(scenario, "multi"))
            assert(closed == (!strcmp(argv[2], "--all") ? 7U : 5U));
        else if (strcmp(argv[2], "--serving-system"))
            assert(opened == 0);
    } else {
        rc = probe_service(service, "uim", QMI_UIM_GET_CARD_STATUS);
        assert(sent == 2 && closed == 1);
    }
    assert(opened == closed);
    printf("MOCK_QUERY_COUNT=%u\n", total_queries);
    return rc;
}
'''


@unittest.skipUnless(shutil.which('cc'), 'no host C compiler')
class MarbleQrtrProbeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(tmp.cleanup)
        cls.harness = Path(tmp.name) / 'mock_qrtr'
        proc = subprocess.run(
            ['cc', '-std=c11', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-I', str(ROOT),
             '-x', 'c', '-o', str(cls.harness), '-'],
            input=HARNESS, text=True, capture_output=True,
        )
        if proc.returncode:
            raise AssertionError(proc.stderr)

    def run_case(self, case, code, marker, queries):
        proc = subprocess.run([str(self.harness), case], capture_output=True, text=True, timeout=3)
        self.assertEqual(proc.returncode, code, proc.stdout + proc.stderr)
        self.assertIn(marker, proc.stdout)
        self.assertIn(f'MOCK_QUERY_COUNT={queries}', proc.stdout)
        return proc.stdout

    def test_complete_unique_snapshot_routes_to_advertised_remote(self):
        for case in ('unique', 'duplicates', 'deletes-other'):
            with self.subTest(case=case):
                out = self.run_case(case, 0, 'request_result=ok', 1)
                self.assertIn('status=unique_remote node=9643 port=0x5336 instance=0x3101', out)
                self.assertNotIn('CARD_SLOTS', out)
                self.assertNotIn('NR5G', out)

    def test_deletions_and_non_lookup_traffic_do_not_prove_presence(self):
        for case in ('deleted', 'deleted-only', 'deleted-after-eof', 'empty', 'unrelated',
                     'old-enum', 'wrong-control-sender'):
            with self.subTest(case=case):
                self.run_case(case, 1, 'status=not_listed', 0)

    def test_only_real_eof_and_valid_datagram_can_complete_lookup(self):
        for case in ('no-eof', 'bad-eof-type', 'bad-eof-fields', 'foreign-eof', 'short-eof',
                     'long-eof', 'truncated-eof', 'invalid-address', 'invalid-port', 'overflow',
                     'conflicting-instance'):
            with self.subTest(case=case):
                self.run_case(case, 1, 'status=incomplete', 0)

    def test_never_routes_ambiguous_or_local_only_peer(self):
        self.run_case('ambiguous', 1, 'status=ambiguous count=2', 0)
        self.run_case('local-plus-remote', 1, 'status=ambiguous count=2', 0)
        self.run_case('local-only', 1, 'status=local_only', 0)

    def test_qmi_reply_requires_same_peer_and_valid_result(self):
        for case in ('wrong-qmi-endpoint', 'withdraw-during-query', 'ambiguous-during-query',
                     'bad-qmi-length', 'bad-qmi-tlv', 'truncated-qmi'):
            with self.subTest(case=case):
                self.run_case(case, 1, 'status=unconfirmed', 1)
        self.run_case('qmi-rejected', 3, 'request_result=failed error=0x002a', 1)

    def test_cli_services_never_queries_dms_and_all_queries_only_uim_and_nas(self):
        for mode, queries in (('--services', 0), ('--all', 2)):
            with self.subTest(mode=mode):
                proc = subprocess.run([str(self.harness), 'multi', mode],
                                      capture_output=True, text=True, timeout=3)
                self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
                self.assertIn('QRTR_UNIQUE_REMOTE_TOTAL=5', proc.stdout)
                self.assertIn(f'MOCK_QUERY_COUNT={queries}', proc.stdout)
                self.assertNotIn('QMI_QUERY name=dms', proc.stdout)

    def test_cli_respects_privacy_and_no_socket_support(self):
        for option, code, marker in (
            ('--help', 0, 'QMI/QRTR probe'),
            ('--device-ids', 2, 'usage:'),
            ('--hex', 2, 'usage:'),
            ('--services', 2, 'status=socket_error'),
        ):
            with self.subTest(option=option):
                proc = subprocess.run([str(self.harness), 'unavailable', option],
                                      capture_output=True, text=True, timeout=3)
                self.assertEqual(proc.returncode, code, proc.stdout + proc.stderr)
                self.assertIn(marker, proc.stdout + proc.stderr)
        proc = subprocess.run([str(self.harness), 'unique', '--serving-system'],
                              capture_output=True, text=True, timeout=3)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn('name=nas request_result=ok', proc.stdout)


if __name__ == '__main__':
    unittest.main()
