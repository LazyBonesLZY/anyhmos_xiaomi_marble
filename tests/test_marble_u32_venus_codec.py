#!/usr/bin/env python3
"""Offline unit tests and artifact checks for Batch U3.2a (Venus V4L2 M2M + OpenMAX/Codec HDI 4.0)."""
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
PACKAGER = TOOLS / 'build_marble_h20_u32_venus_codec.py'
BASE_U3_IMG = Path('/mnt/e/marble-tmp/vendor_boot_h20_u3_media_display_10ddf24b.img')
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


class TestBatchU32VenusCodec(unittest.TestCase):
    def test_venus_v4l2_and_omx_host_suites(self):
        v4l2_c = ROOT / 'compat/codec/venus_v4l2.c'
        omx_c = ROOT / 'compat/codec/omx_core_venus.c'
        with tempfile.TemporaryDirectory(prefix='u32_host_') as tmp:
            v_exe = Path(tmp) / 'venus_v4l2_host_test'
            subprocess.run(
                ['cc', '-std=c11', '-DVENUS_V4L2_HOST_TEST', '-O2', '-Wall', '-Wextra', '-Werror',
                 str(v4l2_c), '-o', str(v_exe)],
                check=True,
            )
            r1 = subprocess.run([str(v_exe)], check=True, capture_output=True, text=True)
            self.assertIn('VENUS_V4L2_HOST_TEST_OK', r1.stdout)

            o_exe = Path(tmp) / 'omx_venus_host_test'
            subprocess.run(
                ['cc', '-std=c11', '-DOMX_VENUS_HOST_TEST', '-O2', '-Wall', '-Wextra', '-Werror',
                 '-pthread', str(omx_c), str(v4l2_c), '-o', str(o_exe)],
                check=True,
            )
            r2 = subprocess.run([str(o_exe)], check=True, capture_output=True, text=True, timeout=10)
            self.assertIn('OMX_VENUS_HOST_TEST_OK', r2.stdout)

    def test_cross_elf_contracts(self):
        omx_so = U3_BUILD / 'libOMX_Core.z.so'
        buf_so = U3_BUILD / 'libdisplay_buffer_vdi_impl.z.so'
        loop_bin = U3_BUILD / 'native_venus_decode_test'
        probe_bin = U3_BUILD / 'native_codec_probe'
        plane_bin = U3_BUILD / 'plane_layout_contract'
        for p in (omx_so, buf_so, loop_bin, probe_bin, plane_bin):
            self.assertTrue(p.is_file(), f'Missing compiled ELF: {p}')

        readelf = LLVM_BIN / 'llvm-readelf'
        nm = LLVM_BIN / 'llvm-nm'
        if readelf.is_file() and nm.is_file():
            dyn = subprocess.check_output([str(readelf), '-d', str(omx_so)], text=True)
            needed = re.findall(r'\(NEEDED\)\s+Shared library:\s+\[([^\]]+)\]', dyn)
            self.assertEqual(needed, ['libc.so'])
            self.assertIn('[libOMX_Core.z.so]', dyn)

            syms = subprocess.check_output([str(nm), '-D', '--defined-only', str(omx_so)], text=True)
            defined = {line.split()[-1] for line in syms.splitlines() if line.strip() and line.split()[1] in ('T', 'W')}
            for req in (
                'OMX_Init', 'OMX_Deinit', 'OMX_ComponentNameEnum', 'OMX_GetHandle',
                'OMX_FreeHandle', 'OMX_GetRolesOfComponent', 'OMX_GetComponentsOfRole',
                'OMX_SetupTunnel', 'OMX_GetContentPipe',
            ):
                self.assertIn(req, defined, f'Missing OMX symbol: {req}')

            buf_dyn = subprocess.check_output([str(readelf), '-d', str(buf_so)], text=True)
            buf_needed = re.findall(r'\(NEEDED\)\s+Shared library:\s+\[([^\]]+)\]', buf_dyn)
            self.assertEqual(buf_needed, ['libc++.so', 'libc.so'])
            self.assertIn('[libdisplay_buffer_vdi_impl.z.so]', buf_dyn)

            buf_syms = subprocess.check_output([str(nm), '-D', '--defined-only', str(buf_so)], text=True)
            buf_def = {line.split()[-1] for line in buf_syms.splitlines() if line.strip() and line.split()[1] in ('T', 'W')}
            self.assertIn('CreateDisplayBufferVdi', buf_def)
            self.assertIn('DestroyDisplayBufferVdi', buf_def)

            for exe in (loop_bin, probe_bin, plane_bin):
                interp = subprocess.check_output([str(readelf), '-l', str(exe)], text=True)
                self.assertIn('/lib/ld-musl-aarch64.so.1', interp)

    def test_hcb_video_omx_and_image_codec_preserved(self):
        if not DONOR_FS.is_dir():
            self.skipTest(f'Missing donor fs: {DONOR_FS}')
        hcb = p4.build_phase4_hdf_hcb(
            wifi_chip_donor=DONOR_FS,
            wpa_on_demand_donor=DONOR_FS,
            a2dp_donor=DONOR_FS,
            ril_donor=DONOR_FS,
            location_host=True,
            displayengine=True,
            codec_image_host=True,
            codec_video_omx=True,
        )
        root = parse_hcb_tree(hcb)
        top = node_children_map(root)
        self.assertIn('device_info', top)
        self.assertIn('image_codec_config', top)
        self.assertIn('codec_config', top)

        hosts = [c for c in top['device_info'][2] if c[0] == 'node']
        self.assertEqual(len(hosts), 9)
        codec_host = node_children_map(hosts[8])
        self.assertIn('codec_image_device', codec_host)
        self.assertIn('codec_omx_idl_device', codec_host)
        omx_dev = node_attrs_map(codec_host['codec_omx_idl_device'][2][0])
        self.assertEqual(omx_dev['moduleName'], 'libcodec_driver.z.so')
        self.assertEqual(omx_dev['serviceName'], 'codec_component_manager_service')
        self.assertEqual(omx_dev['deviceMatchAttr'], 'media_codec_capabilities')

        cc_children = node_children_map(top['codec_config'])
        cc_attrs = node_attrs_map(top['codec_config'])
        self.assertEqual(cc_attrs['match_attr'], 'media_codec_capabilities')
        decs = node_children_map(cc_children['VideoHwDecoders'])
        encs = node_children_map(cc_children['VideoHwEncoders'])
        self.assertEqual(set(decs), {'HDF_video_hw_dec_avc_marble', 'HDF_video_hw_dec_hevc_marble'})
        self.assertEqual(set(encs), {'HDF_video_hw_enc_avc_marble', 'HDF_video_hw_enc_hevc_marble'})
        for cap_node in (*decs.values(), *encs.values()):
            attrs = node_attrs_map(cap_node)
            self.assertEqual(attrs['isSupportPassthrough'], 0)
            self.assertEqual(attrs['isSoftwareCodec'], 0)
            self.assertEqual(attrs['supportPixelFmts'], ('arr', [24, 25, 28]))
            self.assertTrue(attrs['name'].startswith('OMX.marble.video.'))

    def test_packager_audio_guard_stage_and_roundtrip(self):
        self.assertIn('0.03f', (ROOT / 'compat/audio/audio_vdi.c').read_text(encoding='utf-8'))
        self.assertIn('0.03f', (ROOT / 'compat/audio/pal_server.c').read_text(encoding='utf-8'))

        spec = importlib.util.spec_from_file_location('build_u32', PACKAGER)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        self.assertEqual(len(mod.GUARD_MEMBERS), 24)

        sample_stage = b'#!/bin/sh\nset -eu\nu3_libs=0\n' + mod.STAGE_U32_ANCHOR
        patched = mod.patch_stage(sample_stage)
        self.assertNotIn(b'bind_once', patched)
        self.assertNotIn(b'skipped ', patched)
        self.assertIn(b'libOMX_Core.z.so', patched)
        self.assertIn(b'MARBLE_U32_STAGE_READY', patched)
        with self.assertRaises(ValueError):
            mod.patch_stage(patched)

        report_path = U3_BUILD / 'h20-u32-venus-codec-report.json'
        if report_path.is_file() and BASE_U3_IMG.is_file():
            rep = json.loads(report_path.read_text(encoding='utf-8'))
            self.assertEqual(rep['base']['sha256'], mod.BASE_SHA)
            self.assertEqual(len(rep['guard_members_verified']), 24)
            cand_path = Path(rep['candidate']['path'])
            self.assertTrue(cand_path.is_file())
            self.assertEqual(cand_path.stat().st_size, 100663296)

            _, r = mod.load_oracle()
            img_bytes = r.read_original(cand_path)
            lay = r.layout(img_bytes)
            with tempfile.TemporaryDirectory(prefix='u32_inspect_') as tmp:
                got, *_ = r.extract_entries(cand_path, img_bytes, lay, Path(tmp))
                root = parse_hcb_tree(got[mod.HCB_MEMBER][1])
                top = node_children_map(root)
                self.assertIn('codec_config', top)
                self.assertIn('image_codec_config', top)
                stage_txt = got[mod.STAGE_MEMBER][1].decode('utf-8')
                subprocess.run(['sh', '-n'], input=stage_txt.encode('utf-8'), check=True)
                self.assertIn('MARBLE_U32_STAGE_READY', stage_txt)
                self.assertNotIn('bind_once', stage_txt)
                self.assertNotIn('skipped ', stage_txt)
                for req in (
                    'hmos-compat-marble/graphics/libOMX_Core.z.so',
                    'hmos-compat-marble/graphics/native_venus_decode_test',
                    'hmos-compat-marble/graphics/native_codec_probe',
                    'hmos-compat-marble/graphics/libdisplay_buffer_vdi_impl.z.so',
                    'hmos-compat-marble/graphics/plane_layout_contract',
                ):
                    self.assertIn(req, got)
                self.assertEqual(
                    got['hmos-compat-marble/graphics/libdisplay_buffer_vdi_impl.z.so'][1],
                    (U3_BUILD / 'libdisplay_buffer_vdi_impl.z.so').read_bytes(),
                )
                self.assertEqual(
                    got['hmos-compat-marble/graphics/libOMX_Core.z.so'][1],
                    (U3_BUILD / 'libOMX_Core.z.so').read_bytes(),
                )


if __name__ == '__main__':
    unittest.main()
