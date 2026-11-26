#!/usr/bin/env python3
"""Assemble a self-verifying Fastboot one-click flash package for Xiaomi marble (AnyHMOS)."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import stat
import struct
import sys
import zipfile
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[2]
BOOT_LIMIT_BYTES = 201_326_592       # 192 MiB
VENDOR_BOOT_LIMIT_BYTES = 100_663_296  # 96 MiB
SUPER_BYTES = 9_663_676_416          # 9 GiB exact marble super
VBMETA_LIMIT_BYTES = 131_072         # 128 KiB
ANDROID_SPARSE_MAGIC = 0xED26FF3A

REQUIRED_PLATFORM_TOOLS = (
    'fastboot.exe',
    'AdbWinApi.dll',
    'AdbWinUsbApi.dll',
    'libwinpthread-1.dll',
    'NOTICE.txt',
    'source.properties',
)
OPTIONAL_PLATFORM_TOOLS = (
    'mke2fs.exe',
    'mke2fs.conf',
    'make_f2fs.exe',
)


def sha256_file(path: Path, chunk_size: int = 4 * 1024 * 1024) -> str:
    h = hashlib.sha256()
    with path.open('rb') as stream:
        while True:
            chunk = stream.read(chunk_size)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


def validate_boot_image(path: Path) -> int:
    if not path.is_file():
        raise FileNotFoundError(f'Missing boot image: {path}')
    size = path.stat().st_size
    if size < 4096 or size > BOOT_LIMIT_BYTES:
        raise ValueError(f'Invalid boot.img size {size} (must be 4096..{BOOT_LIMIT_BYTES})')
    with path.open('rb') as f:
        magic = f.read(8)
    if magic != b'ANDROID!':
        raise ValueError(f'boot.img missing ANDROID! header magic: {magic!r}')
    return size


def validate_vendor_boot_image(path: Path) -> int:
    if not path.is_file():
        raise FileNotFoundError(f'Missing vendor_boot image: {path}')
    size = path.stat().st_size
    if size < 4096 or size > VENDOR_BOOT_LIMIT_BYTES:
        raise ValueError(f'Invalid vendor_boot.img size {size} (must be 4096..{VENDOR_BOOT_LIMIT_BYTES})')
    with path.open('rb') as f:
        magic = f.read(8)
    if magic != b'VNDRBOOT':
        raise ValueError(f'vendor_boot.img missing VNDRBOOT header magic: {magic!r}')
    return size


def validate_vbmeta_image(path: Path, label: str) -> int:
    if not path.is_file():
        raise FileNotFoundError(f'Missing {label} image: {path}')
    size = path.stat().st_size
    if size < 256 or size > VBMETA_LIMIT_BYTES:
        raise ValueError(f'Invalid {label} size {size} (must be 256..{VBMETA_LIMIT_BYTES})')
    with path.open('rb') as f:
        magic = f.read(4)
    if magic != b'AVB0':
        raise ValueError(f'{label} missing AVB0 header magic: {magic!r}')
    return size


def validate_sparse_super_image(path: Path) -> tuple[int, int]:
    if not path.is_file():
        raise FileNotFoundError(f'Missing super image: {path}')
    size = path.stat().st_size
    if size < 28:
        raise ValueError(f'super.img too small ({size} bytes)')
    with path.open('rb') as f:
        hdr = f.read(28)
    magic, maj, minv, fhdr_sz, chdr_sz, blk_sz, total_blks, total_chunks, _ = struct.unpack(
        '<IHHHHIIII', hdr
    )
    if magic != ANDROID_SPARSE_MAGIC or maj != 1 or fhdr_sz < 28 or chdr_sz < 12:
        raise ValueError(f'super.img is not a valid Android sparse image (magic=0x{magic:08x}, v{maj}.{minv})')
    expanded = blk_sz * total_blks
    if expanded != SUPER_BYTES:
        raise ValueError(
            f'super.img expanded size {expanded} != expected Xiaomi marble super size {SUPER_BYTES}'
        )
    if total_chunks == 0:
        raise ValueError('super.img has 0 sparse chunks')
    return size, expanded


def sanitize_report(obj: Any) -> Any:
    """Strip host-specific absolute filesystem paths before embedding reports into a shareable package."""
    if isinstance(obj, dict):
        cleaned: dict[str, Any] = {}
        for k, v in obj.items():
            if k == 'path' and isinstance(v, str):
                cleaned[k] = Path(v).name
            else:
                cleaned[k] = sanitize_report(v)
        return cleaned
    if isinstance(obj, list):
        return [sanitize_report(item) for item in obj]
    return obj


def write_bat_wrapper(dest: Path, mode: str, extra_args: str = '') -> None:
    suffix = f' {extra_args}' if extra_args else ''
    content = (
        '@echo off\r\n'
        'setlocal\r\n'
        f'powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Flash-Marble.ps1" -Mode {mode}{suffix}\r\n'
        'if errorlevel 1 (\r\n'
        '    echo.\r\n'
        '    echo Operation failed. Check the error message above and logs\\fastboot-*.txt.\r\n'
        ')\r\n'
        'pause\r\n'
    )
    dest.write_text(content, encoding='utf-8', newline='')


def build_readme(
    *,
    package_type: str,
    batch: str,
    donor: str,
    kernel_release: str,
    boot_sha: str,
    vendor_boot_sha: str,
    super_sha: str | None,
    device_boot_tested: bool,
    verified_boot_id: str | None,
) -> str:
    title_kind = '完整刷机包 (Full Install + Upgrade)' if package_type == 'full' else '增量升级包 (Upgrade Only: boot + vendor_boot)'
    boot_status = (
        f'已通过实机冷启动与验收 (`boot_id={verified_boot_id}`)'
        if device_boot_tested and verified_boot_id
        else ('已通过实机启动验收' if device_boot_tested else '尚未标注实机启动验收')
    )
    full_section = ''
    if package_type == 'full':
        full_section = f"""
