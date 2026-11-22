# 当前功能源码 / Current component sources

每个功能只保留当前已验证基线中最完善的实现。选择依据是实际刷入的 v146 载荷和分项验收，不是版本号最大。旧试验、旧实现、逐版构建链已从当前仓库树删除。完整逐文件来源见 `SOURCE-SELECTION.json`。

数据分区挂载与持久日志检查已在该基线上修正，支持正常格式化的 ext4；此修复已通过离线测试，尚待真机刷入。参见 [USERDATA.md](USERDATA.md)。

One selected implementation is retained per component, together with the shared code, generated interfaces and upstream patches it needs. Incomplete features remain incomplete; the rebuilt-runtime v146 acceptance is documented in docs/VERIFICATION.md.

| 功能 / Component | 当前源码 / Current sources | 选取依据 / Selection |
| --- | --- | --- |
| 内核接口 / Kernel | `devices/sheng/kernel/`（及未来的 `devices/marble/kernel/`） | 匹配官方配置的 GKI 与类型化 IPC 导出。 |
| Binder / AccessToken | `compat/ipc/` | 实际使用的类型化导出与单向发送方 PID 实现。 |
| 显示、亮度、熄屏 / Display, brightness, screen power | `compat/display/composer_vdi.cpp`, `native_display_wake.cpp`, `native_refresh_rate.cpp` | 直扫/帧节奏/144Hz 模式切换与 OFF latch；唤醒 helper。 |
| Buffer / 媒体元数据 | `compat/display/buffer_vdi.cpp` | v130，包含 crop/YUV/RGB 与全局 alpha 支持。 |
| GPU / EGL / 同步 | `compat/graphics/`（`mesa-turnip.patch`、`mesa-gallium-egl.patch`、`dmabuf_sync/`） | 当前冻结 Turnip/Gallium-EGL 配置及 dma-buf 同步内核模块（旧补丁归档于 `archive/sheng/legacy-mesa/`）。 |
| 触控 / Touch | `compat/touch/` | `sheng` service-v2 bridge、原坐标解析/过滤及 launcher（`marble` 计划走内核原生 evdev）。 |
| 音频 / Audio | `compat/audio/`, `devices/sheng/runtime/audio/` | VDI（含实验采集）、PAL server/client/lifecycle、服务发布及 PD mapper。 |
| Wi-Fi | `compat/wifi/`, `devices/sheng/runtime/wifi/` | 当前连接路径与 v134 真实协议代际适配。 |
| 蓝牙 / Bluetooth | `compat/bluetooth/`, `devices/sheng/runtime/bluetooth/` | v139 当前检查接口、v136 Bionic 后端，以及 v141 能力纠正。 |
| 电池与温控 / Battery and thermal | `compat/battery/`, `compat/power/`, `devices/sheng/runtime/power/` | 真电量/快充适配器 HDI、原厂充电与温控服务兼容。 |
| 传感器 / Sensors | `compat/sensors/`, `devices/sheng/runtime/sensors/` | 高通 SSC（加速度/陀螺仪）后端与 HDI 服务。 |
| 账户与数据 / Account and database | `compat/framework/account/`, `compat/framework/database/` | account HMAC 和 storage launcher、database v2。 |
| UserIAM | `compat/useriam/` | 软件 4.0 backend + 4.1 ABI adapter；两层协同调用。 |
| 服务等待与内存 / Service waits and memory | `compat/framework/memmgr/`, `devices/sheng/runtime/boot/apply-memmgr-v2.sh`, `apply-msdp-local-v1.sh` | 实际加载的修补脚本与汇编。 |
| 启动、按键与桌面 / Startup and desktop | `compat/bootstrap/`, `compat/services/`, `devices/sheng/runtime/` | 最小入口、v121 USB probe、启动声音、锁屏 helper 与 PC 模式默认配置。 |
| BootControl | `compat/bootcontrol/`, `devices/sheng/runtime/boot/mark-boot-success-v135.sh` | 原 HAL 与桌面验收门槛。 |
| 宿主机调试与测试 / Host tools & tests | `host-tools/*.ps1`, `tools/`, `tests/` | 串口/传输/截图、分区备份恢复、编译打包脚本与离线契约测试。 |

`runtime/` 中脚本和配置从 v146 ramdisk 的最终内容提取，文件名保留被 init 和启动程序引用的名称。文件名中的 v116/v136 等不表示仓库同时维护多套旧实现。原厂库、固件、应用、编译产物和完整镜像不在仓库内。

Runtime names are preserved for compatibility with the accepted image. Interface generations, shared helpers and staged patches needed by the current system are dependencies, not alternate historical implementations. Removed experiments remain recoverable from Git history; the current tree contains the selected sources only.
