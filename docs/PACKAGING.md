# 打包、一键刷机包组装与源码清单 / Packaging & Fastboot Bundle

本仓库不托管闭源 `.img` 大镜像、原厂固件、APK/HAP 应用包或私有签名证书，但提供完整的 **Xiaomi `marble` Fastboot 一键刷机包组装与跨平台刷机脚本**，以及全仓源码哈希校验机制。

This repository does not redistribute closed-source ROM images, vendor firmware, HAP/APK binaries, or private signing keys. It provides the self-verifying Fastboot bundle assembler and cross-platform flash scripts for Xiaomi `marble`, alongside repository source-integrity tooling.

## 1. Xiaomi `marble` Fastboot 一键刷机包

核心脚本与离线测试：

- 打包组装器：[assemble_marble_fastboot_package.py](../tools/packaging/assemble_marble_fastboot_package.py)
- Windows 一键刷机脚本（PowerShell 5.1+）：[Flash-Marble.ps1](../tools/packaging/Flash-Marble.ps1)（打包时自动生成 3~5 个双击 `.bat` 封装）
- Linux / macOS 刷机脚本：[flash-marble.sh](../tools/packaging/flash-marble.sh)
- 离线单元测试：[test_marble_fastboot_package.py](../tests/test_marble_fastboot_package.py)

### 两种刷机包形态

| 形态 | 包含镜像 | 适用场景 | 触碰分区 |
| --- | --- | --- | --- |
| **增量升级包（`upgrade`）** | `boot.img`（192 MiB）+ `vendor_boot.img`（96 MiB） | 已装 AnyHMOS 设备升级到 Batch M6 | 仅 `boot_a`、`vendor_boot_a` + `set_active a`（**完整保留 `super` 与 `userdata`**） |
| **完整刷机包（`full`）** | 上述镜像 + `vbmeta.img` + `vbmeta_system.img` + `super.img`（9 GiB 稀疏镜像） | 首次从原厂 MIUI/HyperOS 迁入或全量恢复 | `vbmeta_a`/`vbmeta_system_a`（关闭 verity/verification）、`boot_a`、`vendor_boot_a`、`super` + `set_active a` |

### 组装命令示例

```sh
# 1) 组装轻量增量升级包（可选 --zip 同时输出压缩包）
python3 tools/packaging/assemble_marble_fastboot_package.py \
  --out /path/to/anyhmos-marble-m6-fastboot-upgrade \
  --boot /path/to/boot_h20_m6_codesign.img \
  --vendor-boot /path/to/vendor_boot_h20_m6_codesign.img \
  --platform-tools /path/to/platform-tools \
  --build-report work/marble-m6-build/m6-build-report.json \
  --batch M6 \
  --device-boot-tested --boot-id 52d5c34b-ba82-46a6-8679-2b1099e2e688 \
  --zip

# 2) 组装完整一键刷机包
python3 tools/packaging/assemble_marble_fastboot_package.py \
  --out /path/to/anyhmos-marble-m6-fastboot-full \
  --boot /path/to/boot_h20_m6_codesign.img \
  --vendor-boot /path/to/vendor_boot_h20_m6_codesign.img \
  --super /path/to/super_marble_phase3.img \
  --vbmeta /path/to/vbmeta_a.img \
  --vbmeta-system /path/to/vbmeta_system_a.img \
  --platform-tools /path/to/platform-tools \
  --build-report work/marble-m6-build/m6-build-report.json \
  --batch M6 \
  --device-boot-tested --boot-id 52d5c34b-ba82-46a6-8679-2b1099e2e688
```

### 内置安全守卫

1. **打包期校验**：校验 `boot.img`（`ANDROID!` 魔数且 `<= 192 MiB`）、`vendor_boot.img`（`VNDRBOOT` 魔数且 `<= 96 MiB`）、`vbmeta*.img`（`AVB0` 魔数）、`super.img`（合法 Android 稀疏镜像 `0xed26ff3a` 且展开精确等于 `9,663,676,416` 字节），自动脱敏构建报告中的本机绝对路径，生成 `PACKAGE.json` 与 `SHA256SUMS`。
2. **刷写前强校**：`Flash-Marble.ps1` 与 `flash-marble.sh` 均先逐项校验全部文件字节数与 SHA-256，再通过只读 `fastboot getvar` 确认 `product == marble`、`unlocked == yes`、`is-userspace == no`（Bootloader Fastboot 而非 `fastbootd`）及各分区容量。
3. **分区隔离与二次确认**：`marble` 无 `init_boot` 分区；永不写入 `abl`/`xbl`/`modem*`/`dsp*`/`bluetooth*`/`persist`/`recovery*`/`dtbo*`；`Upgrade` 需键入 `UPGRADE MARBLE`，`FullInstall` 需键入 `FLASH MARBLE FULL`，仅当显式附加 `-FormatUserdataExt4` / `--format-userdata-ext4` 且二次键入 `FORMAT MARBLE USERDATA EXT4` 时才擦除 `metadata` 并格式化 `userdata` 为 ext4（详见 [USERDATA.md](USERDATA.md)）。刷写完成后强制执行 `fastboot set_active a`。

### 验证生成的刷机包

```sh
# 离线单测
python3 -m unittest tests/test_marble_fastboot_package.py -v

# 对实际生成的目录执行离线校验
./flash-marble.sh verify
# Windows PowerShell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Flash-Marble.ps1 -Mode Verify
```

## 2. 仓库源码完整性清单 / Source inventory

修改仓库内任何跟踪源码或文档后运行：

```sh
python3 tools/maint/refresh_manifest.py
python3 tools/maint/verify_sources.py
```

- `SOURCE-MANIFEST.json`：仓库内所有跟踪文本源码的路径、字节数与 SHA-256（排除自身与 `SHA256SUMS`）。
- `SHA256SUMS`：逐文件及 `SOURCE-MANIFEST.json` 的校验和。
- `archive/sheng/RELEASE.json`：原参考机型 `sheng` 的历史基线归档。

上述维护命令只做离线检查，不触碰设备、不上传远端。编译产物、原厂库、镜像、签名私钥/证书、设备备份与个人日志严禁提交。

另见 [BUILDING.md](BUILDING.md)（外部构建依赖）与 [PROVENANCE.md](PROVENANCE.md)（版权归属与许可边界）。
