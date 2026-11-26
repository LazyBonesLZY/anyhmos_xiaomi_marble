#!/usr/bin/env python3
"""Offline unit tests and artifact checks for Batch U3.2b (Venus HEIC decode VDI)."""
import importlib.util
import json
import re
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / 'tools/packaging'
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import build_marble_phase4_images as p4

U3_BUILD = ROOT / 'work/marble-u3-build'
DONOR_FS = ROOT / 'work/donor-rom/ADY-AL10-207.0.0.111-SP10/fs'
PACKAGER = TOOLS / 'build_marble_h20_u32b_heif_codec.py'
BASE_U32_IMG = Path('/mnt/e/marble-tmp/vendor_boot_h20_u32_venus_codec_fd7a6b4a.img')
LLVM_BIN = ROOT / 'work/ohos-sdk/installed/native/llvm/bin'


def parse_hcb_tree(data: bytes):
    magic, _, ver_minor, _, _ = struct.unpack_from('<5I', data, 0)
    assert magic == 0xA00AA00A and ver_minor == 7

    def read_str(buf, pos):
        end = buf.index(0, pos)
        return buf[pos:end].decode('utf-8'), end + 1

    def parse(buf, pos):
        tag = buf[pos]
        pos += 1
        if tag == 1:
            name, pos = read_str(buf, pos)
            sz = struct.unpack_from('<I', buf, pos)[0]
            pos += 4
            end = pos + sz
            children = []
            while pos < end:
                ch, pos = parse(buf, pos)
                children.append(ch)
            assert pos == end
            return ('node', name, children), pos
        if tag == 2:
            name, pos = read_str(buf, pos)
            val, pos = parse(buf, pos)
            return ('attr', name, val), pos
        if tag == 4:
            cnt = struct.unpack_from('<H', buf, pos)[0]
            pos += 2
            items = []
            for _ in range(cnt):
                v, pos = parse(buf, pos)
                items.append(v)
            return ('arr', items), pos
        if tag == 0x10:
            return buf[pos], pos + 1
        if tag == 0x11:
            return struct.unpack_from('<H', buf, pos)[0], pos + 2
        if tag == 0x12:
            return struct.unpack_from('<I', buf, pos)[0], pos + 4
        if tag == 0x13:
            return struct.unpack_from('<Q', buf, pos)[0], pos + 8
        if tag == 0x14:
            return read_str(buf, pos)
        raise AssertionError(f'Unknown HCB tag 0x{tag:x}')

    root, end_pos = parse(data, 20)
    assert end_pos == len(data)
    return root


def node_children_map(node):
    return {c[1]: c for c in node[2]}


def node_attrs_map(node):
    return {c[1]: c[2] for c in node[2] if c[0] == 'attr'}