### 首次完整安装（从原厂 MIUI / HyperOS / Android 迁入）

1. 确保小米 `marble`（Redmi Note 12 Turbo / POCO F5）**已解锁 Bootloader**，并备份好个人数据与原厂 `boot` / `vendor_boot` / `vbmeta*`。
2. 手机关机后按 **音量下 + 电源键** 进入 **Bootloader Fastboot**（注意不是 `fastbootd`），用稳定数据线直连电脑 USB 口。
3. **先校验刷机包**：
   - Windows：双击 `1-Verify-Package.bat`
   - Linux / macOS：`./flash-marble.sh verify`
4. **只读设备预检**（核对 `product=marble`、`unlocked=yes`、分区容量）：
   - Windows：双击 `2-Inspect-Device.bat`
   - Linux / macOS：`./flash-marble.sh inspect`
5. **执行完整刷入**：
   - **保留现有 ext4 `userdata`（或自行进 Recovery 格式化 ext4）**：
     - Windows：双击 `4-Flash-Full-Install.bat`（按提示输入 `FLASH MARBLE FULL`）
     - Linux / macOS：`./flash-marble.sh full`
   - **首次从加密 Android 迁入且希望顺带格式化 `userdata` 为 ext4（会清除全部用户数据！）**：
     - Windows：双击 `5-Flash-Full-Install-FormatUserdata.bat`（需先后输入 `FLASH MARBLE FULL` 与 `FORMAT MARBLE USERDATA EXT4`）
     - Linux / macOS：`./flash-marble.sh full --format-userdata-ext4`

> **关于 `userdata`**：`marble` 启动脚本严格禁用自动格式化，只挂载正常的未加密 **ext4** `userdata`（首次进入 Batch M6 时会无损自动置位 ext4 `RO_COMPAT_VERITY` 以支持内核 `fs-verity` 代码签名）。若首次从原厂加密 Android 刷入而未将 `userdata` 格式化为 ext4，开机将拒绝挂载。

"""

    super_row = f'| `images/super.img` | `super` (9 GiB sparse) | `{super_sha}` |\n' if super_sha else ''
    vbmeta_rows = (
        '| `images/vbmeta.img` | `vbmeta_a` (`--disable-verity --disable-verification`) | 见 `SHA256SUMS` |\n'
        '| `images/vbmeta_system.img` | `vbmeta_system_a` (`--disable-verity --disable-verification`) | 见 `SHA256SUMS` |\n'
        if package_type == 'full'
        else ''
    )

    return f"""# Xiaomi `marble` AnyHMOS Fastboot 一键刷机包（Batch {batch}）

- **适用机型**：Xiaomi `marble`（Redmi Note 12 Turbo / POCO F5，SM7475 / Snapdragon 7+ Gen 2）
- **刷机包类型**：{title_kind}
- **Donor 基线**：`{donor}`
- **内核版本**：`{kernel_release}`（原生移植 OpenHarmony `fs/code_sign/` + `fs-verity` + ECDSA NIST P-256/P-384，657 个厂商模块 KABI 0 冲突）
- **实机验证状态**：{boot_status}
- **目标槽位**：`a`（脚本刷完后强制执行 `fastboot set_active a`）

