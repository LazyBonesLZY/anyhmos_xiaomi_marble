# 小米 `marble`（Redmi Note 12 Turbo / POCO F5）设备目录

本目录用于存放 `marble`（高通 **SM7475 / Snapdragon 7+ Gen 2**，`ukee` 平台）专属的硬件参数、Linux 5.10 GKI 内核配置/导出补丁、模块加载脚本与运行时配置。

## 关键硬件特征（相对参考机型 `sheng`）

- **内核**：`android12-5.10` GKI（区别于 `sheng` 的 `5.15.178`，IPC 与 `dmabuf_sync` 需适配 5.10 内核 API）。
- **引导分区**：无 `init_boot` 分区；`boot`（~192 MiB）包含内核与 generic ramdisk，`vendor_boot`（~96 MiB）存放厂商模块与 `/compat` 载荷，另有独立 `recovery`（~100 MiB）。
- **`super` 分区**：约 **9 GiB**（`0x240000000` 字节，小于 `sheng` 的 11 GiB）。
- **显示**：6.67 英寸 **1080×2400 OLED 60/90/120Hz 单 DSI**（`composer_vdi.cpp` 需去除 `sheng` 的双 DSI 3048×2032 强制分屏）。
- **GPU**：**Adreno 725**（`a725`，Mesa Turnip + KGSL）。
- **USB**：Type-C **USB 2.0**（无 DP Retimer `ps5169`）。
- **触控**：Goodix Berlin（`goodix_core.ko`）或 FocalTech（`focaltech_touch.ko`）+ `xiaomi_touch.ko`，内核原生 `evdev` 上报（大概率无需 `sheng` 的 THP 用户态桥接）。
- **无线与音频**：WCN6855 / QCA6490（Wi-Fi 6/6E + BT 5.3）、高通 PAL/AGM + ADSP（`ukee` 声卡，双扬声器 + 3.5mm 耳机孔）、高通 SSC 传感器。

## 目录规划

- `device.json`：机器可读的 `marble` 平台规格定义（实机采集后回填精确字节数与版本串）。
- `scripts/collect-marble-baseline.sh`：Phase 0 只读实机信息采集脚本（在已 Root 的原厂系统或 TWRP Shell 中运行）。
- `kernel/`（Phase 1 创建）：`marble` 5.10 `kernel.config` 与内核符号导出补丁。
- `runtime/`（Phase 2~5 创建）：按 `init/`、`profiles/`、`boot/`、`graphics/`、`audio/`、`wifi/`、`bluetooth/`、`sensors/`、`power/` 组织的运行时脚本与配置。

详细分阶段 Bring-up 计划见 [../../docs/MARBLE_PORTING_PLAN.md](../../docs/MARBLE_PORTING_PLAN.md)。
