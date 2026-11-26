# 验证范围 / Verification scope

下列记录来自本项目本地真机测试记录；原始设备日志、应用遥测、网络标识及屏幕内容未收入公开包。构建报告和文件哈希可以帮助识别产物，不能独立代替实物功能验收。

The following summarizes local device tests. Private raw logs, app telemetry, network identifiers and screenshots are excluded. Build reports and hashes identify artifacts; they do not independently establish hardware functionality.

## Marble Batch M6（2026-11-27）：内核 `code_sign` / `fs-verity` / ECDSA 移植与真机 HAP 完整验签安装

当前真机基线 `boot_a = f80fe1b1e47dcae5c759beb7f8005279beb79804b6414da40f3379aea423ee6a` + `vendor_boot_a = 577e5a5eecf784580d64960757f5d7a271d186dac27f63358eb7d5c14281bc45`（冷启动 `BOOT_ID = 52d5c34b-ba82-46a6-8679-2b1099e2e688`，保留 `userdata` 及原厂底层分区）：

- **KABI 与构建审计**：Linux `5.10.274-Bouquet-anyhmos #4` 移植 OpenHarmony `fs/code_sign/`、`FS_IOC_ENABLE_CODE_SIGN`（ext4 + f2fs）、ECDSA NIST P-256/P-384 与 PKCS#7 `OID_ownerid`；657 个厂商模块（31,941 条 `vmlinux` 符号链接）`crc_mismatches=0`、`vermagic_mismatches=0`，真机 `LSMOD_COUNT=242`、`MOD_ERRORS=0`。
- **零旁路配置与 ext4 `verity`**：移除旧 Phase 3 `libcode_sign_utils.z.so` 旁路补丁，实机 `/system/lib64/libcode_sign_utils.z.so` 为原版供体（`2dd39556…`）；`marble_ext4_verity_enable` 经内核日志回放后无损置位 `userdata` `s_feature_ro_compat = 0x0000807b`；`/dev/code_sign` (`10:127`)、`/proc/sys/fs/verity/{require_signatures,developer_mode}=1`、`.fs-verity` 密钥环（`fs_verity_key0: X509.ecdsa-nist-p384`）与 `key_enable`/`local_code_sign` 均正常工作。
- **正控实测**：华为 AGC 调试签名 HAP（`com.lazzyy.test`）经官方 `bm install` 返回 `install bundle successfully.` 并上线桌面；内核对主包 `entry.hap` 及全部 9 个 `libs/arm64/*.so` 均完成 `sha256-ce` + `code_sign_verify_certchain` 验签；`lsattr` 实测 10/10 文件均带 `V`（`FS_VERITY_FL`），全页读回 `sha256sum` 零错误且写入被内核拒绝（`Operation not permitted`）。
- **负控实测**：未签名同包被严格拒绝（`code:9568320 error: no signature file.`）；1 字节篡改已签名包被严格拒绝（`code:9568324 error: signature verification failed due to out of integrity.`）。详见 [MARBLE_BATCH_PLAN.md](MARBLE_BATCH_PLAN.md) 与 [MARBLE_HANDOFF.md](MARBLE_HANDOFF.md)。

## Marble Batch M5（2026-11-26）：7 项实机问题集中修复与复验

历史基线 `vendor_boot_a = c4b6a64f857fac5f144662b7240ef147c0687a77f87c0e60b6fac6fbf23a2f07`（冷启动 `BOOT_ID = f93f2eb6-4e82-49d9-ae5d-92fbd94b289b`，`boot_a = c83a8468…` 未动，保留 `userdata`）：

- **IMS 短信真实发包通过**：修正 `QMI_WMS_BIND_SUBSCRIPTION (0x004c)` 及 3GPP `SMS-SUBMIT` (`tech=1`) 路由后，冷启动真机自动发出 3GPP IMS 短信首包即通过运营商网络受理（`IMS_SMS_SEND slot=1 tech=1 att=0 fmt=0x06 ims=1 sc=9 tpdu=124 pdu=133 rc=0 qerr=0x0` → `IMS_SMS_CB_DONE ok=1 ref=193 err=0 ipc_rc=0`）。
- **HD VoLTE/VoNR 通话接口就绪**：`ImsCallStub` 与 `libril_vendor_qmi` 去重上线，真机 `CellularCall` 回执 `RegisterImsCallCallback success`、`GetImsCallProxy success`、`[slot1] GetImsSwitchStatusResponse active 1, isVolteSupport 1`，`ImsCallRemoteObjectPtr is nullptr` 为 `0`；真实双端语音体验待人工拨打复测。
- **相机主摄全视场 + 流畅度 + CDAF**：SM7475 机内实测 `4624x3472 RAW10 → 1280x960 NV21` 耗时 **`6.525 ms/帧`**（`1920x1080 NV21` `5.984 ms/帧`）；真机 OV64B40 `4624x3472` 连续采集 25/25 帧，GT9764 CDAF 清晰度从 `81` 自动收敛至 `1092`（锁定 `dac=480`），全 4 颗物理光学镜头 HDI 探针 `errors=0`。
- **震动 / 双击亮屏 / 抬起亮屏 / 锁屏 96B PIN / 实况窗 AOD+Reminder**：`vibrator_host` 早于 `SA 3602` 启动且 `event6` 保持 `3034:3034`，`SceneBoard` 手势 `IsSupportEffect` 返回 `state:1`；`event10` 自动就绪且 `multimodalinput` 启动即持有 fd；`motion_host` 熄屏实测触发 `STREAM_START mode=tilt_to_wake msgid=514`；`pin_auth` 兼容 64B/96B 宿主机单测与冷启动 `HILOG_BAD_PIN=0`；`SA 3203/3204/65840` 全部在线且 `HILOG_AOD_TO/REM_FAIL/LV_TO=0`。
- **回归与边界**：`BOOT_COMPLETED=true`、`WLAN0_CARRIER=1`、`DMESG_OOPS=0`。物理 USB 默认保持 COM9 ACM 救援口（未切 HDC）；指纹传感器（TrustZone/SPI）与卓易通/商店华为原厂设备证明和容器镜像属硬件/安全边界。详见 [MARBLE_BATCH_PLAN.md](MARBLE_BATCH_PLAN.md) 与 [MARBLE_HANDOFF.md](MARBLE_HANDOFF.md)。

## Marble H20：原快手视频黑屏修复

