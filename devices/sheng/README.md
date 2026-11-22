# 参考机型 `sheng`（小米平板 6S Pro 12.4，SM8550）

本目录保留原 `sheng` 移植中全部机型专属的内核配置、GKI 符号导出补丁以及按子系统重新归类后的运行时脚本和配置，作为 `marble` 移植的权威对照实现。

## 目录结构

- `kernel/`
  - `kernel.config`：匹配官方 OS3.0.303.0.WNXCNXM 的 Linux 5.15.178 GKI 配置。
  - `gki-sheng-exports.patch` & `gki-sheng-exports/`：向 5.15 GKI 导出鸿蒙 IPC 所需只读 CFI 函数表的内核补丁。
- `runtime/`（由原平铺的 `runtime/compat/graphics/` 与 `runtime/audio/audio/` 重新归类）
  - `init/`：`hmos-init-debug.cfg`、各子系统 `init-*.cfg`、musl 命名空间配置、沙箱与默认设置。
  - `profiles/`：各 SystemAbility / 服务 JSON profile。
  - `boot/`：开机编排脚本（`start-native-desktop-v116.sh`、`init-userdata-ext4.sh`、`mark-boot-success-v135.sh`、`apply-*.sh`、`prepare-*.sh`）。
  - `graphics/`：KGSL 驱动加载脚本与 Turnip Vulkan ICD 配置。
  - `audio/`：音频初始化脚本及 `scripts/` 下的 ADSP/PAL 驱动加载脚本。
  - `wifi/`：`cnss2`、`qrtr-mhi`、`qca_cld3_kiwi_v2` 驱动与 Wi-Fi 启动脚本。
  - `bluetooth/`：蓝牙驱动加载、QTI 桥接启动脚本与服务配置。
  - `sensors/`：FastRPC 与 SSC 传感器启动脚本。
  - `power/`：小米原厂充电认证进程启动脚本。

原 `sheng` 历史验收报告、旧版 README 与被替代的旧补丁已移至 [../../archive/sheng/](../../archive/sheng/README.sheng.md)。
