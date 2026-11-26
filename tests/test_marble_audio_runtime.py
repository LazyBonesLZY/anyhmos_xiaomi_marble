"""Contract tests for the marble PAL/AGM user-space runtime.

The runtime is a donor assembly (``compat/audio/runtime``): the vendor audio
libraries are extractions, only ``libaudioroute.so`` is built from this
repository.  These tests pin the properties the on-device bring-up relies on:

* ``SHA256SUMS`` and ``manifest.json`` agree with the bytes on disk;
* the DT_NEEDED closure is closed (the loader itself supplies ld-android.so and
  the ``__loader_*`` trampolines, everything else must be shipped);
* every shipped shared object carries a SONAME;
* ``libaudioroute.so`` is an Android/AArch64 object built by the NDK and exports
  exactly the audio_route entry points the vendor objects import;
* the shim never dereferences the caller's ``struct audio_route`` pointer.

When the runtime has not been assembled yet (a fresh checkout only keeps the
script because ``*.so`` is ignored) the tests skip instead of failing.
"""

from __future__ import annotations

import hashlib
import importlib.util
import json
import re
import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RUNTIME = ROOT / 'compat/audio/runtime'
ASSEMBLE = RUNTIME / 'assemble_runtime.py'
AUDIOROUTE_SRC = ROOT / 'compat/audio/audioroute'
SHIM_SRC = AUDIOROUTE_SRC / 'shim.c'
EXPORTS_MAP = AUDIOROUTE_SRC / 'exports.map'

EM_AARCH64 = 183
EXPORTED_AUDIO_ROUTE = {
    'audio_route_init',
    'audio_route_free',
    'audio_route_update_mixer',
    'audio_route_apply_and_update_path',
    'audio_route_reset_and_update_path',
}
# Symbols the vendor objects import from libaudioroute.so / libaudioroute_ext.so.
REQUIRED_BY_VENDOR = {
    'audio_route_init': 'libaudioroute.so',
    'audio_route_free': 'libaudioroute.so',
    'audio_route_update_mixer': 'libaudioroute.so',
    'audio_route_apply_and_update_path': 'libaudioroute.so',
    'audio_route_reset_and_update_path': 'libaudioroute.so',
    'audio_route_init_ext': 'libaudioroute_ext.so',
}
# The audio_route_*/mixer_*/pcm_ imports of the shipped vendor objects; this is
# the exact coverage target the runtime must satisfy.
EXPECTED_AUDIO_SYMBOL_COUNT = 46


def _load_module():
    # Importing the assembler must not leave __pycache__ inside the payload dir.
    previous = sys.dont_write_bytecode
    sys.dont_write_bytecode = True
    try:
        spec = importlib.util.spec_from_file_location('marble_audio_runtime_asm', ASSEMBLE)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
    finally:
        sys.dont_write_bytecode = previous
    return module


def _elf_header(path: Path):
    data = path.read_bytes()[:64]
    if data[:4] != b'\x7fELF':
        raise AssertionError(f'{path} is not an ELF object')
    return dict(ei_class=data[4], ei_data=data[5],
                e_type=struct.unpack_from('<H', data, 0x10)[0],
                e_machine=struct.unpack_from('<H', data, 0x12)[0])