## 安全边界（脚本硬性守卫）

- 仅允许刷入 `product == marble`、`unlocked == yes`、`is-userspace == no`（Bootloader Fastboot）的设备。
- 刷写前强制全量核对 `PACKAGE.json` 中每个文件的字节数与 SHA-256 哈希。
- **永不触碰**：`abl`、`xbl`、`modem*`、`dsp*`、`bluetooth*`、`persist`、`recovery*`、`dtbo*`（且 `marble` 无 `init_boot` 分区）。
- **默认不清除 `userdata`**：`Upgrade` 模式只刷 `boot_a` + `vendor_boot_a`，严格保留 `super` 与 `userdata`。

## 快速使用

### 已有 AnyHMOS 系统升级（推荐，保留 `super` 与全部用户数据）

1. 手机进入 Bootloader Fastboot（音量下 + 电源键），连接电脑。
2. Windows：依次双击 `1-Verify-Package.bat` → `2-Inspect-Device.bat` → `3-Flash-Upgrade-KeepData.bat`（按提示输入 `UPGRADE MARBLE`）。
3. Linux / macOS：
   ```bash
   ./flash-marble.sh verify
   ./flash-marble.sh inspect
   ./flash-marble.sh upgrade
   ```
{full_section}
## 包含镜像摘要

| 文件 | 目标分区 | SHA-256 |
| --- | --- | --- |
| `images/boot.img` | `boot_a` (192 MiB) | `{boot_sha}` |
| `images/vendor_boot.img` | `vendor_boot_a` (96 MiB) | `{vendor_boot_sha}` |
{vbmeta_rows}{super_row}
完整文件清单与校验和见 `PACKAGE.json` 与 `SHA256SUMS`，每次刷写日志保存在 `logs/fastboot-*.txt`。

## 已知边界说明

