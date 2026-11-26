"""marble AGM in-process passthrough (agmipc shim) deliverable checks.

Static only: nothing here flashes, calls adb/fastboot, or needs the phone.  The
call-chain evidence and the on-device acceptance criteria live in
``work/marble-agmipc-report.md``; this module only proves that the shim source,
manifest and build product agree with each other.

A missing build product skips the ELF checks with an explicit reason instead of
failing the suite - not being built is not the same as being wrong.
"""
from __future__ import annotations

import hashlib
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / 'compat/audio/agmipc'
BUILD_SCRIPT = SRC / 'build_agmipc_shim.sh'
GENERATOR = SRC / 'gen_shim_sources.py'
MANIFEST = SRC / 'agm_symbols.txt'
SHIM_SRC = SRC / 'libagm_passthrough.c'
STUB_SRC = SRC / 'agmipc_fail_stub.S'
OUT_DIR = ROOT / 'work/marble-agmipc-shim'
PRODUCT = OUT_DIR / 'libhmos_agmipc_shim.so'
PROVENANCE = OUT_DIR / 'PROVENANCE.md'
REPORT = ROOT / 'work/marble-agmipc-report.md'
REFERENCE = ROOT / 'work/marble-audio-dump/lib64/libagmclient.so'
SDK_BIN = ROOT / 'work/ohos-sdk/installed/native/llvm/bin'

# 20 bytes: hint #32 (bti c), adr/adrp+add, ldr, br x16
TRAMPOLINE_BYTES = 20


def _tool(name: str, fallback: str) -> str | None:
    candidate = SDK_BIN / f'llvm-{name}'
    if candidate.is_file():
        return str(candidate)
    return shutil.which(fallback)


def _read_manifest() -> list[str]:
    return [
        line.strip()
        for line in MANIFEST.read_text(encoding='utf-8').splitlines()
        if line.strip() and not line.strip().startswith('#')
    ]


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open('rb') as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b''):
            digest.update(chunk)
    return digest.hexdigest()