在 `vendor_boot_a=c687adec…`、boot `02573073-bdae-414a-b73e-2b1b76e4f774` 的真实快手前台，原外部 YUV 图像由 `handle=1` 的 Create `-11`（H19 对照）恢复为 `handle=0x8000` 的 Create `0`、Allocate `true`、Bind `0`；同线程 RGB 继续使用 `handle=1` 并成功。相隔 1.77 秒的同一视频画面显示实际像素、字幕及进度变化。结果覆盖本次视频流的解码输出到屏幕链路；在线流选用的解码器类别、音质、音画同步及其他格式待独立核验。[原始证据索引与镜像指纹](MARBLE_BATCH_PLAN.md#第43轮h20-修复快手黑视频真实-yuv-导入及画面通过)。

### 固定媒体样本：WAV 和 AAC 时间线真机通过

原 H20 `c687adec…`、boot `b4d02895-b1be-4ef9-9fbb-3a692c130b52`，在真机原 AVPlayer fd 路径分别播放同组 6 秒 PCM WAV 与 AAC/M4A。两者均通过播放推进、暂停稳定、`4200→1200 ms` 的双向拖动（回调与实际读回一致）、恢复推进、正常 EOS 和同步释放；文件仅临时放在 RAM，验收后已清理。此结果仅覆盖媒体时间线，扬声器出声、音乐 App、固定 MP4 的帧内容与拖动、硬解和 WebView 待独立实机取证。[实测索引](MARBLE_BATCH_PLAN.md#h20-live-animation-and-audio) · [固定样本与主机参考](../tests/fixtures/marble-media/README.md)。NV21 格式 25 仍未在 H20 实机追踪中得到证实。

### 开机动画单增量：真机出帧、退场及 WLAN 回归

`vendor_boot_a=dff03e31…`、boot `370caf66-b91a-4a5b-a385-5bd79b8a29b4`，原动画 PID 5656 在 graphics UID `1003` 下由 init 启动；在动画活跃的同一开机，实际 DSI RGB 主平面先为黑，随后取得 HUAWEI 字样、淡出及粒子变化的不同画面。原 `started/finished=true`，退出后无残留动画进程，桌面获得前台，WLAN 网关有实测回包。[五帧对照](../work/marble-dual-20261123/m15-h20anim-1795520969-evidence.png) · [实测索引](MARBLE_BATCH_PLAN.md#h20-live-animation-and-audio)。本结果是采样的扫描输出与交接证据，不覆盖动画全程帧率、面板外部录制或声音。

### Marble 无线电：蓝牙 ROM 真回包，蜂窝仍离线

同一 boot `370caf66-b91a-4a5b-a385-5bd79b8a29b4`，QCA6490 按原厂串口流控和时钟条件短暂上电，真实返回 `soc=0x400c1211 rom=0x0201 patch=0x38e6`。UART 释放、控制器下电和临时文件清理均有回执，WLAN 保持在线。只通过控制器首次握手，未下载固件或验系统蓝牙。MSS 始终 `offline`，真机 QRTR 查询未见 WDS/DMS/NAS/UIM/PDC；原厂 MDT 的分段长度匹配不代表 PAS 鉴权或蜂窝可用。[实测索引](MARBLE_BATCH_PLAN.md#marble-radio-first-check)。

### Marble U2.1：应用商店与主题商店的设备证明未通过

实机读回 `vendor_boot_a=974b7b75…`，WLAN carrier 为 `1`。应用商店服务在运行，但未发现华为 TEE 用户态服务、设备接口或证书 HDI 实现；历史 `dcm_service` 日志另有实际连接拒绝。此前 U1.6 两家商店都记录过 `getDeviceToken=-10002`；主题商店的同进程样本中账号授权与 `fetch` 先返回 `0`。SDK 返回与商店业务结果须分开，U2.1 本轮只做只读核查，未重新触发两家店的业务操作。未刷机或改动设备证明链。[分层证据与待验门槛](MARBLE_BATCH_PLAN.md#marble-store-attestation)。

## 普通 ext4 挂载修复 / Normal ext4 mount fix

移除数据分区挂载和持久日志中的固定 UUID、固定容量与布局标记限制。11 个隔离 loop 镜像用例通过，覆盖随机 UUID、无标记、原 UUID、已有文件保留、错误分区名、重复挂载及空白/非 ext4 分区；实际执行 ext4 挂载和原 C 存储身份检查。AArch64 bootstrap 编译通过，vendor_boot 解包往返及无关条目检查通过。此次未连接或刷写平板，不能将此前的桌面验收作为本次修复的真机结果。报告见 [userdata-ext4-fix-verification.json](../archive/sheng/analysis/userdata-ext4-fix-verification.json)。

The fixed UUID, capacity and layout-marker prerequisites were removed from mounting and persistent logging. Eleven disposable loop-image cases passed, including real mounts and the original C storage identity check. The AArch64 bootstrap compiled and vendor_boot package checks passed. The tablet was not accessed or flashed for this change; previous desktop acceptance does not validate the new image on hardware.

| 项目 / Item | 证据与结论 / Evidence and conclusion |
| --- | --- |
| 桌面 / Desktop | 多轮原生截图与用户操作确认；v145 自动启动约 172.10s 完成，原 BootControl 门槛后成功。Native desktop captures across rounds; v145 startup completed and passed its original boot-success gates. |
| 持久载荷 / Persistence | v118 主载荷迁 vendor_boot，最小 init_boot；v145 整分区读回 SHA 与构建值一致。Exact v145 partition readback matched its image hash. |
| 点击等待 / Tap stall | 同步 Binder 等待约 1922ms，局部 MSDP 修补后的对照约 7ms，用户确认改善。Not an end-to-end latency benchmark. |
| 电池 / Battery | v131 原 NDK capacity=94、plugged=2，与同轮 sysfs 及系统状态栏对应。No full charge/thermal/standby qualification. |
| Wi-Fi | v134 原框架标准=6、真实 WPA 协商=6、状态栏小6；没有 Wi-Fi 7 实链验证。Negotiated capability is not throughput. |
| 蓝牙 / Bluetooth | v139 设置开关 OFF/ON、原服务状态与发现附近设备；未配对未知外设，蓝牙音频未验。Discovery does not establish pairing or A2DP. |
| 音频 / Audio | v142 用户确认视频暂停/续播有声，真实 PAL 流关闭/重开有记录；v143 实际重启开机声音由用户确认。No exhaustive routing/quality test. |
| 媒体 / Media | v129 真实视频 surface 帧序列及 HEIC NDK 像素对照；属于软件路径/像素验证，不代表硬解或全部格式。 |
| EGL/WebView | v144 原生 fixture 复现首次无绘制 swap 的尺寸回退，修补后 fixture 消费正确像素；真实网页仍白屏。Fixture passing is not WebView acceptance. |
| 电源 / Power | 旧版已复现 OFF 后 late frame 重新 ACTIVE1；v145 捕获 `POWER_OFF_LATCH suppressed late frame without DRM commit`。用户最新实测明确确认熄屏已好，短按熄屏按此验收。Earlier explicit ON requests had possible manual interference; they do not override the latest physical acceptance. |
| USB | 多轮冷/普通重启完整回包；仍有角色切换/在途通信问题。60s 超限后的 `exit=-1 signal=9` 属于完整终止回复，已修主机误判。 |

## v145 验收边界 / v145 acceptance boundary

当前 v145 镜像为 100663296 字节，SHA-256 `bf39eaa2f45386d09621a9a4cfbbd4725e673cb046fb777d277a40eea70e6895`。相对 v143 只改变以下 ramdisk 条目：

- `sheng-compat-v118/graphics/native_display_wake`
- `sheng-compat-v118/graphics/libdisplay_composer_vdi_impl.z.so`
- `sheng-compat-v118/MANIFEST.json`

解包往返、无关条目、DTB、bootconfig 和 manifest 校验通过。实际刷入/读回并启动成功，**用户确认短按熄屏已正常**。长时间保持熄屏、多轮实体按键循环、深睡与异常恢复未完成完整验收，单独保留为待验证项。

Container roundtrip and unchanged-entry checks passed; the flashed partition matched and boot succeeded. **The user confirmed physical screen-off now works.** Prolonged standby, repeated physical-button cycles, deep sleep and abnormal recovery remain separate open acceptance items; this is not a claim of complete power-management parity.

## 历史与当前 / Historical versus current

模块分项可用不意味着每个新版都完成全部回归。蓝牙配对、蓝牙音频、硬解、WebView、144Hz、TEE、相机和全面稳定性均不能标为完成。发生新的故障应保留旧验收的版本/场景范围，同时单独记录新问题，不能把历史成功外推成当前全通过。

Historical success is tied to its version and scenario. Use the main README's detailed matrix for open items. A diagnostic pattern, screenshot, service registration, return code or advertised capability must not be substituted for the corresponding user-visible acceptance test.

## 2026-10-31 源码整理检查 / Source cleanup checks

当前直接构建入口的 37 个目标已在准备好的 WSL 环境编译通过，UserIAM 4.0 后端、4.1 adapter 与驱动也已编译通过。Mesa/内核补丁可应用于记录的 upstream HEAD；串口 PTY 回归测试和 17 个 PowerShell 脚本的语法检查通过。构建产物不随源码提交。详见 `analysis/current-source-build-verification.json`。

All 37 direct build targets and the UserIAM backend/adapter/driver compiled in the prepared WSL environment. Selected Mesa/kernel patches passed application checks against the pinned upstream revisions; the serial PTY fixture and syntax checks for 17 PowerShell scripts passed. Full Mesa/kernel builds were not repeated. The subsequent rebuilt-runtime device test is described below.

## v146 重编译真机验收 / Rebuilt-runtime device acceptance

在本轮 37 个直接目标和 UserIAM 构建后，34 个运行文件装入 v146。设备三个启动分区先完整备份；仅在 TWRP 中写 vendor_boot_a，并核对整分区读回 SHA256。重启约 171.73 秒完成，音频、Wi-Fi、蓝牙启动 exit=0；桌面解锁通过并生成原生系统截图。用户确认既有应正常的功能均正常并授权发布。未完成项目仍按原边界标注，没有声称新增硬解、WebView、144Hz 或安全能力。

After compilation, 34 runtime files were packaged as v146. The device boot trio was backed up; only vendor_boot_a was written, and its complete readback matched. Reboot startup completed in about 171.73 seconds; audio/Wi-Fi/Bluetooth startup exited successfully, desktop unlock passed and a native system screenshot was captured. The user accepted the previously expected working features and authorized publication. Previously unfinished features remain open. See `analysis/rebuilt-source-v146-verification.json`.

## marble Phase 4 真机进度 / marble Phase 4 device progress（2026-11-23）

> **2026-11-24 授权更新**：boot_a 与 vendor_boot_a 均可按适配需要修改和刷写，不再限制 boot 固定。下面“未刷/不变”描述对应历史试验。整镜不逐轮大量留存，最少恢复基线和必要输入规则见[分批计划](MARBLE_BATCH_PLAN.md#目标与边界)。

当前装机和实测结论见 [M15 G：真机应用启动与 WLAN 联网](#m15-g真机应用启动与-wlan-联网)。下面按轮次保留历史对照；旧版未通过的项目只代表当时的状态。

本节只记 `marble`（Redmi Note 12 Turbo / POCO F5）实测。donor 为 ADY-AL10 `207.0.0.111(SP10)`；活动槽 `a`，仅写 `vendor_boot_a`。记录本地镜像 SHA-256、设备分区读回 SHA-256、对应 `boot_id`，以及与实验相关的有效配置或映射库哈希：

| 候选 / Candidate | vendor_boot SHA-256 | 结论 / Result |
| --- | --- | --- |
| Phase 4 连接修复 | `60f5f3a456001641e2be4c56b7a2ab7fc5b36a817b9648926a0ab15bba82f2f9` | 对照基线 |
| 时钟修复 | `b586d4b1c3c069e0ee5220176fcbd9706c05a8d3ccd3cf50c379037e80af185f` | `SA 3702` 发布成功 |
| 电源修复（当轮部署） | `9f41e1bb2e464a2e360eb56ae913493d9fcefba065119c42da99e2054966d234` | `SA 3301/3302/3308` 在线 |
| 原生异步提交试验 | `f51b85a9ee4995c22677d9c1e17a47424e6450b9c56c1280b7fff6a9670f7234` | 已回滚，结果与对照一致 |

### 已证明 / Established

- **时间服务**：ADY `libtime_system_ability.z.so` 的 `CreateTimerFds` 用错误的基址/索引组合访问定时器 fd 表，真机在 `TimerManager::GetInstance+396`（`0x3c458`）以 `SIGSEGV(SEGV_MAPERR)@0x1fc67544802` 崩溃。补丁把 vendor 时钟 ID 12 的槽位写成 `-1` 后跳回公共索引更新，六组 fd（epoll 用 0–5、取消用 5）与错误清理路径不变。真机日志为 `CreateTimerFds# create fd:[7 8 9 10 11 12 ]`、`Publish SA:3702 result:0`、`SA_READY id=3702`，不再出现该崩溃。
- **电源链**：早前从 `sheng` 迁来的 `libsensor_client.z.so` 重定位把调用槽 `0x78` 改到 `0x80`，于是选中 `LoadSystemAbility(int, ISystemAbilityLoadCallback)` 而不是 ADY 原本的超时重载；`0x2710`（10000 ms 超时）被当成回调指针解引用，`powermgr` 在 `Thread/SaInit0` 以 `SEGV_MAPERR@0x2710` 崩溃，并使 `SA 3301/3302/3308` 全部缺席。保留 ADY 原库后三个 SA 恢复在线，崩溃消失。该配方已标记 `retired_reason`，构建与单文件补丁工具都会拒绝它。
- **GPU 写入**：独立探针在同一 `boot_id` 内对 37×23 线性 RGBA DMA-BUF 依次清红、清青，每轮 `vkQueueSubmit`/`vkWaitForFences` 成功后再经 `OH_NativeBuffer_MapPlanes` 核对行距并读五点，两轮均 5/5 匹配（`result=verified_samples`）。设备为 `Turnip Adreno (TM) 725`，donor handle `0x8000`。这证明该缓冲区的 Vulkan 写入对 CPU 可见，**不**代表纹理采样、SceneBoard 有 buffer 或面板可见内容。
- **排除项**：把 `RSClientToRenderConnectionProxy::CommitTransaction` 从强制 SYNC 还原成 ADY 原生 ASYNC（文件偏移 `0x438CFC` 单字节，`01008052`→`21008052`，输出 SHA-256 `9eda038c303adeaee1ebc1d582ab23281cedd3b79357535d19fe07061b2e1a24`）后，`SCBDesktop/SCBWallpaper/SCBScreenLock` 仍是 0×0，SceneBoard 仍因 `THREAD_BLOCK_6S` 被 AppMS 杀死，KMS 采样仍是 2,592,000 像素全 0。因此该 SYNC 补丁不是黑屏根因。

### 未通过 / Open

- **可见桌面**：`render_service`、`SA 4606/4607`、SceneBoard 窗口注册都存在，但 `SCBDesktop`、`SCBWallpaper`、`SCBScreenLock` 的矩形一直是 `[0 0 0 0]`；`hwstartupguide` 首个窗口 `Connect timeout persistentId:14`；RS 树里的 `SCBScreen-Temp` 有 1080×2400 边界但 `hasDrawContent: 0`。采样自真实 KMS DMA-BUF 的整帧 `nonzero_pixels=0`，面板是否点亮仍未验证。
- **SceneBoard 被冻结杀死**：主线程出现 `THREAD_BLOCK_3S`/`6S`，`AppMS` 报 `Will kill faultApp, pid: ... errorName: THREAD_BLOCK_6S`。`hisysevent write ret: -5` 同时出现，多个应用在同一时段阻塞，说明是共享依赖而非单一应用逻辑；具体消费者尚未定位。
- **自动重启**：约 239–240 秒整机重置，A 槽重试耗尽后需 `set_active a` 才恢复（重置后为 7）。重置原因（内核/固件/user-space 看门狗）尚无同轮证据。
- **触控、背光与休眠**：Goodix `evdev` 节点与 `multimodalinput` 的 FD 打开有记录，但没有物理触摸事件、亮度写入或休眠/唤醒的端到端验收。

### 2026-11-23 第二轮真机（新证据）

| 结论 | 证据 |
| --- | --- |
| `hisysevent` 写入失败已归零 | 修前 `errno=2` 每 boot 960 条；加入 `hiview` 服务声明（两个 AF_UNIX dgram socket）+ `hmos_hisysevent_sink` 后，同一 boot `hilog -x` 里 `unix/socket/hisysevent` 计数 **0**，`/dev/unix/socket/` 出现 `hisysevent`、`hisysevent_fast` |
| 约 240 s 自动重启不再复现 | 连续多次采样 `uptime` 30–40 分钟、`boot_id` 不变 |
| `/vendor` 是只读 erofs | `$R/vendor` 挂载为 `erofs ro`；向 `/vendor/etc` 写入返回 `EROFS`，且会让 `set -eu` 的 stage 脚本整段退出 → 音频配置必须 **bind mount**，不能拷贝 |
| PAL 运行时已进镜像并可启动 | `/compat/audio-runtime/lib64` 47 个库；`PAL_LOG_6_AGM: session_obj_init: 1339 Error:22` → `PAL_SERVER_AGM_INIT_RC=22`，卡在 AGM session 初始化 |
| 亮度可写、`actual_brightness` 恒 0 | `brightness` 834→12 生效回读一致；`actual_brightness` 读回始终 0（驱动不反映），不能作为判据 |
| 重启后亮度/设置回到默认 | `default_settings.json` 里 `settings.display.auto_screen_brightness=1`，且该 seed 每 boot 由只读系统分区重新生成；各服务自己的 `Settings.xml` 在 userdata 上会保存 |
| 软件进不了 bootloader | `reboot bootloader` 返回 1；`misc` 分区只读（`bootonce-bootloader` 写不进）；`param sys.powerctl reboot,bootloader` 无反应 → 刷机需手动「音量下+电源」 |
| TEE 转发档被内核挡住 | dmesg 有 `QSEECOM: qseecom_init_control: qseecom.qsee_version = 0x1402000`，但 `/proc/misc`、`/sys/class/misc` 无 `qseecom`，`/dev/qseecom` 不存在 |
| MTP 不可行 | 内核无 `CONFIG_USB_F_MTP`；OHOS usbfn 只认华为私有 `f_generic.mtp`，donor `mtpfs` 为空桩 → 只保留 HDC（`ffs.hdc`）路径 |

### 2026-11-23 第三轮真机（子系统实测）

| 结论 | 证据 |
| --- | --- |
| 视频软解可用 | `native_codec_probe --instance video/avc 1280 720` → `create=0 configure=0 prepare=0 start=0 stop=0 destroy=0`；`CODEC_PROBE_SUMMARY entries=12 hw=0 sw=12 decode=10 encode=2`（AVC/HEVC/MPEG4/MPEG2/AAC/MP3/FLAC…） |
| 蓝牙固件链路通 | `bluetooth_a` 挂载后 `FIRMWARE_SELECTED rampatch=hpbtfw20.tlv nvm=hpnv20.bin`、`CHECK_RESULT=ok`（tty/power/socket 全过） |
| 蓝牙卡在芯片应答 | 真启动：`HCI_TIMEOUT opcode=0xfc00` → `BRINGUP_FAILED stage=edl_version`；已加上电 250 ms 延时 + `tcflush` + 3 次重试（待刷） |
| HDC 不是内核缺项，是脚本判据错 | `marble_defconfig:550` 与现役内核 ikconfig 均 `CONFIG_USB_CONFIGFS_F_FS=y`；`f_fs.c` 只在首次 `mkdir functions/ffs.<name>` 时才 `register_filesystem()`，脚本用 `/proc/filesystems` 判据必然误判 `USB_NO_FUNCTIONFS_SUPPORT`（exit 42），已改为建 `functions/ffs.hmos_probe` 探测 |
| WiFi 缺 supplicant | 点击开关后 `SA 1124` READY、`SA 1120` unavailable；Settings `WIFI_NOT_ACTIVE 2501000`；`wpa_supplicant` 进程 0，镜像里没有该二进制，只有 `/vendor/etc/wifi/wpa_supplicant.conf` 与 6 个 `libwpa*` |
| chroot 内 `#!/busybox sh` 失效 | 子系统脚本在 `/hmos-root` 里执行时 `/busybox` 不存在 → `exit 127`；stage 现在把 `/busybox` 拷进 `$R/busybox` |

### 2026-11-23 第四轮（HDC 打通、WiFi 契约确认）

| 结论 | 证据 |
| --- | --- |
| HDC 设备侧通了 | `param set const.security.developermode.state true` 后 `hdcd` 常驻（pid 非 0），`/dev/usb-ffs/hdc` 出现 `ep0,ep1,ep2`，`sys.usb.ffs.ready=1`，`/sys/class/udc/a600000.dwc3/state=configured` |
| `hdcd` 静默退出真因 | 二进制里有 `non developer mode, hdcd does not start` 与 `const.security.developermode.state` 字符串；该 param 未置时无输出直接退出 |
| USB functionfs 判据误报 | `mkdir functions/ffs.hmos_probe` 前 `/proc/filesystems` 无 `functionfs`，之后出现（懒注册）；脚本改成建探测目录 |
| WiFi supplicant 形态 | supplicant 不是独立进程：`libwpa_interface_service_2.0.z.so` 通过 `wpa_main@1.0` 在 `wpa_host` 内起线程；`wpa_supplicant` 二进制只是 demo 壳。设备 `/vendor/lib64/libwpa.z.so` 含 `wpa_main` |
| WiFi 尚缺的一项 | `/data/service/el1/public/wifi/wpa_supplicant/wpa_supplicant.conf` 与 `sockets/wpa` 目录；补上后需重启 `wpa_host` 并由用户点 WLAN 触发 `WpaMainThread` |

### 2026-11-23 第五轮（主题 / 定位服务上线）

| 结论 | 证据 |
| --- | --- |
| 主题服务可用 | `param set marble.debug.start.theme/wallpaper true` 后 `NATIVE_SA_READY id=65846`（theme_manager）与 `id=3705`（wallpaper） |
| 定位服务可用 | `param set marble.debug.start.location true` 后 `locationhub` 进程存在、`NATIVE_SA_READY id=2802` |
| 二者原先缺的不是服务定义 | 两个 cfg 只有 jobs/`disabled:1` 的服务，靠 param 条件拉起；默认启动脚本原先未置这两个 param |
| 基带 | `start-modem-marble.sh`、`init-modem-marble.cfg`、`telephony-sa-table.json`、`qmi_probe` 已进 payload，尚未上机跑 dry-run |

### 2026-11-23 第六轮（存储服务缺口）

| 结论 | 证据 |
| --- | --- |
| Settings「存储」取不到数据的真因 | `storage_manager`(5003) 在线，但 `disk_manager`(8640)、`StorageSpaceMgr`(8650)、`device_storage_manager_service`(5000) 三个 SA **全部 unavailable**；hilog 另有 `StorageDaemon: Read result failed, code is: 66`、`GetMetaDataSize/GetFreeSize` 报错 |
| 三者依赖都在镜像里 | `libdisk_manager_server.z.so`、`libstorage_space_manager.z.so`、`libdevice_storage_manager_service.z.so` 与对应 profile 均存在（各 3/3） |
| 手工以 root 启动无效 | `sa_main /system/profile/...` 直接起三个服务：进程 0、日志 0 字节（缺 init 的 uid/secon 上下文） |
| 已接线 | `init-platform-marble.cfg` 增加三个服务（uid/gid/secon 照 donor）+ `param:marble-storage` job；`start-phase4` 默认置 `marble.debug.start.storage=true`，**待刷机验证** |

### 2026-11-23 第七轮（HAP 安装失败定性）

| 结论 | 证据 |
| --- | --- |
| HAP 安装失败不是加密/加壳/ TEE | `bm install -p /system/app/AILife/phone-default-signed.hap` → `code:9568266`，hilog 原文：`on finished result is 9568266, [MSG_ERR_INSTALL_PERMISSIO…]`、`error: install permission denied.`、`CreateStreamInstaller failed` |
| 该码语义 | **install permission denied**（调用方缺少 `ohos.permission.INSTALL_BUNDLE`），与包体加解密无关 |
| 正确安装通道 | donor 用 `sys_installer_sa`（uid root，持 `INSTALL_BUNDLE`/`UNINSTALL_BUNDLE`）+ `installs` 服务；`installs` 我们原有 cfg 已定义，`sys_installer_sa` 本轮补入 |
| 手工 `sa_main sys_installer_sa` 无效 | 与 disk_manager 同因：非 init 上下文拿不到 samgr 注册，`SA 4101 unavailable` |

### 2026-11-23 第八轮（音频 AGM 根因：硬编码 ACDB 板名）

| 结论 | 证据 |
| --- | --- |
| `session_obj_init Error:22` 的来源 | `libagm.so` `session_obj_init.cfi @0x1e028`：`device_init()` 成功 → `graph_init()` 返回 22 |
| 22 的确切原因 | `graph_init.cfi @0x156d4` 里硬编码 `snprintf("/vendor/etc/acdbdata/" "Forte")`，`opendir` 失败即 `return 22`；donor 板名是 Forte，marble 实际叫 `ukee_mtp` |
| 修法 | 在 `/vendor/etc/acdbdata/` 下提供 `Forte/`（放 ukee_mtp 的 `.acdb`/`.qwsp`）+ `/data/vendor/audio/acdbdata/delta`；真机验证日志变为 `Load file: …Forte/MTP_ukee_acdb_cal.acdb` |
| 修完后的新错误 | `graph_init: 486 gsl_init failed error -114`（GSL 内部码，非 errno）；`dmesg` 显示 `spf_core_callback … status 0`、`spf_core_apm_close_all`，说明 SPF/APM 在跑，下一步查 `gpr_init`/`acdb_init` 哪一步返回 |

### 2026-11-23 第九轮（AGM `gsl_init` 定位到缺失内核节点）

| 结论 | 证据 |
| --- | --- |
| `gsl_init failed error -114` 的原始返回值 | `libagm.so` 的映射表 `unk_27008`（每项 16 字节）里 **index 9 → -114**，即 `gsl_init` 返回 **9** |
| 9 来自哪一步 | `libar-gsl.so` 的 `gsl_init.cfi` 依次调用 `gpr_init` → `acdb_init` → `gsl_sg_pool_init` → `gsl_global_persist_cal_pool_init`，任一非 0 直接返回 |
| `libar-gpr.so` 需要三个节点 | 字符串：`/dev/aud_pasthru_adsp`、`/dev/aud_pasthru_modem`、`/dev/gpr_channel` |
| 真机缺两个 | `/proc/misc` 无 gpr 条目、`/sys/class/aud_pasthru_modem` 不存在、`/dev` 里 `gpr|pasthru` 计数 0（`aud_passthru_adsp` 由我们自建） |
| 内核侧只有 gprbus | `techpack/audio/ipc/gpr-lite.c` 注册的是 `gprbus`（bus 驱动），**没有 misc/char 设备**；全树 grep `gpr_channel` 无命中 |
| stage 已补探测 | 现在会尝试建 `aud_pasthru_modem`/`gpr_channel` 并打印 `MARBLE_GPR_NODE=ok|missing` |

### 2026-11-23 第十轮（AGM `gsl_init=9` 追到具体调用）

用 LD_PRELOAD 追踪垫片（`compat/audio/agmtrace/trace_subinit.c`）逐层包住子步骤，真机输出：

```
AGMTRACE gpr_init=0
AGMTRACE acdb_init=0
AGMTRACE gsl_sg_pool_init=0
AGMTRACE gsl_global_persist_cal_pool_init=0
AGMTRACE gsl_spf_ss_state_init=0
AGMTRACE gsl_shmem_init=0
AGMTRACE gsl_mdf_utils_shmem_alloc=0
AGMTRACE gsl_dynamic_module_mgr_init=0
AGMTRACE gsl_do_load_bootup_dyn_modules=9     ← 唯一非 0
```

结论：`gsl_init` 返回 9 来自 **`gsl_do_load_bootup_dyn_modules`**；libagm 的映射表 index 9 → `-114`，与日志一致。
该函数内部可能的失败点（`gsl_do_load_bootup_dyn_modules.cfi`）：`336 acdb_ioctl failed`、`352 Failed to allocate GPR packet`、`365 acdb get amdb bootup load modules failed`、`376 Load modules cmd failure`、`384 spf status`。
ACDB 文件（`MTP_ukee_acdb_cal.acdb` / `MTP_ukee_workspaceFileXml.qwsp`）与 dump 里的同名同大小；全机无 `amdb` 文件，donor vendor 也无 `acdbdata`，故下一轮要判定 9 是否等价「AMDB bootup 模块表缺失」。

另：FE PCM 直连实验（`compat/audio/pcm_fe_probe.c`）证明 `pcm_open` 成功但 `pcm_write` 返回 `EINVAL(22)`——AudioReach 架构下没有 DSP graph 就不能写，绕过 AGM 走 ALSA 不可行。

### 2026-11-23 第十一轮（对既有修复的缺陷复核）

| 发现 | 证据 | 处置 |
| --- | --- | --- |
| `hisysevent` sink 实际**没在跑** | 真机 `ps` 里 `hmos_hisysevent` 计数 0，但 `/dev/unix/socket` 仍有 2 个 socket 文件（僵尸 socket） | 在 `start-phase4` 加监督块：不在就重启并写 `hisysevent-sink.out`，日志打 `HISYSEVENT_SINK=` |
| 亮度被服务覆盖 | 存档 `brightness_last=40`，实际 `brightness=3`（几乎全黑） | 把 apply 从 6d 移到 `wake_unlock` 之后，并在 3 秒后再 apply 一次；日志 `BRIGHTNESS_APPLY/APPLY2` |
| `hiview` 服务会崩溃循环 | 它需要 `/dev/bbox`、`/dev/hwlog_exception`，本内核没有 | 定义保留但 `disabled:1` + `once:1` + `start-mode:condition`；契约测试同步更新 |
| 设备仍在旧镜像上 | `vendor_boot_a=fd576a13…`、`uptime=15663s` | ANS/隐私/存储/installer **仍未上机验证**，不得记为已修 |
| `/dev/snd` 43 个节点是运行时手建 | 非镜像生效 | 镜像版本需刷后复验 |

### 2026-11-23 第十二轮（GSL 模块注册被 DSP 拒绝）

用 `ar_log` 拦截（`compat/audio/agmtrace`）拿到确切失败点：

```
AGMLOG gsl_do_register_dynamic_modules:275 gsl.Register modules cmd 0x100101e failure:9
AGMLOG gsl_do_load_bootup_dyn_modules:311 gsl.failed to register dynamic modules 9
AGMTRACE gsl_do_load_bootup_dyn_modules=9
```

即：**GPR 命令 `0x100101e`（Register modules）被 DSP 以状态 9 拒绝**，这是 DSP 侧语义，不是主机配置。

已排除的因素（逐一实测，错误码不变）：

| 试验 | 结果 |
| --- | --- |
| 补齐 ACDB 的 `Forte/` 目录 | 通过（`Load file: MTP_ukee_acdb_cal.acdb`），错误推进到本层 |
| 给 `/data/vendor/audio/acdbdata/delta` 放标定文件 | 无效 |
| 只读挂载 `dsp_a` 到 `/dsp` | 无效 |
| ACDB 文件与 dump 对比 | cal 文件 sha256 完全一致（`33e6b522…`） |
| `libar-gsl/libar-gpr/libar-acdb/libagmclient` 与原厂 tar 对比 | 完全一致；仅 `libar-pal/libagm` 不同 |

新发现：`dsp_a/adsp` 里有 **41 个 DSP 动态模块**（`AudioSphereModule.so.1`、`aac_dec_module.so.1`、`SAPlusCmnModule.so.1`…），这正是注册命令要引用的目标集合；下一步要对比「ACDB 的 bootup 模块列表」与「`dsp_a/adsp` 实际文件」。

### 2026-11-23 第十三轮（对镜像做端到端自检 + 找回两块丢失的启动逻辑）

| 检查 | 结果 |
| --- | --- |
| 新增 `compat/verify/marble_verify_all.sh`（2141 B，串口单条可调） | 真机跑通，输出 `BOOT/IMAGE/SA*/SINK/SOCK/HISYSEVENT/BRIGHT/SND/GPR/PAL/HDC/WIFI/VERDICT` 全表 |
| 解包构建产物核对 | 21 个关键产物全在；`foundation-app-core.json` 含 **3203/3204**；`init-platform-marble.cfg` 50 个服务含存储三件套 + `sys_installer_sa` |
| **发现并修复：蓝牙启动块丢失** | `start-phase4-marble.sh` 里 `BLUETOOTH_STATUS` 整段消失（此前 `11d` 也丢过一次）→ 已恢复 |
| **发现并修复：USB/HDC 启动块丢失** | `USB_START=`/`HDC_READY=` 整段消失 → 已恢复 |
| 防复发 | 新增 `tests/test_marble_boot_blocks.py`（7 项）：钉住 stage/start 的 21 个标记 + 解包镜像断言产物/ANS/存储服务 |

### 2026-11-23 第十四轮（首次真机验证 `b211f460…`，五类结论明确）

镜像 `b211f460ef7a15c0…` 已刷入 A 槽（`set_active a` 后重启），身份采集与预期一致：
`vendor_boot=b211f460…`、`boot_a=c83a8468…`（未变）、`hmos_bt_bridge_marble=62b09485…`（新二进制生效）、
`init-platform=8c5e65cf…`。

| 项 | 结果 | 说明 |
| --- | --- | --- |
| ANS 通知 SA 3203 | **READY** | 开机 80s 时还 missing，178s 时 ready；`/system/profile/foundation.json` 已含 3203（2665 B，grep 命中） |
| 隐私 3505 / 指示器 66090 | **READY** | 全权限版定义未生效也仍然就绪（donor 自带定义在跑） |
| softbus 4700 / ressched 1901 / 无障碍 801 | **READY** | 开机后 `marble.debug.start.extra` 生效后被拉起 |
| hisysevent sink | **alive**，`HISYSEVENT_ERRNO2=0`，socket=2 | 镜像内 chroot 启动方式有效 |
| `/dev/snd` | **43 节点 / 20 capture** | 本轮无任何手工 mknod，**确认为镜像枚举产生** |
| 亮度持久化 | **saved=834 now=834** | 用户把滑块移到 834，watcher 记录并回读一致；时机修复有效 |
| 音频 PAL | 仍 `PAL_SERVER_AGM_INIT_RC=-114` | **DSP 启动前挂载 `dsp_a` 的假设被否证**：`MARBLE_DSP_FS_MOUNTED` 已成但错误不变 |
| 存储三件套 5000/8640/8650 | **missing** | 库与 profile 都在设备上（`libstorage_space_manager` 354016、`libstorage_manager` 990040、`libsys_installer` 6557800），但 **init 看不到这些服务的定义** |
| installer 4101 | **missing** | 同上；dmesg 有 `SAFWK: Set process name to sys_installer_sa`，随后未注册 |
| GPR | **missing** | 内核确实不提供该节点（非我们未建） |
| HDC | daemon 未起 | `usb-autostart` 标记文件不存在（opt-in 未启用） |
| WiFi | `wpa_host=6175`，`thread=0` | supplicant 线程仍未起，需在设置里点一次 WLAN |

**init 配置来源待定**：PID 1 是 `/compat/bin/init --second-stage`（我们自己的 init，根为 overlay）；
`/proc/1/root/system/etc/init` 与 `/hmos-root/system/etc/init` 都是 0 个 cfg，`/vendor/etc/init` 有 50 个 HAL cfg
但都不含这些服务；`samgr`（pid 3779）的 ppid=1 证明 init 确实启动了它 → **init 的配置路径还没找到**，
这是存储三件套/installer 未启动的直接原因。

### 2026-11-23 应用启动回归：确认原生服务调度权限缺失

故障现场：boot `711a101f-d46f-4b35-90af-20c553a3d5c4`，`vendor_boot_a=e260a6ec…`，`boot_a=c83a8468…`。用户报告桌面打开应用失败。串口对照见 `work/marble-dual-20261123/` 中的 `apps-now.serial.txt`、`apps-user100-trial.serial.txt`、`apps-repair-baseline.serial.txt`、`sa-load-control.serial.txt`。

- `/data` 是真实 ext4、`rw`、使用率约 2%；采样未见 ext4 I/O 错误。`accountmgr`(200)、应用框架 180/401/501、存储 5003 在线。存储容量页的总量/空闲量由 SA5003 提供，不依赖其余三项都常驻。缺少 HMFS 私有统计节点时原实现跳过补偿并返回原始 `statvfs` 空闲量，不能据此判成分区损坏或用假节点修复。
- 设置有真实应用进程和窗口，但窗口未获得桌面焦点；`aa` 与 `bm` 是独立调用者，`aa` 打印错误也可能返回 0。故障范围必须以从桌面打开实际应用的结果为准。
- `distributeddata`(1301) 在本轮约 63 秒上线，约 1713 秒后仍不可用、反复重载超时；不是 `accountmgr`。另外确认 4101 的真实按需请求已交给本机服务，SAFWK 收到来自 samgr 的 code 1 后权限检查失败。对照 ADY 的 `LocalAbilityManagerStub`，该请求核对原生调用者的 `MANAGE_SYSTEM_ABILITY`。旧镜像实际 init 种子 `63c7dc88…` 含原有 imports，但 samgr 缺该角色权限；不能把这个现象判成 init 没发现服务。
- 修复候选 `311b642b5af845c6db9d25b4ed42eda461c97d65b125f4beaddae2cdd861818d`：在 Phase 4 打包阶段使用带 donor 哈希校验的原角色恢复逻辑，生成有效 init 种子 `b21a5a81a1b68e036378ecdfede51d53a01e12b8ac266cfa132020da258835a8`，为同一个 samgr UID/域恢复 ADY 原版的 22 条权限和 2 条 ACL。补回 8640/8650 的原厂数据目录初始配置；移除会被 bootstrap 覆盖的第二份 init 配置。二进制检查逻辑、应用角色、原厂分区及 `boot_a` 不变。冷启动后仍须验证 1301 恢复与两款应用真正获得前台。

### 2026-11-23 应用回退对照及按需服务真机验收

- `311b642b…` / boot `86037bbb…`：`boot_a=c83a8468…` 未变，PID 1 的有效配置和打包种子同为 `b21a5a81…`。同一探针对比旧版的四项加载失败，新版从未发布状态对 `8640/8650/5000/4101` 发出正常的 `LoadSystemAbility` 请求，全部拿到对象并复查成功。`5000` 随即由 SAMGR 回收，业务事件与后续实际调用仍待验。`4101` 发布成功也只证明安装服务可调度，不代表 `bm` 的独立调用令牌或任意 HAP 安装已修。日志：`work/marble-dual-20261123/311b-ondemand-verdict.serial.txt`。
- **纠正第十四轮的判断**：bootstrap 在 stage 返回后把 `/hmos-init-debug.cfg` 安装到 `/system/etc/init.cfg`，绝对路径目录的位置不等于配置有没有被导入。真机已看到四个服务对应进程、UID、域；补回 samgr 的原厂角色后按需发布成功。移除无效的第二份 init 配置，无需改 PID 1 的二进制。
- `311b642b…` 上桌面回归仍在：1301 首发成功后，`RdbServiceImpl::AfterOpen` 报 30 秒超时，watchdog 随后退出旧进程，SAMGR 删除 1301；后续有短暂重新发布，但又掉线。设备管理/SoftBus 失败在同一时间窗出现，尚待确认最先阻塞的调用。焦点一度回到锁屏也是独立现象，没有证据把所有应用故障归给一个服务。日志：`work/marble-dual-20261123/app-first-death-context.serial.txt`、`before-m15-app-control.serial.txt`。
- **已回退并实测可用**：仅回刷 `vendor_boot_a=9f64479d8567706f6a6b1cd375475b26916a22f6b6a997debe34ec372f28646a`，boot `48780524-6e0e-450c-8d3b-284d94176732`，`boot_a` 不变，`/data` 保留且为 ext4 `rw`。在同一 boot 约 1695 秒及 1938 秒用原生输入点击桌面，窗口焦点分别切到 `settings0`（PID 10775）和 `clock0`（PID 11372），两次之后 1301 均在线。不是用 root `aa` 的退出码判断；此前 CLI 的 `10108101` 对应 `caller invalid`，不同于桌面入口。日志：`work/marble-dual-20261123/m15-settings-wake-tap-1795416818.serial.txt`、`m15-clock-wake-tap-1795417060.serial.txt`。
- **回退版本仍有缺口**：设置日志仍有 `hisysevent` socket 缺失和设备管理加载失败。已在源码修正亮度启动调用的 `applet not found`（直接执行 chroot 内的工具而非把路径当 BusyBox applet），尚未部署该源码改动。HAP 实装、实况窗、音频/录音、WLAN 联网与 HDC 主机连接仍各自验收，不能用回到桌面或 SA 发布代替。
- **B：M15 仅恢复原厂 samgr 角色**（`86ab6421ea962d8ab03dc2794674eaec91b725964e01d4d64eb6641516c29afa`，boot `913d1536…`）。仅改 seed 与清单，PID 1 的有效 seed `a67f7a8923c5a5ccb292ae819e773d4f54820bacc3b4cd20d0c0899d70ea2bdc` 与候选一致。约 464 秒从桌面打开 `settings0` / `clock0`，1301 到 1319 秒仍由 PID 3240 提供。在这组对照里，仅恢复角色没有复现 `311b` 的回归；不能由此判断较新镜像中究竟是哪一项导致超时。B 没加四服务定义。证据：[前台](../work/marble-dual-20261123/m15-role-b-two-launches-1795423023.serial.txt) · [观察窗口](../work/marble-dual-20261123/m15-role-b-past-old-window.serial.txt) · [构建报告](../work/marble-m15-role-control.MXDrsj/report.json)。
- **C：B + 独立存储调度配置**，当轮装入 `vendor_boot_a=0e710df5091df335e4c0bf73bb6e354a32317b507d808def3235379c201e3716`，boot `42d92f33-13ae-4076-84a7-ef82e8c48e06`。`boot_a=c83a8468…` 和 B 的 seed 不变，实际平台配置 `af41e99e66a2085142cfd49cc599f44eabd65575f3f9ef4bc444fc365b87b8c8` 与打包一致。C 先不启用新条件，两个应用已从桌面打开。4101 在条件未启用时经原生按需加载成功。然后本 boot 设置非持久 `marble.debug.start.storage_trial=true`，init 拉起三个存储容器、两目录权限正确；8640/8650/5000 均能按需获得对象。约 734 秒再次验证 8640 在进程退出后动态启动；5000 两次发布后很快由 SAMGR 回收，均能重新加载，但持续业务调用未验。启用后设置、时钟取得前台，1301 到 1294 秒的采样仍为 PID 3240。未进行 HAP 实装或配额转换；该参数重启后需重新启用。[C 构建报告](../work/marble-m15-role-storage.1v2ynS/report.json) · [重载](../work/marble-dual-20261123/m15-c-reload-results.serial.txt) · [启用后前台](../work/marble-dual-20261123/m15-c-two-launches-after-gate.serial.txt)。

### 2026-11-23 D 版通知与时钟实况窗

- **当轮装入 D**：`vendor_boot_a=c1619de2e43fb8ed29e1064c4468b15a73b93bccd82ae17c22ca4b604e251bd7`、boot `f3d5d7e2-402e-4e30-915e-cc85b1718195`。以 C 为基线，仅在 `foundation-app-core.json` 加原版 3203（依赖 3299），更新清单；3204、init、权限、平台配置、二进制及 `boot_a` 不变。实际 `/system/profile/foundation.json` SHA `e6b3e8dd26bbad669634ac7b90ff30e690f99d638729f17f963cc811486971b1`，foundation 实际映射原版 `libans.z.so`，3203/3299 已发布。[D 构建报告](../work/marble-m15-notify-control.LeNOUk/report.json) · [设备采样](../work/marble-dual-20261123/m15-d-settled-1795426338.serial.txt)。
- 同一 boot 从桌面实测 `settings0` 和 `clock0` 获得前台。打开原生时钟的一分钟倒计时，返回桌面实际出现 **00:53 的实况窗胶囊**；Clock 日志发布成功、SceneBoard 保存及更新、到期发布成功。此项覆盖系统时钟的通知/实况窗，不外推任意应用的通知权限、常驻行为或到期声音。[前台日志](../work/marble-dual-20261123/m15-d-two-launches.serial.txt) · [实况窗截图](../work/marble-dual-20261123/m15-d-timer-live.jpg) · [发布与更新](../work/marble-dual-20261123/m15-d-timer-outcome.serial.txt#L17-L36)。
- D 重新启用本轮 `storage_trial`，四项按需加载均通过，5000 又一次成功重新加载；约 1228 秒 1301 仍由首发 PID 3239 提供。对照期内没复现早前 1301 掉线，此结果不等于已经定位 `311b` 的唯一触发项。[调度](../work/marble-dual-20261123/m15-d-storage-verdict.serial.txt) · [同轮复查](../work/marble-dual-20261123/m15-d-maintained-services.serial.txt)。
- HAP 仍分两层：4101 的原生按需调度已过；C 的 root `bm dump` 返回错误但退出码为 0，不能借此判断目标包未安装。原生 `marble_bundle_cli` 的只读试验只证实启动为 UID 1000，未证实包查询成功或实际安装。没有对已知数据状态不明的用户包做替换试装。

### 2026-11-23 设置存储页：缺的是 UID 配额统计

- M15 同一 boot 中实际进入设置的存储页：应用大小能列出，汇总长时间停在 `0 GB/0 GB`。`/data` 为真实 ext4 `rw`，总量 `235882442752` B，可用约 `229050000000` B，使用率 2%。截图 `work/marble-dual-20261123/m15-storage-settled.jpg`；日志 `m15-storage-page-1795417886-nav.serial.txt`、`m15-storage-live-1795418028.serial.txt`。SA5002/5003 已上线并不能证明汇总成功。
- `m15-storage-full-error.serial.txt`：JS `13900042` 包装的原生业务码是 `13602204`。在设备与 donor 哈希一致的 `libstorage_manager.z.so` 中，`GetUserStorageStats` 透传 `GetFileStorageStats` 的错误；后者通过 SA5002 请求 FileManager 的 UID 用量。`storage_daemon`（`a9ae8c40…`）调用 `quotactl(Q_GETQUOTA, USRQUOTA)`，失败时回 `13602204`。独立 `GetFreeSize` 照常返回真实可用空间，HMFS 可选元数据缺失不是 0/0 的原因。
- 只读控制：设备节点与 `/data` 同为 259:17，内核支持 quota，ext4 特性与 inode 显示未配置 UID quota。在该 boot 的 chroot 中两种块设备路径的 `Q_GETFMT` 和 `Q_GETQUOTA`（UID 20001006）都返回 `ESRCH`，与 quota 未启用相符；这不是在原调用域抓到的 syscall trace。日志 `m15-quota-device-baseline.serial.txt`、`m15-quota-query-result.serial.txt`。
- 在隔离的 64 MiB 临时 ext4 副本上，启用内部用户配额前查询为 `ESRCH`，启用后按实际分配得到 `16384 → 32768 → 16384` B；既有文件内容、属主、模式与 UUID 保持原样，原镜像未变。见 `work/marble-dual-20261123/storage-quota-fixture.log`。**没有转换或格式化手机 userdata**。完整汇总修复还需独立备份/离线迁移验收，或由可正常构建签署的 Settings 使用原生 total/free API 显示摘要并标明分类不可用。不能把未知类别填成零，也不伪造 HMFS 统计。

### 2026-11-23 WLAN：修通芯片与 WPA 调度，真机扫描通过

| 对照 | 唯一功能差异与实际结果 |
| --- | --- |
| D（`c1619de2…`，boot `f3d5d7e2…`） | Settings 原生请求 `4097` 进入 `EnableWifi`/`WifiToggled(1,0)`，传输/异常为 `0/0`，业务返回 `1`，未进入 WPA。`WifiToggled` 对空 controller 返回 `1`；启动日志显示 `StartChipHdi` 找不到 `chip_interface_service`，而实装 HCB 缺该节点。见[调用追踪](../work/marble-dual-20261123/m15-d-wlan-trace-1795429025.serial.txt#L17-L35)和[初始化日志](../work/marble-dual-20261123/m15-d-wlan-controller-logs.serial.txt#L43-L48)。 |
| E（`3c458ced…`，boot `dfc34a4b…`） | 只在现有 `wifi_host` 恢复单个原厂芯片 HDI 节点，原库与权限不变。HCB 实装 `e9421e7a…`，原厂芯片库实际映射，开关业务返回 `0` 并进入 WPA。下一步 `LoadDevice(wpa_interface_service)` 在设备管理端动态列表未找到，双端一致返回 `-202`；原 HCB 把它列为预加载，而供体是按需加载。见[离线差分](../work/marble-m15-chip-hdi.7ubhdbv5/report.json)和[双端追踪](../work/marble-dual-20261123/m15-e-wpa-load-1795431421.serial.txt#L15-L34)。 |
| F（`ffd33f03…`，boot `0932cdc1…`） | 在 E 基础上：WPA 的 HCB `preload=0 → 2`，init 去掉手动 `start wpa_host` 并恢复该 host 原厂 `ondemand=true`，UID、能力、权限、原库不变。应用可从桌面启动；原厂 `WpaMainThread` 出现且设置真的显示附近加密网络。采样无关联（`carrier=0`、无 IPv4）；此处只验 **WLAN 开启与扫描**，未验指定 AP 的认证、DHCP、DNS/实际流量。见[构建报告](../work/marble-m15-wpa-ondemand.gy37bj3j/report.json)、[前台](../work/marble-dual-20261123/m15-f-two-launches-wlan.serial.txt)和[设备复查](../work/marble-dual-20261123/m15-f-readback-1795432632.serial.txt)。 |

- F 与 E 只差 HCB、WLAN init 及清单中的 3 条原始 CPIO 记录，其他 401 条相同。全量 Phase 4 构建中对应显式参数 `--wifi-chip-hdi --wpa-ondemand`；用带 D/E 基线身份校验的复跑脚本生成此处单变量镜像。F 未装额外 supplicant 配置模板：现有 STA 文件与目录保留，P2P 文件缺项尚未作为故障处理。历史候选 `5e779fa6…` 只做过离线打包，仍含 `311b` 基线回归。
- `marble_identity.sh` 与 `marble_verify_all.sh` 区分镜像指纹、有效配置及就绪检查。启动时进程、SA、接口或 `ffs_ready` 出现不代表客户端操作正常；WLAN 以真实网络扫描、认证及收发分层判定。

### M15 G：真机应用启动与 WLAN 联网

2026-11-23 20:46（UTC+8）进度记录。装机 `vendor_boot_a=de7d8b6d2af89a6aa4a4d0e7b907c2ac1dd0638a958db5da48f24d64510d520f`，boot `e85209ff-82f2-48be-b6e0-0bc0ecb7114e`。G 比 F 仅增加一条私有音频诊断归档记录，原有 404 条 CPIO 记录（含 init、HCB、启动脚本）逐字节保留。设备读回的 WLAN init、HCB 和诊断归档与镜像一致。只刷 `vendor_boot_a`，`boot_a=c83a8468…` 保持原样；未格式化或迁移 userdata。直接回退 F 的镜像及完整指纹见[装机与回退](MARBLE_HANDOFF.md#2-当前装机镜像与已验证项)。[构建差分](../work/marble-m15-agm-layer.ZCVqd9/report.json#L273-L306) · [设备读回](../work/marble-dual-20261123/m15-g-identity-1795434853.serial.txt#L405-L437)。

- **应用启动**：同一 G boot 用 UID 2000 输入从桌面点图标，时钟 `clock0`（PID 7858 / 窗口 35）、设置 `settings0`（PID 8250 / 窗口 34）分别取得前台。约 2666 秒联网实测时，1301 仍在线，`distributeddata` 仍是首发 PID 3238。结论限于实际测试应用和观察窗口；`311b` 的 RDB 超时触发点仍未完全定位。[前台日志](../work/marble-dual-20261123/m15-g-two-launches-wlan.serial.txt#L48-L104)。
- **WLAN**：用户在 G 连接后，`wlan0` 为 `up`、`carrier=1`，有 IPv4 地址、两条全局 IPv6 地址和经 WLAN 的默认路由。指定 `wlan0` 向网关、公网 IP 分别发 2 包，各回 2 包，接口 RX/TX 计数各增加 4。[联网日志](../work/marble-dual-20261123/m15-g-wlan-connected-1795437468.serial.txt#L282-L307) · [网络服务复查](../work/marble-dual-20261123/m15-g-network-lifecycle-1795437689.serial.txt#L53-L67)。随后在同镜像新 boot `5c9288b4-01de-4507-9e63-e5652a2dc9e0`（`vendor_boot_a=de7d8b6d…`）验证：
  - **重启自动重连**：未经人工操作，`wlan0` 已为 `operstate=up`、`carrier=1`，保留 IPv4 地址（`valid_lft forever`；`/data/service/el1/public/dhcp/dhcp_cache.conf` 344 B）与 `table 2014` 默认路由（`default via <ip4> dev wlan0`）。[预检日志](../work/marble-dual-20261123/m15-g-preflight-dns-udid-1795440483.serial.txt#L822-L844) · [路由复查](../work/marble-dual-20261123/m15-g-net-probe-1795441303-prep.serial.txt#L2-L9)。
  - **原生 DNS 与 OpenSSL HTTPS**：`/dev/unix/socket/dnsproxyd` 权限为 `1098:1097 0660`（`netsysnative:netsys_socket`），`/etc/resolv.conf` 缺失，`/system/etc/security/certificates` 含 125 项。用临时探针 `native_net_probe` 在 chroot 内实测：`UID 0`（以及附带 `--supp-gid 1097` 的应用 UID `20020092`）均可经原生 `getaddrinfo` 解析 `connectivitycheck.platform.hicloud.com`（`v4=10`）与 `example.com`（`v4=2`），完成 IPv4 TCP 连接、OpenSSL TLS 证书校验（`verify_rc=0 ca_file=1 ca_dir=1`）并收到 `HTTP/1.1 204 No Content`（190 B）与 `HTTP/1.1 200 OK`（923 B）。未带 `1097` 的裸 `UID 20020092` 因无权访问 `dnsproxyd` 返回 `rc=-2`（对照实测 `SETTINGS_PROC Groups: 1002 3099 3817`）。DHCP 动态续租交互与 GUI 应用内 HTTPS 仍各自待验。[root/无组对比](../work/marble-dual-20261123/m15-g-net-probe-1795440863-exec.serial.txt#L1-L21) · [应用UID+1097通过](../work/marble-dual-20261123/m15-g-net-probe-1795441303-exec-supp.serial.txt#L1-L18)。
- **存储调度**：F 同一 boot 复验 4101、8640、8650、5000；5000 发布后被 SAMGR 快速回收，再次正常加载。`storage_trial` 只在启用它的 boot 生效；此结果不代表持续业务调用或设置页汇总已修。G 读回 `/data` 仍是 ext4 `rw`。[F 调度复查](../work/marble-dual-20261123/m15-f-storage-qa-1795433341-result.serial.txt#L42-L74)。
- **通知 / 实况窗**：F 同一个 60 秒计时器启动后，桌面实况窗显示 `00:53`；到期日志发布成功，但约 680 秒后的截图仍有胶囊，数字变成 `10:27`，符合超过原截止时刻后继续递增。Clock 离线反汇编确认到期路径会发布并尝试取消通知，没有取消成功的回执；SceneBoard 的结束判定仍待实证。勿将倒计时显示和到期收尾合并验收。[到期日志](../work/marble-dual-20261123/m15-f-timer-and-audio-nodes.serial.txt#L26-L30) · [到期分析](../work/marble-timer-expiry-abc.435990/report.md)。

### 当前未闭合的子系统

- **HAP**：F 上原 init 启动的 UID 1000 helper、user 100 实际查询到已安装的时钟包，[正控日志](../work/marble-dual-20261123/m15-f-native-bundle-clock-1795432792.serial.txt#L15-L29)。独立调试 HAP 已通过 [SDK 离线签名和摘要验证](../work/marble-hap-fixture.Ws945r/verified.bynu1824/verify-app.log#L34-L38)，但测试 profile 未绑定本机；现有 helper 的安装白名单也不接收该包。在 G（`5c9288b4`）实测 `param get const.product.udid`、`const.product.devUdid` 与 `ohos.boot.sn` 均返回 `errNum is:106!`，`bm get -u` 返回 `error: failed to get udid`（`const.product.manufacturer=HUAWEI`、`const.product.model=ohos`、`const.security.developermode.state=true`，`/system/lib64/init/libudidmodule.z.so` 存在但未入 `/system/lib64/init/autorun/`）[预检日志](../work/marble-dual-20261123/m15-g-preflight-dns-udid-1795440483.serial.txt#L848-L850) · [devUdid 复查](../work/marble-dual-20261123/m15-g-udid-check-1795443012.serial.txt#L1-L34)。待在下一候选补齐 `ohos.boot.sn` / UDID 初始化并绑定本机调试签名后，再以授权入口完成安装、启动及卸载；未替换用户包。
- **HDC**：已取得并实跑 SDK Windows `hdc.exe` 3.2.0b（需同包 `libusb_shared.dll`）。宿主脚本的离线测试覆盖正确 FunctionFS 实例及两个端点、已有绑定保护、跳过虚拟 UDC；G 未导入 HDC init 配置。`hmos_probe` 的 ACM 是 COM9，独占物理 UDC，保留不动。待端点实测、主机授权、实际文件哈希往返。[ACM/共存取证](../work/marble-usb-turn4.md)。
- **PAL / 录音**：G 在隔离根实跑 `linker64 --list`，解析 `libc.so`、`libdl.so`，退出 `0`。[G 装载日志](../work/marble-dual-20261123/m15-g-bionic-list-1795434945.serial.txt#L349-L367) · [诊断层说明](../work/marble-m15-agm-layer.ZCVqd9/README.md)。在 G 新 boot `5c9288b4` 上，首轮未挂 `/proc/asound/cards` 的脚本于 `update_snd_card_info` 退出 `rc=-2`（未触碰 DSP，`AGM_KERNEL_EVENTS 0`）；随后补齐只读 `/proc/asound/cards` 绑定并确认 `AGM_KERNEL_EVENTS 0` / `owners=0` 后，执行单 owner 原厂 `Forte` `agm_init_probe`：**成功加载 `libagm.so`、解析 `/proc/asound/pcm` 全部 42 项接口（`0:0..0:41`）、加载 `/vendor/etc/acdbdata/Forte/Forte_workspaceFileXml.qwsp` 与 `Forte_acdb_cal.acdb`，完成 `GPR INIT START` 与 `spf_core_apm_close_all`（`op 1001013 status 0`），返回 `AGM_PROBE INIT rc=0` 与 `AGM_PROBE DEINIT rc=0`（`AGM_TRIAL_APP_RC 0`、无残留占用者 `owners=0`）**。[cards 预检](../work/marble-dual-20261123/m15-g-agm-cards-5c9288b4-preflight-1795442697-result.serial.txt) · [Forte 冷态通过结果](../work/marble-dual-20261123/m15-g-agm-cards-5c9288b4-forte-1795442842-result.serial.txt#L171-L219)。由此确认 `MTP_ukee_acdb_cal.acdb` 确为历史 `-114` 根因，而原厂 `Forte` ACDB 可在真机冷态 ADSP 上通过 `agm_init`/`agm_deinit`；下一步将完整 PAL/AGM 服务接入 `vendor_boot` 并分别验收发声与录音。
- **日志缺口**：F 在 1269 秒时仍缺两个 `hisysevent` socket，当轮日志采样匹配到 893 条相关 `errno=2`；G（`5c9288b4`）实测同样 `HISYS_PID=none`、`HISYS_SOCK=none`，`hilog -x` 命中 `unix/socket/hisysevent` 868 条，确认 G 尚未合入 `hisysevent` sink。[F 日志采样](../work/marble-dual-20261123/m15-f-past-window-1795433554.serial.txt#L24-L30) · [G 实测](../work/marble-dual-20261123/m15-g-preflight-dns-udid-1795440483.serial.txt#L845-L847)。

历史候选镜像（`vendor_boot_a`，按时间顺序）：

| 候选 | SHA-256 | 结论 |
| --- | --- | --- |
| 音频 + hiview 配置 | `681b3bff0bad23c6a19bd9e0fe1ddf811c7548d7ef3a5521137978e7e5b2e6a7` | PAL 起进程，AGM EINVAL |
| + hisysevent sink | `d9955c15ee4d0406c6673a18e3b6a050631634c1342e696c8dcb3ac9e79e2d1f` | `errno=2` 归零 |
| + 子系统脚本/cfg/库（待刷） | `7bb253cb1c091ef12544eb55db59272d88bdfd2fd00264094de764cc9c228057` | 179 个 payload 文件，含定位/主题/蓝牙/USB/HDC/编解码/软 TEE |

### 复现 / Reproduce

```sh
python3 tools/packaging/build_marble_phase4_images.py --rs-screen-order \
  --phase4-build work/marble-phase4-connection.bl5xLy/build --out <新目录>/images
# 可选单变量试验
python3 tools/packaging/build_marble_phase4_images.py --rs-screen-order --rs-native-commit \
  --phase4-build work/marble-phase4-connection.bl5xLy/build --out <新目录>/images
```

打包器只接受全新的 `--out`，解包往返与未改动条目都会逐项核对；`--rs-native-commit` 只替换 RS 配方一个文件，其余条目与对照一致。
