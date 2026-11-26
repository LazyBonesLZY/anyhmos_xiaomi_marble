#!/usr/bin/env python3
"""Build Xiaomi marble Phase 4 images (boot_phase4.img + vendor_boot_phase4.img).

Phase 4 brings up the full hardware display, GPU, touch, and desktop stack on top
of the already-flashed Phase 3 read-only super (ADY-AL10 + marble vendor_dlkm) and
ext4 userdata partitions:
  - Single-DSI 1080x2400 OLED composer VDI (HMOS_SPLIT_SCANOUT=0) + DMA-heap buffer VDI
  - Mesa Turnip Adreno 725 KGSL Vulkan ICD + Mesa Gallium-EGL/Zink runtime
  - Adreno 725 GPU + Goodix 9916r touch firmware (/stock-gpu-firmware -> /compat/firmware)
  - 5-host HDF HCB (allocator, composer, userauth, WLAN, WPA), with an opt-in chip HDI trial
  - SHA-256-guarded ADY-AL10 donor patches (RAM-only)
  - Optional RS screen order, native commit, or H20 native YUV handle recipes
  - UserIAM 4.0/4.1 software backend, appspawn /compat sandbox overlay, desktop init cfgs
"""

from __future__ import annotations

import argparse
import gzip
import json
import tarfile
from pathlib import Path
import struct
import subprocess
import sys

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

from build_marble_phase2_images import (
    BOOT_LIMIT_BYTES,
    COMPAT_DIR,
    ROOT,
    VENDOR_BOOT_LIMIT_BYTES,
    attach_boot_v4_signature,
    compress_ramdisk,
    decompress_ramdisk,
    extract_full_single_vendor_ramdisk,
    extract_tar_modules,
    pack_cpio_newc,
    parse_cpio_newc,
    preserve_and_update_avb_footer,
    prune_module_metadata,
    put_file,
    replace_arg_value,
    sha256_bytes,
    unpack_with_args,
)
from build_marble_phase3_images import (
    PHASE3_DBKEY_RECIPES,
    PHASE3_DONOR_RECIPES,
    compile_phase3_binaries,
    generate_phase3_donor_patch_script,
)
from liblp_sparse import sha256_file
from marble_diagnostics import load_diagnostics
from samgr_original_role import SAMGR_INPUTS, restore_samgr_role


PHASE4_EXTRA_RECIPES = [
    'libvulkan.so.json',
    'librender_service_base.z.so.json',
]

PHASE4_ALL_RECIPES = PHASE3_DONOR_RECIPES + PHASE3_DBKEY_RECIPES + PHASE4_EXTRA_RECIPES
RS_SCREEN_ORDER_RECIPE = 'librender_service.z.so.json'
RS_COMMIT_CONTROL_RECIPE = 'librender_service_base.z.so.json'
RS_NATIVE_COMMIT_RECIPE = 'librender_service_base.native-commit.json'
RS_YUV_NATIVE_HANDLE_RECIPE = 'librender_service_base.yuv-native-handle.json'
RS_YUV_NATIVE_HANDLE_LINE_SHA256 = '02761efbd1271ed614aa8317620f1750eb8c763d96a7d07c9c8ad1d9db4ede98'
RS_YUV_NATIVE_TURNIP_SHA256 = 'de21593bf75316332f112ad8ae746d9583daffaa195cb12c16eddf6a936d1c35'
TURNIP_PAYLOAD = 'libvulkan_freedreno.so.gz'


def phase4_init_seed(source: Path, donor_fs_dir: Path) -> tuple[bytes, dict]:
    """Prepare the seed hmos_root installs after the RAM staging scripts."""
    donor_inputs = {
        path: (donor_fs_dir / 'system' / path.lstrip('/')).read_bytes()
        for path in SAMGR_INPUTS
    }
    return restore_samgr_role(source.read_bytes(), donor_inputs)


def phase4_wifi_supplicant_payload(root: Path) -> dict[str, tuple[bytes, int]]:
    """Ship only the config stage and its blank templates, not a replacement libwpa."""
    wifi = root / 'compat/wifi'
    sources = {
        'marble-wifi-supplicant-stage.sh': (wifi / 'marble-wifi-supplicant-stage.sh', 0o755),
        'etc/wpa_supplicant.conf': (wifi / 'runtime/etc/wpa_supplicant.conf', 0o644),
        'etc/p2p_supplicant.conf': (wifi / 'runtime/etc/p2p_supplicant.conf', 0o644),
    }
    payload = {}
    blank = b'ctrl_interface=/data/service/el1/public/wifi/sockets/wpa\n'
    for name, (path, mode) in sources.items():
        if path.is_symlink() or not path.is_file():
            raise ValueError(f'Expected a regular Wi-Fi staging artifact: {path}')
        blob = path.read_bytes()
        if name.startswith('etc/') and blob != blank:
            raise ValueError(f'Wi-Fi template is not the reviewed blank config: {path}')
        payload[f'graphics/wifi-supplicant/{name}'] = (blob, mode)
    return payload


def phase4_donor_recipes(
    rs_screen_order: bool = False, *, rs_native_commit: bool = False, rs_yuv_native_handle: bool = False,
) -> list[str]:
    if rs_native_commit and rs_yuv_native_handle:
        raise ValueError('Native commit and H20 YUV handle recipes cannot be combined')
    render_recipe = RS_COMMIT_CONTROL_RECIPE
    if rs_native_commit:
        render_recipe = RS_NATIVE_COMMIT_RECIPE
    elif rs_yuv_native_handle:
        render_recipe = RS_YUV_NATIVE_HANDLE_RECIPE
    names = [render_recipe if name == RS_COMMIT_CONTROL_RECIPE else name for name in PHASE4_ALL_RECIPES]
    if rs_screen_order:
        names.append(RS_SCREEN_ORDER_RECIPE)
    return names


def verify_h20_yuv_inputs(patch_script: str, turnip_gzip: bytes) -> None:
    """Check the emitted render command and the driver tested against it."""
    prefix = 'apply_one "$R/system/lib64/librender_service_base.z.so" '
    render_lines = [line for line in patch_script.splitlines() if line.startswith(prefix)]
    if (len(render_lines) != 1 or
            sha256_bytes(render_lines[0].encode('utf-8')) != RS_YUV_NATIVE_HANDLE_LINE_SHA256):
        raise ValueError('H20 YUV render command differs from the verified input')
    if sha256_bytes(gzip.decompress(turnip_gzip)) != RS_YUV_NATIVE_TURNIP_SHA256:
        raise ValueError('H20 YUV handle recipe requires the verified Turnip binary')


def _hcb_val(kind: str, value) -> bytes:
    if kind == 'u8':
        return b'\x10' + struct.pack('<B', value)
    if kind == 'u16':
        return b'\x11' + struct.pack('<H', value)
    if kind == 'u32':
        return b'\x12' + struct.pack('<I', value)
    if kind == 'u64':
        return b'\x13' + struct.pack('<Q', value)
    if kind == 'str':
        return b'\x14' + value.encode('utf-8') + b'\x00'
    if kind == 'arr':
        body = b''.join(_hcb_val(k, v) for k, v in value)
        return b'\x04' + struct.pack('<H', len(value)) + body
    raise ValueError(f'Unsupported HCB value kind: {kind}')


def _hcb_term(name: str, kind: str, value) -> bytes:
    return b'\x02' + name.encode('utf-8') + b'\x00' + _hcb_val(kind, value)


def _hcb_node(name: str, children: list[bytes]) -> bytes:
    body = b''.join(children)
    return b'\x01' + name.encode('utf-8') + b'\x00' + struct.pack('<I', len(body)) + body


WIFI_CHIP_DONOR_HCB = 'chip_prod/etc/hdfconfig/hdf_default.hcb'
WIFI_CHIP_DONOR_PINS = {
    WIFI_CHIP_DONOR_HCB: 'b085bdaef2504d6e8cfc893a3bba0669e556c4f851ff8e2249288987f1918662',
    'vendor/lib64/libchip_hdi_driver.z.so': '66aafb9c4838e425e82a3542a0cd2e56e1c1e8bb6bbaebb0981cad68b4c05c18',
    'vendor/lib64/libchip_stub_2.0.z.so': '5cc3ab6e6ad91878cbae791f0460cff859d0f2a2959e3ddbfd97cea9d8d04cee',
    'vendor/lib64/libchip_controller_chip_interface_service_2.0.z.so': 'ad33ee541038e661ec8405d05f4dd3a765d1396e89f0329658da46e9aaea1fd3',
    'vendor/lib64/libwifi_hal_hw.z.so': 'dc5541153349d413ee6b87c761e98406638540ff6b4de39dfe3ee5ff316ebd59',
}

WPA_DONOR_INIT = 'chip_prod/etc/init/hdf_devhost.cfg'
WPA_DONOR_PINS = {
    WIFI_CHIP_DONOR_HCB: WIFI_CHIP_DONOR_PINS[WIFI_CHIP_DONOR_HCB],
    WPA_DONOR_INIT: 'af998c899eb5ba1a691386ffb6cdcf747721d05cf887f11f4aef99baf65dbdce',
    'vendor/lib64/libwpa_hdi_c_device.z.so': '6c99602392bc86321867b135cc3fdda04d8af66dd7511b9544326e160c16ca8f',
    'vendor/lib64/libwpa_interface_service_2.0.z.so': '93235d387dbd8af453c109732000b00cf74cdd0731cddbad2c7a36c908b52c63',
}

A2DP_DONOR_PINS = {
    WIFI_CHIP_DONOR_HCB: WIFI_CHIP_DONOR_PINS[WIFI_CHIP_DONOR_HCB],
    WPA_DONOR_INIT: WPA_DONOR_PINS[WPA_DONOR_INIT],
    'vendor/etc/hdfconfig/a2dp_adapter_config.json': 'a1a8026c0354d0deeeb22c3ab59727a2a59f7bfcf6e75694bc74039a8050beab',
    'vendor/lib64/libaudio_bluetooth_hdi_adapter_server.z.so': '849cc5f74cb995cc2a619f9122c6dba868ac0a3fe571897ba1dab2be2df208c4',
    'vendor/lib64/libhdi_audio_bluetooth.z.so': '28947e4885316dd34a7c9e7ed34963494f5c15f39f973085f43c0ee8b12769b6',
    'vendor/lib64/libbluetooth_a2dp_hdi_driver.z.so': '1427e40bce40702cfde627be90142ea61908e50bffabeffbca836731c1edf44a',
    'vendor/lib64/libbluetooth_a2dp_stub_2.0.z.so': '01f34a7b8590f0a22d313b03ccc48962b1cc4c020256ac038037734b1ef17405',
    'vendor/lib64/libbluetooth_audio_session_service_2.0.z.so': '23cd38d88db27594d8b2823fd535a3dcdd10a4877786b433163bd1a1f4288820',
    'vendor/lib64/libbluetooth_audio_session.z.so': '52fc03ef2baddbf9fdc14a37e783e973905090886db558679a7a978ec6a99971',
    'vendor/lib64/passthrough/libaudio_bluetooth_hdi_proxy_server.z.so': '56ba53137d88f1c5b593b64bd1b90219a73b7eaf60e3027aed8157d4ea4125fd',
}