class TestBatchU32bHeifCodec(unittest.TestCase):
    def test_heif_vdi_and_probe_host_suites(self):
        v4l2_c = ROOT / 'compat/codec/venus_v4l2.c'
        heif_c = ROOT / 'compat/codec/heif_vdi_impl.c'
        probe_c = ROOT / 'compat/codec/native_heif_probe.c'
        with tempfile.TemporaryDirectory(prefix='u32b_host_') as tmp:
            h_exe = Path(tmp) / 'heif_vdi_host_test'
            subprocess.run(
                ['cc', '-std=c11', '-DHEIF_VDI_HOST_TEST', '-O2', '-Wall', '-Wextra', '-Werror',
                 '-pthread', str(heif_c), str(v4l2_c), '-o', str(h_exe)],
                check=True,
            )
            r1 = subprocess.run([str(h_exe)], check=True, capture_output=True, text=True, timeout=10)
            self.assertIn('HEIF_VDI_HOST_TEST_OK', r1.stdout)

            p_exe = Path(tmp) / 'heif_probe_host_test'
            subprocess.run(
                ['cc', '-std=c11', '-DHEIF_PROBE_HOST_TEST', '-O2', '-Wall', '-Wextra', '-Werror',
                 str(probe_c), '-o', str(p_exe)],
                check=True,
            )
            r2 = subprocess.run([str(p_exe)], check=True, capture_output=True, text=True, timeout=10)
            self.assertIn('HEIF_PROBE_HOST_TEST_OK', r2.stdout)

    def test_cross_elf_contracts(self):
        heif_so = U3_BUILD / 'libheifdecode_vdi_impl.z.so'
        probe_bin = U3_BUILD / 'native_heif_probe'
        self.assertTrue(heif_so.is_file(), f'Missing compiled ELF: {heif_so}')
        self.assertTrue(probe_bin.is_file(), f'Missing compiled ELF: {probe_bin}')

        readelf = LLVM_BIN / 'llvm-readelf'
        nm = LLVM_BIN / 'llvm-nm'
        if readelf.is_file() and nm.is_file():
            dyn = subprocess.check_output([str(readelf), '-d', str(heif_so)], text=True)
            needed = re.findall(r'\(NEEDED\)\s+Shared library:\s+\[([^\]]+)\]', dyn)
            self.assertEqual(needed, ['libc.so'])
            self.assertIn('[libheifdecode_vdi_impl.z.so]', dyn)

            syms = subprocess.check_output([str(nm), '-D', '--defined-only', str(heif_so)], text=True)
            defined = {line.split()[-1] for line in syms.splitlines() if line.strip() and line.split()[1] in ('T', 'W')}
            self.assertIn('GetCodecHeifDecodeHwi', defined)

            interp = subprocess.check_output([str(readelf), '-l', str(probe_bin)], text=True)
            self.assertIn('/lib/ld-musl-aarch64.so.1', interp)

    def test_hcb_heif_decoder_and_existing_codecs(self):
        if not DONOR_FS.is_dir():
            self.skipTest(f'Missing donor fs: {DONOR_FS}')
        with self.assertRaises(ValueError):
            p4.build_phase4_hdf_hcb(codec_heif_hw=True)

        hcb = p4.build_phase4_hdf_hcb(
            wifi_chip_donor=DONOR_FS,
            wpa_on_demand_donor=DONOR_FS,
            a2dp_donor=DONOR_FS,
            ril_donor=DONOR_FS,
            location_host=True,
            displayengine=True,
            codec_image_host=True,
            codec_video_omx=True,
            codec_heif_hw=True,
        )
        root = parse_hcb_tree(hcb)
        top = node_children_map(root)
        self.assertIn('image_codec_config', top)
        self.assertIn('codec_config', top)

        ic_children = node_children_map(top['image_codec_config'])
        img_decs = node_children_map(ic_children['ImageHwDecoders'])
        self.assertEqual(set(img_decs), {'HDF_Image_hw_dec_jpeg_marble', 'HDF_Image_hw_dec_heif_marble'})

        heif_attrs = node_attrs_map(img_decs['HDF_Image_hw_dec_heif_marble'])
        self.assertEqual(heif_attrs['role'], 1)
        self.assertEqual(heif_attrs['type'], 0)
        self.assertEqual(heif_attrs['name'], 'HDF.marble.decoder.heif')
        self.assertEqual(heif_attrs['maxInst'], 4)
        self.assertEqual(heif_attrs['isSoftwareCodec'], 0)
        self.assertEqual(heif_attrs['minWidth'], 128)
        self.assertEqual(heif_attrs['minHeight'], 128)
        self.assertEqual(heif_attrs['maxWidth'], 8192)
        self.assertEqual(heif_attrs['maxHeight'], 8192)
        self.assertEqual(heif_attrs['widthAlignment'], 2)
        self.assertEqual(heif_attrs['heightAlignment'], 2)
        self.assertEqual(heif_attrs['maxSample'], 16)
        self.assertEqual(heif_attrs['supportPixelFmts'], ('arr', [3, 12, 20, 24, 25]))

    def test_packager_audio_guard_stage_and_roundtrip(self):
        self.assertIn('0.03f', (ROOT / 'compat/audio/audio_vdi.c').read_text(encoding='utf-8'))
        self.assertIn('0.03f', (ROOT / 'compat/audio/pal_server.c').read_text(encoding='utf-8'))

        spec = importlib.util.spec_from_file_location('build_u32b', PACKAGER)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        self.assertEqual(len(mod.GUARD_MEMBERS), 29)

        sample_stage = b'#!/bin/sh\nset -eu\nu3_libs=0\nu32_bins=2\n' + mod.STAGE_U32B_ANCHOR
        patched = mod.patch_stage(sample_stage)
        self.assertNotIn(b'bind_once', patched)
        self.assertNotIn(b'skipped ', patched)
        self.assertIn(b'libheifdecode_vdi_impl.z.so', patched)
        self.assertIn(b'persist.multimedia.image.heifhardwaredecode.enabled=true', patched)
        self.assertIn(b'MARBLE_U32B_STAGE_READY', patched)
        self.assertLess(patched.index(b'MARBLE_U32B_STAGE_READY'), patched.index(b'MARBLE_U32_STAGE_READY'))
        with self.assertRaises(ValueError):
            mod.patch_stage(patched)

        report_path = U3_BUILD / 'h20-u32b-heif-codec-report.json'
        if report_path.is_file() and BASE_U32_IMG.is_file():
            rep = json.loads(report_path.read_text(encoding='utf-8'))
            self.assertEqual(rep['base']['sha256'], mod.BASE_SHA)
            self.assertEqual(len(rep['guard_members_verified']), 29)
            cand_path = Path(rep['candidate']['path'])
            self.assertTrue(cand_path.is_file())
            self.assertEqual(cand_path.stat().st_size, 100663296)

            _, r = mod.load_oracle()
            img_bytes = r.read_original(cand_path)
            lay = r.layout(img_bytes)
            with tempfile.TemporaryDirectory(prefix='u32b_inspect_') as tmp:
                got, *_ = r.extract_entries(cand_path, img_bytes, lay, Path(tmp))
                root = parse_hcb_tree(got[mod.HCB_MEMBER][1])
                top = node_children_map(root)
                self.assertIn('image_codec_config', top)
                self.assertIn('codec_config', top)
                ic = node_children_map(top['image_codec_config'])
                self.assertIn('HDF_Image_hw_dec_heif_marble', node_children_map(ic['ImageHwDecoders']))
                stage_txt = got[mod.STAGE_MEMBER][1].decode('utf-8')
                subprocess.run(['sh', '-n'], input=stage_txt.encode('utf-8'), check=True)
                self.assertIn('MARBLE_U32B_STAGE_READY', stage_txt)
