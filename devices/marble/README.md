# 小米 `marble`（Redmi Note 12 Turbo / POCO F5）设备目录

本目录存放 `marble`（高通 **SM7475 / Snapdragon 7+ Gen 2**，`taro`/`ukee` 平台）的实机验证硬件基线、Linux 5.10 内核符号导出补丁、构建脚本与后续运行时配置。

## 已实机验证的关键硬件特征（Phase 0 基线）

- **内核基线**：开源内核选用 [`Pzqqt/android_kernel_xiaomi_marble`](https://github.com/Pzqqt/android_kernel_xiaomi_marble)（`bouquet_ci` 分支，Linux `5.10.274`，`marble_defconfig`，Slim LLVM 23.1.6，启用 `CONFIG_CFI_CLANG` + `CONFIG_LTO_CLANG_THIN` + `CONFIG_MODVERSIONS`）。
- **引导分区**：无 `init_boot` 分区；`boot_a`/`boot_b` = **192 MiB**（`201326592` 字节，含内核 + generic ramdisk），`vendor_boot_a`/`vendor_boot_b` = **96 MiB**（`100663296` 字节，存放厂商模块与 `/compat` 载荷），独立 `recovery_a`/`recovery_b` = **100 MiB**（`104857600` 字节）。
- **`super` 分区**：精确 **9 GiB**（`9663676416` 字节，`/dev/block/sda32`）。
- **显示**：单 DSI `card0-DSI-1`（`qcom,mdss_dsi_m16t_36_0d_0b_dsc_vid`，OLED），模式 `1080x2400` @ `30/60/90/120Hz`，背光 `/sys/class/backlight/panel0-backlight`（`max_brightness=4095`），无需双 DSI 分屏。
- **GPU**：**Adreno 725**（`Adreno725v1`，`gpucc-waipio.ko` + `msm_kgsl.ko`，固件 `a730_sqe.fw` / `gmu_gen70000.bin`，Mesa Turnip + KGSL）。
- **USB**：`a600000.ssusb` / `a600000.dwc3`（`mode=peripheral`），驱动包含 `dwc3-msm.ko`、`phy-msm-snps-eusb2.ko`、`phy-msm-snps-hs.ko`、`phy-msm-ssusb-qmp.ko`、`repeater-i2c-eusb2.ko`、`nb7vpq904m.ko`、`pmic_glink.ko`、`ucsi_glink.ko`。
- **触控**：`goodix_core.ko` + `xiaomi_touch.ko`，内核原生注册 Linux `evdev` 输入节点 `goodix_ts`（无需 `sheng` 的 THP 用户态桥接）。
- **无线与音频**：Wi-Fi `qca_cld3_qca6490.ko`（固件 `/vendor/firmware_mnt/image/qca6490/`）+ BT `btpower.ko`/`bt_fm_slim.ko`/`msm_geni_serial.ko`；音频声卡 `ukee-mtp-snd-card`（`aw882xx_dlkm.ko` + `wcd938x_dlkm.ko` + `spf_core_dlkm.ko`，`remoteproc0 = 3000000.remoteproc-adsp`）。
- **蜂窝 / 相机 / NFC / 震动**：高通 MPSS + IPA v5.0 / QMAPv5（5G NR SA / 4G LTE / VoLTE / VoNR / IMS 短信）；4 颗物理镜头（后置 OV64B40 64MP + GT9764 对焦马达、前置 S5K3P9、超广角 IMX355、微距 GC02M1）与双色温闪光灯；NXP SN110T NCI 2.0 NFC（`/dev/nq-nci`）；`qcom-hv-haptics` 线性马达（`/dev/input/event6`）。
- **传感器与供电**：传感器位于独立 SLPI（`remoteproc3 = 2400000.remoteproc-slpi` + `sensors_ssc.ko`，含 `tcs3701` ALS 与 `tilt_to_wake`）；快充节点 `/sys/class/qcom-battery/` 与小米 `mi_thermald`（`mi_thermal_interface.ko`）均已适配。

## 目录结构

- [`device.json`](device.json)：机器可读的 `marble` 实机硬件与内核规格定义。
- [`scripts/collect-marble-baseline.sh`](scripts/collect-marble-baseline.sh)：Phase 0 只读实机信息采集脚本。
- [`scripts/backup-marble-boot.sh`](scripts/backup-marble-boot.sh)：Phase 2 只读分区备份脚本（通过已 Root ADB 导出 `boot`/`vendor_boot`/`dtbo`/`vbmeta*` 并做双向 SHA-256 校验，同时解包分析引导头结构）。
- [`boot/marble_init_entry.c`](boot/marble_init_entry.c)：Phase 2 极简无 libc PID1 入口（放入 `boot.img` ramdisk，将 `vendor_boot` 中的 `/hmos-compat-marble/*` 链接至根并转交 `hmos-probe`）。
- [`kernel/marble-exports.patch`](kernel/marble-exports.patch) 与 [`kernel/marble-exports/`](kernel/marble-exports/drivers/android/marble_hmos_exports.c)：为 `Pzqqt/android_kernel_xiaomi_marble` 注入带 CFI 类型检查的 `hmos_ipc_api_v1` 内核导出表。
- `runtime/`：按子系统组织的 `marble` 运行时脚本与配置（`anco/`、`aod/`、`audio/`、`bluetooth/`、`camera/`、`init/`、`location/`、`motion/`、`nfc/`、`pinauth/`、`power/`、`sensors/`、`telephony/`、`thermal/`、`touch/`、`vibrator/`、`wifi/`），与 19-host HDF 配置配合使用。
- 构建与打包脚本：[`../../tools/build/build_marble_kernel.sh`](../../tools/build/build_marble_kernel.sh)、[`../../tools/packaging/build_marble_phase4_images.py`](../../tools/packaging/build_marble_phase4_images.py) 及批次打包脚本（最新 [`../../tools/packaging/build_marble_h20_m5_fixes.py`](../../tools/packaging/build_marble_h20_m5_fixes.py)）。
- GitHub Actions 工作流（按需手动触发）：[`../../.github/workflows/build-marble-kernel.yml`](../../.github/workflows/build-marble-kernel.yml)。

最新真机装机状态见 [`../../docs/MARBLE_HANDOFF.md`](../../docs/MARBLE_HANDOFF.md)，分批适配记录见 [`../../docs/MARBLE_BATCH_PLAN.md`](../../docs/MARBLE_BATCH_PLAN.md)，总体架构规划见 [`../../docs/MARBLE_PORTING_PLAN.md`](../../docs/MARBLE_PORTING_PLAN.md)。