RIL_DONOR_PINS = {
    WIFI_CHIP_DONOR_HCB: WIFI_CHIP_DONOR_PINS[WIFI_CHIP_DONOR_HCB],
    'vendor/lib64/libril_driver.z.so': 'ed46cebf4108bb4f0b75e5973393f41dc42be9e2fef127267017e13c1df9352f',
    'vendor/lib64/libhril_hdf.z.so': 'd432d898b307ff25800f2aa084c7bc0973a44685fc1312c33fc6229e181e6b5c',
    'vendor/lib64/libhril.z.so': 'a77fc00e0a93476f709c6bf4a1c342632d6724d442cba664f63d2b8918e0746f',
}


def _phase4_ril_host(donor_fs: Path) -> bytes:
    """Validate and emit donor riladapter_host HCB node with preload=2 (demand-gated) and omit riladapterext_device."""
    inputs = _pinned_hdf_inputs(donor_fs, RIL_DONOR_PINS, 'RIL HDI')
    donor_hcb = inputs[WIFI_CHIP_DONOR_HCB]

    def _ril_subnodes(preload: int) -> list[bytes]:
        ril_dev = _hcb_node('ril_device', [_hcb_node('device0', [
            _hcb_term('policy', 'u8', 2),
            _hcb_term('priority', 'u8', 100),
            _hcb_term('moduleName', 'str', 'libril_driver.z.so'),
            _hcb_term('serviceName', 'str', 'ril_service'),
            _hcb_term('preload', 'u8', preload),
            _hcb_term('permission', 'u16', 436),
            _hcb_term('deviceMatchAttr', 'str', ''),
        ])])
        riladapter_dev = _hcb_node('riladapter_device', [_hcb_node('device0', [
            _hcb_term('policy', 'u8', 2),
            _hcb_term('priority', 'u8', 100),
            _hcb_term('moduleName', 'str', 'libhril_hdf.z.so'),
            _hcb_term('serviceName', 'str', 'cellular_radio1'),
            _hcb_term('preload', 'u8', preload),
            _hcb_term('permission', 'u16', 436),
            _hcb_term('deviceMatchAttr', 'str', ''),
        ])])
        return [ril_dev, riladapter_dev]

    host_terms = [
        _hcb_term('hostName', 'str', 'riladapter_host'),
        _hcb_term('priority', 'u8', 50),
        _hcb_term('uid', 'str', 'radio'),
        _hcb_term('gid', 'arr', [
            ('str', 'radio'),
            ('str', 'shell'),
            ('str', 'gps'),
            ('str', 'battery_extra_host'),
            ('str', 'oeminfo_nvme_server'),
        ]),
        _hcb_term('critical', 'arr', [('u8', 0), ('u8', 10), ('u8', 240)]),
        _hcb_term('caps', 'arr', [
            ('str', 'DAC_OVERRIDE'),
            ('str', 'DAC_READ_SEARCH'),
            ('str', 'CAP_NET_ADMIN'),
            ('str', 'CAP_NET_RAW'),
        ]),
    ]
    donor_ril_dev, donor_adapter_dev = _ril_subnodes(0)
    expected_prefix = b''.join(host_terms) + donor_ril_dev + donor_adapter_dev
    if (donor_hcb.count(donor_ril_dev) != 1 or
            donor_hcb.count(donor_adapter_dev) != 1 or
            donor_hcb.count(expected_prefix) != 1):
        raise ValueError('Donor riladapter HCB node does not match expected ADY-AL10 layout')
    return _hcb_node('riladapter', host_terms + _ril_subnodes(2))


def _pinned_hdf_inputs(donor_fs: Path, pins: dict[str, str], label: str) -> dict[str, bytes]:
    inputs = {}
    for rel, digest in pins.items():
        path = donor_fs / rel
        if path.is_symlink() or not path.is_file():
            raise ValueError(f'Missing regular {label} donor input: {path}')
        inputs[rel] = path.read_bytes()
        if sha256_bytes(inputs[rel]) != digest:
            raise ValueError(f'{label} donor SHA-256 mismatch: {path}')
    return inputs


def _phase4_wifi_chip_device(donor_fs: Path) -> bytes:
    """Use the verified ADY device object; never substitute a chip implementation."""
    inputs = _pinned_hdf_inputs(donor_fs, WIFI_CHIP_DONOR_PINS, 'chip HDI')
    device = _hcb_node('wifi_chip_device', [_hcb_node('device0', [
        _hcb_term('policy', 'u8', 2),
        _hcb_term('priority', 'u8', 100),
        _hcb_term('moduleName', 'str', 'libchip_hdi_driver.z.so'),
        _hcb_term('serviceName', 'str', 'chip_interface_service'),
        _hcb_term('preload', 'u8', 0),
        _hcb_term('permission', 'u16', 436),
        _hcb_term('deviceMatchAttr', 'str', ''),
    ])])
    if inputs[WIFI_CHIP_DONOR_HCB].count(device) != 1:
        raise ValueError('Expected exactly one byte-identical ADY chip device node')
    return device


def _phase4_wpa_device(preload: int) -> bytes:
    return _hcb_node('wpa_device', [_hcb_node('device0', [
        _hcb_term('policy', 'u8', 2),
        _hcb_term('preload', 'u8', preload),
        _hcb_term('priority', 'u8', 100),
        _hcb_term('moduleName', 'str', 'libwpa_hdi_c_device.z.so'),
        _hcb_term('serviceName', 'str', 'wpa_interface_service'),
        _hcb_term('permission', 'u16', 436),
        _hcb_term('deviceMatchAttr', 'str', ''),
    ])])


def phase4_wpa_ondemand_init(source: bytes, donor_fs: Path) -> bytes:
    """Let the native HDI manager start the existing host, not a second owner."""
    inputs = _pinned_hdf_inputs(donor_fs, WPA_DONOR_PINS, 'WPA HDI')
    doc = json.loads(source)
    donor = json.loads(inputs[WPA_DONOR_INIT])
    hosts = [s for s in doc['services'] if s['name'] == 'wpa_host']
    roles = [s for s in donor['services'] if s['name'] == 'wpa_host']
    if len(hosts) != 1 or len(roles) != 1:
        raise ValueError('Missing or duplicate WPA host')
    host, role = hosts[0], roles[0]
    role_fields = ('uid', 'gid', 'caps', 'secon', 'permission')
    if (role.get('ondemand') is not True or 'ondemand' in host or
            host.get('path') != ['/vendor/bin/hdf_devhost', '-i', '4', '-n', 'wpa_host'] or
            any(host.get(key) != role[key] for key in role_fields)):
        raise ValueError('Unexpected marble or donor WPA host identity')
    starts = [j for j in doc['jobs'] for cmd in j['cmds'] if cmd.split() == ['start', 'wpa_host']]
    if (len(starts) != 1 or starts[0].get('condition') != 'marble.debug.start.wifi=true' or
            'start wpa_host' not in starts[0]['cmds']):
        raise ValueError('Unexpected WPA start job; keep the original config')
    starts[0]['cmds'].remove('start wpa_host')
    host['ondemand'] = True
    return (json.dumps(doc, indent=2, ensure_ascii=True) + '\n').encode()


def _phase4_a2dp_host(donor_fs: Path) -> bytes:
    """Use the verified ADY a2dp_host declaration (audio_bluetooth_hdi_service + bluetooth_audio_session_service)."""
    inputs = _pinned_hdf_inputs(donor_fs, A2DP_DONOR_PINS, 'A2DP HDI')
    bt_dev0 = _hcb_node('device0', [
        _hcb_term('policy', 'u8', 2),
        _hcb_term('priority', 'u8', 100),
        _hcb_term('preload', 'u8', 2),
        _hcb_term('moduleName', 'str', 'libaudio_bluetooth_hdi_adapter_server.z.so'),
        _hcb_term('serviceName', 'str', 'audio_bluetooth_hdi_service'),
        _hcb_term('permission', 'u16', 436),
        _hcb_term('deviceMatchAttr', 'str', ''),
    ])
    session_dev0 = _hcb_node('device0', [
        _hcb_term('policy', 'u8', 2),
        _hcb_term('priority', 'u8', 100),
        _hcb_term('preload', 'u8', 2),
        _hcb_term('moduleName', 'str', 'libbluetooth_a2dp_hdi_driver.z.so'),
        _hcb_term('serviceName', 'str', 'bluetooth_audio_session_service'),
        _hcb_term('permission', 'u16', 436),
        _hcb_term('deviceMatchAttr', 'str', ''),
    ])
    host = _hcb_node('audio_bluetooth', [
        _hcb_term('hostName', 'str', 'a2dp_host'),
        _hcb_term('priority', 'u8', 50),
        _hcb_term('caps', 'arr', [('str', 'SYS_NICE')]),
        _hcb_term('initconfig', 'arr', [('str', '"importance" : -20')]),
        _hcb_node('bluetooth_device', [bt_dev0]),
        _hcb_node('a2dp_interface_device', [session_dev0]),
        _hcb_term('uid', 'str', ''),
        _hcb_term('gid', 'arr', [('str', '')]),
    ])
    if inputs[WIFI_CHIP_DONOR_HCB].count(host) != 1:
        raise ValueError('Expected exactly one byte-identical ADY audio_bluetooth host node')
    return host


def _phase4_location_host() -> bytes:
    """Build marble clean-room location_host HCB node (on-demand preload=2)."""
    gnss_dev0 = _hcb_node('device0', [
        _hcb_term('policy', 'u8', 2),
        _hcb_term('priority', 'u8', 100),
        _hcb_term('preload', 'u8', 2),
        _hcb_term('moduleName', 'str', 'libmarble_gnss_hdi_driver.z.so'),
        _hcb_term('serviceName', 'str', 'gnss_interface_service'),
        _hcb_term('permission', 'u16', 436),
        _hcb_term('deviceMatchAttr', 'str', ''),
    ])
    return _hcb_node('location', [
        _hcb_term('hostName', 'str', 'location_host'),
        _hcb_term('priority', 'u8', 50),
        _hcb_term('uid', 'str', 'location_host'),
        _hcb_term('gid', 'arr', [('str', 'location_host'), ('str', 'system'), ('str', 'gps'), ('str', 'radio')]),
        _hcb_term('caps', 'arr', [('str', 'DAC_OVERRIDE'), ('str', 'DAC_READ_SEARCH')]),
        _hcb_node('location_gnss_device', [gnss_dev0]),
    ])


def _phase4_displayengine_device() -> bytes:
    """Build marble displayengine_device node inside composer_host (V1.1 wrapper)."""
    return _hcb_node('displayengine_device', [_hcb_node('device0', [
        _hcb_term('policy', 'u8', 2),
        _hcb_term('priority', 'u8', 100),
        _hcb_term('moduleName', 'str', 'libdisplayengine_driver.z.so'),
        _hcb_term('serviceName', 'str', 'display_engine_wrapper_service'),
        _hcb_term('preload', 'u8', 0),
        _hcb_term('permission', 'u16', 436),
        _hcb_term('deviceMatchAttr', 'str', ''),
    ])])