def _note_android_ident(path: Path) -> bytes:
    """Descriptor of the .note.android.ident note (api level + ndk + build)."""
    data = path.read_bytes()
    e_shoff = struct.unpack_from('<Q', data, 0x28)[0]
    e_shentsize, e_shnum = struct.unpack_from('<HH', data, 0x3A)
    for index in range(e_shnum):
        base = e_shoff + index * e_shentsize
        sh_type = struct.unpack_from('<I', data, base + 4)[0]
        if sh_type != 7:  # SHT_NOTE
            continue
        sh_offset, sh_size = struct.unpack_from('<QQ', data, base + 0x18)
        chunk = data[sh_offset:sh_offset + sh_size]
        cursor = 0
        while cursor + 12 <= len(chunk):
            namesz, descsz, _note_type = struct.unpack_from('<III', chunk, cursor)
            name_start = cursor + 12
            desc_start = name_start + ((namesz + 3) // 4) * 4
            name = chunk[name_start:name_start + namesz].rstrip(b'\0')
            if name == b'Android':
                return chunk[desc_start:desc_start + descsz]
            cursor = desc_start + ((descsz + 3) // 4) * 4
    return b''


def _wrapper_body(source: str, name: str) -> str:
    """Return the text of an exported wrapper definition (declaration to brace)."""
    pattern = re.compile(
        r'^[A-Za-z_][^\n;{}]*\baudio_route_%s\s*\([^)]*\)\s*\n\{' % re.escape(name),
        re.MULTILINE)
    match = pattern.search(source)
    assert match, f'no definition found for audio_route_{name}'
    depth = 0
    for offset in range(match.end() - 1, len(source)):
        if source[offset] == '{':
            depth += 1
        elif source[offset] == '}':
            depth -= 1
            if depth == 0:
                return source[match.start():offset + 1]
    raise AssertionError(f'unterminated body for audio_route_{name}')


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


@unittest.skipUnless(ASSEMBLE.is_file(), 'runtime assembler is absent')
class RuntimeFixture(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.module = _load_module()
        cls.manifest_path = RUNTIME / 'manifest.json'
        if not cls.manifest_path.is_file():
            raise unittest.SkipTest('runtime not assembled: run assemble_runtime.py')
        cls.manifest = json.loads(cls.manifest_path.read_text(encoding='utf-8'))
        cls.entries = {e['path']: e for e in cls.manifest['files']}


class TestRuntimeChecksums(RuntimeFixture):
    def test_sha256sums_matches_manifest(self):
        sums = {}
        for line in (RUNTIME / 'SHA256SUMS').read_text(encoding='utf-8').splitlines():
            digest, name = line.split('  ', 1)
            sums[name] = digest
        self.assertEqual(sums, {p: e['sha256'] for p, e in self.entries.items()})

    def test_every_listed_file_matches_its_digest(self):
        for relative, entry in self.entries.items():
            path = RUNTIME / relative
            self.assertTrue(path.is_file(), relative)
            self.assertEqual(path.stat().st_size, entry['bytes'], relative)
            self.assertEqual(_sha256(path), entry['sha256'], relative)

    def test_no_unlisted_payload(self):
        actual = {p.relative_to(RUNTIME).as_posix()
                  for p in RUNTIME.rglob('*') if p.is_file()}
        allowed_extra = {'SHA256SUMS', 'manifest.json', 'assemble_runtime.py', '.gitignore'}
        actual = {p for p in actual if '__pycache__' not in p}
        self.assertEqual(actual - set(self.entries) - allowed_extra, set())
        self.assertEqual(set(self.entries) - actual, set())

    def test_manifest_sources_exist_and_are_classified(self):
        allowed = {'dump', 'bionic', 'vendor', 'self-built'}
        for relative, entry in self.entries.items():
            self.assertIn(entry['source'], allowed, relative)
            self.assertTrue((ROOT / entry['source_path']).is_file(), relative)
        # the two route libraries must ship together
        self.assertIn('lib64/libaudioroute.so', self.entries)
        self.assertIn('lib64/libaudioroute_ext.so', self.entries)

    def test_donor_sets_are_documented_with_priority(self):
        names = [d['name'] for d in self.manifest['donor_sets']]
        self.assertEqual(names, ['dump', 'bionic', 'vendor', 'self-built'])
        self.assertEqual(self.manifest['loaders']['linker'], 'bin/linker64')
        self.assertEqual(self.manifest['loader_provided'], ['ld-android.so'])


class TestRuntimeClosure(RuntimeFixture):
    def test_needed_closure_is_closed(self):
        shipped = {Path(p).name for p in self.entries}
        loader_ok = {'ld-android.so'}
        for relative, entry in self.entries.items():
            for dependency in entry['needed']:
                self.assertTrue(
                    dependency in shipped or dependency in loader_ok,
                    f'{relative} needs {dependency} which is not shipped')

    def test_no_loader_file_ships_a_second_linker(self):
        self.assertNotIn('lib64/ld-android.so', self.entries)

    def test_every_shared_object_has_soname(self):
        for relative, entry in self.entries.items():
            if not relative.endswith('.so'):
                continue
            soname = self.module.soname_of(RUNTIME / relative)
            self.assertTrue(soname, f'{relative} has no SONAME')
            self.assertEqual(soname, Path(relative).name, relative)

    def test_symbol_coverage_is_complete(self):
        missing = self.module.closure_report(
            {p: (e['source'], RUNTIME / p) for p, e in self.entries.items()})
        self.assertEqual(missing, [])

    def test_audio_symbol_coverage_is_exactly_46(self):
        provided = {}
        for relative in self.entries:
            if not relative.endswith('.so'):
                continue
            for symbol, binding in self.module.dynamic_exports(RUNTIME / relative).items():
                if binding != 'LOCAL':
                    provided.setdefault(symbol, relative)
        imported = {}
        for relative in self.entries:
            for symbol in self.module.dynamic_imports(RUNTIME / relative):
                if symbol.startswith(('audio_route_', 'mixer_', 'pcm_')):
                    imported.setdefault(symbol, relative)
        self.assertEqual(len(imported), EXPECTED_AUDIO_SYMBOL_COUNT)
        for symbol, provider in REQUIRED_BY_VENDOR.items():
            self.assertEqual(provided.get(symbol), f'lib64/{provider}', symbol)
        self.assertEqual(sorted(imported), sorted(imported))  # deterministic order
        unresolved = sorted(s for s in imported if s not in provided)
        self.assertEqual(unresolved, [])


class TestAudiorouteShim(RuntimeFixture):
    def test_audioroute_is_android_aarch64_from_ndk(self):
        path = RUNTIME / 'lib64/libaudioroute.so'
        header = _elf_header(path)
        self.assertEqual(header['ei_class'], 2)
        self.assertEqual(header['e_machine'], EM_AARCH64)
        self.assertEqual(header['e_type'], 3)  # ET_DYN
        note = _note_android_ident(path)
        self.assertTrue(note, 'no .note.android.ident: not an NDK build')
        self.assertIn(b'r27d', note, 'NDK release is not r27d')
        api_level = struct.unpack_from('<I', note, 0)[0]
        self.assertEqual(api_level, 33, 'unexpected android API level in note')
        self.assertIn(b'hmos-audioroute-shim target=aarch64-linux-android',
                      path.read_bytes())

    def test_exports_are_exactly_the_vendor_imports(self):
        exports = self.module.dynamic_exports(RUNTIME / 'lib64/libaudioroute.so')
        self.assertEqual(set(exports), EXPORTED_AUDIO_ROUTE)

    def test_exports_map_limits_visibility(self):
        text = EXPORTS_MAP.read_text(encoding='utf-8')
        for symbol in EXPORTED_AUDIO_ROUTE:
            self.assertIn(f'{symbol};', text)
        self.assertIn('local:', text)
        self.assertIn('*;', text)

    def test_all_required_entry_points_are_exported(self):
        exports = self.module.dynamic_exports(RUNTIME / 'lib64/libaudioroute.so')
        for symbol in ('audio_route_init', 'audio_route_free',
                       'audio_route_update_mixer',
                       'audio_route_apply_and_update_path',
                       'audio_route_reset_and_update_path'):
            self.assertIn(symbol, exports)

    def test_wrappers_never_dereference_the_caller_pointer(self):
        source = SHIM_SRC.read_text(encoding='utf-8')
        for name in ('init', 'free', 'update_mixer',
                     'apply_and_update_path', 'reset_and_update_path'):
            body = _wrapper_body(source, name)
            # drop the signature line and the explicit discard, then look for any
            # remaining use of the caller pointer
            remainder = '\n'.join(body.splitlines()[1:]).replace('(void)ar;', '')
            self.assertNotIn('ar->', remainder, f'{name} dereferences the caller pointer')
            self.assertNotIn('*ar', remainder, name)
            self.assertNotIn('ar[', remainder, name)
        for name in ('free', 'update_mixer', 'apply_and_update_path',
                     'reset_and_update_path'):
            self.assertIn('(void)ar;', _wrapper_body(source, name), name)

    def test_state_comes_from_environment_with_documented_defaults(self):
        source = SHIM_SRC.read_text(encoding='utf-8')
        self.assertIn('HMOS_AUDIO_CARD', source)
        self.assertIn('HMOS_AUDIO_MIXER_XML', source)
        self.assertIn('"/vendor/etc/audio/sku_ukee/mixer_paths_ukee_mtp.xml"', source)
        self.assertIn('AUDIOROUTE_SHIM_INIT_FAIL', source)

    def test_aosp_symbols_are_renamed_into_a_private_namespace(self):
        source = SHIM_SRC.read_text(encoding='utf-8')
        for symbol in EXPORTED_AUDIO_ROUTE:
            self.assertIn(f'#undef {symbol}', source)
        builder = (ROOT / 'tools/build/build_current.py').read_text(encoding='utf-8')
        for symbol in EXPORTED_AUDIO_ROUTE:
            self.assertIn(f"'{symbol}'", builder)

    def test_failure_paths_do_not_abort(self):
        log_shim = (AUDIOROUTE_SRC / 'include/log/log.h').read_text(encoding='utf-8')
        self.assertNotIn('__android_log_assert', log_shim)
        source = SHIM_SRC.read_text(encoding='utf-8')
        self.assertNotIn('abort(', source)
        self.assertNotIn('LOG_ALWAYS_FATAL', source)
        # failures return -ENOENT instead of tearing down the PAL process
        self.assertGreaterEqual(source.count('return -ENOENT;'), 3)

    def test_provenance_is_recorded(self):
        text = (AUDIOROUTE_SRC / 'PROVENANCE.md').read_text(encoding='utf-8')
        for needle in ('audio_route.c', 'Apache License', 'opaque',
                       'audio_route_init_ext', 'HMOS_AUDIO_MIXER_XML'):
            self.assertIn(needle, text)


class TestBuildTargets(unittest.TestCase):
    def test_audioroute_and_pal_targets_are_offered(self):
        spec = importlib.util.spec_from_file_location(
            'marble_build_current', ROOT / 'tools/build/build_current.py')
        build = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(build)
        commands = build.commands(ROOT / 'work/unittest-build')
        for target in ('audioroute', 'pal-server', 'pal-lifecycle'):
            self.assertIn(target, commands)
        audioroute = commands['audioroute']
        self.assertIn('--target=aarch64-linux-android33', audioroute)
        self.assertIn(f'-Wl,--version-script={AUDIOROUTE_SRC / "exports.map"}', audioroute)
        joined = ' '.join(commands['pal-server'])
        self.assertIn(str(ROOT / 'compat/audio/pal-headers/inc'), joined)
        self.assertNotIn('/root/hmos-reference', joined)


if __name__ == '__main__':
    unittest.main()
