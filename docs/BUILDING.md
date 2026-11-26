# 当前源码构建 / Building the selected sources

本仓库采用**“通用鸿蒙兼容层（`compat/`）+ 机型目录（`devices/marble/` 为当前主线目标、`devices/sheng/` 为参考基线）”**分层结构，保留各子系统当前选定实现及其编译/打包脚本。

The tree keeps the selected implementation of each component. It does not redistribute proprietary donor images, stock firmware, private signing keys, or flashable `.img` artifacts.

## 外部输入 / External inputs

### `marble` 主线构建输入（SM7475 · Batch M6）
- **开源内核与工具链**：[`Pzqqt/android_kernel_xiaomi_marble`](https://github.com/Pzqqt/android_kernel_xiaomi_marble)（`bouquet_ci`，Linux `5.10.274`，`marble_defconfig`，Slim LLVM 23.1.6，开启 `CONFIG_CFI_CLANG` + `CONFIG_LTO_CLANG_THIN` + `CONFIG_MODVERSIONS`），通过 `tools/build/build_marble_kernel.sh` 叠加 `devices/marble/kernel/marble-exports.patch` 与 Batch M6 `devices/marble/kernel/marble-codesign.patch`（`fs/code_sign/` + `fs-verity` + ECDSA NIST P-256/P-384）并执行 657 模块 KABI CRC/vermagic 审计。
- **Donor ROM 与原厂固件**：手机 HarmonyOS Donor `ADY-AL10 207.0.0.111(SP10)` 链接库/系统文件与 `marble` 本机只读原厂 `boot`/`vendor_boot`/`vbmeta*`/`vendor` 备份（均自行准备，不随仓库分发）。
- **Harmony native SDK & 交叉编译环境**：Harmony native SDK `6.0.0.47`、AArch64 Linux GCC/Clang 与 Bionic/C++ 链接环境。
- **批次与一键刷机打包**：`tools/packaging/build_marble_h20_m6_codesign.py` 打包 Batch M6 `boot_a`/`vendor_boot_a`，[assemble_marble_fastboot_package.py](../tools/packaging/assemble_marble_fastboot_package.py) 组装跨平台自校验 Fastboot 一键刷机包（见 [PACKAGING.md](PACKAGING.md)）。

### `sheng` 参考基线输入（SM8550 · 归档参考）
- `sheng` 官方 OS3.0.303.0.WNXCNXM 的匹配内核配置/模块/固件，以及 MatePad Air (`GEM-W00`) HarmonyOS donor 链接库；`external/upstream-sources.json` 记录上游版本，许可证位于 `external/licenses/`。

全新安装的 ext4 `userdata` 挂载要求见 [USERDATA.md](USERDATA.md)。普通未加密 ext4 即可直接挂载，启动程序严格禁用自动格式化；`marble` Batch M6 首次启动时由 `compat/services/marble_ext4_verity_enable.c` 无损启用 ext4 `RO_COMPAT_VERITY` 以支持内核 `fs-verity` 代码签名。

## 构建入口 / Build entry points

在 WSL/Linux 使用 `tools/build/build_current.py`。它只编译显式选定的目标，不自动刷写或连接设备。

```sh
python3 tools/build/build_current.py --list
python3 tools/build/build_current.py composer allocator display-wake --dry-run
python3 tools/build/build_current.py composer allocator display-wake
python3 tools/build/build_current.py audio-vdi audio-loader audio-manager
python3 tools/build/build_current.py bootstrap init-entry touch touch-launcher wifi-category
```

默认编译输出为 `work/current-build/`，构建命令和产物哈希写入其 `build-report.json`。`--dry-run` 可先检查依赖路径。当前脚本覆盖常用适配目标；未提供目标的源文件仍需按原接口与工具链自行构建，不可据此声称完整 ROM 已能一键重建。

| 环境变量 / Variable | 默认位置 / Default |
| --- | --- |
| `HMOS_DEPS`（兼容 `SHENG_DEPS`） | `/root/hmos-work/graphics-deps` |
| `HMOS_SDK`（兼容 `SHENG_SDK`） | `/root/hmos-work/sdk-6.0.0.47/installed/native` |
| `HMOS_UPSTREAM`（兼容 `SHENG_UPSTREAM`） | `$HMOS_DEPS/source` |
| `HMOS_DONOR_LINK`（兼容 `SHENG_DONOR_LINK`） | 仓库 `work/donor-hdi-link` |
| `HMOS_STAGE`（兼容 `SHENG_STAGE`） | `$HMOS_DEPS/stage` |
| `HMOS_BIONIC_LINK`（兼容 `SHENG_BIONIC_LINK`） | 仓库 `work/audio-bionic-v1/lib64` |
| `HMOS_PAL_INCLUDE`（兼容 `SHENG_PAL_INCLUDE`） | `/root/hmos-reference/pal-sm8550-a13/inc` |
| `HMOS_ANDROID_CPP_HEADERS`（兼容 `SHENG_ANDROID_CPP_HEADERS`） | `/root/hmos-work/clang-prebuilt/clang-r450784e/include/c++/v1` |
| `HMOS_BT_VENDOR_LINK`（兼容 `SHENG_BT_VENDOR_LINK`） | 仓库 `work/donor-bluetooth-v136/vendor/lib64` |

`compat/ipc/Makefile`（`hmos_ipc.ko`）与 `compat/graphics/dmabuf_sync/kernel/Makefile`（`hmos_dmabuf_sync.ko`）为跨机型共享内核模块，需在匹配内核源码和配置下构建：
- `marble`（Linux 5.10.274）：配套 `devices/marble/kernel/marble-exports.patch` + `marble_hmos_exports.c`（导出 `hmos_ipc_api_v1`）及 Batch M6 `devices/marble/kernel/marble-codesign.patch` + `devices/marble/kernel/marble-codesign/`（`/dev/code_sign`、`FS_IOC_ENABLE_CODE_SIGN`、ECDSA NIST P-256/P-384），通过 `tools/build/build_marble_kernel.sh` 或 `.github/workflows/build-marble-kernel.yml` 构建并执行 CFI / vermagic / `Module.symvers` CRC 校验。
- `sheng`（参考机型 5.15.178）：配套 `devices/sheng/kernel/gki-sheng-exports.patch`。

Mesa 补丁以记录的 upstream HEAD 为基础，当前冻结配置位于 `compat/graphics/`（`apply_mesa.py`、`mesa-gallium-egl.patch`、`mesa-turnip.patch`）；旧版补丁归档于 `archive/sheng/legacy-mesa/`。

UserIAM 使用 `tools/build/build_useriam.py`，需自行准备 target-compatible OpenSSL 头、4.0/4.1 donor stub 和 HDF host 库。`HMOS_USERIAM_VENDOR_LIBS`、`HMOS_USERIAM_HOST_LIBS`、`HMOS_OPENSSL_INCLUDE`（兼容旧 `SHENG_*` 前缀）可指定路径。`compat/useriam/software-backend.patch` 描述当前软件后端所需的上游差异，需审查应用到匹配来源；其 4.0 后端和 4.1 adapter 共同组成当前实现，不应互相删减。

## 配置与补丁 / Runtime and patches

`devices/sheng/runtime/` 保留按子系统归类后的 `sheng` 脚本与配置，`patches/donor/gem-w00-111-sp12/` 是当前合并修补配方。它们保留全文件输入/输出 SHA-256 和逐段 before/after 校验，可用 `tools/packaging/apply_guarded_elf_patch.py` 生成副本。原库不分发。

Some runtime scripts intentionally apply dependent stages during startup. Those stages are part of the current boot path, not independently selectable old versions. Runtime checksum constants identify the accepted binaries; newly rebuilt outputs may differ and require independent packaging checks and device validation. Do not blindly copy build output into a ramdisk or remove guards to force a mismatch through.

## 调试与检查 / Debugging and checks

COM probe 不是 ADB。用 `host-tools/Invoke-ProbeCommand.ps1`、文件传输和截图脚本操作；串口请求必须串行。单请求上限约 60 秒，超时完整回复不等于失联。已有测试机数据不应按旧流程清除。

`tools/maint/verify_sources.py` 校验当前源码清单、语法、链接和本地依赖；`tools/maint/refresh_manifest.py` 只更新哈希清单，不打包、不上传。功能验收仍以 [VERIFICATION.md](VERIFICATION.md) 为准。