def _hcb_typed_int(v: int) -> tuple[str, int]:
    if 0 <= v <= 0xFF:
        return ('u8', v)
    if 0 <= v <= 0xFFFF:
        return ('u16', v)
    return ('u32', v)


def _hcb_iterm(name: str, v: int) -> bytes:
    kind, val = _hcb_typed_int(v)
    return _hcb_term(name, kind, val)


def _hcb_uarr(name: str, vals: list[int]) -> bytes:
    return _hcb_term(name, 'arr', [_hcb_typed_int(v) for v in vals])


def _phase4_codec_host(video_omx: bool = False) -> bytes:
    """Build marble clean-room codec_host HCB node hosting codec_image_service (and optional codec_omx_idl_device)."""
    img_dev0 = _hcb_node('device0', [
        _hcb_term('policy', 'u8', 2),
        _hcb_term('priority', 'u8', 200),
        _hcb_term('moduleName', 'str', 'libcodec_image_driver.z.so'),
        _hcb_term('serviceName', 'str', 'codec_image_service'),
        _hcb_term('deviceMatchAttr', 'str', 'image_codec_capabilities'),
        _hcb_term('preload', 'u8', 0),
        _hcb_term('permission', 'u16', 436),
    ])
    devices = [_hcb_node('codec_image_device', [img_dev0])]
    if video_omx:
        omx_dev0 = _hcb_node('device0', [
            _hcb_term('policy', 'u8', 2),
            _hcb_term('priority', 'u8', 200),
            _hcb_term('moduleName', 'str', 'libcodec_driver.z.so'),
            _hcb_term('serviceName', 'str', 'codec_component_manager_service'),
            _hcb_term('deviceMatchAttr', 'str', 'media_codec_capabilities'),
            _hcb_term('preload', 'u8', 0),
            _hcb_term('permission', 'u16', 436),
        ])
        devices.append(_hcb_node('codec_omx_idl_device', [omx_dev0]))
    return _hcb_node('codec', [
        _hcb_term('hostName', 'str', 'codec_host'),
        _hcb_term('priority', 'u8', 50),
        _hcb_term('uid', 'str', 'codec_host'),
        _hcb_term('gid', 'arr', [('str', 'codec_host'), ('str', 'vcodec'), ('str', 'dev_dma_heap'), ('str', 'graphics')]),
        _hcb_term('caps', 'arr', [('str', 'DAC_OVERRIDE'), ('str', 'DAC_READ_SEARCH')]),
        *devices,
    ])


def _marble_video_cap_node(
    node_name: str,
    *,
    role: int,
    codec_type: int,
    comp_name: str,
    profiles: list[int],
    min_w: int,
    min_h: int,
    min_blk: int,
    min_bps: int,
    max_bps: int,
    measured_fps: list[int],
    bitrate_modes: list[int],
    max_fps: int,
) -> bytes:
    return _hcb_node(node_name, [
        _hcb_iterm('role', role),
        _hcb_iterm('type', codec_type),
        _hcb_term('name', 'str', comp_name),
        _hcb_uarr('supportProfiles', profiles),
        _hcb_iterm('maxInst', 16),
        _hcb_iterm('isSoftwareCodec', 0),
        _hcb_iterm('canSwapWidthHeight', 1),
        _hcb_iterm('isSupportPassthrough', 0),
        _hcb_uarr('processModeMask', [1, 2]),
        _hcb_uarr('capsMask', [1]),
        _hcb_iterm('minBitRate', 10000),
        _hcb_iterm('maxBitRate', 100000000),
        _hcb_iterm('minWidth', min_w),
        _hcb_iterm('minHeight', min_h),
        _hcb_iterm('maxWidth', 4096),
        _hcb_iterm('maxHeight', 4096),
        _hcb_iterm('widthAlignment', 2),
        _hcb_iterm('heightAlignment', 2),
        _hcb_iterm('minBlockCount', min_blk),
        _hcb_iterm('maxBlockCount', 65536),
        _hcb_iterm('minBlocksPerSecond', min_bps),
        _hcb_iterm('maxBlocksPerSecond', max_bps),
        _hcb_iterm('blockSizeWidth', 16),
        _hcb_iterm('blockSizeHeight', 16),
        _hcb_uarr('supportPixelFmts', [24, 25, 28]),
        _hcb_uarr('measuredFrameRate', measured_fps),
        _hcb_uarr('bitRateMode', bitrate_modes),
        _hcb_iterm('minFrameRate', 1),
        _hcb_iterm('maxFrameRate', max_fps),
    ])


def _phase4_video_codec_config() -> bytes:
    """Build top-level codec_config HCS tree for SM7475 Venus OpenMAX components."""
    avc_profiles = [1, 131072, 2, 131072, 8, 131072]
    hevc_profiles = [1, 65536]
    dec_fps = [176, 144, 600, 600, 1280, 720, 240, 240, 1920, 1080, 120, 120, 3840, 2160, 60, 60]
    enc_fps = [1280, 720, 120, 120, 1920, 1080, 60, 60]

    encoders = [
        _marble_video_cap_node(
            'HDF_video_hw_enc_avc_marble',
            role=1, codec_type=1, comp_name='OMX.marble.video.encoder.avc',
            profiles=avc_profiles, min_w=144, min_h=144,
            min_blk=81, min_bps=81, max_bps=972000,
            measured_fps=enc_fps, bitrate_modes=[1, 2], max_fps=120,
        ),
        _marble_video_cap_node(
            'HDF_video_hw_enc_hevc_marble',
            role=2, codec_type=1, comp_name='OMX.marble.video.encoder.hevc',
            profiles=hevc_profiles, min_w=144, min_h=144,
            min_blk=81, min_bps=81, max_bps=972000,
            measured_fps=enc_fps, bitrate_modes=[1, 2], max_fps=120,
        ),
    ]
    decoders = [
        _marble_video_cap_node(
            'HDF_video_hw_dec_avc_marble',
            role=1, codec_type=0, comp_name='OMX.marble.video.decoder.avc',
            profiles=avc_profiles, min_w=64, min_h=64,
            min_blk=16, min_bps=16, max_bps=1944000,
            measured_fps=dec_fps, bitrate_modes=[], max_fps=240,
        ),
        _marble_video_cap_node(
            'HDF_video_hw_dec_hevc_marble',
            role=2, codec_type=0, comp_name='OMX.marble.video.decoder.hevc',
            profiles=hevc_profiles, min_w=64, min_h=64,
            min_blk=16, min_bps=16, max_bps=1944000,
            measured_fps=dec_fps, bitrate_modes=[], max_fps=240,
        ),
    ]
    return _hcb_node('codec_config', [
        _hcb_term('match_attr', 'str', 'media_codec_capabilities'),
        _hcb_term('use_openmax', 'u8', 0),
        _hcb_node('VideoHwEncoders', encoders),
        _hcb_node('VideoHwDecoders', decoders),
        _hcb_node('VideoSwEncoders', []),
        _hcb_node('VideoSwDecoders', []),
        _hcb_node('AudioHwEncoders', []),
        _hcb_node('AudioHwDecoders', []),
        _hcb_node('AudioSwEncoders', []),
        _hcb_node('AudioSwDecoders', []),
    ])


def _phase4_image_codec_config(*, heif_hw: bool = False) -> bytes:
    """Build top-level image_codec_config HCS tree for marble codec_image_service."""
    jpeg_dec = _hcb_node('HDF_Image_hw_dec_jpeg_marble', [
        _hcb_term('role', 'u8', 0),
        _hcb_term('type', 'u8', 0),
        _hcb_term('name', 'str', 'HDF.marble.decoder.jpeg'),
        _hcb_term('maxInst', 'u8', 4),
        _hcb_term('isSoftwareCodec', 'u8', 0),
        _hcb_term('minWidth', 'u8', 64),
        _hcb_term('minHeight', 'u8', 64),
        _hcb_term('maxWidth', 'u16', 8192),
        _hcb_term('maxHeight', 'u16', 8192),
        _hcb_term('widthAlignment', 'u8', 2),
        _hcb_term('heightAlignment', 'u8', 2),
        _hcb_term('maxSample', 'u8', 8),
        _hcb_term('supportPixelFmts', 'arr', [('u8', 12), ('u8', 25)]),
    ])
    decoders = [jpeg_dec]
    if heif_hw:
        decoders.append(_hcb_node('HDF_Image_hw_dec_heif_marble', [
            _hcb_term('role', 'u8', 1),
            _hcb_term('type', 'u8', 0),
            _hcb_term('name', 'str', 'HDF.marble.decoder.heif'),
            _hcb_term('maxInst', 'u8', 4),
            _hcb_term('isSoftwareCodec', 'u8', 0),
            _hcb_term('minWidth', 'u8', 128),
            _hcb_term('minHeight', 'u8', 128),
            _hcb_term('maxWidth', 'u16', 8192),
            _hcb_term('maxHeight', 'u16', 8192),
            _hcb_term('widthAlignment', 'u8', 2),
            _hcb_term('heightAlignment', 'u8', 2),
            _hcb_term('maxSample', 'u8', 16),
            _hcb_term('supportPixelFmts', 'arr', [
                ('u8', 3), ('u8', 12), ('u8', 20), ('u8', 24), ('u8', 25),
            ]),
        ]))
    return _hcb_node('image_codec_config', [
        _hcb_term('match_attr', 'str', 'image_codec_capabilities'),
        _hcb_node('ImageHwEncoders', []),
        _hcb_node('ImageHwDecoders', decoders),
    ])


def _phase4_camera_host() -> bytes:
    """Build marble clean-room camera_host HCB node (host index 9) hosting camera_service via VDI."""
    cam_dev0 = _hcb_node('device0', [
        _hcb_term('policy', 'u8', 2),
        _hcb_term('priority', 'u8', 100),
        _hcb_term('preload', 'u8', 0),
        _hcb_term('moduleName', 'str', 'libcamera_host_service_1.0.z.so'),
        _hcb_term('serviceName', 'str', 'camera_service'),
        _hcb_term('permission', 'u16', 436),
        _hcb_term('deviceMatchAttr', 'str', ''),
    ])
    return _hcb_node('camera', [
        _hcb_term('hostName', 'str', 'camera_host'),
        _hcb_term('priority', 'u8', 50),
        _hcb_term('uid', 'str', 'camera_host'),
        _hcb_term('gid', 'arr', [
            ('str', 'camera_host'),
            ('str', 'dev_camera'),
            ('str', 'media'),
            ('str', 'graphics'),
            ('str', 'dev_dma_heap'),
            ('str', 'system'),
        ]),
        _hcb_term('caps', 'arr', [('str', 'DAC_OVERRIDE'), ('str', 'DAC_READ_SEARCH')]),
        _hcb_node('camera_device', [cam_dev0]),
    ])


