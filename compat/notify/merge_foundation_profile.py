#!/usr/bin/env python3
"""Merge the missing notification SAs into a foundation SA profile.

Why this exists
---------------
`ans` (AdvancedNotificationService, SA 3203) is not a process: it is a system
ability registered inside the `foundation` process. So "notification is dead"
cannot be fixed by any init `.cfg` service stanza -- the SA list that `sa_main`
reads is `/system/profile/foundation.json`, and on this port that file is
overwritten at boot with `/graphics/foundation-app-core.json`
(`devices/marble/runtime/boot/stage-phase4-marble.sh:377`), which is a copy of
`devices/sheng/runtime/profiles/foundation-app-core.json`.  That trimmed list
carries 10 SAs and drops 9 donor SAs, `libans.z.so` among them.

This tool merges `compat/notify/ans-foundation-sa.json` (byte-for-byte donor
entries) into such a profile:

    python3 compat/notify/merge_foundation_profile.py --check \
        --profile devices/sheng/runtime/profiles/foundation-app-core.json
    python3 compat/notify/merge_foundation_profile.py \
        --profile devices/sheng/runtime/profiles/foundation-app-core.json

The default `--profile` is the source profile of the port; the tool never
touches a device.  Exit status: 0 = already merged / merge written,
2 = merge needed but `--check` was given, 1 = inconsistent input.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_PROFILE = ROOT / 'devices/sheng/runtime/profiles/foundation-app-core.json'
DEFAULT_FRAGMENT = Path(__file__).resolve().with_name('ans-foundation-sa.json')


class ProfileError(RuntimeError):
    """Input profile and fragment cannot be merged consistently."""


def load_json(path: Path) -> dict:
    try:
        return json.loads(path.read_text(encoding='utf-8'))
    except FileNotFoundError as exc:
        raise ProfileError(f'missing file: {path}') from exc
    except json.JSONDecodeError as exc:
        raise ProfileError(f'invalid JSON in {path}: {exc}') from exc


def _sas(profile: dict, origin: str) -> list:
    sas = profile.get('systemability')
    if not isinstance(sas, list):
        raise ProfileError(f'{origin}: "systemability" is not a list')
    for sa in sas:
        if not isinstance(sa, dict) or not isinstance(sa.get('name'), int):
            raise ProfileError(f'{origin}: system ability entry without integer "name": {sa!r}')
    return sas


def plan(profile: dict, fragment: dict) -> tuple[list, list, list]:
    """Return (added, skipped, problems) without mutating the inputs."""
    if fragment.get('process') != profile.get('process'):
        raise ProfileError(
            f'process mismatch: profile={profile.get("process")!r} '
            f'fragment={fragment.get("process")!r}')
    have = {sa['name']: sa for sa in _sas(profile, 'profile')}
    added, skipped, problems = [], [], []
    for sa in _sas(fragment, 'fragment'):
        name = sa['name']
        if name in have:
            if have[name] == sa:
                skipped.append(name)
            else:
                problems.append(
                    f'SA {name} already present with different content: '
                    f'{json.dumps(have[name], sort_keys=True)} != '
                    f'{json.dumps(sa, sort_keys=True)}')
            continue
        deps = [d for d in sa.get('depend') or [] if d not in have]
        if deps:
            problems.append(
                f'SA {name} depends on {deps} which is absent from '
                f'the target profile; sa_main would never satisfy the dependency')
        added.append(sa)
    return added, skipped, problems


def render(profile: dict, added: list) -> str:
    merged = dict(profile)
    merged['systemability'] = list(_sas(profile, 'profile')) + added
    return json.dumps(merged, indent=4, ensure_ascii=False) + '\n'


def write_atomic(path: Path, text: str) -> None:
    fd, tmp = tempfile.mkstemp(prefix=path.name + '.', dir=str(path.parent))
    try:
        with os.fdopen(fd, 'w', encoding='utf-8') as handle:
            handle.write(text)
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


def main(argv: list) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--profile', type=Path, default=DEFAULT_PROFILE)
    parser.add_argument('--fragment', type=Path, default=DEFAULT_FRAGMENT)
    parser.add_argument('--check', action='store_true',
                        help='do not write; report whether the profile needs the merge')
    parser.add_argument('--quiet', action='store_true')
    args = parser.parse_args(argv)

    profile = load_json(args.profile)
    fragment = load_json(args.fragment)
    added, skipped, problems = plan(profile, fragment)
    for problem in problems:
        print(f'ERROR {problem}', file=sys.stderr)
    if problems:
        return 1
    ids = [sa['name'] for sa in added]
    if not args.quiet:
        print(f'profile={args.profile}')
        print(f'fragment={args.fragment}')
        print(f'already_present={skipped}')
        print(f'to_add={ids}')
    if args.check:
        return 2 if added else 0
    if added:
        write_atomic(args.profile, render(profile, added))
        if not args.quiet:
            print(f'merged={ids} -> {args.profile}')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))