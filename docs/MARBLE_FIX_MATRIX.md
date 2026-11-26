# marble 修复矩阵 / Fix matrix（2026-11-23）

每一行都必须能回答三个问题：**改在哪、怎么判、现在什么状态**。
状态只允许四种：`device`（真机验证过）、`static`（只在镜像/源码里核对过）、
`runtime`（在运行系统上手建等效验证过，重启即失效）、`open`（仍未解决）。

> 下方是早期样本的历史记录。最新装机与验收见[交接进度](MARBLE_HANDOFF.md)和[读回指纹](MARBLE_EXPECTED_AFTER_FLASH.md#当前装机h20-yuv-native)。

| # | 修复 | 落在哪 | 判据（真机上看什么） | 状态 |
| --- | --- | --- | --- | --- |
| 1 | 单 DSI 1080×2400 面板点亮 + SceneBoard 桌面 + 触摸 + DPI/状态栏 | `composer_vdi.cpp`、`display_manager_config.xml`、Turnip/Zink | `card0-DSI-1=enabled`、`SCBDesktop8 [0 0 1080 2400]`、用户可见桌面 | `device` |
| 2 | `hisysevent` 不再 `errno=2` | `hisysevent_sink.c` + stage `11e` + start 监督块 | `SOCK hisysevent≥1`、`HISYSEVENT_ERRNO2=0`、`SINK_PID=alive` | `device`（sink 曾静默死掉，监督块为 `static`） |
| 3 | 亮度持久化 + 关自动亮度 | `brightness_persist.c`、seed 里 `auto_screen_brightness=0`、start `wake_unlock` 后 apply×2 | `BRIGHT saved=40 now=40` | `runtime`：真机 chroot 内 `apply` 返回 `ok want=40 observed=40 previous=3`（把 3 改回 40 并回读一致）；开机时机仍待刷机验证 |
| 4 | `/vendor/etc` 可写（音频 XML/ACDB/kvh2xml） | stage `11d`：tmpfs upper + overlay + 拷贝 `audio-marble` | `MARBLE_VENDOR_ETC_OVERLAY xml=14 kv=1` | `runtime`（真机等效验证；镜像路径 `static`） |
| 5 | AGM 的 HIDL 绕行 | `libhmos_agmipc_shim.so`（48 trampoline → `libagm`） | `AGMIPC_SHIM_INIT_OK direct=48 fallback=0` | `device` |
| 6 | AGM 硬编码板名 `Forte` | stage 建 `/vendor/etc/acdbdata/Forte` + `/data/vendor/audio/acdbdata` | 错误从 `Error:22` 推进到 `gsl_init` 层 | `device` |
| 7 | DSP 启动前挂载 `dsp_a` | stage：`boot_adsp` 之前 mount `/dsp` | `MARBLE_DSP_FS_MOUNTED mods=41`、`PAL_SERVER_AGM_INIT_RC` 不再 -114 | `static`（**关键待验证项**） |
| 8 | 蓝牙固件/节点 | stage `11d2`：`bluetooth_a` 只读挂载 + `ttyHS0`/`btpower` 节点 | `MARBLE_BT_FIRMWARE_MOUNTED files=3`、`CHECK_RESULT=ok` | `device`（自检 ok）；`HCI_TIMEOUT 0xfc00` 仍 `open` |
| 9 | 蓝牙 EDL 重试 | `marble_bt_bridge.c`：上电 250 ms + `tcflush` + 3 次重试 | `EDL_VERSION_ATTEMPT n=…`、`BRIDGE_LISTENING=1` | `static` |
| 10 | HDC（USB FunctionFS） | stage 建 gadget + `hdcd`；start `6f`（marker 门控） | `HDC daemon=<pid> ffs_ready=1`、`/dev/usb-ffs/hdc` 有 `ep0..ep2` | `device`（设备侧）；缺主机 `hdc` 客户端 `open` |
| 11 | 通知 ANS(3203) 与实况窗 | `foundation-marble.json`（sheng 10 SA + donor 3203/3204），stage 覆盖 `/system/profile/foundation.json` | `SA 3203=ready`、SceneBoard 不再 `Failed to connect to the service` | `static`（镜像内已核对 12 个 SA） |
| 12 | 隐私全权限 + 指示器 | `privacy_service` 按 donor 18 权限/6 ACL/`critical[1,4,240]`；+ `super/security_privacy_server` | `SA 3505=ready`、`SA 66090=ready` | `runtime`（旧权限下已 READY）；新权限 `static` |
| 13 | 存储三件套 | `disk_manager`(8640)、`StorageSpaceMgr`(8650)、`device_storage_manager_service`(5000) | 三个 `SA …=ready`、Settings「存储」有数据 | `static`（服务定义/uid/secon 照 donor） |
| 14 | HAP 安装权限 | `sys_installer_sa`(4101) 补入；`installs` 去重 | `SA 4101=ready`、`bm install` 不再 `9568266` | `static`；错误码语义已定性 `device` |
| 15 | 录音 capture 节点 | stage `11c`：枚举 `/sys/class/sound/pcmC0D*` 全量建节点 | `SND total=43 capture=20` | `runtime`（手建验证）；镜像路径 `static` |
| 16 | 补齐 20 个系统 SA | `init-platform-marble.cfg`：softbus/ressched/cert/accessibility/backup/deviceauth/deviceprofile/sensors/mdns/media_monitor + file/security/midi/audio_suite + nfc/flashlight/…（分 `extra`/`extra2`/`extra3` 三档） | 各 `SA …=ready`；`dmkit` 1050 条错误应归零 | `static`（默认只开 `extra`） |
| 17 | 启动回归（`$ART` 未定义致 6b–9 全跳） | start `6a` 前定义 `ART` | 开机即进桌面，无需人工补 param | `static`（改为默认路径） |
| 18 | chroot `#!/busybox sh` 失效 | stage `11c2` 拷 `/busybox` 进 `$R/busybox` | `MARBLE_CHROOT_BUSYBOX=ok` | `device`（`rc=127` → 正常执行） |
| 19 | 一键验证 | `compat/verify/marble_verify_all.sh`（2141 B） | 输出 `VERDICT pass/fail` 全表 | `device`（已跑出基线） |
| 20 | 防「整块代码静默丢失」 | `tests/test_marble_boot_blocks.py`（13 项：标记 + 顺序 + 解包断言） | `python3 -m unittest tests.test_marble_boot_blocks` | `static`（已抓到 3 次丢失：11d、蓝牙、USB/HDC） |

| 21 | 服务定义与 donor 逐字段对齐 | `tests/test_marble_service_parity.py`（5 项：身份/权限/ACL/gid/`critical` 策略） | `python3 -m unittest tests.test_marble_service_parity` | `static`（本轮抓到 4 个服务权限丢失并修复：`disk_manager`、`StorageSpaceMgr`、`device_manager`、`hiview`） |

| 22 | 服务路径与 foundation 库存在性 | `tests/test_marble_service_paths.py`（3 项：`/system`·`/vendor` 路径解析、foundation 12 个 libpath、`/compat` 引用在镜像内） | `python3 -m unittest tests.test_marble_service_paths` | `static`（`libwms.z.so` 在 donor 提取与 `/system/lib64` 都查不到，但真机 `native_sa_check 4606 = READY`，属「库被并入其它单元」，已列入 `DEVICE_ONLY_LIBS` 并注明） |

| 23 | 安装/麦克风/采集三条权限链的声明侧 | `tests/test_marble_permission_chain.py`（7 项） | 测试全绿：`sys_installer_sa` 持 `INSTALL_BUNDLE`/`UNINSTALL_BUNDLE`；`privacy_service` 持 `MICROPHONE_CONTROL`/`CAMERA_CONTROL`/`SET_SUPER_PRIVACY`；`accesstoken_service`(SA 3503) 已配；音频策略两套都声明 `Built-In Mic`（donor chip_prod 5 处、marble sku_ukee 8 处） | `static`（声明侧齐；运行时仍需 PAL） |

| 24 | 常驻工具必须在 chroot 内启动 | stage `11e/11f`、start 监督块改为 `chroot "$R" /compat/bin/…`；`tests/test_marble_boot_blocks.py` 增 2 条断言 | 真机逐个 chroot 执行：`hmos_brightness_persist`(`get`/`apply`)、`tee_backend_probe`、`native_codec_probe`、`hmos_bt_bridge_marble --check` 全部 `rc=0` | `runtime`（主机路径启动 musl PIE 会 `rc=127`，这是 sink 静默死掉的直接原因） |
| 25 | 蓝牙自检在 chroot 下通过 | `hmos_bt_bridge_marble --check` | `FIRMWARE_SELECTED rampatch=hpbtfw20.tlv nvm=hpnv20.bin`、`CHECK_ONLY=1 tty_exists=1 power_exists=1 socket_dir_ok=1`、`CHECK_RESULT=ok` | `runtime` |

| 26 | 镜像内二进件必须与重建产物一致 | `tests/test_marble_boot_blocks.py::ImageBinaryFreshnessTests`（2 项：蓝牙桥接、hisysevent sink 的 sha256 必须等于本地构建产物） | 测试全绿；真机侧对照：新镜像里 `hmos_bt_bridge_marble = 62b09485…`（含 EDL 重试），而设备当前跑的是旧镜像里的旧二进制 | `static`（本轮之所以要加这条：旧二进制曾连发数版镜像，"补丁无效"其实是没打进去） |

| 27 | 四个世界的身份台账 | `docs/MARBLE_IDENTITY.md`：源码 / 候选镜像 / 集成目录 / 设备实际加载，各自 SHA256 | 设备实测：`vendor_boot_a=fd576a13…`、`compat/bin/hmos_bt_bridge_marble=1f4d9f9b…`（重建版 `62b09485…`）、`compat/etc/init-platform-marble.cfg=8ffbb2b1…`（源码 `8c5e65cf…`） | `device`（台账本身即实测）；结论：四层都落后，禁止把源码层的"已修"说成设备已生效 |

| 28 | 负向对照（改前/改后） | `docs/MARBLE_NEGATIVE_CONTROLS.md`：14 条修复各自的旧现象原文 + 新判据 | 例：`Error:22`（ACDB 路径）、`rc=127`（主机路径启动 musl PIE）、`saved=40 now=3`（亮度被覆盖）、`9568266`（安装权限）、`1f4d9f9b…`（旧二进制） | `device`/`static` 混合（每条标注验证层） |
| 29 | 刷机后期望值表 | `docs/MARBLE_EXPECTED_AFTER_FLASH.md`：候选 sha、各二进件/配置期望哈希、`verify_all` 期望字段 | `tests/test_marble_verify_all.py::ExpectedValuesDocTests` 断言表格与当前产物同步 | `static`（表格自动跟随，避免过期期望把回归当预期） |

## 当时未关闭的项（历史）

1. **音频 DSP 模块注册**：`Register modules cmd 0x100101e` 被 DSP 以 status 9 拒绝；已把 `dsp_a` 挂载前移（第 7 行），需刷机验证是否只是「模块集在 DSP 启动时不可见」。
2. **蓝牙 EDL**：`HCI_TIMEOUT opcode=0xfc00`，重试补丁未上机。
3. **WiFi supplicant**：`wpa_host` 在跑但 `thread=0`（`WpaMainThread` 未起），需要用户在设置里点一次 WLAN 触发；supplicant 契约已确认（`libwpa.z.so` + `wpa_main@1.0`）。
4. **HDC 主机侧**：设备侧 `ffs_ready=1`，缺 Windows/WSL 的 `hdc` 客户端。
5. **attestation 类**：证书/公钥齐全（29 个），但设备证明需要 TEE 私钥，**不可行**，已定性。

## 刷机后一次可判定的项

`SA 3203 / 3505 / 66090 / 5000 / 8640 / 8650 / 4101 / 4700 / 1901 / 801`、
`BRIGHT now`、`SND total/capture`、`GPR`、`MARBLE_DSP_FS_MOUNTED`、`PAL_SERVER_AGM_INIT_RC`、
`BLUETOOTH_STATUS`（marker 门控）、`HDC_READY`。

以上全部由 `sh /compat/bin/marble_verify_all.sh` 一条命令输出。