def build_marble_camera_host_config_hcb() -> bytes:
    """Build OpenHarmony HCB v0.7 /vendor/etc/hdfconfig/camera_host_config.hcb pointing at our bind-mounted VDI."""
    root_node = _hcb_node('root', [
        _hcb_term('module', 'str', 'marble'),
        _hcb_term('vdiLibList', 'arr', [('str', 'libcamera_external_usb_camera.so')]),
    ])
    header = struct.pack('<IIIII', 0xA00AA00A, 0, 7, 0, len(root_node))
    return header + root_node


def _phase4_nfc_host() -> bytes:
    """Build marble clean-room nfc_host HCB node (host index 10) hosting nfc_interface_service v1.1."""
    nfc_dev0 = _hcb_node('device0', [
        _hcb_term('policy', 'u8', 2),
        _hcb_term('priority', 'u8', 100),
        _hcb_term('preload', 'u8', 0),
        _hcb_term('moduleName', 'str', 'libnfc_hdi_driver.z.so'),
        _hcb_term('serviceName', 'str', 'nfc_interface_service'),
        _hcb_term('permission', 'u16', 436),
        _hcb_term('deviceMatchAttr', 'str', ''),
    ])
    return _hcb_node('nfc', [
        _hcb_term('hostName', 'str', 'nfc_host'),
        _hcb_term('priority', 'u8', 50),
        _hcb_term('uid', 'str', 'nfc_host'),
        _hcb_term('gid', 'arr', [('str', 'nfc_host'), ('str', 'nfc'), ('str', 'system')]),
        _hcb_term('caps', 'arr', [('str', 'DAC_OVERRIDE'), ('str', 'DAC_READ_SEARCH')]),
        _hcb_node('nfc_device', [nfc_dev0]),
    ])


def _phase4_thermal_host() -> bytes:
    """Build marble clean-room thermal_host HCB node (host index 11) hosting thermal_interface_service v1.1."""
    thermal_dev0 = _hcb_node('device0', [
        _hcb_term('policy', 'u8', 2),
        _hcb_term('priority', 'u8', 100),
        _hcb_term('preload', 'u8', 0),
        _hcb_term('moduleName', 'str', 'libthermal_driver.z.so'),
        _hcb_term('serviceName', 'str', 'thermal_interface_service'),
        _hcb_term('permission', 'u16', 436),
        _hcb_term('deviceMatchAttr', 'str', ''),
    ])
    return _hcb_node('thermal', [
        _hcb_term('hostName', 'str', 'thermal_host'),
        _hcb_term('priority', 'u8', 50),
        _hcb_term('uid', 'str', 'power_host'),
        _hcb_term('gid', 'arr', [('str', 'power_host'), ('str', 'powerctrl'), ('str', 'system')]),
        _hcb_term('caps', 'arr', [('str', 'DAC_READ_SEARCH')]),
        _hcb_node('thermal_device', [thermal_dev0]),
    ])


def _phase4_power_host() -> bytes:
    """Build marble clean-room power_host HCB node (host index 12) hosting power_interface_service v1.3."""
    power_dev0 = _hcb_node('device0', [
        _hcb_term('policy', 'u8', 2),
        _hcb_term('priority', 'u8', 100),
        _hcb_term('preload', 'u8', 0),
        _hcb_term('moduleName', 'str', 'libpower_driver.z.so'),
        _hcb_term('serviceName', 'str', 'power_interface_service'),
        _hcb_term('permission', 'u16', 436),
        _hcb_term('deviceMatchAttr', 'str', ''),
    ])
    return _hcb_node('power', [
        _hcb_term('hostName', 'str', 'power_host'),
        _hcb_term('priority', 'u8', 50),
        _hcb_term('uid', 'str', 'power_host'),
        _hcb_term('gid', 'arr', [('str', 'power_host'), ('str', 'powerctrl'), ('str', 'system')]),
        _hcb_term('caps', 'arr', [('str', 'DAC_OVERRIDE'), ('str', 'DAC_READ_SEARCH')]),
        _hcb_node('power_device', [power_dev0]),
    ])


def _phase4_usb_host() -> bytes:
    """Build marble clean-room usb_host HCB node (host index 13) hosting usb_port_interface_service v2.0."""
    usb_port_dev0 = _hcb_node('device0', [
        _hcb_term('policy', 'u8', 2),
        _hcb_term('priority', 'u8', 100),
        _hcb_term('preload', 'u8', 0),
        _hcb_term('moduleName', 'str', 'libusb_port_driver.z.so'),
        _hcb_term('serviceName', 'str', 'usb_port_interface_service'),
        _hcb_term('permission', 'u16', 436),
        _hcb_term('deviceMatchAttr', 'str', 'usb_device_interface_driver'),
    ])
    return _hcb_node('usb', [
        _hcb_term('hostName', 'str', 'usb_host'),
        _hcb_term('priority', 'u8', 50),
        _hcb_term('uid', 'str', 'usb_host'),
        _hcb_term('gid', 'arr', [('str', 'usb_host'), ('str', 'system')]),
        _hcb_term('caps', 'arr', [('str', 'DAC_OVERRIDE'), ('str', 'DAC_READ_SEARCH')]),
        _hcb_node('usb_port_device', [usb_port_dev0]),
    ])


def _phase4_memorytracker_host() -> bytes:
    """Build marble clean-room memorytracker_host HCB node (host index 14) hosting memory_tracker_interface_service v1.0."""
    mt_dev0 = _hcb_node('device0', [
        _hcb_term('policy', 'u8', 2),
        _hcb_term('priority', 'u8', 100),
        _hcb_term('preload', 'u8', 0),
        _hcb_term('moduleName', 'str', 'libmemorytracker_driver.z.so'),
        _hcb_term('serviceName', 'str', 'memory_tracker_interface_service'),
        _hcb_term('permission', 'u16', 436),
        _hcb_term('deviceMatchAttr', 'str', ''),
    ])
    return _hcb_node('memorytracker', [
        _hcb_term('hostName', 'str', 'memorytracker_host'),
        _hcb_term('priority', 'u8', 50),
        _hcb_term('uid', 'str', 'memmgr'),
        _hcb_term('gid', 'arr', [('str', 'memmgr'), ('str', 'system')]),
        _hcb_term('caps', 'arr', [('str', 'DAC_READ_SEARCH'), ('str', 'SYS_PTRACE')]),
        _hcb_node('memorytracker_device', [mt_dev0]),
    ])


def _phase4_vibrator_host() -> bytes:
    """Build marble clean-room vibrator_host HCB node (host index 15) hosting vibrator_interface_service v2.0."""
    vib_dev0 = _hcb_node('device0', [
        _hcb_term('policy', 'u8', 2),
        _hcb_term('priority', 'u8', 100),
        _hcb_term('preload', 'u8', 0),
        _hcb_term('moduleName', 'str', 'libvibrator_driver.z.so'),
        _hcb_term('serviceName', 'str', 'vibrator_interface_service'),
        _hcb_term('permission', 'u16', 436),
        _hcb_term('deviceMatchAttr', 'str', ''),
    ])
    return _hcb_node('vibrator', [
        _hcb_term('hostName', 'str', 'vibrator_host'),
        _hcb_term('priority', 'u8', 50),
        _hcb_term('uid', 'str', 'vibrator_host'),
        _hcb_term('gid', 'arr', [('str', 'vibrator_host'), ('str', 'dev_vibrator'), ('str', 'input'), ('str', 'system')]),
        _hcb_term('caps', 'arr', [('str', 'DAC_OVERRIDE'), ('str', 'DAC_READ_SEARCH'), ('str', 'SYS_NICE')]),
        _hcb_node('vibrator_device', [vib_dev0]),
    ])


HOSTAPD_DONOR_PINS = {
    WIFI_CHIP_DONOR_HCB: WIFI_CHIP_DONOR_PINS[WIFI_CHIP_DONOR_HCB],
    WPA_DONOR_INIT: WPA_DONOR_PINS[WPA_DONOR_INIT],
    'vendor/lib64/libhostapd_hdi_c_device.z.so': '1acb8c3b559783d2ee1e8bc723b9eb8dca2099135291ee233153369159f52132',
    'vendor/lib64/libhostapd_interface_service_1.0.z.so': '35283fd7d826580151c9c3c59fa9b58f914eb496b83aac424f37084a7d0b66c6',
    'vendor/lib64/libhostapd_stub_1.0.z.so': '343e257309f3a060c81accd5656776bdf5922c009305ecc19f75dbe7667e29c0',
    'vendor/lib64/libhostapd_hdi_client.z.so': 'e8c49249409083c17b9b06b65616b070b5d262387cbc795ad2e1b6ba7151e054',
}


def _phase4_hostapd_host(donor_fs: Path, *, preload: int = 2) -> bytes:
    """Emit donor hostapd_host HCB node at host index 16 (hostapd_interface_service)."""
    inputs = _pinned_hdf_inputs(donor_fs, HOSTAPD_DONOR_PINS, 'Hostapd HDI')

    def _build(pl: int) -> bytes:
        dev = _hcb_node('wpa_device', [_hcb_node('device0', [
            _hcb_term('policy', 'u8', 2),
            _hcb_term('preload', 'u8', pl),
            _hcb_term('priority', 'u8', 100),
            _hcb_term('moduleName', 'str', 'libhostapd_hdi_c_device.z.so'),
            _hcb_term('serviceName', 'str', 'hostapd_interface_service'),
            _hcb_term('permission', 'u16', 436),
            _hcb_term('deviceMatchAttr', 'str', ''),
        ])])
        return _hcb_node('hostapd', [
            _hcb_term('hostName', 'str', 'hostapd_host'),
            _hcb_term('priority', 'u8', 50),
            _hcb_term('caps', 'arr', [
                ('str', 'DAC_OVERRIDE'),
                ('str', 'DAC_READ_SEARCH'),
                ('str', 'NET_ADMIN'),
                ('str', 'NET_RAW'),
            ]),
            _hcb_term('initconfig', 'arr', [('str', '"secon" : "u:r:wifi_host:s0"')]),
            _hcb_term('uid', 'str', 'wifi'),
            _hcb_term('gid', 'arr', [('str', 'wifi'), ('str', 'wifi_group'), ('str', 'wifi_host')]),
            dev,
        ])

    if inputs[WIFI_CHIP_DONOR_HCB].count(_build(2)) != 1:
        raise ValueError('Expected exactly one byte-identical ADY hostapd host node')
    return _build(preload)


def _phase4_pin_auth_host() -> bytes:
    """Build marble pure-userspace software pin_auth_host HCB node (host index 17) hosting pin_auth_interface_service v3.0."""
    pin_dev0 = _hcb_node('device0', [
        _hcb_term('policy', 'u8', 2),
        _hcb_term('priority', 'u8', 200),
        _hcb_term('preload', 'u8', 0),
        _hcb_term('moduleName', 'str', 'libpin_auth_driver.z.so'),
        _hcb_term('serviceName', 'str', 'pin_auth_interface_service'),
        _hcb_term('permission', 'u16', 436),
        _hcb_term('deviceMatchAttr', 'str', ''),
    ])
    return _hcb_node('pin_auth', [
        _hcb_term('hostName', 'str', 'pin_auth_host'),
        _hcb_term('priority', 'u8', 50),
        _hcb_term('uid', 'str', 'useriam'),
        _hcb_term('gid', 'arr', [('str', 'useriam'), ('str', 'uhdf_driver')]),
        _hcb_term('caps', 'arr', [('str', 'DAC_OVERRIDE'), ('str', 'DAC_READ_SEARCH')]),
        _hcb_node('pin_auth_device', [pin_dev0]),
    ])


