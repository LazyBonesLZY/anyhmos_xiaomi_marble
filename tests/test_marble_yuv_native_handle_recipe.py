"""Host-only differential test for ADY CreateVkImage's in-place YUV recipe."""

import hashlib
import io
import json
import random
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DONOR = ROOT / ('work/donor-rom/ADY-AL10-207.0.0.111-SP10/fs/system/system/lib64/'
                'librender_service_base.z.so')
RECIPES = ROOT / 'patches/donor/ady-al10-111-sp10'
CONTROL = RECIPES / 'librender_service_base.z.so.json'
YUV = RECIPES / 'librender_service_base.yuv-native-handle.json'
ORIGINAL_SHA = 'd1482a820092766eb52f77f8268f21dd9eea7149045669553c7aaa66ca6c9b07'
CONTROL_SHA = 'e916d1850510f557cd5f3a22e2b038870edb7581b91b1ea63414adbf6ce28876'
YUV_SHA = 'b4c7c0b86a42fd3e329fae5444bb5ff17f176aaff5313873d53bd6d17c17157a'

START = 0x3DFFBC  # After the prologue: SP allocated and X29 = SP+0xA0.
JOIN = 0x3E0060   # Before the width CBZ and after the patched NZCV restoration.
EXT_LOAD = 0x3E010C  # Out-of-line load reached only for VkFormat == 0.
GUARD_PASS = 0x3E0088
GUARD_REJECT = 0x3E0118
CODE_END = GUARD_REJECT + 4  # Include the first instruction at the reject target.
VA_BIAS = 0x1000

PAGE = 0x1000
STACK = 0x700000
SP = STACK + 0x800
DATA = 0x710000
CONTEXT, OUTPUT, PROPS, EXTENT = (DATA + n for n in (0x10, 0x20, 0x100, 0x200))
IMAGE_INFO = 0x10
MEMORY_INFO = 0x70
FORMAT_INFO = 0x88
HANDLE_AT = SP - STACK + MEMORY_INFO + 16


def outside_constructor(recipe):
    changes = []
    for c in recipe['changes']:
        offset = c['offset']
        end = offset + len(bytes.fromhex(c['before']))
        if not START - VA_BIAS <= offset < end <= JOIN - VA_BIAS:
            changes.append((offset, c['before'], c['after']))
    return changes


class YuvNativeHandleRecipeMetadataTests(unittest.TestCase):
    def test_recipe_preserves_the_historical_commit_edit_and_usage_guard(self):
        control, yuv = (json.loads(p.read_text()) for p in (CONTROL, YUV))
        self.assertEqual(control['input_sha256'], ORIGINAL_SHA)
        self.assertEqual(control['output_sha256'], CONTROL_SHA)
        self.assertEqual(yuv['input_sha256'], ORIGINAL_SHA)
        self.assertEqual(yuv['control_sha256'], CONTROL_SHA)
        self.assertEqual(yuv['output_sha256'], YUV_SHA)
        self.assertEqual(yuv['path'], control['path'])
        self.assertEqual(outside_constructor(control), [(0x438CFC, '21008052', '01008052')])
        self.assertEqual(outside_constructor(yuv), outside_constructor(control))
        for recipe in (control, yuv):
            guards = {g['offset']: g['bytes'] for g in recipe['guards']}
            self.assertEqual(guards[0x3DEA74], 'e9008052')  # caller MOV W9, #7


class YuvNativeHandleRecipeEmulationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not DONOR.is_file():
            raise unittest.SkipTest(f'ADY SP10 donor fixture missing: {DONOR}')
        try:
            import capstone
            import unicorn
            from elftools.elf.elffile import ELFFile
            from unicorn import arm64_const
        except ImportError as exc:
            raise unittest.SkipTest(f'Host emulation dependency unavailable: {exc}') from exc
        cls.uc, cls.reg = unicorn, arm64_const
        cls.decoder = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_LITTLE_ENDIAN)
        cls.decoder.detail = True

        with tempfile.TemporaryDirectory(prefix='marble-yuv-native-handle-') as tmp:
            original = DONOR.read_bytes()
            patched = []
            for name, recipe in (('control', CONTROL), ('yuv', YUV)):
                output = Path(tmp) / f'{name}.so'
                result = subprocess.run([
                    sys.executable, str(ROOT / 'tools/packaging/apply_guarded_elf_patch.py'),
                    '--manifest', str(recipe), '--input', str(DONOR), '--output', str(output),
                ], capture_output=True, text=True, cwd=ROOT)
                if result.returncode:
                    raise AssertionError(f'{recipe.name}: {result.stderr}')
                patched.append(output.read_bytes())
        control, yuv = patched
        low, high = START - VA_BIAS, JOIN - VA_BIAS
        if len(yuv) != len(control) or yuv[:low] != control[:low] or yuv[high:] != control[high:]:
            raise AssertionError('YUV output differs from historical control outside CreateVkImage')
        cls.code = {}
        for name, raw, expected_sha in (
            ('original', original, ORIGINAL_SHA), ('control', control, CONTROL_SHA), ('yuv', yuv, YUV_SHA),
        ):
            if hashlib.sha256(raw).hexdigest() != expected_sha:
                raise AssertionError(f'{name}: unexpected ELF SHA-256')
            elf = ELFFile(io.BytesIO(raw))
            if not (elf.elfclass == 64 and elf.little_endian and elf['e_machine'] == 'EM_AARCH64'):
                raise AssertionError(f'{name}: expected little-endian AArch64 ELF64')
            offsets = list(elf.address_offsets(START, CODE_END - START))
            if offsets != [low]:
                raise AssertionError(f'{name}: unexpected file mapping for VA {START:#x}: {offsets}')
            cls.code[name] = raw[low:low + CODE_END - START]

    def test_branch_anchors_in_the_actual_elf_bytes(self):
        # Independent decode checks that both dimension CBZs and the
        # out-of-line format==0 path still lead to their original destinations.
        for name, code in self.code.items():
            with self.subTest(elf=name):
                insns = {i.address: i for i in self.decoder.disasm(code, START)}
                for pc, reg, dest in (
                    (0x3DFFD8, 'w8', EXT_LOAD),
                    (JOIN, 'w7', GUARD_PASS),
                    (0x3E0068, 'w8', GUARD_PASS),
                ):
                    insn = insns[pc]
                    self.assertEqual(insn.mnemonic, 'cbz')
                    self.assertEqual(insn.reg_name(insn.operands[0].reg), reg)
                    self.assertEqual(insn.operands[1].imm, dest)
                back = insns[0x3E0114]
                self.assertEqual((back.mnemonic, back.operands[0].imm), ('b', 0x3DFFDC))

    def emulate(self, code, initial_stack, rng, fmt, external, dims, protected):
        uc, regs = self.uc, self.reg
        engine = uc.Uc(uc.UC_ARCH_ARM64, uc.UC_MODE_ARM)
        code_page = START & -PAGE
        engine.mem_map(code_page, 2 * PAGE)
        engine.mem_write(START, code)
        engine.mem_protect(code_page, 2 * PAGE, uc.UC_PROT_READ | uc.UC_PROT_EXEC)
        engine.mem_map(STACK, PAGE, uc.UC_PROT_READ | uc.UC_PROT_WRITE)
        engine.mem_write(STACK, initial_stack)
        engine.mem_map(DATA, PAGE, uc.UC_PROT_READ | uc.UC_PROT_WRITE)
        engine.mem_write(DATA, rng.randbytes(PAGE))
        engine.mem_write(PROPS + 16, struct.pack('<I4xQ', fmt, external))
        engine.mem_write(EXTENT, struct.pack('<3I', *dims))
        engine.mem_protect(DATA, PAGE, uc.UC_PROT_READ)

        def reg_id(name):
            return getattr(regs, f'UC_ARM64_REG_{name.upper()}')

        # Poison scratch/callee-saved registers as well as stack. Only the
        # post-prologue ABI inputs are then overwritten.
        for n in range(31):
            engine.reg_write(reg_id(f'x{n}'), rng.getrandbits(64))
        entry = dict(x0=CONTEXT, x1=OUTPUT, x2=PROPS, x3=EXTENT,
                     x4=4 if fmt == 0 else 7, x5=protected, sp=SP, x29=SP + 0xA0,
                     nzcv=rng.randrange(16) << 28)
        for name, value in entry.items():
            engine.reg_write(reg_id(name), value)

        trace = []

        def trace_instruction(cpu, address, _size, _data):
            if address == GUARD_REJECT:
                cpu.emu_stop()  # Stop before logging or a Vulkan/TLS dispatch.
            else:
                trace.append(address)

        engine.hook_add(uc.UC_HOOK_CODE, trace_instruction)
        engine.emu_start(START, JOIN, count=128)
        self.assertEqual(engine.reg_read(reg_id('pc')), JOIN)

        # At JOIN, W7 feeds the first CBZ, X3 feeds the other extent reads,
        # X19/X20 are dispatch inputs, and SP/X29 delimit the frame. Also
        # contract-check W5 and W8 (still depth here, later reloaded as height).
        # The guard overwrites W9/W10 before reading them, and never reads W11.
        live = {name: engine.reg_read(reg_id(name)) for name in
                ('w5', 'w7', 'w8', 'x3', 'x19', 'x20', 'sp', 'x29', 'nzcv')}
        stack = bytes(engine.mem_read(STACK, PAGE))
        constructor_trace = tuple(trace)
        trace.clear()
        engine.emu_start(JOIN, GUARD_PASS, count=16)
        return live, stack, constructor_trace, engine.reg_read(reg_id('pc')), tuple(trace)

    def check_stack(self, stack, initial, fmt, external, dims, protected, handle):
        # LP64 Vulkan ABI, reconstructed from the original donor. Compare the
        # full byte representations, not just logical fields; padding started
        # with random bytes and must be initialized the same way as the donor.
        info = (struct.pack('<I4xQ', 14, SP + MEMORY_INFO)
                + struct.pack('<13I', 0x800 if protected else 0, 1, fmt, *dims,
                              1, 1, 1, 0, 4 if fmt == 0 else 7, 0, 0)
                + struct.pack('<4xQI4x', 0, 0))
        memory = struct.pack('<I4xQI4x', 1000072001, SP + FORMAT_INFO, handle)
        external_format = struct.pack('<I4xQQ', 1000452005, 0, external if fmt == 0 else 0)
        untouched = bytearray(stack)
        for offset, expected, label in (
            (IMAGE_INFO, info, 'VkImageCreateInfo'),
            (MEMORY_INFO, memory, 'VkExternalMemoryImageCreateInfo'),
            (FORMAT_INFO, external_format, 'VkExternalFormatOHOS'),
        ):
            start = SP - STACK + offset
            stop = start + len(expected)
            self.assertEqual(stack[start:stop], expected, label)
            untouched[start:stop] = initial[start:stop]
        self.assertEqual(bytes(untouched), initial, 'stack outside the three Vulkan structures')

    def test_only_undefined_format_changes_handle_vs_historical_control(self):
        formats = ((0, 24), (0, 28), (0, 29), (0, 0), (0, 0xCAFEFFEEDDCCBBAA),
                   (37, 0), (44, 24), (0x80000000, 29))  # Includes a synthetic W-register boundary.
        dimensions = (
            ((960, 720, 1), GUARD_PASS),
            ((0, 721, 3), GUARD_PASS),     # width CBZ taken
            ((961, 0, 7), GUARD_PASS),     # height CBZ taken
            ((2, 4, 12_500_000), GUARD_PASS),    # exact area/volume limit
            ((2, 4, 12_500_001), GUARD_REJECT),  # one beyond the limit
        )
        for fmt, external in formats:
            for dims, guard_target in dimensions:
                for protected in (0, 1):
                    for seed in (5, 23, 0x8ACE):
                        with self.subTest(format=hex(fmt), external=hex(external), dims=dims,
                                          protected=protected, seed=seed):
                            self.compare_case(fmt, external, dims, guard_target, protected, seed)

    def compare_case(self, fmt, external, dims, guard_target, protected, seed):
        initial = random.Random(seed).randbytes(PAGE)
        handles = dict(original=0x8000, control=1, yuv=0x8000 if fmt == 0 else 1)
        observations = {}
        for name, code in self.code.items():
            live, stack, trace, target, guard_trace = self.emulate(
                code, initial, random.Random(seed + 1), fmt, external, dims, protected)
            self.assertEqual(live, dict(w5=protected, w7=dims[0], w8=dims[2], x3=EXTENT,
                                        x19=OUTPUT, x20=CONTEXT, sp=SP, x29=SP + 0xA0,
                                        nzcv=0 if protected else 0x40000000), name)
            self.assertEqual(EXT_LOAD in trace, fmt == 0, name)
            self.assertEqual(target, guard_target, name)
            self.assertEqual(0x3E0068 in guard_trace, dims[0] != 0, name)
            self.assertEqual(0x3E0074 in guard_trace, dims[0] != 0 and dims[1] != 0, name)
            self.check_stack(stack, initial, fmt, external, dims, protected, handles[name])
            # Keep the padding of handleTypes (the high four bytes of the
            # proposed 64-bit store) in the differential; mask only the field.
            observations[name] = (live, stack[:HANDLE_AT], stack[HANDLE_AT + 4:], trace, guard_trace)
        self.assertEqual(observations['control'], observations['original'])
        self.assertEqual(observations['yuv'], observations['control'])


if __name__ == '__main__':
    unittest.main()
