# 仓库架构与目录规范

本文档说明整理后的仓库分层原则，便于在保留 `sheng` 可验证参考的同时，低耦合地推进 `marble`（以及后续其他高通机型）移植。

## 分层设计

1. **`compat/` — 跨机型鸿蒙兼容层源码**
   - 存放用 C/C++/汇编编写的鸿蒙侧与 Android Bionic 侧适配代码（IPC 内核模块、DRM Composer/Allocator VDI、Mesa 补丁与 `dmabuf_sync`、Audio VDI/PAL、BT/Wi-Fi 桥接、5G/4G QMI RIL 与 IMS Core、CamX/ISP 相机、Venus OMX 硬编解码、GNSS/NFC、SSC 传感器与 Motion 抬起亮屏、Vibrator 震动、UserIAM 与纯用户态 PinAuth、无 TEE Framework、Bootstrap 引导程序）。
   - 移植新机型时优先通过构建宏或运行时探测复用 `compat/`，避免复制整套源码。

2. **`devices/<codename>/` — 机型专属资产隔离区**
   - 每个机型独立维护自己的 `device.json`、`kernel/`（GKI 配置与导出补丁）、`runtime/`（按 `anco/`、`aod/`、`audio/`、`bluetooth/`、`camera/`、`init/`、`location/`、`motion/`、`nfc/`、`pinauth/`、`power/`、`sensors/`、`telephony/`、`thermal/`、`touch/`、`vibrator/`、`wifi/` 细分）与 `scripts/`。
   - `devices/sheng/` 为已验证参考实现；`devices/marble/` 为当前目标机型实现（已完成 Batch M5 实机闭环）。

3. **`patches/` — 受守卫的二进制补丁配方**
   - 仅保留当前在用的 donor / display / init 补丁配方（全文件 SHA-256 + 指令前后文校验），不存放闭源二进制本身。

4. **`tools/` — 按职责拆分的宿主机脚本**
   - `tools/build/`：交叉编译与内核 IPC 模块构建脚本（`build_current.py`、`build_display_drm.py`、`build_ipc_gki.sh`、`build_sensors.py`、`build_useriam.py`）。
   - `tools/packaging/`：华为全量包提取、Donor 审计、Ramdisk 修改、稀疏镜像/Super 组装与服务打包辅助模块。
   - `tools/maint/`：源码清单生成（`refresh_manifest.py`）、全仓完整性与语法/链接校验（`verify_sources.py`）、运行态基线采集脚本。

5. **`host-tools/` — Windows 宿主机调试与救砖工具**
   - 原 `output/` 目录重命名而来，集中存放经 USB ACM 串口收发命令、传输文件、截屏及备份/恢复启动分区的 PowerShell 脚本。

6. **`tests/`、`external/`、`archive/`**
   - `tests/`：离线 Python 单元测试与 C/C++ 契约测试。
   - `external/`：上游版本清单、依赖构建记录与许可证原文。
   - `archive/sheng/`：`sheng` 历史版本报告（`analysis/`、`evidence/`）、旧版 README 与被替代的旧补丁归档。