def _phase4_motion_host() -> bytes:
    """Build marble clean-room motion_host HCB node (host index 18) hosting motion_interface_service v1.1."""
    motion_dev0 = _hcb_node('device0', [
        _hcb_term('policy', 'u8', 2),
        _hcb_term('priority', 'u8', 100),
        _hcb_term('preload', 'u8', 0),
        _hcb_term('moduleName', 'str', 'libmotion_driver.z.so'),
        _hcb_term('serviceName', 'str', 'motion_interface_service'),
        _hcb_term('permission', 'u16', 436),
        _hcb_term('deviceMatchAttr', 'str', ''),
    ])
    return _hcb_node('motion', [
        _hcb_term('hostName', 'str', 'motion_host'),
        _hcb_term('priority', 'u8', 50),
        _hcb_term('uid', 'str', 'motion_host'),
        _hcb_term('gid', 'arr', [
            ('str', 'motion_host'),
            ('str', 'composer_host'),
            ('str', 'dev_lcd_backlight'),
            ('str', 'sensor_host'),
            ('str', 'system'),
        ]),
        _hcb_term('caps', 'arr', [('str', 'DAC_OVERRIDE'), ('str', 'DAC_READ_SEARCH')]),
        _hcb_node('motion_device', [motion_dev0]),
    ])


def build_phase4_hdf_hcb(*, wifi_chip_donor: Path | None = None,
                         wpa_on_demand_donor: Path | None = None,
                         a2dp_donor: Path | None = None,
                         ril_donor: Path | None = None,
                         location_host: bool = False,
                         displayengine: bool = False,
                         codec_image_host: bool = False,
                         codec_video_omx: bool = False,
                         codec_heif_hw: bool = False,
                         camera_host: bool = False,
                         nfc_host: bool = False,
                         thermal_host: bool = False,
                         power_host: bool = False,
                         usb_host: bool = False,
                         memorytracker_host: bool = False,
                         vibrator_host: bool = False,
                         hostapd_donor: Path | None = None,
                         pin_auth_host: bool = False,
                         motion_host: bool = False) -> bytes:
    """Build marble Phase 4 OpenHarmony HCB v0.7 with 5..15 hosts:
      0: allocator_host     (liballocator_driver_1.0.z.so -> allocator_service)
      1: composer_host      (libdisplay_composer_driver_1.0.z.so [+ opt-in displayengine_device])
      2: hmos_userauth_host (libhmos_user_auth_driver.so -> user_auth_interface_service)
      3: wifi_host          (WLAN; opt-in donor chip HDI in the same host)
      4: wpa_host           (libwpa_hdi_c_device.z.so -> wpa_interface_service)
      5: a2dp_host          (opt-in donor audio_bluetooth_hdi_service + bluetooth_audio_session_service)
      6: riladapter_host    (opt-in gated ril_service + cellular_radio1, preload=2)
      7: location_host      (opt-in clean-room gnss_interface_service, preload=2)
      8: codec_host         (opt-in codec_image_service + optional codec_component_manager_service)
      9: camera_host        (opt-in camera_service -> libcamera_host_service_1.0.z.so -> marble VDI)
     10: nfc_host           (opt-in nfc_interface_service -> libnfc_hdi_driver.z.so -> marble SN110T HDI)
    """
    allocator_dev0 = _hcb_node('device0', [
        _hcb_term('policy', 'u8', 2),
        _hcb_term('priority', 'u8', 100),
        _hcb_term('moduleName', 'str', 'liballocator_driver_1.0.z.so'),
        _hcb_term('serviceName', 'str', 'allocator_service'),
        _hcb_term('preload', 'u8', 0),
        _hcb_term('permission', 'u16', 436),
        _hcb_term('deviceMatchAttr', 'str', ''),
    ])
    allocator_host = _hcb_node('allocator', [
        _hcb_term('hostName', 'str', 'allocator_host'),
        _hcb_term('priority', 'u8', 50),
        _hcb_term('gid', 'arr', [('str', 'allocator_host'), ('str', 'graphics'), ('str', 'dev_dma_heap')]),
        _hcb_node('allocator_device', [allocator_dev0]),
        _hcb_term('uid', 'str', ''),
        _hcb_term('caps', 'arr', [('str', '')]),
    ])

    composer_dev0 = _hcb_node('device0', [
        _hcb_term('policy', 'u8', 2),
        _hcb_term('priority', 'u8', 100),
        _hcb_term('moduleName', 'str', 'libdisplay_composer_driver_1.0.z.so'),
        _hcb_term('serviceName', 'str', 'display_composer_service'),
        _hcb_term('preload', 'u8', 0),
        _hcb_term('permission', 'u16', 436),
        _hcb_term('deviceMatchAttr', 'str', ''),
    ])
    composer_host = _hcb_node('display_composer', [
        _hcb_term('hostName', 'str', 'composer_host'),
        _hcb_term('priority', 'u8', 50),
        _hcb_term('uid', 'arr', [('str', 'composer_host')]),
        _hcb_term('gid', 'arr', [
            ('str', 'composer_host'),
            ('str', 'graphics'),
            ('str', 'dev_dma_heap'),
            ('str', 'dev_memory'),
            ('str', 'dev_lcd_backlight'),
            ('str', 'oeminfo_nvme_server'),
        ]),
        _hcb_node('composer_device', [composer_dev0]),
        *([_phase4_displayengine_device()] if displayengine else []),
        _hcb_term('caps', 'arr', [('str', '')]),
    ])

    userauth_dev0 = _hcb_node('device0', [
        _hcb_term('policy', 'u8', 2),
        _hcb_term('priority', 'u8', 200),
        _hcb_term('moduleName', 'str', 'libhmos_user_auth_driver.so'),
        _hcb_term('serviceName', 'str', 'user_auth_interface_service'),
        _hcb_term('preload', 'u8', 0),
        _hcb_term('permission', 'u16', 436),
        _hcb_term('deviceMatchAttr', 'str', ''),
    ])
    userauth_host = _hcb_node('user_auth', [
        _hcb_term('hostName', 'str', 'hmos_userauth_host'),
        _hcb_term('priority', 'u8', 50),
        _hcb_term('uid', 'str', 'useriam'),
        _hcb_term('gid', 'arr', [('str', 'useriam'), ('str', 'uhdf_driver')]),
        _hcb_node('user_auth_device', [userauth_dev0]),
        _hcb_term('caps', 'arr', [('str', '')]),
    ])

    wifi_dev0 = _hcb_node('device0', [
        _hcb_term('policy', 'u8', 2),
        _hcb_term('priority', 'u8', 100),
        _hcb_term('moduleName', 'str', 'libwifi_hdi_c_device.z.so'),
        _hcb_term('serviceName', 'str', 'wlan_interface_service'),
        _hcb_term('preload', 'u8', 0),
        _hcb_term('permission', 'u16', 436),
        _hcb_term('deviceMatchAttr', 'str', ''),
    ])
    wifi_host = _hcb_node('wifi', [
        _hcb_term('hostName', 'str', 'wifi_host'),
        _hcb_term('priority', 'u8', 50),
        _hcb_term('caps', 'arr', [
            ('str', 'DAC_OVERRIDE'),
            ('str', 'DAC_READ_SEARCH'),
            ('str', 'NET_ADMIN'),
            ('str', 'NET_RAW'),
        ]),
        _hcb_term('gid', 'arr', [('str', 'wifi_host'), ('str', 'wifi_group')]),
        _hcb_node('wifi_c_device', [wifi_dev0]),
        *([_phase4_wifi_chip_device(wifi_chip_donor)] if wifi_chip_donor is not None else []),
        _hcb_term('uid', 'str', ''),
    ])

    wpa_device = _phase4_wpa_device(0)
    if wpa_on_demand_donor is not None:
        inputs = _pinned_hdf_inputs(wpa_on_demand_donor, WPA_DONOR_PINS, 'WPA HDI')
        wpa_device = _phase4_wpa_device(2)
        if inputs[WIFI_CHIP_DONOR_HCB].count(wpa_device) != 1:
            raise ValueError('Expected exactly one byte-identical ADY WPA device node')
    wpa_host = _hcb_node('wpa', [
        _hcb_term('hostName', 'str', 'wpa_host'),
        _hcb_term('priority', 'u8', 50),
        _hcb_term('caps', 'arr', [
            ('str', 'DAC_OVERRIDE'),
            ('str', 'DAC_READ_SEARCH'),
            ('str', 'NET_ADMIN'),
            ('str', 'NET_RAW'),
        ]),
        _hcb_term('uid', 'str', 'wifi'),
        _hcb_term('gid', 'arr', [('str', 'wifi'), ('str', 'wifi_group'), ('str', 'wifi_host')]),
        wpa_device,
    ])

    if location_host and (a2dp_donor is None or ril_donor is None):
        raise ValueError('location_host=True requires a2dp_donor and ril_donor so location_host is host index 7')
    if codec_image_host and not location_host:
        raise ValueError('codec_image_host=True requires location_host=True so codec_host is host index 8')
    if codec_video_omx and not codec_image_host:
        raise ValueError('codec_video_omx=True requires codec_image_host=True')
    if codec_heif_hw and not codec_image_host:
        raise ValueError('codec_heif_hw=True requires codec_image_host=True')
    if camera_host and not codec_image_host:
        raise ValueError('camera_host=True requires codec_image_host=True so camera_host is host index 9')
    if nfc_host and not camera_host:
        raise ValueError('nfc_host=True requires camera_host=True so nfc_host is host index 10')
    if thermal_host and not nfc_host:
        raise ValueError('thermal_host=True requires nfc_host=True so thermal_host is host index 11')
    if power_host and not thermal_host:
        raise ValueError('power_host=True requires thermal_host=True so power_host is host index 12')
    if usb_host and not power_host:
        raise ValueError('usb_host=True requires power_host=True so usb_host is host index 13')
    if memorytracker_host and not usb_host:
        raise ValueError('memorytracker_host=True requires usb_host=True so memorytracker_host is host index 14')
    if vibrator_host and not memorytracker_host:
        raise ValueError('vibrator_host=True requires memorytracker_host=True so vibrator_host is host index 15')
    if hostapd_donor is not None and not vibrator_host:
        raise ValueError('hostapd_donor requires vibrator_host=True so hostapd_host is host index 16')
    if pin_auth_host and hostapd_donor is None:
        raise ValueError('pin_auth_host=True requires hostapd_donor so pin_auth_host is host index 17')
    if motion_host and not pin_auth_host:
        raise ValueError('motion_host=True requires pin_auth_host=True so motion_host is host index 18')
    device_info = _hcb_node('device_info', [
        _hcb_term('match_attr', 'str', 'hdf_manager'),
        allocator_host,
        composer_host,
        userauth_host,
        wifi_host,
        wpa_host,
        *([_phase4_a2dp_host(a2dp_donor)] if a2dp_donor is not None else []),
        *([_phase4_ril_host(ril_donor)] if ril_donor is not None else []),
        *([_phase4_location_host()] if location_host else []),
        *([_phase4_codec_host(video_omx=codec_video_omx)] if codec_image_host else []),
        *([_phase4_camera_host()] if camera_host else []),
        *([_phase4_nfc_host()] if nfc_host else []),
        *([_phase4_thermal_host()] if thermal_host else []),
        *([_phase4_power_host()] if power_host else []),
        *([_phase4_usb_host()] if usb_host else []),
        *([_phase4_memorytracker_host()] if memorytracker_host else []),
        *([_phase4_vibrator_host()] if vibrator_host else []),
        *([_phase4_hostapd_host(hostapd_donor)] if hostapd_donor is not None else []),
        *([_phase4_pin_auth_host()] if pin_auth_host else []),
        *([_phase4_motion_host()] if motion_host else []),
    ])
    root_node = _hcb_node('root', [
        device_info,
        *([_phase4_image_codec_config(heif_hw=codec_heif_hw)] if codec_image_host else []),
        *([_phase4_video_codec_config()] if codec_video_omx else []),
    ])
    header = struct.pack('<IIIII', 0xA00AA00A, 0, 7, 0, len(root_node))
    return header + root_node


