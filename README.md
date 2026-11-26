# 小米 marble（Redmi Note 12 Turbo / POCO F5）· HarmonyOS NEXT 移植仓库

[目录架构](docs/ARCHITECTURE.md) · [marble 移植规划](docs/MARBLE_PORTING_PLAN.md) · [构建说明](docs/BUILDING.md) · [源码索引](docs/SOURCE_MAP.md) · [来源与许可](docs/PROVENANCE.md)

本项目旨在基于 [AnyHarmonyOSNext/anyhmos_xiaomi_sheng](https://github.com/AnyHarmonyOSNext/anyhmos_xiaomi_sheng) 已验证的 **“只读 `super` + 内存 `/compat` 叠加”** 架构，将 HarmonyOS NEXT 移植到 **小米 `marble`（Redmi Note 12 Turbo / POCO F5，SM7475 / Snapdragon 7+ Gen 2）**。

仓库已完成目录重构，将**通用鸿蒙兼容层**、**目标机型 `marble`**、**参考机型 `sheng`**、**构建/打包/宿主机工具**与**历史归档**彻底分离。

## 仓库目录结构

| 目录 | 定位与内容 |
| --- | --- |
| `compat/` | 鸿蒙兼容层核心源码（IPC 内核模块、DRM Display VDI、Mesa/dmabuf_sync、Audio VDI/PAL、BT/Wi-Fi 桥接、SSC 传感器、UserIAM、无 TEE Framework、Bootstrap 等）。 |
| `devices/marble/` | **目标机型（`marble`）**：硬件参数定义、Phase 0 实机信息采集脚本、后续 5.10 内核导出与运行时脚本存放区。 |
| `devices/sheng/` | **参考机型（`sheng`）**：原小米平板 6S Pro 12.4 的 5.15 内核配置/导出补丁，以及按子系统归类后的运行时脚本与配置（`init/`、`profiles/`、`boot/`、`graphics/`、`audio/`、`wifi/`、`bluetooth/`、`sensors/`、`power/`）。 |
| `patches/` | 带 SHA-256 与指令上下文守卫的 ELF 二进制补丁配方（`donor/gem-w00-111-sp12/`、`display/`、`init/`）。 |
| `tools/` | 宿主机工程脚本，按职责拆分为 `build/`（组件编译）、`packaging/`（Donor 提取与镜像/Ramdisk 组装）、`maint/`（清单刷新与源码校验）。 |
| `host-tools/` | Windows 宿主机调试工具（原 `output/`），包含 USB ACM 串口命令执行、文件传输、截屏及分区备份/恢复脚本（`*.ps1`）。 |
| `docs/` | 架构说明、marble 移植路线图、构建/打包/Userdata 说明及原 `sheng` 验收范围文档。 |
| `tests/` | Python 打包/守卫单元测试与 C/C++ 显示、IPC、串口契约测试。 |
| `external/` | 上游源码版本记录（`upstream-sources.json`）、依赖构建记录及完整上游许可证（`licenses/`）。 |
| `archive/sheng/` | 原 `sheng` 历史归档（旧版 README、v146 `RELEASE.json`/`SOURCE-SELECTION.json`、历史验收报告 `analysis/`/`evidence/`、被替代的旧版 donor/Mesa 补丁）。 |

## `marble` 移植状态

当前已完成 **Phase 0 实机采集** 与 **Phase 1 5.10 内核/IPC 编译审计**，共享兼容层 `compat/` 已去机型化统一为 `hmos_*` 命名：

- [x] 导入 `sheng` 完整参考基线并完成多机型分层重构
- [x] Phase 0：在真机运行 [collect-marble-baseline.sh](devices/marble/scripts/collect-marble-baseline.sh) 并回填 [devices/marble/device.json](devices/marble/device.json)
- [x] Phase 1：完成 Linux 5.10.274 内核导出补丁（`marble_hmos_exports.c` / `hmos_ipc_api_v1`）、`hmos_ipc.ko`、`hmos_dmabuf_sync.ko`、配套模块与静态 IPC 契约测试编译及 CFI/CRC 审计
- [ ] Phase 2：`boot.img` ramdisk + `vendor_boot.img` 引导与 USB ACM 串口闭环（含实机备份与 IPC 契约验收）
- [ ] Phase 3：9 GiB `super` 打包、ext4 `userdata` 挂载与鸿蒙核心服务拉起
- [ ] Phase 4：单 DSI 1080×2400 桌面和触控已上机；设置、时钟从桌面进入前台已实测，仍有独立子系统待验收。
- [ ] Phase 5：Wi-Fi（QCA6490）、PAL 音频、蓝牙与电池充电连通
- [ ] Phase 6：SSC 传感器、温控、120Hz 刷新率优化与手机 Donor 迁移

**最新真机进度（2026-11-23）：** M15 G 已恢复应用启动，Wi‑Fi 已通过网关与公网 IP 收发实测。存储容量汇总、实况窗到期、HAP 安装、录音及 HDC 传输仍待验收。见[进度与回退](docs/MARBLE_HANDOFF.md#0-当前实机状态)及[真机证据](docs/VERIFICATION.md#m15-g真机应用启动与-wlan-联网)。完整路线见[移植规划](docs/MARBLE_PORTING_PLAN.md)。

**刷写与工件规则（2026-11-24 更新）**：用户允许按适配需要刷写 boot_a 和 vendor_boot_a，保留 userdata 及其它原厂底层分区；不逐轮堆积整镜备份。功能批次、最少恢复基线和依赖清理见[分批计划](docs/MARBLE_BATCH_PLAN.md)。

## 常用维护命令

```sh
# 列出可构建的兼容层目标
python3 tools/build/build_current.py --list

# 运行全部离线 Python 单元测试
python3 -m unittest discover -s tests -p "test_*.py" -v

# 修改源码后刷新清单并执行全仓完整性校验
python3 tools/maint/refresh_manifest.py
python3 tools/maint/verify_sources.py
```

## 许可与致谢

本项目基于 [AnyHarmonyOSNext/anyhmos_xiaomi_sheng](https://github.com/AnyHarmonyOSNext/anyhmos_xiaomi_sheng) 与 [YiKongk/AnyHarmonyOS](https://github.com/YiKongk/AnyHarmonyOS) 等社区移植成果，原始版权、上游来源与许可证边界详见 [docs/PROVENANCE.md](docs/PROVENANCE.md) 与 `external/licenses/`。本仓库不分发闭源系统镜像、原厂固件、应用或 SDK。