- **USB 串口救援口**：默认保持物理 USB 为 ACM 串口（COM 调试救援口），不会自动切换为 HDC。
- **指纹识别**：依赖 QSEE TrustZone 与原厂签名 TA，当前硬件边界不可用。
- **卓易通（ANCO）/ 华为商店设备证明**：依赖华为原厂设备证书与闭源容器镜像，本项目不作绕过。
- **HAP 应用安装**：已完整实现原生内核 `code_sign` + `fs-verity`（支持官方 `hap-sign-tool` + AGC 调试凭证签名安装，未签名与篡改包均被内核严格拒绝）。
"""


def assemble_package(
    *,
    out_dir: Path,
    boot_path: Path,
    vendor_boot_path: Path,
    super_path: Path | None = None,
    vbmeta_path: Path | None = None,
    vbmeta_system_path: Path | None = None,
    platform_tools_dir: Path | None = None,
    build_report_path: Path | None = None,
    batch: str = 'M6',
    donor: str = 'ADY-AL10 207.0.0.111(SP10)',
    kernel_release: str = '5.10.274-Bouquet-anyhmos',
    device_boot_tested: bool = False,
    verified_boot_id: str | None = None,
    allow_overwrite: bool = False,
    create_zip: bool = False,
) -> dict[str, Any]:
    if super_path is not None and (vbmeta_path is None or vbmeta_system_path is None):
        raise ValueError('--super requires both --vbmeta and --vbmeta-system')
    if super_path is None and (vbmeta_path is not None or vbmeta_system_path is not None):
        raise ValueError('--vbmeta and --vbmeta-system are only used together with --super')

    validate_boot_image(boot_path)
    validate_vendor_boot_image(vendor_boot_path)
    if super_path is not None:
        assert vbmeta_path is not None and vbmeta_system_path is not None
        validate_sparse_super_image(super_path)
        validate_vbmeta_image(vbmeta_path, 'vbmeta.img')
        validate_vbmeta_image(vbmeta_system_path, 'vbmeta_system.img')

    package_type = 'full' if super_path is not None else 'upgrade'

    if out_dir.exists():
        if not allow_overwrite:
            raise FileExistsError(f'Output directory already exists: {out_dir} (pass --allow-overwrite)')
        shutil.rmtree(out_dir)

    images_dir = out_dir / 'images'
    images_dir.mkdir(parents=True, exist_ok=True)

    # Copy verified images and cache their SHA-256 once to avoid re-reading multi-GiB files
    cached_hashes: dict[str, str] = {}
    shutil.copyfile(boot_path, images_dir / 'boot.img')
    shutil.copyfile(vendor_boot_path, images_dir / 'vendor_boot.img')
    boot_sha = sha256_file(images_dir / 'boot.img')
    vendor_boot_sha = sha256_file(images_dir / 'vendor_boot.img')
    cached_hashes['images/boot.img'] = boot_sha
    cached_hashes['images/vendor_boot.img'] = vendor_boot_sha

    super_sha: str | None = None
    if package_type == 'full':
        assert super_path is not None and vbmeta_path is not None and vbmeta_system_path is not None
        shutil.copyfile(vbmeta_path, images_dir / 'vbmeta.img')
        shutil.copyfile(vbmeta_system_path, images_dir / 'vbmeta_system.img')
        shutil.copyfile(super_path, images_dir / 'super.img')
        super_sha = sha256_file(images_dir / 'super.img')
        cached_hashes['images/super.img'] = super_sha

    # Copy Windows platform-tools if provided
    if platform_tools_dir is not None:
        bin_dir = out_dir / 'bin'
        bin_dir.mkdir(exist_ok=True)
        for tool in REQUIRED_PLATFORM_TOOLS:
            src = platform_tools_dir / tool
            if not src.is_file():
                raise FileNotFoundError(f'Missing required platform-tools file: {src}')
            shutil.copyfile(src, bin_dir / tool)
        for tool in OPTIONAL_PLATFORM_TOOLS:
            src = platform_tools_dir / tool
            if src.is_file():
                shutil.copyfile(src, bin_dir / tool)

    # Copy sanitized build report if provided
    if build_report_path is not None:
        raw_report = json.loads(build_report_path.read_text(encoding='utf-8'))
        reports_dir = out_dir / 'reports'
        reports_dir.mkdir(exist_ok=True)
        (reports_dir / 'build-report.json').write_text(
            json.dumps(sanitize_report(raw_report), indent=2, ensure_ascii=False) + '\n',
            encoding='utf-8',
            newline='\n',
        )

    # Copy cross-platform flashing scripts
    ps1_src = REPO_ROOT / 'tools/packaging/Flash-Marble.ps1'
    sh_src = REPO_ROOT / 'tools/packaging/flash-marble.sh'
    shutil.copyfile(ps1_src, out_dir / 'Flash-Marble.ps1')
    sh_dest = out_dir / 'flash-marble.sh'
    shutil.copyfile(sh_src, sh_dest)
    sh_dest.chmod(sh_dest.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)

    # Generate Windows double-click .bat wrappers
    write_bat_wrapper(out_dir / '1-Verify-Package.bat', 'Verify')
    write_bat_wrapper(out_dir / '2-Inspect-Device.bat', 'Inspect')
    write_bat_wrapper(out_dir / '3-Flash-Upgrade-KeepData.bat', 'Upgrade')
    if package_type == 'full':
        write_bat_wrapper(out_dir / '4-Flash-Full-Install.bat', 'FullInstall')
        write_bat_wrapper(
            out_dir / '5-Flash-Full-Install-FormatUserdata.bat',
            'FullInstall',
            '-FormatUserdataExt4',
        )

    # Write flashing README
    (out_dir / 'README-FLASH.md').write_text(
        build_readme(
            package_type=package_type,
            batch=batch,
            donor=donor,
            kernel_release=kernel_release,
            boot_sha=boot_sha,
            vendor_boot_sha=vendor_boot_sha,
            super_sha=super_sha,
            device_boot_tested=device_boot_tested,
            verified_boot_id=verified_boot_id,
        ),
        encoding='utf-8',
        newline='\n',
    )

    # Build PACKAGE.json and SHA256SUMS over all files (excluding logs, PACKAGE.json, SHA256SUMS)
    file_records: list[dict[str, Any]] = []
    for path in sorted(out_dir.rglob('*')):
        rel = path.relative_to(out_dir)
        if (
            not path.is_file()
            or rel.parts[0] == 'logs'
            or path.name in {'PACKAGE.json', 'SHA256SUMS'}
        ):
            continue
        rel_posix = rel.as_posix()
        file_records.append(
            {
                'path': rel_posix,
                'bytes': path.stat().st_size,
                'sha256': cached_hashes.get(rel_posix) or sha256_file(path),
            }
        )

    manifest: dict[str, Any] = {
        'kind': 'marble-anyhmos-fastboot',
        'device': 'marble',
        'models': ['Redmi Note 12 Turbo', 'POCO F5'],
        'package_type': package_type,
        'batch': batch,
        'donor': donor,
        'kernel_release': kernel_release,
        'target_slot': 'a',
        'has_init_boot': False,
        'boot_limit_bytes': BOOT_LIMIT_BYTES,
        'vendor_boot_limit_bytes': VENDOR_BOOT_LIMIT_BYTES,
        'super_bytes': SUPER_BYTES if package_type == 'full' else None,
        'device_boot_tested': device_boot_tested,
        'verified_boot_id': verified_boot_id if device_boot_tested else None,
        'contains_userdata_seed': False,
        'preserves_userdata_in_upgrade': True,
        'untouched_partitions': [
            'abl',
            'xbl',
            'modem',
            'dsp',
            'bluetooth',
            'persist',
            'recovery',
            'dtbo',
        ],
        'files': file_records,
    }
    manifest_path = out_dir / 'PACKAGE.json'
    manifest_path.write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False) + '\n',
        encoding='utf-8',
        newline='\n',
    )
    manifest_sha = sha256_file(manifest_path)

    checksum_lines = [f'{rec["sha256"]}  {rec["path"]}\n' for rec in file_records]
    checksum_lines.append(f'{manifest_sha}  PACKAGE.json\n')
    (out_dir / 'SHA256SUMS').write_text(''.join(checksum_lines), encoding='utf-8', newline='\n')

    zip_path: Path | None = None
    if create_zip:
        zip_path = out_dir.with_suffix('.zip')
        if zip_path.exists():
            zip_path.unlink()
        with zipfile.ZipFile(zip_path, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=1) as zf:
            for path in sorted(out_dir.rglob('*')):
                if path.is_file():
                    arcname = Path(out_dir.name) / path.relative_to(out_dir)
                    zf.write(path, arcname.as_posix())

    return {
        'package': str(out_dir),
        'package_type': package_type,
        'batch': batch,
        'files': len(file_records) + 2,
        'total_bytes': sum(rec['bytes'] for rec in file_records)
        + manifest_path.stat().st_size
        + (out_dir / 'SHA256SUMS').stat().st_size,
        'boot_sha256': boot_sha,
        'vendor_boot_sha256': vendor_boot_sha,
        'super_sha256': super_sha,
        'zip': str(zip_path) if zip_path else None,
        'device_flashed': False,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True, help='Output package directory')
    parser.add_argument('--boot', type=Path, required=True, help='Verified marble boot.img')
    parser.add_argument('--vendor-boot', type=Path, required=True, help='Verified marble vendor_boot.img')
    parser.add_argument('--super', dest='super_img', type=Path, default=None, help='Optional 9 GiB sparse super.img (enables full package)')
    parser.add_argument('--vbmeta', type=Path, default=None, help='Optional vbmeta.img (required with --super)')
    parser.add_argument('--vbmeta-system', type=Path, default=None, help='Optional vbmeta_system.img (required with --super)')
    parser.add_argument('--platform-tools', type=Path, default=None, help='Optional Windows platform-tools directory containing fastboot.exe')
    parser.add_argument('--build-report', type=Path, default=None, help='Optional JSON build report (sanitized before embedding)')
    parser.add_argument('--batch', default='M6', help='Batch label (default: M6)')
    parser.add_argument('--donor', default='ADY-AL10 207.0.0.111(SP10)', help='Donor ROM identifier')
    parser.add_argument('--kernel-release', default='5.10.274-Bouquet-anyhmos', help='Kernel UTS release string')
    parser.add_argument('--device-boot-tested', action='store_true', help='Mark package images as live boot-tested on hardware')
    parser.add_argument('--boot-id', default=None, help='Verified live kernel boot_id UUID')
    parser.add_argument('--allow-overwrite', action='store_true', help='Replace existing output directory')
    parser.add_argument('--zip', action='store_true', help='Also create a sibling .zip archive')
    args = parser.parse_args(argv)

    summary = assemble_package(
        out_dir=args.out,
        boot_path=args.boot,
        vendor_boot_path=args.vendor_boot,
        super_path=args.super_img,
        vbmeta_path=args.vbmeta,
        vbmeta_system_path=args.vbmeta_system,
        platform_tools_dir=args.platform_tools,
        build_report_path=args.build_report,
        batch=args.batch,
        donor=args.donor,
        kernel_release=args.kernel_release,
        device_boot_tested=args.device_boot_tested,
        verified_boot_id=args.boot_id,
        allow_overwrite=args.allow_overwrite,
        create_zip=args.zip,
    )
    print(json.dumps(summary, indent=2, ensure_ascii=False))
    return 0


if __name__ == '__main__':
    sys.exit(main())