def patch_appdata_sandbox_with_compat(raw_json: bytes) -> bytes:
    """Inject a read-only/bind /compat mount into ADY-AL10 appdata-sandbox JSON."""
    doc = json.loads(raw_json.decode('utf-8'))
    compat_mount = {
        'src-path': '/compat',
        'sandbox-path': '/compat',
        'sandbox-flags': ['bind', 'rec'],
        'check-action-status': 'true',
    }
    common = doc.get('common')
    if not isinstance(common, list) or not common:
        raise ValueError('Unexpected appdata-sandbox JSON structure (missing common list)')
    injected = False
    for section in common:
        for base in section.get('app-base', []):
            mps = base.get('mount-paths')
            if isinstance(mps, list):
                if not any(isinstance(m, dict) and m.get('src-path') == '/compat' for m in mps):
                    mps.insert(0, compat_mount)
                injected = True
                break
        if injected:
            break
    if not injected:
        raise ValueError('No mount-paths found in appdata-sandbox common[].app-base[]')
    return (json.dumps(doc, indent=2) + '\n').encode('utf-8')


def build_phase4_images(
    stock_boot: Path,
    stock_vendor_boot: Path,
    artifacts_dir: Path,
    donor_fs_dir: Path,
    recipes_dir: Path,
    busybox: Path,
    phase3_compat_dir: Path,
    phase4_build_dir: Path,
    firmware_dir: Path,
    out_dir: Path,
    *,
    cc: str = 'aarch64-linux-gnu-gcc',
    diagnostics_dir: Path | None = None,
    rs_screen_order: bool = False,
    rs_native_commit: bool = False,
    rs_yuv_native_handle: bool = False,
    wifi_chip_hdi: bool = False,
    wpa_ondemand: bool = False,
) -> dict:
    selected_recipes = phase4_donor_recipes(
        rs_screen_order, rs_native_commit=rs_native_commit, rs_yuv_native_handle=rs_yuv_native_handle,
    )
    if any((out_dir / name).exists() or (out_dir / name).is_symlink() for name in (
        'work', 'boot_phase4.img', 'vendor_boot_phase4.img', 'phase4-build-report.json',
    )):
        raise FileExistsError(f'Phase 4 output already exists; choose a fresh --out: {out_dir}')
    patch_script = generate_phase3_donor_patch_script(recipes_dir, selected_recipes)
    patch_script = patch_script.replace('MARBLE_PHASE3_DONOR_PATCHES_ALL_OK', 'MARBLE_PHASE4_DONOR_PATCHES_ALL_OK')
    turnip_blob = None
    if rs_yuv_native_handle:
        turnip_blob = (phase4_build_dir / TURNIP_PAYLOAD).read_bytes()
        verify_h20_yuv_inputs(patch_script, turnip_blob)
    # Freeze the measured bytes before building; never fall back to split historical outputs.
    diagnostics_payload = load_diagnostics(phase4_build_dir if diagnostics_dir is None else diagnostics_dir)
    work = out_dir / 'work'
    work.mkdir(parents=True)

    kernel_image = artifacts_dir / 'Image'
    ipc_ko = artifacts_dir / 'modules/hmos_ipc.ko'
    dmabuf_ko = artifacts_dir / 'modules/hmos_dmabuf_sync.ko'
    vb_mods_tar = artifacts_dir / 'vendor_boot_modules.tar.gz'
    vd_mods_tar = artifacts_dir / 'vendor_dlkm_modules.tar.gz'

    marble_rt = ROOT / 'devices/marble/runtime'
    sheng_prof = ROOT / 'devices/sheng/runtime/profiles'

    init_phase4_cfg = marble_rt / 'init/hmos-init-phase4.cfg'
    init_desktop_cfg = marble_rt / 'init/init-desktop-marble.cfg'
    init_platform_cfg = marble_rt / 'init/init-platform-marble.cfg'
    init_hardware_cfg = marble_rt / 'init/init-hardware-marble.cfg'
    init_wifi_cfg = marble_rt / 'init/init-wifi-marble.cfg'
    display_mgr_xml = marble_rt / 'init/display_manager_config.xml'
    turnip_icd = marble_rt / 'graphics/hmos-turnip.json'
    userdata_sh = marble_rt / 'boot/init-userdata-ext4.sh'
    stage_sh = marble_rt / 'boot/stage-phase4-marble.sh'
    start_sh = marble_rt / 'boot/start-phase4-marble.sh'
    start_wifi_sh = marble_rt / 'wifi/start-wifi-marble.sh'

    # hmos_root() installs this exact seed after all stage scripts. Restore the
    # donor's native samgr role here; keep its UID, domain and startup jobs.
    init_cfg_bytes, samgr_role = phase4_init_seed(init_phase4_cfg, donor_fs_dir)
    for cfg_path in (init_desktop_cfg, init_platform_cfg, init_hardware_cfg, init_wifi_cfg, turnip_icd):
        json.loads(cfg_path.read_text(encoding='utf-8'))

    patch_script_path = work / 'apply-donor-patches-phase4.sh'
    patch_script_path.write_text(patch_script, encoding='utf-8', newline='\n')
    subprocess.run(['bash', '-n', str(patch_script_path)], check=True)
    subprocess.run(['bash', '-n', str(stage_sh)], check=True)
    subprocess.run(['bash', '-n', str(start_sh)], check=True)
    subprocess.run(['bash', '-n', str(start_wifi_sh)], check=True)
    wifi_supplicant = phase4_wifi_supplicant_payload(ROOT)
    subprocess.run(['bash', '-n', str(ROOT / 'compat/wifi/marble-wifi-supplicant-stage.sh')], check=True)

    # Match the HDF dynamic-device declaration with the native init lifecycle.
    phase4_hcb = build_phase4_hdf_hcb(
        wifi_chip_donor=donor_fs_dir if wifi_chip_hdi else None,
        wpa_on_demand_donor=donor_fs_dir if wpa_ondemand else None,
    )
    init_wifi_bytes = init_wifi_cfg.read_bytes()
    if wpa_ondemand:
        init_wifi_bytes = phase4_wpa_ondemand_init(init_wifi_bytes, donor_fs_dir)

    # Patch ADY-AL10 appdata-sandbox JSONs with /compat mount.
    sb_main = patch_appdata_sandbox_with_compat(
        (donor_fs_dir / 'system/system/etc/sandbox/appdata-sandbox.json').read_bytes()
    )
    sb_iso = patch_appdata_sandbox_with_compat(
        (donor_fs_dir / 'system/system/etc/sandbox/appdata-sandbox-isolated.json').read_bytes()
    )

    # Compile updated marble_init_entry + hmos-probe (with stage/start-phase4-marble.sh support).
    bins = compile_phase3_binaries(work / 'bin', cc=cc)

    # --- Repack boot_phase4.img ---
    boot_unpack = work / 'boot-unpack'
    boot_args = unpack_with_args(stock_boot, boot_unpack)
    stock_ramdisk = boot_unpack / 'ramdisk'
    boot_cpio_in = work / 'boot-ramdisk.cpio'
    boot_comp = decompress_ramdisk(stock_ramdisk, boot_cpio_in)
    boot_entries = parse_cpio_newc(boot_cpio_in.read_bytes())
    boot_inode = [max((f[0] for f, _ in boot_entries.values()), default=10) + 1]
    original_init_blob = boot_entries.get('init.real', boot_entries.get('init', (None, None)))[1]
    put_file(boot_entries, 'init', bins['init_entry'].read_bytes(), 0o755, boot_inode)

    boot_cpio_out = work / 'boot-ramdisk-phase4.cpio'
    boot_cpio_out.write_bytes(pack_cpio_newc(boot_entries))
    boot_ramdisk_out = work / 'boot-ramdisk-phase4.img'
    compress_ramdisk(boot_cpio_out, boot_ramdisk_out, boot_comp)

    boot_repack_args = replace_arg_value(boot_args, '--kernel', str(kernel_image))
    boot_repack_args = replace_arg_value(boot_repack_args, '--ramdisk', str(boot_ramdisk_out))
    boot_out = out_dir / 'boot_phase4.img'
    subprocess.run(['mkbootimg', *boot_repack_args, '--output', str(boot_out)], check=True)
    boot_payload = attach_boot_v4_signature(boot_out.read_bytes(), boot_unpack / 'boot_signature')
    boot_final, boot_avb_updated = preserve_and_update_avb_footer(boot_payload, stock_boot.read_bytes())
    boot_out.write_bytes(boot_final)
    if boot_out.stat().st_size > BOOT_LIMIT_BYTES:
        raise ValueError(f'boot_phase4.img exceeds 192 MiB limit: {boot_out.stat().st_size}')

    # --- Repack vendor_boot_phase4.img ---
    vb_unpack = work / 'vendor_boot-unpack'
    vb_args = unpack_with_args(stock_vendor_boot, vb_unpack)
    vb_ramdisks = sorted(p for p in vb_unpack.glob('vendor_ramdisk*') if p.is_file())
    target_rd = target_rd_comp = target_entries = None
    for rd in vb_ramdisks:
        cpio_path = work / (rd.name + '.cpio')
        try:
            comp = decompress_ramdisk(rd, cpio_path)
        except subprocess.CalledProcessError:
            if len(vb_ramdisks) == 1 and extract_full_single_vendor_ramdisk(stock_vendor_boot, rd):
                comp = decompress_ramdisk(rd, cpio_path)
            else:
                raise
        parsed = parse_cpio_newc(cpio_path.read_bytes())
        if any(name.endswith('modules.load') for name in parsed):
            target_rd, target_rd_comp, target_entries = rd, comp, parsed
            break
    if target_rd is None:
        target_rd = vb_ramdisks[0]
        cpio_path = work / (target_rd.name + '.cpio')
        target_rd_comp = decompress_ramdisk(target_rd, cpio_path)
        target_entries = parse_cpio_newc(cpio_path.read_bytes())

    vb_inode = [max((f[0] for f, _ in target_entries.values()), default=100) + 1]
    vb_only_mods = extract_tar_modules(vb_mods_tar)
    built_mods = dict(vb_only_mods)
    for k, v in extract_tar_modules(vd_mods_tar).items():
        built_mods.setdefault(k, v)

    mod_prefix = 'lib/modules/'
    for name in list(target_entries):
        if name.startswith(mod_prefix) and name.endswith('.ko'):
            base = Path(name).name
            if base in built_mods:
                put_file(target_entries, name, built_mods[base], 0o644, vb_inode)
            else:
                del target_entries[name]
    for mod, blob in sorted(vb_only_mods.items()):
        rel = mod_prefix + mod
        if rel not in target_entries:
            put_file(target_entries, rel, blob, 0o644, vb_inode)
    valid_ko_names = {
        Path(name).name for name in target_entries
        if name.startswith(mod_prefix) and name.endswith('.ko')
    }
    prune_module_metadata(target_entries, valid_ko_names)

    # Assemble Phase 4 payload under /hmos-compat-marble/
    payload_files: dict[str, tuple[bytes, int]] = {
        'hmos-probe': (bins['hmos_probe'].read_bytes(), 0o755),
        'hmos-ipc.ko': (ipc_ko.read_bytes(), 0o644),
        'hmos-dmabuf-sync.ko': (dmabuf_ko.read_bytes(), 0o644),
        'token_contract': ((artifacts_dir / 'tests/token_contract').read_bytes(), 0o755),
        'binder_nested': ((artifacts_dir / 'tests/binder_nested').read_bytes(), 0o755),
        'binder_oneway_pid': ((artifacts_dir / 'tests/binder_oneway_pid').read_bytes(), 0o755),
        'busybox': (busybox.read_bytes(), 0o755),
        'hmos-init-debug.cfg': (init_cfg_bytes, 0o644),
        'hmos-native-hilog': (b'1\n', 0o644),
        'graphics/init-userdata-ext4.sh': (userdata_sh.read_bytes(), 0o755),
        'graphics/stage-phase4-marble.sh': (stage_sh.read_bytes(), 0o755),
        'graphics/start-phase4-marble.sh': (start_sh.read_bytes(), 0o755),
        'graphics/apply-donor-patches-phase4.sh': (patch_script_path.read_bytes(), 0o755),
        'graphics/hdf_default_phase4.hcb': (phase4_hcb, 0o644),
        'graphics/hmos-turnip.json': (turnip_icd.read_bytes(), 0o644),
        'graphics/init-desktop-marble.cfg': (init_desktop_cfg.read_bytes(), 0o644),
        'graphics/init-platform-marble.cfg': (init_platform_cfg.read_bytes(), 0o644),
        'graphics/init-hardware-marble.cfg': (init_hardware_cfg.read_bytes(), 0o644),
        'graphics/init-wifi-marble.cfg': (init_wifi_bytes, 0o644),
        'graphics/start-wifi-marble.sh': (start_wifi_sh.read_bytes(), 0o755),
        **wifi_supplicant,
        'graphics/display_manager_config.xml': (display_mgr_xml.read_bytes(), 0o644),
        'graphics/appdata-sandbox-marble.json': (sb_main, 0o644),
        'graphics/appdata-sandbox-isolated-marble.json': (sb_iso, 0o644),
        'graphics/oobe_page_offline.json': ((ROOT / 'devices/sheng/runtime/init/oobe_page_offline.json').read_bytes(), 0o644),
    }

    # Stock marble QCA6490 WCNSS_qcom_cfg.ini & Awinic speaker amp firmware.
    wcnss_cfg = ROOT / 'work/marble-wifi-config/vendor/etc/wifi/qca6490/WCNSS_qcom_cfg.ini'
    if wcnss_cfg.is_file():
        payload_files['graphics/WCNSS_qcom_cfg.ini'] = (wcnss_cfg.read_bytes(), 0o644)
    aw_fw = ROOT / 'work/marble-audio-fw/vendor/firmware/aw882xx_acf.bin'
    if aw_fw.is_file():
        payload_files['graphics/aw882xx_acf.bin'] = (aw_fw.read_bytes(), 0o644)

    # Donor chip_prod audio policy XMLs (required by audio_server SA 3001/3009).
    for acfg in ('audio_policy_configuration.xml', 'audio_cust_config.xml'):
        acfg_path = donor_fs_dir / 'chip_prod/etc/audio' / acfg
        if acfg_path.is_file():
            payload_files[f'graphics/{acfg}'] = (acfg_path.read_bytes(), 0o644)

    # PAL/AGM runtime for the vendor audio stack: Bionic loader + vendor libs +
    # our OHOS-side PAL shims.  Without it audio_server waits 5000 ms for
    # IAudioPolicy and the volume keys never work.
    audio_runtime = ROOT / 'compat/audio/runtime'
    if audio_runtime.is_dir():
        for item in sorted(audio_runtime.rglob('*')):
            if not item.is_file() or item.name == 'assemble_runtime.py':
                continue
            rel = item.relative_to(audio_runtime).as_posix()
            payload_files[f'graphics/audio-runtime/{rel}'] = (item.read_bytes(), 0o755)
        agm_shim = ROOT / 'work/marble-agmipc-shim/libhmos_agmipc_shim.so'
        if agm_shim.is_file():
            payload_files['graphics/audio-runtime/lib64/libhmos_agmipc_shim.so'] = (
                agm_shim.read_bytes(), 0o755)
        agmipc_bin = ROOT / 'work/marble-agmipc/bin/agmipc-service'
        if agmipc_bin.is_file():
            payload_files['graphics/audio-runtime/bin/agmipc-service'] = (
                agmipc_bin.read_bytes(), 0o755)
        agmipc_impl = ROOT / 'work/marble-agmipc/lib64/vendor.qti.hardware.AGMIPC@1.0-impl.so'
        if agmipc_impl.is_file():
            payload_files['graphics/audio-runtime/lib64/vendor.qti.hardware.AGMIPC@1.0-impl.so'] = (
                agmipc_impl.read_bytes(), 0o755)
        for shim_name in ('libhmos_pal_server.so', 'libhmos_pal_lifecycle.so'):
            shim = phase4_build_dir / shim_name
            if not shim.is_file():
                raise FileNotFoundError(f'Missing PAL shim artifact: {shim}')
            payload_files[f'graphics/audio-runtime/lib64/{shim_name}'] = (
                shim.read_bytes(), 0o755)

    # PAL/AGM resolve /vendor/etc/{audio,acdbdata,...}; HarmonyOS has no /vendor,
    # so the original marble files ride along and get bind-mounted at boot.
    stock_vendor_tar = ROOT / 'work/marble-stock-vendor/marble-stock-vendor.tar.gz'
    if stock_vendor_tar.is_file():
        wanted_prefixes = (
            'vendor/etc/audio/sku_ukee/',
            'vendor/etc/acdbdata/ukee_mtp/',
        )
        wanted_files = {
            'vendor/etc/card-defs.xml',
            'vendor/etc/kvh2xml.xml',
            'vendor/etc/usecaseKvManager.xml',
        }
        with tarfile.open(stock_vendor_tar) as stock_tar:
            for member in stock_tar.getmembers():
                if not member.isfile():
                    continue
                if member.name in wanted_files or member.name.startswith(wanted_prefixes):
                    relative = member.name.split('vendor/etc/', 1)[1]
                    payload_files[f'graphics/audio-marble/etc/{relative}'] = (
                        stock_tar.extractfile(member).read(), 0o644)

    # Subsystem payloads from the porting workstreams.  Every service/script
    # here is gated at runtime (disabled=1 / param conditions), so shipping them
    # does not change the default boot path.
    subsys_scripts = (
        ('devices/marble/runtime/location/start-location-marble.sh', 0o755),
        ('devices/marble/runtime/theme/start-theme-marble.sh', 0o755),
        ('devices/marble/runtime/bluetooth/start-bluetooth-marble.sh', 0o755),
        ('devices/marble/runtime/usb/start-usb-marble.sh', 0o755),
        ('devices/marble/runtime/usb/stop-usb-marble.sh', 0o755),
        ('devices/marble/runtime/usb/hdc-usb-permit.sh', 0o755),
        ('devices/marble/runtime/codec/start-codec-marble.sh', 0o755),
        ('compat/verify/marble_verify_all.sh', 0o755),
        ('compat/verify/marble_identity.sh', 0o755),
        ('work/marble-acdb-forte/vendor/etc/acdbdata/Forte/Forte_acdb_cal.acdb', 0o644),
        ('work/marble-acdb-forte/vendor/etc/acdbdata/Forte/Forte_workspaceFileXml.qwsp', 0o644),
        ('devices/marble/runtime/telephony/start-modem-marble.sh', 0o755),
        ('work/marble-bluetooth-build/hmos_bt_hdi_publisher_marble', 0o755),
        ('work/marble-huks-build/arm64/tee_backend_probe', 0o755),
    )
    for rel, mode in subsys_scripts:
        artifact = ROOT / rel
        if not artifact.is_file():
            raise FileNotFoundError(f'Missing subsystem artifact: {artifact}')
        payload_files[f'graphics/{artifact.name}'] = (artifact.read_bytes(), mode)
    for rel in (
        'devices/marble/runtime/location/init-location-marble.cfg',
        'devices/marble/runtime/theme/init-theme-marble.cfg',
        'devices/marble/runtime/bluetooth/init-bluetooth-marble.cfg',
        'devices/marble/runtime/usb/init-usb-marble.cfg',
        'devices/marble/runtime/location/locationhub-marble.json',
        'devices/marble/runtime/telephony/init-modem-marble.cfg',
        'devices/marble/runtime/telephony/telephony-sa-table.json',
    ):
        artifact = ROOT / rel
        payload_files[f'graphics/{artifact.name}'] = (artifact.read_bytes(), 0o644)
    for rel in (
        'work/marble-bluetooth-build/libbt_vendor.z.so',
        'work/marble-huks-build/arm64/libhuks_engine_core_standard.z.so',
        'work/marble-huks-build/arm64/libhuks_hdi_service_1.1.z.so',
    ):
        artifact = ROOT / rel
        payload_files[f'graphics/{artifact.name}'] = (artifact.read_bytes(), 0o644)

    payload_files.update(diagnostics_payload)

    # Profiles (verified 100% SA-compatible with ADY-AL10)
    # marble profile = sheng's foundation set + donor ANS/reminder SAs (3203/3204)
    payload_files['graphics/foundation-app-core.json'] = (
        (marble_rt / 'init/foundation-marble.json').read_bytes(), 0o644)
    for prof in (
        'useriam-core.json',
        'installs-debug-start.json',
        'msdp-input-only.json',
        'pasteboard-service-eager.json',
        'powermgr.json',
        'i18n_service.json',
        'service_router.json',
        'download_server.json',
        'wifi-manager-eager.json',
        'deviceinfoservice.json',
        'device_manager.json',
    ):
        payload_files[f'graphics/{prof}'] = ((sheng_prof / prof).read_bytes(), 0o644)

    # Phase 3 compat shims (account hmac, dbkeys, storage ram launcher + no-tee)
    for fname, mode in (
        ('libhmos_account_hmac.so', 0o644),
        ('libdbkeys.so', 0o644),
        ('libhmos_ram_no_tee.so', 0o644),
        ('storage-ram-launcher', 0o755),
    ):
        p = phase3_compat_dir / fname
        if not p.is_file():
            raise FileNotFoundError(f'Missing Phase 3 compat artifact: {p}')
        payload_files[f'graphics/{fname}'] = (p.read_bytes(), mode)

    # Phase 4 display/GPU/useriam/service artifacts
    for fname, mode in (
        ('goodix_core.ko', 0o644),
        (TURNIP_PAYLOAD, 0o644),
        ('zink-runtime-marble.tar.gz', 0o644),
        ('libdisplay_composer_vdi_impl.z.so', 0o644),
        ('libdisplay_buffer_vdi_impl.z.so', 0o644),
        ('libhmos_el5_unavailable.so', 0o644),
        ('libhmos_sceneboard_optional_sa.so', 0o644),
        ('libhmos_wifi_category_v134.so', 0o644),
        ('libhmos_userauth_4_0.so', 0o644),
        ('libuser_auth_interface_service_4.1.z.so', 0o644),
        ('libhmos_user_auth_driver.so', 0o644),
        ('libhmos_surface_diagnostic.so', 0o644),
        ('libhmos_boot_vulkan.so', 0o644),
        ('native_render_launcher', 0o755),
        ('native_display_wake', 0o755),
        ('native_screenlock_request', 0o755),
        ('native_brightness_launcher', 0o755),
        ('native_bundle_cli', 0o755),
        ('native_battery_hdi_publisher_v131', 0o755),
        ('native_sa_check', 0o755),
        ('native_sa_dump', 0o755),
        ('native_input_dispatch', 0o755),
        ('bootanimation-launcher', 0o755),
        ('hmos_display_scanout', 0o755),
        ('hmos_kms_diagnostic', 0o755),
        ('capture_scanout', 0o755),
        ('hmos-pd-mapper', 0o755),
        ('hmos_hisysevent_sink', 0o755),
        ('hmos_brightness_persist', 0o755),
    ):
        if fname == TURNIP_PAYLOAD and turnip_blob is not None:
            # Package the exact bytes checked above, rather than reopening the input.
            payload_files[f'graphics/{fname}'] = (turnip_blob, mode)
            continue
        p = phase4_build_dir / fname
        if not p.is_file():
            raise FileNotFoundError(f'Missing Phase 4 build artifact: {p}')
        payload_files[f'graphics/{fname}'] = (p.read_bytes(), mode)

    # Adreno 725 GPU + Goodix 9916r touch firmware under stock-gpu-firmware/
    fw_files = sorted(p for p in firmware_dir.iterdir() if p.is_file())
    if not fw_files:
        raise FileNotFoundError(f'No firmware files in {firmware_dir}')
    for fw in fw_files:
        payload_files[f'stock-gpu-firmware/{fw.name}'] = (fw.read_bytes(), 0o644)

    if original_init_blob is not None:
        payload_files['init.android.original'] = (original_init_blob, 0o755)

    manifest = {}
    for rel_name, (blob, mode) in payload_files.items():
        put_file(target_entries, f'{COMPAT_DIR}/{rel_name}', blob, mode, vb_inode)
        manifest[rel_name] = {'bytes': len(blob), 'mode': oct(mode), 'sha256': sha256_bytes(blob)}
    put_file(
        target_entries,
        f'{COMPAT_DIR}/MANIFEST.json',
        (json.dumps(manifest, indent=2) + '\n').encode('utf-8'),
        0o644,
        vb_inode,
    )

    vb_cpio_out = work / (target_rd.name + '-phase4.cpio')
    vb_cpio_out.write_bytes(pack_cpio_newc(target_entries))
    vb_rd_out = work / (target_rd.name + '-phase4.img')
    compress_ramdisk(vb_cpio_out, vb_rd_out, target_rd_comp)

    vb_repack_args = list(vb_args)
    for i, tok in enumerate(vb_repack_args):
        if tok == str(target_rd):
            vb_repack_args[i] = str(vb_rd_out)
    vb_out = out_dir / 'vendor_boot_phase4.img'
    subprocess.run(['mkbootimg', *vb_repack_args, '--vendor_boot', str(vb_out)], check=True)
    vb_payload = vb_out.read_bytes()
    vb_final, vb_avb_updated = preserve_and_update_avb_footer(vb_payload, stock_vendor_boot.read_bytes())
    vb_out.write_bytes(vb_final)
    if vb_out.stat().st_size > VENDOR_BOOT_LIMIT_BYTES:
        raise ValueError(f'vendor_boot_phase4.img exceeds 96 MiB limit: {vb_out.stat().st_size}')

    # Verify both boot partitions before recording a successful candidate.
    verify_boot = work / 'verify-boot'
    unpack_with_args(boot_out, verify_boot)
    if sha256_file(verify_boot / 'kernel') != sha256_file(kernel_image):
        raise ValueError('Phase 4 boot kernel round-trip mismatch')
    v_boot_cpio = work / 'verify-boot.cpio'
    decompress_ramdisk(verify_boot / 'ramdisk', v_boot_cpio)
    if v_boot_cpio.read_bytes() != boot_cpio_out.read_bytes():
        raise ValueError('Phase 4 boot ramdisk round-trip mismatch')

    verify_vb = work / 'verify-vb'
    unpack_with_args(vb_out, verify_vb)
    v_vb_cpio = work / 'verify-vb.cpio'
    decompress_ramdisk(verify_vb / target_rd.name, v_vb_cpio)
    if v_vb_cpio.read_bytes() != vb_cpio_out.read_bytes():
        raise ValueError('Phase 4 vendor_boot ramdisk round-trip mismatch')

    report = {
        'boot_phase4': {
            'path': str(boot_out),
            'bytes': boot_out.stat().st_size,
            'payload_bytes': len(boot_payload),
            'limit_bytes': BOOT_LIMIT_BYTES,
            'avb_footer_updated': boot_avb_updated,
            'sha256': sha256_file(boot_out),
        },
        'vendor_boot_phase4': {
            'path': str(vb_out),
            'bytes': vb_out.stat().st_size,
            'payload_bytes': len(vb_payload),
            'limit_bytes': VENDOR_BOOT_LIMIT_BYTES,
            'avb_footer_updated': vb_avb_updated,
            'sha256': sha256_file(vb_out),
        },
        'phase4_donor_recipes_count': len(selected_recipes),
        'diagnostics_inputs': {name: sha256_bytes(data) for name, (data, _) in diagnostics_payload.items()},
        'samgr_original_role': samgr_role,
        'rs_screen_order_trial': rs_screen_order,
        'rs_native_commit_trial': rs_native_commit,
        'rs_yuv_native_handle': rs_yuv_native_handle,
        'wifi_chip_hdi_trial': wifi_chip_hdi,
        'wpa_ondemand_trial': wpa_ondemand,
        'hdf_hcb_sha256': sha256_bytes(phase4_hcb),
        'wifi_init_sha256': sha256_bytes(init_wifi_bytes),
        'firmware_files_count': len(fw_files),
        'payload_files_count': len(payload_files),
        'roundtrip_verified': True,
    }
    (out_dir / 'phase4-build-report.json').write_text(
        json.dumps(report, indent=2) + '\n', encoding='utf-8', newline='\n'
    )
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stock-boot', type=Path, default=ROOT / 'work/marble-backup/stock/boot_a.img')
    parser.add_argument('--stock-vendor-boot', type=Path, default=ROOT / 'work/marble-backup/stock/vendor_boot_a.img')
    parser.add_argument('--artifacts', type=Path, default=ROOT / 'work/marble-kernel-artifacts')
    parser.add_argument('--donor-fs', type=Path, default=ROOT / 'work/donor-rom/ADY-AL10-207.0.0.111-SP10/fs')
    parser.add_argument('--recipes', type=Path, default=ROOT / 'patches/donor/ady-al10-111-sp10')
    parser.add_argument('--busybox', type=Path, default=ROOT / 'work/marble-backup/stock/busybox-aarch64')
    parser.add_argument('--phase3-compat', type=Path, default=ROOT / 'work/marble-compat-libs')
    parser.add_argument('--phase4-build', type=Path, default=ROOT / 'work/marble-phase4-build')
    parser.add_argument('--diagnostics-dir', type=Path,
                        help='Verified diagnostics output from marble_diagnostics.py (default: --phase4-build)')
    parser.add_argument('--firmware-dir', type=Path, default=ROOT / 'work/marble-firmware')
    parser.add_argument('--out', type=Path, default=ROOT / 'work/marble-phase4-out',
                        help='New output directory; leave earlier candidates and logs intact')
    parser.add_argument('--cc', default='aarch64-linux-gnu-gcc')
    parser.add_argument('--rs-screen-order', action='store_true',
                        help='Trial: read the ADY display capability before classifying the physical screen')
    render_recipes = parser.add_mutually_exclusive_group()
    render_recipes.add_argument('--rs-yuv-native-handle', action='store_true',
                                help='Use the H20 YUV/RGB render recipe with the verified H20 Turnip binary')
    render_recipes.add_argument('--rs-native-commit', action='store_true',
                        help='Trial: retain ADY async CommitTransaction with the marble one-way PID loader')
    parser.add_argument('--wifi-chip-hdi', action='store_true',
                        help='Trial: restore the verified ADY chip_interface_service under the existing wifi_host')
    parser.add_argument('--wpa-ondemand', action='store_true',
                        help='Trial: use the ADY dynamic-device declaration and native WPA host startup')
    args = parser.parse_args(argv)
    report = build_phase4_images(
        stock_boot=args.stock_boot,
        stock_vendor_boot=args.stock_vendor_boot,
        artifacts_dir=args.artifacts,
        donor_fs_dir=args.donor_fs,
        recipes_dir=args.recipes,
        busybox=args.busybox,
        phase3_compat_dir=args.phase3_compat,
        phase4_build_dir=args.phase4_build,
        firmware_dir=args.firmware_dir,
        out_dir=args.out,
        cc=args.cc,
        diagnostics_dir=args.diagnostics_dir,
        rs_screen_order=args.rs_screen_order,
        rs_native_commit=args.rs_native_commit,
        rs_yuv_native_handle=args.rs_yuv_native_handle,
        wifi_chip_hdi=args.wifi_chip_hdi,
        wpa_ondemand=args.wpa_ondemand,
    )
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
