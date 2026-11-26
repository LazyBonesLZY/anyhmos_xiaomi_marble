#!/usr/bin/env python3
"""Generate boot-pinned, RAM-only fixture scripts without running them."""
import argparse
import hashlib
import re
from pathlib import Path

HERE = Path(__file__).resolve().parent
PAGES = {'p': 'fixture.html', 'n': 'no-js.html'}
SCRIPT_LIMIT = 2800  # serial_local.run requires strictly fewer bytes


def scripts(run: str, boot: str) -> dict[str, bytes]:
    if not re.fullmatch(r'[a-z0-9]{1,12}', run):
        raise ValueError('run must be 1–12 lowercase ASCII letters or digits')
    if not re.fullmatch(r'[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}', boot):
        raise ValueError('boot must be the current canonical boot_id')
    sums = {name: digest for digest, name in
            (line.split() for line in (HERE / 'SHA256SUMS').read_text(encoding='ascii').splitlines())}
    if set(sums) != set(PAGES.values()):
        raise ValueError('Unexpected fixture checksum list')
    header = (f'set -eu\nB=/busybox\nD=/dev/marble-web-fixture-{run}\n'
              f'[ "$($B cat /proc/sys/kernel/random/boot_id)" = "{boot}" ] || exit 19\n')
    guard = '[ -d "$D" ] && [ ! -L "$D" ]\n'
    check_ram = ('$B awk \'$2=="/dev"&&($3=="tmpfs"||$3=="devtmpfs"){ok=1}END{exit !ok}\' /proc/mounts\n')
    generated = {}

    def add(name, body):
        text = (header + body).encode('ascii')
        if not 0 < len(text) < SCRIPT_LIMIT:
            raise ValueError(f'{name} exceeds serial fixture size limit ({len(text)} bytes)')
        generated[name] = text

    verified = []
    for suffix, name in PAGES.items():
        body = (HERE / name).read_bytes()
        digest = hashlib.sha256(body).hexdigest()
        if (digest != sums[name] or not body.endswith(b'\n') or b'\r' in body or
                b'END_MARBLE_HTML' in body):
            raise ValueError(f'Unexpected bytes in {name}')
        path = f'$D/{run}{suffix}.html'
        verify = f'printf "%s\\n" "{digest}  {path}" | $B sha256sum -c -\n'
        # -C prevents replacing an existing regular file on repeated transfer.
        intro = check_ram + 'umask 077\n$B mkdir "$D"\n' if suffix == 'p' else guard + 'umask 077\n'
        add(f'push-{suffix}.sh', intro + 'set -C\n' +
            f'$B cat >"{path}" <<\'END_MARBLE_HTML\'\n{body.decode("ascii")}END_MARBLE_HTML\n' + verify)
        verified.append(f'[ -f "{path}" ] && [ ! -L "{path}" ]\n' + verify)
    add('serve.sh', guard + ''.join(verified) + '$B cat /proc/uptime\n' +
        'exec $B timeout -k 2 40 $B httpd -f -vv -c /dev/null -p 127.0.0.1:18719 -h "$D"\n')
    add('cleanup.sh', guard + f'$B rm -f "$D/{run}p.html" "$D/{run}n.html"\n$B rmdir "$D"\n')
    return generated


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', required=True)
    parser.add_argument('--boot', required=True)
    parser.add_argument('--out', required=True, type=Path, help='A new local directory')
    args = parser.parse_args()
    payload = scripts(args.run, args.boot)
    args.out.mkdir(parents=False)
    for name, data in payload.items():
        with (args.out / name).open('xb') as f:
            f.write(data)
        print(f'{name}\t{len(data)} bytes')


if __name__ == '__main__':
    main()
