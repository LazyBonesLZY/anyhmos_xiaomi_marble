# 小米 marble（Redmi Note 12 Turbo / POCO F5）HarmonyOS NEXT 移植规划

本规划基于 `devices/sheng/` 与 `compat/` 已验证的非侵入式架构（**`super` 只读不动，兼容层放 `vendor_boot` 并在开机释放至内存 `/compat` 叠加运行**），针对目标机型 **`marble`（Redmi Note 12 Turbo / POCO F5，SM7475 / Snapdragon 7+ Gen 2）** 制定分阶段移植路线。

## 1. 平台核心差异（`sheng` vs `marble`）

| 维度 | `sheng`（参考） | `marble`（目标） | 关键改动 |
| --- | --- | --- | --- |
| **SoC / 平台** | SM8550 (`kalama`) | SM7475 (`ukee` / `sm8450` 族) | 替换驱动模块链、时钟与固件 |
| **内核与 GKI** | Linux **5.15.178** (`android13-5.15`) | Linux **5.10.x** (`android12-5.10`) | `compat/ipc/` 与 `dmabuf_sync` 反向适配 5.10 API |
| **启动分区** | `boot` + `init_boot` + `vendor_boot` | `boot`（含 generic ramdisk）+ `vendor_boot` + `recovery`，**无 `init_boot`** | 最小 PID1 入口放入 `boot.img` ramdisk，`vendor_boot` 保持 96 MiB 放兼容载荷 |
| **`super` 容量** | 11 GiB | **9 GiB**（`0x240000000` B） | 严格控制 `system`+`vendor`+`sys_prod`+`vendor_dlkm` 体积，`preload` 外置 |
| **屏幕显示** | 3048×2032 120/144Hz **双 DSI 分屏** | **1080×2400** 60/90/120Hz OLED **单 DSI** | `composer_vdi.cpp` 去双 Plane 与固定分辨率硬编码，改为单 Plane / 动态分辨率 |
| **GPU** | Adreno 740 (`gpucc-kalama`) | **Adreno 725** (`a725`) | 换 `marble` `gpucc`/GPU 固件，核对 Mesa Turnip A725 设备定义 |
| **USB 调试** | USB 3.2 + `ps5169` Retimer | **USB 2.0 Type-C**（无 `ps5169`） | 重写 `hmos_probe` 中的 `usbmods[]`，移除 `1-0028` reprobe |
| **触控** | Novatek `nt36532` THP 用户态算法桥接 | Goodix Berlin / FocalTech + `xiaomi_touch` **内核原生 `evdev`** | 可停用 `hmos_thp_bridge`，直接暴露 `/dev/input/eventX` |
| **无线** | WCN7850 (`qca_cld3_kiwi_v2`) | **QCA6490 / WCN6855** (`qcacld-3.0`) | 替换模块链、固件、BT 块设备动态解析与 QTI BT Bionic 库 |
| **音频 / 传感器** | 高通 PAL/AGM + ADSP + SSC | **同架构**（`ukee` 声卡 + 3.5mm 耳机 + SSC） | 复用 VDI/SSC 骨架，仅替换 `marble` Bionic 库、XML 与固件 |

## 2. Linux 5.10 GKI 反向适配重点（已验证通过）

- `compat/ipc/`（`hmos_ipc.ko`）& `devices/marble/kernel/`（`marble_hmos_exports.c` 导出 `hmos_ipc_api_v1`）：
  - `binder_alloc.c` 中的 `vma_lookup()` → `< 5.13` 使用 `find_vma()` + 区间检查
  - `binder_alloc.c` 中的 `memset_page()` / `kmap_local_page()` / `kunmap_local()` → `< 5.11` 使用 `kmap()` / `kunmap()`
  - 保持只读结构体 `EXPORT_SYMBOL_GPL(hmos_ipc_api_v1)` 方式导出，不破坏 CFI 与 `modversions` CRC
- `compat/graphics/dmabuf_sync/kernel/hmos_dmabuf_sync.c`（`hmos_dmabuf_sync.ko`）：
  - `dma_resv_get_fences()` → `< 5.14` 使用 `dma_resv_get_fences_rcu()`

## 3. 分阶段 Bring-up 路线图（均已上机推进至 Batch M5）

- **Phase 0（基线与备份）— 已完成**：备份 `boot`/`vendor_boot`/`dtbo`/`vbmeta*`/`recovery`/`persist`，运行 [collect-marble-baseline.sh](../devices/marble/scripts/collect-marble-baseline.sh) 采集完整实机档案并回填 [devices/marble/device.json](../devices/marble/device.json)。
- **Phase 1（5.10 内核导出 + IPC）— 已完成**：基于 `Pzqqt/android_kernel_xiaomi_marble`（Linux 5.10.274）编译 `Image`、`hmos_ipc.ko`、`hmos_dmabuf_sync.ko`、配套厂商模块与静态 AArch64 IPC 契约测试，全量通过 CFI/CRC 审计。
- **Phase 2（双分区引导 + USB 串口）— 已完成**：`marble_init_entry` 入 `boot.img` ramdisk，`hmos-probe` 与兼容层入 `vendor_boot.img`，USB ACM（COM9）串口命令执行、传输与截屏链路稳定运行。
- **Phase 3（9 GiB `super` + Userdata + OHOS Init）— 已完成**：已直接迁移至手机 Donor `ADY-AL10 207.0.0.111(SP10)`，挂载只读逻辑分区与未加密 ext4 `userdata`（`libhmos_ram_no_tee.so`），拉起 `hilogd` → `samgr` → `hdf_devmgr`（19 个 HDF host）→ `foundation` / `SceneBoard`。
- **Phase 4（单 DSI 显示 + Adreno 725 + 原生触控）— 已完成**：单 DSI 1080×2400 @ 120Hz OLED、Adreno 725 Turnip、Goodix 原生 `evdev` 触控、挖孔居中、色彩/护眼（`SDE_DSPP_PCC_V4`）、开机动画及双击亮屏（`marble_touch_wakeup`）均已上机。
- **Phase 5（无线 + 音频 + 电池充电 + 蜂窝通信）— 已完成**：QCA6490 Wi-Fi/SoftAP、蓝牙、ADSP PAL 音频与通话语音、67W 充电信息、MPSS/IPA v5.0 5G/4G 数据、VoLTE/VoNR `ImsCallInterface` 与 IMS 短信（`fmt=0x06 ims=1`）均已闭环。
- **Phase 6（传感器 + 相机 + NFC + 震动 + 温控 + 密码 + 实况窗）— 已完成（Batch M5）**：SLPI/SSC 自动亮度与抬起亮屏、4 颗物理相机（主摄 OV64B40 全视场 4:3 `4624x3472` + 4 线程直转 NV21 + GT9764 CDAF）、手电筒、Venus 硬编解码、NXP SN110T NFC、线性马达震动、`mi_thermald` 温控、纯用户态 64B/96B 锁屏密码及 AOD（`SA 65840`）/ Reminder（`SA 3204`）实况窗刷新均已实机闭环。剩余已知边界（COM9 救援口与 HDC 互斥、指纹 TrustZone/SPI 限制、卓易通/应用商店华为设备证明与容器镜像缺失）详见 [MARBLE_HANDOFF.md](MARBLE_HANDOFF.md) 与 [MARBLE_BATCH_PLAN.md](MARBLE_BATCH_PLAN.md)。
