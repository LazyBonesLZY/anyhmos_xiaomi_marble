# 当前功能源码 / Current component sources

每个功能只保留当前已验证基线中最完善的实现。选择依据是实际刷入的 v146 载荷和分项验收，不是版本号最大。旧试验、旧实现、逐版构建链已从当前仓库树删除。完整逐文件来源见 `SOURCE-SELECTION.json`。

数据分区挂载与持久日志检查已在该基线上修正，支持正常格式化的 ext4；此修复已通过离线测试，尚待真机刷入。参见 [USERDATA.md](USERDATA.md)。

One selected implementation is retained per component, together with the shared code, generated interfaces and upstream patches it needs. Incomplete features remain incomplete; the rebuilt-runtime v146 acceptance is documented in docs/VERIFICATION.md.

| 功能 / Component | 当前源码 / Current sources | 选取依据 / Selection |
| --- | --- | --- |
| 内核接口 / Kernel | `devices/marble/kernel/`（及参考 `devices/sheng/kernel/`） | 匹配内核配置的类型化 IPC 导出表（`hmos_ipc_api_v1`）。 |
| Binder / AccessToken | `compat/ipc/` | 实际使用的类型化导出与单向发送方 PID 实现。 |
| 显示、亮度、熄屏 / Display, brightness, screen power | `compat/display/composer_vdi.cpp`, `native_display_wake.cpp`, `native_refresh_rate.cpp` | 直扫/帧节奏/144Hz 模式切换与 OFF latch；唤醒 helper。 |
| Buffer / 媒体元数据 | `compat/display/buffer_vdi.cpp` | v130，包含 crop/YUV/RGB 与全局 alpha 支持。 |
| GPU / EGL / 同步 | `compat/graphics/`（`mesa-turnip.patch`、`mesa-gallium-egl.patch`、`dmabuf_sync/`） | 当前冻结 Turnip/Gallium-EGL 配置及 dma-buf 同步内核模块（旧补丁归档于 `archive/sheng/legacy-mesa/`）。 |
| 触控与双击亮屏 / Touch & DT2W | `compat/touch/`, `devices/marble/runtime/touch/` | `marble` 走内核原生 `goodix_ts` evdev + `marble_touch_wakeup`（`xiaomi_touch` `POLLPRI` → uinput `event10`）；`sheng` 保留 service-v2 bridge。 |
| 音频与通话语音 / Audio & Voice | `compat/audio/`, `devices/marble/runtime/audio/` | VDI、PAL server/client/lifecycle（含 `PAL_STREAM_VOICE_CALL` 听筒/免提/BT SCO）、服务发布及 PD mapper。 |
| Wi-Fi 与热点 / Wi-Fi & SoftAP | `compat/wifi/`, `devices/marble/runtime/wifi/` | QCA6490 STA 与 `hostapd_host` SoftAP 个人热点。 |
| 蓝牙 / Bluetooth | `compat/bluetooth/`, `devices/marble/runtime/bluetooth/` | QCA6490 HCI/A2DP 桥接与检查接口。 |
| 蜂窝、IMS 短信与 VoLTE/VoNR / Telephony | `compat/telephony/`, `devices/marble/runtime/telephony/` | `hmos_rmtfs`、`hmos_tqftpserv`、`libril_vendor_qmi.z.so`（5G/4G/IPA v5.0/QMAPv5/CS 通话去重）与 `hmos_ims_core_service`（`SA 4014` IMSA + IMS SMS `fmt=0x06` + `ImsCallInterface` `QMI_VOICE`）。 |
| 相机与手电筒 / Camera & Torch | `compat/camera/`, `devices/marble/runtime/camera/` | SM7475 CamX/ISP 4 颗物理镜头（OV64B40 全视场 4624×3472 直转 NV21 + GT9764 CDAF）、`camera_host` V1_2 与手电筒。 |
| 硬件视频编解码 / Codec | `compat/codec/` | 高通 Venus `libOMX_Core.z.so`（H.264/HEVC `DYNAMIC_HANDLE` 硬编解码）。 |
| 定位与 NFC / GNSS & NFC | `compat/location/`, `compat/nfc/`, `devices/marble/runtime/{location,nfc}/` | `QMI_LOC` GNSS HDI 与 NXP SN110T NCI 2.0 NFC HDI/`SA 1140`。 |
| 电池、电源与温控 / Battery, Power & Thermal | `compat/battery/`, `compat/power/`, `devices/marble/runtime/{power,thermal}/` | 快充适配器 HDI、Power/Thermal HDI 与小米 `mi_thermald` 运行时包装。 |
| 传感器、抬起亮屏与震动 / Sensors, Motion & Vibrator | `compat/sensors/`, `compat/motion/`, `compat/vibrator/`, `devices/marble/runtime/{sensors,motion,vibrator}/` | SLPI/SSC 传感器与 `tcs3701` 自动亮度、Motion HDI v1.1 `tilt_to_wake` 抬起亮屏与 `qcom-hv-haptics` Vibrator HDI v2.0。 |
| 账户、数据与 UserIAM/PIN / Account, Data & PinAuth | `compat/framework/`, `compat/useriam/`, `compat/pinauth/`, `devices/marble/runtime/pinauth/` | `libhmos_ram_no_tee.so`（EL2 未加密）、UserIAM 4.0/4.1 与纯用户态 64B/96B `pin_auth` v3.0。 |
| AOD / 实况窗 / ANCO 运行时 | `devices/marble/runtime/{aod,anco,init}/` | `aod_service`（`SA 65840`）、`foundation` Reminder（`SA 3204`）与 ANCO 本地服务配置。 |
| 启动、按键与桌面 / Startup and desktop | `compat/bootstrap/`, `compat/services/`, `devices/marble/runtime/` | 最小入口、`hmos-probe` USB ACM 串口、锁屏 helper 与开机动画。 |
| 宿主机调试与测试 / Host tools & tests | `host-tools/*.ps1`, `tools/`, `tests/` | 串口/传输/截图、分区备份恢复、编译打包脚本与离线单元测试。 |

`runtime/` 中脚本和配置从 v146 ramdisk 的最终内容提取，文件名保留被 init 和启动程序引用的名称。文件名中的 v116/v136 等不表示仓库同时维护多套旧实现。原厂库、固件、应用、编译产物和完整镜像不在仓库内。

Runtime names are preserved for compatibility with the accepted image. Interface generations, shared helpers and staged patches needed by the current system are dependencies, not alternate historical implementations. Removed experiments remain recoverable from Git history; the current tree contains the selected sources only.
