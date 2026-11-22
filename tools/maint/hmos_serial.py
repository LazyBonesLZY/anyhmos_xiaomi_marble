#!/usr/bin/env python3
"""Zero-dependency Linux USB ACM serial client for hmos-probe (/dev/ttyACM*)."""

import argparse
import base64
import glob
import os
import secrets
import select
import sys
import termios
import time


def find_default_port():
    ports = sorted(glob.glob('/dev/ttyACM*'))
    if not ports:
        raise SystemExit('No /dev/ttyACM* device found; connect marble running hmos-probe.')
    return ports[0]


def open_serial(port):
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    attrs = termios.tcgetattr(fd)
    # Raw 115200 8N1, -hupcl, local, no flow control
    attrs[0] = 0
    attrs[1] = 0
    attrs[2] = (attrs[2] & ~(termios.CSIZE | termios.PARENB | termios.CSTOPB | termios.HUPCL)) | termios.CS8 | termios.CLOCAL | termios.CREAD
    attrs[3] = 0
    attrs[4] = termios.B115200
    attrs[5] = termios.B115200
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    return fd


def drain_input(fd, duration=0.2):
    end = time.monotonic() + duration
    chunks = []
    while time.monotonic() < end:
        r, _, _ = select.select([fd], [], [], max(0.01, end - time.monotonic()))
        if not r:
            continue
        try:
            data = os.read(fd, 4096)
        except BlockingIOError:
            continue
        if not data:
            break
        chunks.append(data)
    return b''.join(chunks)


def send_line(fd, line):
    payload = (line.rstrip('\r\n') + '\n').encode('utf-8')
    view = memoryview(payload)
    sent = 0
    while sent < len(payload):
        _, w, _ = select.select([], [fd], [], 2.0)
        if not w:
            raise TimeoutError('Timed out writing command to USB ACM port')
        n = os.write(fd, view[sent:])
        sent += n


def read_until(fd, marker=None, timeout=15.0, idle=1.5, stream=True):
    deadline = time.monotonic() + timeout
    last_rx = time.monotonic()
    buf = bytearray()
    marker_bytes = marker.encode('utf-8') if marker else None
    while time.monotonic() < deadline:
        wait = min(0.25, max(0.01, deadline - time.monotonic()))
        r, _, _ = select.select([fd], [], [], wait)
        if r:
            try:
                data = os.read(fd, 4096)
            except BlockingIOError:
                continue
            if data:
                last_rx = time.monotonic()
                buf.extend(data)
                if stream:
                    sys.stdout.buffer.write(data)
                    sys.stdout.buffer.flush()
                if marker_bytes and marker_bytes in buf:
                    # Collect the remainder of the marker line
                    if buf.find(b'\n', buf.find(marker_bytes)) != -1:
                        return bytes(buf), True
        elif marker_bytes is None and (time.monotonic() - last_rx) >= idle and buf:
            return bytes(buf), True
    return bytes(buf), marker_bytes is None


def build_shell_command(script, req_id=None):
    req_id = req_id or secrets.token_hex(4)
    if not req_id.isalnum() or len(req_id) > 16:
        raise ValueError('Request identifier must be 1..16 alphanumeric characters')
    encoded = base64.b64encode(script.encode('utf-8')).decode('ascii')
    return req_id, f'cmd {req_id} {encoded}'


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', help='USB ACM serial device (default: first /dev/ttyACM*)')
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument('--cmd', help='Raw hmos-probe command (e.g. status, replay, ipc, dmabuf, test-token, test-binder, test-oneway, reboot-bootloader)')
    group.add_argument('--shell', help='BusyBox shell script sent via base64 cmd worker')
    group.add_argument('--tail', action='store_true', help='Continuously stream logs from /dev/ttyACM*')
    parser.add_argument('--expect', help='Stop reading once this substring appears on a complete line')
    parser.add_argument('--timeout', type=float, default=30.0, help='Maximum wait seconds (default: 30)')
    parser.add_argument('--idle', type=float, default=2.0, help='Idle cutoff seconds when no --expect marker is given')
    args = parser.parse_args(argv)

    port = args.port or find_default_port()
    fd = open_serial(port)
    try:
        if args.tail:
            while True:
                read_until(fd, marker=None, timeout=3600.0, idle=3600.0, stream=True)
        drain_input(fd, 0.15)
        marker = args.expect
        if args.shell is not None:
            req_id, line = build_shell_command(args.shell)
            if marker is None:
                marker = f'CMD END id={req_id} '
        else:
            line = args.cmd
        send_line(fd, line)
        _, ok = read_until(fd, marker=marker, timeout=args.timeout, idle=args.idle, stream=True)
        return 0 if ok else 1
    finally:
        os.close(fd)


if __name__ == '__main__':
    raise SystemExit(main())
