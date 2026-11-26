# 小米 marble（Redmi Note 12 Turbo / POCO F5）· HarmonyOS NEXT 移植仓库

[目录架构](docs/ARCHITECTURE.md) · [marble 移植规划](docs/MARBLE_PORTING_PLAN.md) · [构建说明](docs/BUILDING.md) · [打包与一键刷机](docs/PACKAGING.md) · [源码索引](docs/SOURCE_MAP.md) · [来源与许可](docs/PROVENANCE.md)

本项目旨在基于 [AnyHarmonyOSNext/anyhmos_xiaomi_sheng](https://github.com/AnyHarmonyOSNext/anyhmos_xiaomi_sheng) 已验证的 **“只读 `super` + 内存 `/compat` 叠加”** 架构，将 HarmonyOS NEXT 移植到 **小米 `marble`（Redmi Note 12 Turbo / POCO F5，SM7475 / Snapdragon 7+ Gen 2）**。

仓库已完成目录重构，将**通用鸿蒙兼容层**、**目标机型 `marble`**、**参考机型 `sheng`**、**构建/打包/宿主机工具**与**历史归档**彻底分离。

## 仓库目录结构

| 目录 | 定位与内容 |
| --- | --- |
| `compat/` | 鸿蒙兼容层核心源码（IPC 内核模块、DRM Display VDI、Mesa/dmabuf_sync、Audio VDI/PAL、BT/Wi-Fi 桥接、SSC 传感器、UserIAM、无 TEE Framework、Bootstrap 等）。 |
| `devices/marble/` | **目标机型（`marble`）**：硬件参数定义、Phase 0/2 采集与备份脚本、Linux 5.10.274 内核符号导出与 Batch M6 `code_sign`/`fs-verity` 补丁，以及 18 个子系统的运行时脚本与配置。 |
| `devices/sheng/` | **参考机型（`sheng`）**：原小米平板 6S Pro 12.4 的 5.15 内核配置/导出补丁，以及按子系统归类后的运行时脚本与配置（`init/`、`profiles/`、`boot/`、`graphics/`、`audio/`、`wifi/`、`bluetooth/`、`sensors/`、`power/`）。 |
| `patches/` | 带 SHA-256 与指令上下文守卫的 ELF 二进制补丁配方（`donor/gem-w00-111-sp12/`、`display/`、`init/`）。 |
| `tools/` | 宿主机工程脚本，按职责拆分为 `build/`（组件编译）、`packaging/`（Donor 提取与镜像/Ramdisk 组装）、`maint/`（清单刷新与源码校验）。 |
| `host-tools/` | Windows 宿主机调试工具（原 `output/`），包含 USB ACM 串口命令执行、文件传输、截屏及分区备份/恢复脚本（`*.ps1`）。 |
| `docs/` | 架构说明、marble 移植路线图、构建/打包/Userdata 说明及原 `sheng` 验收范围文档。 |
| `tests/` | Python 打包/守卫单元测试与 C/C++ 显示、IPC、串口契约测试。 |
| `external/` | 上游源码版本记录（`upstream-sources.json`）、依赖构建记录及完整上游许可证（`licenses/`）。 |
| `archive/sheng/` | 原 `sheng` 历史归档（旧版 README、v146 `RELEASE.json`/`SOURCE-SELECTION.json`、历史验收报告 `analysis/`/`evidence/`、被替代的旧版 donor/Mesa 补丁）。 |

## `marble` 移植状态（2026-11-27 · Batch M6 实机基线）

当前已完成 **Phase 0 ~ Phase 6** 主线移植与 **Batch A ~ M6** 分批功能适配，真机 A 槽当前装机基线为 **Batch M6**（`boot_a = f80fe1b1…`、`vendor_boot_a = 577e5a5e…`，保留 `userdata` 及原厂底层分区，`libhmos_ram_no_tee.so` 保持 EL2 用户数据不加密，19 个 HDF 宿主常驻）：

- [x] 导入 `sheng` 参考基线并完成多机型分层重构（`compat/` 与 `devices/marble/` 解耦）
- [x] Phase 0：在真机运行 [collect-marble-baseline.sh](devices/marble/scripts/collect-marble-baseline.sh) 并回填 [devices/marble/device.json](devices/marble/device.json)
- [x] Phase 1：完成 Linux 5.10.274 内核导出补丁（`marble_hmos_exports.c` / `hmos_ipc_api_v1`）、`hmos_ipc.ko`、`hmos_dmabuf_sync.ko` 与 CFI/CRC 审计
- [x] Phase 2：`boot.img` 最小入口 + `vendor_boot.img` `/compat` 载荷与 USB ACM（COM9）串口闭环
- [x] Phase 3：9 GiB `super`（手机 Donor `ADY-AL10 207.0.0.111(SP10)`）+ ext4 `userdata` + OHOS 核心服务拉起
- [x] Phase 4：单 DSI 1080×2400 OLED（`120Hz` + 挖孔/色彩/护眼）+ Adreno 725 Turnip + Goodix 原生触控 + 桌面与开机动画
- [x] Phase 5：QCA6490 Wi-Fi/SoftAP、蓝牙（HCI/A2DP）、ADSP PAL 音频与通话语音、67W 快充信息与 `mi_thermald` 温控
- [x] Phase 6：SLPI/SSC 传感器（含自动亮度与抬起亮屏）、5G/4G 蜂窝数据/VoLTE/VoNR/IMS 短信、4 颗物理相机与手电筒、Venus 硬编解码、NFC、线性马达震动、双击亮屏、纯用户态锁屏密码（64B/96B）、AOD/实况窗服务，以及 **Linux 5.10.274 原生内核 `code_sign` + `fs-verity` + ECDSA（NIST P-256/P-384）代码签名与真机 HAP 完整验签安装（Batch M6）**

### 功能实机状态一览（如实区分已实测、需交互确认与硬件/安全边界）

| 子系统 | 当前状态（Batch M6 · `f80fe1b1` / `577e5a5e`） | 说明与边界 |
| --- | --- | --- |
| 内核代码签名 / HAP 安装 | **已实机验证（无绕过）** | 移植 OpenHarmony 内核 `fs/code_sign/`（`/dev/code_sign` 5 项 ioctl）、`FS_IOC_ENABLE_CODE_SIGN`（ext4 + f2fs）、ECDSA NIST P-256/P-384 与 PKCS#7 `OID_ownerid`；657 个厂商模块 KABI CRC/vermagic 0 冲突；移除旧 Phase 3 `libcode_sign_utils.z.so` 旁路并无损启用 `userdata` ext4 `RO_COMPAT_VERITY`（`0x807b`）。AGC 调试签名 HAP（DeviceInfoX 10/10 文件、CoolApk 7.1.5 11/11 文件）真机 `bm install` 均通过，全部 `.hap` 与 `.so` 均带 `lsattr` `V`（`FS_VERITY_FL`）且只读不可篡改；未签名与 1 字节篡改负控均被严格拒绝。 |
| 显示 / GPU / 桌面 | **已实机验证** | 1080×2400 单 DSI 120Hz、Adreno 725 Turnip、挖孔居中对齐、`SDE_DSPP_PCC_V4` 色温/护眼、无卡顿异步提交。 |
| 蜂窝网络 / 5G / 流量 | **已实机验证** | MPSS + IPA v5.0 / QMAPv5（`rmnet0` 锁定 `llp=2, ul=5, dl=9`），5G NR SA / 4G LTE 注网、上网与状态栏信号/网速正常。 |
| 短信（IMS SMS） | **已实机验证** | `hmos_ims_core_service` 修正 `QMI_WMS_BIND_SUBSCRIPTION (0x004c)` 及 3GPP `SMS-SUBMIT` (`tech=1`) 路由，实机首包 `fmt=0x06 ims=1 rc=0 qerr=0x0 ok=1 ref=193` 经运营商网络受理。 |
| 语音与 HD 通话（VoLTE/VoNR） | **链路已验证 / 待实测外呼** | 已实现 `ImsCallInterface` (`QMI_VOICE` `CALL_TYPE_VOICE_IP=0x08`) 与 `libril_vendor_qmi` CS/IMS 去重，`CellularCall` 实机回执 `GetImsCallProxy success`、`active 1, isVolteSupport 1`，PAL 语音通路就绪；真实双端语音清晰度待用户实拨确认。 |
| 相机与手电筒 | **已实机验证** | 4 颗物理光学镜头（OV64B40 / S5K3P9 / IMX355 / GC02M1）+ 手电筒 `V1_2` 全通；主摄 OV64B40 默认 4:3 全视场 `4624x3472`，零拷贝 4 线程直转 NV21（SM7475 实测 `6.5ms/帧`）+ GT9764 CDAF 自动对焦（清晰度 `81→1092`）。 |
| 震动与手势触感 | **已实机验证** | `vibrator_host` 提前于 `SA 3602` 启动并保护 `event6`（`qcom-hv-haptics` `3034:3034`），`SceneBoard` 手势 `IsSupportEffect` 返回 `state:1`，短脉宽 LRA 预设调优至 22–28ms / 70–85%。 |
| 双击亮屏 / 抬起亮屏 / 自动亮度 | **链路已验证 / 待手势体验** | `marble_touch_wakeup` 自动建立 `event10` 且 `multimodalinput` 启动即持有 fd；`motion_host` 通过 DRM `DPMS=Off` 实测触发 SLPI `tilt_to_wake` 并带 `PICKUP_RESET` 去重重置；ALS `tcs3701` 暗/亮室自动调光已通过。 |
| 锁屏密码（PIN） | **已修复并解救旧密码** | 兼容供体 64B 与 96B（64B Scrypt + 32B SHA256）`pinData`，不再报 `bad pinData len!`；保持 EL2 `userdata` 不加密。 |
| 实况窗刷新（AOD / Reminder） | **已实机验证** | 新增 `aod_service`（`SA 65840`）并在 `foundation` 启用 `SA 3204`（`libreminder.z.so`），`SceneBoard` 加载 `SA 65840` 耗时 `0ms`，消除 120s 通知订阅超时。 |
| Wi-Fi / 热点 / 蓝牙 / 音频 / GNSS / NFC / 硬编解码 | **已实机验证** | QCA6490 Wi-Fi 与 SoftAP `hostapd_host`、蓝牙、ADSP PAL 放音、GNSS 定位、NXP SN110T NFC `SA 1140`、Venus H.264/HEVC 环回与小米 `mi_thermald` 均已合入并通过回归。 |
| USB 调试（HDC） | **默认保持 COM9 救援口** | 当前物理 USB 固定为 COM9 ACM 串口救援通道，未自动切换 HDC 组合口，以免丢失串口访问。 |
| 指纹识别 | **已知硬件边界** | `fpc1540`/`goodix_3626` SPI 归属 QSEE TrustZone 且无小米签名 TA，暂不可用。 |
| 卓易通（ANCO）/ 华为商店设备证明 | **已知安全边界（不绕过）** | 本地 ANCO 服务（`SA 3006/65601/66816/66818/66854`）稳定在线不再引发死机；因无华为原厂设备证书及预置 `agi.img`/`anco_hmos.img` 容器镜像，云端设备证明与容器下载校验无法通过且本项目不作绕过。 |

详见[当前现场与回退](docs/MARBLE_HANDOFF.md)、[分批里程碑记录](docs/MARBLE_BATCH_PLAN.md)、[刷机后期望值](docs/MARBLE_EXPECTED_AFTER_FLASH.md)与[验证范围](docs/VERIFICATION.md)。

**刷写与工件规则**：按适配需要仅刷写 `boot_a` / `vendor_boot_a`（刷完立刻 `set_active a`），严格保留 `userdata` 与全部原厂底层分区（`abl`/`xbl`/`modem*`/`dsp*`/`bluetooth*`/`persist`/`recovery*`/`dtbo*`）。

## Fastboot 一键刷机包组装与使用

本仓库不直接托管闭源 `.img` 大镜像，而是提供跨平台自校验打包器 [assemble_marble_fastboot_package.py](tools/packaging/assemble_marble_fastboot_package.py) 与刷机脚本（Windows [Flash-Marble.ps1](tools/packaging/Flash-Marble.ps1) + 双击 `.bat` 封装、Linux/macOS [flash-marble.sh](tools/packaging/flash-marble.sh)），支持两种形态：

- **增量升级包（Upgrade，推荐给已有 AnyHMOS 用户）**：仅含 `boot.img` + `vendor_boot.img`，刷入 `boot_a`/`vendor_boot_a` + `set_active a`，**完全保留 `super` 与 `userdata`**。
- **完整一键刷机包（FullInstall，首次从原厂 MIUI/HyperOS 迁入）**：包含 `boot.img` + `vendor_boot.img` + `vbmeta.img` + `vbmeta_system.img` + 9 GiB 稀疏 `super.img`，刷写前强校验 `product == marble`、`unlocked == yes`、`is-userspace == no` 与每个文件 SHA-256；默认不触碰 `userdata`（需首次从加密 Android 迁移时才显式启用 `-FormatUserdataExt4` / `--format-userdata-ext4` 并二次键入确认）。

```sh
# 组装完整 Fastboot 一键刷机包（省略 --super/--vbmeta* 则生成轻量增量升级包）
python3 tools/packaging/assemble_marble_fastboot_package.py \
  --out /path/to/anyhmos-marble-m6-fastboot-full \
  --boot /path/to/boot_m6.img \
  --vendor-boot /path/to/vendor_boot_m6.img \
  --super /path/to/super_marble.img \
  --vbmeta /path/to/vbmeta_a.img \
  --vbmeta-system /path/to/vbmeta_system_a.img \
  --platform-tools /path/to/platform-tools \
  --device-boot-tested --boot-id 52d5c34b-ba82-46a6-8679-2b1099e2e688
```

详见 [打包与一键刷机说明](docs/PACKAGING.md)。

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