def _run(argv: list[str], **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run(argv, capture_output=True, text=True, check=True, **kwargs)


def _dynsym_agm_functions(path: Path) -> list[str]:
    """Defined GLOBAL FUNC agm_* symbols of a shared object."""
    nm = _tool('nm', 'nm')
    if nm is None:
        raise unittest.SkipTest('llvm-nm/nm not available')
    out = _run([nm, '-D', '--defined-only', str(path)]).stdout
    names = []
    for line in out.splitlines():
        fields = line.split()
        if len(fields) < 3:
            continue
        symbol_type, name = fields[-2], fields[-1]
        if symbol_type in ('T', 't', 'W') and name.startswith('agm_'):
            names.append(name)
    return sorted(names)


def _undefined_symbols(path: Path) -> list[str]:
    nm = _tool('nm', 'nm')
    if nm is None:
        raise unittest.SkipTest('llvm-nm/nm not available')
    out = _run([nm, '-D', '--undefined-only', str(path)]).stdout
    names = []
    for line in out.splitlines():
        fields = line.split()
        if len(fields) >= 2:
            names.append(fields[-1].split('@', 1)[0])
    return sorted(names)


def _needed_libraries(path: Path) -> list[str]:
    readelf = _tool('readelf', 'readelf')
    if readelf is None:
        raise unittest.SkipTest('llvm-readelf/readelf not available')
    out = _run([readelf, '-dW', str(path)]).stdout
    return re.findall(r'\(NEEDED\).*?\[(.+?)\]', out)


def _symbol_sizes(path: Path) -> dict[str, int]:
    nm = _tool('nm', 'nm')
    if nm is None:
        raise unittest.SkipTest('llvm-nm/nm not available')
    sizes = {}
    for line in _run([nm, '-D', '-S', '--defined-only', str(path)]).stdout.splitlines():
        fields = line.split()
        if len(fields) < 4:
            continue
        try:
            size = int(fields[1], 16)
        except ValueError:
            continue
        sizes[fields[3]] = size
    return sizes


class ManifestTest(unittest.TestCase):
    """The AGM C API list is the shim's contract."""

    def test_manifest_is_non_trivial_and_unique(self):
        symbols = _read_manifest()
        self.assertEqual(len(symbols), len(set(symbols)), 'duplicate symbols in manifest')
        self.assertGreaterEqual(len(symbols), 40, f'implausibly short AGM API: {len(symbols)}')
        for name in symbols:
            self.assertTrue(name.startswith('agm_'), f'non AGM symbol: {name}')

    def test_manifest_matches_reference_marshaller(self):
        """Chain step 1: the list is the API that libagmclient.so exports."""
        if not REFERENCE.is_file():
            self.skipTest(f'reference marshaller absent: {REFERENCE.relative_to(ROOT)}')
        self.assertEqual(_read_manifest(), _dynsym_agm_functions(REFERENCE))

    def test_manifest_covers_what_pal_imports(self):
        """PAL (libar-pal.so) is the real consumer - its imports must be covered."""
        pal = ROOT / 'compat/audio/runtime/lib64/libar-pal.so'
        if not pal.is_file():
            self.skipTest('libar-pal.so not staged in compat/audio/runtime/lib64')
        imported = {
            name for name in _undefined_symbols(pal) if name.startswith('agm_')
        }
        self.assertTrue(imported, 'libar-pal.so imports no agm_* symbol')
        missing = sorted(imported - set(_read_manifest()))
        self.assertEqual(missing, [], f'PAL calls symbols the shim would not export: {missing}')

    def test_dlopen_target_dependencies_resolve_in_runtime(self):
        """The shim resolves libagm.so at load time, so its NEEDED must be
        reachable from the same LD_LIBRARY_PATH the PAL command uses."""
        impl = ROOT / 'work/marble-audio-dump/lib64/libagm.so'
        runtime = ROOT / 'compat/audio/runtime/lib64'
        if not impl.is_file():
            self.skipTest('libagm.so reference dump absent')
        if not runtime.is_dir():
            self.skipTest('compat/audio/runtime/lib64 not staged')
        missing = [
            dep for dep in _needed_libraries(impl)
            if dep != 'libagm.so' and not (runtime / dep).is_file()
        ]
        self.assertEqual(missing, [], f'libagm.so would fail to dlopen, missing: {missing}')


class SourceTest(unittest.TestCase):
    """Source-level guarantees that do not need a compiler."""

    def test_build_script_parses(self):
        _run(['bash', '-n', str(BUILD_SCRIPT)])

    def test_degradation_path_is_present(self):
        text = SHIM_SRC.read_text(encoding='utf-8')
        self.assertIn('AGMIPC_SHIM_INIT_FAIL', text)
        self.assertIn('reason=', text)
        self.assertIn('AGMIPC_SHIM_INIT_OK', text)
        self.assertIn('RTLD_NEXT', text, 'no fallback to the original marshaller')
        self.assertIn('agmipc_fail_stub', text, 'slot table has no terminal stub')
        self.assertIn('AGMIPC_SHIM_CALL_UNRESOLVED', text)
        self.assertNotRegex(text, r'\babort\s*\(', 'shim must not abort PAL')

    def test_transparent_trampolines_are_generated(self):
        with tempfile.TemporaryDirectory() as tmp:
            _run([
                'python3', str(GENERATOR),
                '--symbols', str(MANIFEST),
                '--out-dir', tmp,
            ])
            gen = Path(tmp)
            header = (gen / 'agmipc_shim_syms.h').read_text(encoding='utf-8')
            asm = (gen / 'agmipc_shim_syms.S').read_text(encoding='utf-8')
            names = _read_manifest()
            self.assertIn(f'#define AGMIPC_SYM_COUNT {len(names)}', header)
            for name in names:
                # One trampoline per symbol, no prototype encoded anywhere.
                self.assertEqual(asm.count(f'\n{name}:'), 1, f'{name}: missing trampoline')
                self.assertIn(f'\tldr\tx16, [x16, #{names.index(name) * 8}]', asm)
            self.assertNotIn('agm_session_open(', asm, 'trampolines must not carry prototypes')
            self.assertEqual(asm.count('\tbr\tx16'), len(names))

    def test_generator_is_deterministic(self):
        with tempfile.TemporaryDirectory() as tmp:
            first = Path(tmp) / 'a'
            second = Path(tmp) / 'b'
            for target in (first, second):
                _run(['python3', str(GENERATOR), '--symbols', str(MANIFEST),
                      '--out-dir', str(target)])
            for name in ('agmipc_shim_syms.S', 'agmipc_shim_syms.h', 'agmipc_shim_names.h'):
                self.assertEqual((first / name).read_text(encoding='utf-8'),
                                 (second / name).read_text(encoding='utf-8'),
                                 f'{name} is not deterministic')

    def test_fail_stub_returns_enodev(self):
        text = STUB_SRC.read_text(encoding='utf-8')
        self.assertIn('movn\tw0, #18', text, 'stub must return -ENODEV (movn #18)')
        self.assertIn('agmipc_report_unavailable', text)


class ProductTest(unittest.TestCase):
    """ELF-level facts about the built shim."""

    def setUp(self):
        if not PRODUCT.is_file():
            self.skipTest('shim 未构建：work/marble-agmipc-shim/libhmos_agmipc_shim.so 不存在，'
                          '先跑 compat/audio/agmipc/build_agmipc_shim.sh')

    def test_exports_exactly_the_agm_api(self):
        exported = [name for name in _dynsym_agm_functions(PRODUCT)]
        self.assertEqual(exported, sorted(_read_manifest()))

    def test_exports_helpers_for_diagnosis(self):
        nm = _tool('nm', 'nm')
        if nm is None:
            self.skipTest('llvm-nm/nm not available')
        out = _run([nm, '-D', '--defined-only', str(PRODUCT)]).stdout
        for helper in ('agmipc_shim_selfcheck', 'agmipc_shim_symbol_names',
                       'agmipc_shim_symbol_count'):
            self.assertIn(helper, out, f'{helper} missing from the shim')

    def test_no_agm_symbol_stays_unresolved(self):
        unresolved = [name for name in _undefined_symbols(PRODUCT) if name.startswith('agm_')]
        self.assertEqual(unresolved, [], f'shim would call back into itself: {unresolved}')

    def test_links_only_libc_and_libdl(self):
        """libagm.so must stay out of the global scope or RTLD_NEXT fallback
        would resolve to the real implementation instead of the marshaller."""
        self.assertEqual(sorted(_needed_libraries(PRODUCT)), ['libc.so', 'libdl.so'])

    def test_every_trampoline_is_transparent(self):
        sizes = _symbol_sizes(PRODUCT)
        for name in _read_manifest():
            self.assertIn(name, sizes, f'{name} not in dynamic symbol table')
            self.assertEqual(sizes[name], TRAMPOLINE_BYTES,
                             f'{name}: trampoline is {sizes[name]} bytes, expected '
                             f'{TRAMPOLINE_BYTES} (bti/adr/ldr/br)')

    def test_trampolines_branch_through_the_slot_table(self):
        objdump = _tool('objdump', 'objdump')
        if objdump is None:
            self.skipTest('llvm-objdump/objdump not available')
        out = _run([objdump, '-d', '--no-show-raw-insn', str(PRODUCT)]).stdout
        labels = re.findall(r'^[0-9a-f]+ <(agm_[a-z0-9_]+)>:$', out, re.M)
        self.assertGreaterEqual(len(labels), len(_read_manifest()))
        self.assertIn('br\tx16', out)
        self.assertNotIn('bl\t', out.split('<agmipc_shim_init>')[0],
                         'trampolines must tail-branch, not call')

    def test_provenance_records_the_built_hash(self):
        if not PROVENANCE.is_file():
            self.skipTest('PROVENANCE.md 缺失')
        text = PROVENANCE.read_text(encoding='utf-8')
        self.assertIn(_sha256(PRODUCT), text, 'PROVENANCE.md sha256 does not match product')
        self.assertIn('build_agmipc_shim.sh', text)
        self.assertIn('未', text + '未', 'provenance must state what is unverified')


class ReportTest(unittest.TestCase):
    """The report is part of the deliverable, not decoration."""

    def setUp(self):
        if not REPORT.is_file():
            self.fail(f'报告缺失: {REPORT.relative_to(ROOT)}')
        self.text = REPORT.read_text(encoding='utf-8')

    def test_states_not_verified_on_device(self):
        self.assertIn('未上机验证', self.text)

    def test_records_the_call_chain_evidence(self):
        for marker in ('libagmclient.so', 'IAGM::getService', 'libagm.so',
                       'byte_12D80', '0x8948', 'EINVAL'):
            self.assertIn(marker, self.text, f'报告缺少调用链证据: {marker}')

    def test_records_preload_and_acceptance(self):
        for marker in ('LD_PRELOAD', 'libhmos_agmipc_shim.so', 'PAL_SERVER_READY=1'):
            self.assertIn(marker, self.text, f'报告缺少上机步骤/判据: {marker}')

    def test_does_not_claim_audio_works(self):
        lowered = self.text.lower()
        for overclaim in ('音频已验证', '音频已通', '已在真机验证通过'):
            self.assertNotIn(overclaim, lowered)


if __name__ == '__main__':
    unittest.main()