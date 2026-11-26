# marble 扩展功能分批适配清单

## 目标与边界

目标自动延续上限已按用户要求提高到 **120 轮**；轮数是工作预算，不是功能完成承诺。逐轮保留具体进展和验收缺口。

用户在 2026-11-24 扩展并补充任务：蓝牙、音视频解码、基带、SIM 读卡、信号、电话、短信、相机、部分应用打不开、主题、开机动画、定位、麦克风、动画卡顿/掉帧、设置菜单或应用等待数秒、序列号，以及 **2026-11-24 23:22 补充**：存储问题（截图/相册图片打不开，可能与部分 App 打不开联动）、动画不够流畅/时不时卡顿、自动亮度与大部分传感器不工作、扬声器声音小（**夜间严禁播放测试扬声器声音音量，用户睡觉**）、貌似没有完整模糊（疑似与主题/渲染配置联动）、蓝牙音频不工作、开机特别慢；并明确**快手暂无需继续反复回归**。此清单替代此前仅对齐 sheng v146 的范围限制；所有项目按因果链归类并在实机逐项诚实验收。

### 按根因因果链重排的修复批次（2026-11-24 23:52 更新）

已基于当前 `0fc0cf63…`（`boot_id=a06e6049…`）实机静默取证梳理出 **8 条共享根因链**，按“先底层阻塞/致命缺陷 → 再业务闭环 → 后高风险硬件”的顺序分批单增量推进：

| 批次 | 覆盖用户问题（同一根因链） | 已证实根因与修复内容 | 验收判据与夜间约束 |
| --- | --- | --- | --- |
| **A（基础开关/扫描已完成）** | 蓝牙开关、冷启动自启、周边设备扫描 | QCA6490 3.2 Mbps + `hpbtfw21.tlv`/`hpnv21g.bin` SIBS 禁用 + 两阶段 HDI/SA 1130 + SA 1191；已在 `0fc0cf63…` 实机设置 UI 闭环（12 个真实周边设备）。 | 保留零回归；配对与 A2DP 音频归入批次 E。 |
| **B1（平台地基 + 开机提速 — 已完成 `65a41a75`）** | ① 开机特别慢（163s→94.8s）；② 设置/应用等待 10–20 秒卡顿；③ 回环 `lo` 断网；④ 主题/圆角 `region_comm/china` 缺失；⑤ `hisysevent` 刷屏噪声 | **已在 `vendor_boot_a=65a41a75…`（`boot_id=a3222ddd…`）冷启动全量闭环**：① `lo` UP（`127.0.0.1/8`、`::1/128`）；② `hmos_hisysevent_sink` 运行（`HISYSEVENT_TRANSPORT` `0` 条、`StartDynamicSystemProcess` `0` 条）；③ `oaid_service`（SA 6101 READY）、`deviceauth_service`、`locationhub`；④ 128 槽 `sceneboard_optional_sa`（保留 `1903/2802` 并补 `4016` TelephonyVSim 等，消 8s 主线程 `THREAD_BLOCK_6S`）；⑤ `baselayer.cfg` 补入 `/sys_prod/variant/region_comm/china`；⑥ `stage=services` 早期并发拉起 Wi-Fi 内核与 `SA 1301` 并在 `stage=scene` 前合流，桌面 `89.05s`、`PHASE4_ORCHESTRATOR_DONE` `94.80s`（提速 68.6s）、`late_resource` `128.77s`（提速 70.6s），Wi-Fi/BT/4700/4802/1901/1903/解锁/截图零回归、全程静音。 | 已闭环，作为批次 C 基线。 |
| **C（显示 60/120Hz 流畅度 + Composer FD 泄漏根治 + 灭亮屏 VSYNC — 已完成 `f99d75ac`）** | ① 动画不够流畅、掉帧卡顿（120Hz 面板仅跑 ~31 FPS）；② 亮屏运行约 15–40 分钟后屏幕彻底卡死；③ 灭屏/亮屏 SDE VSYNC `-22` | **已在 `vendor_boot_a=f99d75ac…`（`boot_id=b2d76d44…`）冷启动全量闭环**：① `composer_vdi.cpp` `PresentFenceCache` 根治 32,747 `sync_file` FD 泄漏（`COMPOSER_PID=3604` 初始 `TOTAL_FDS=18/SYNC_FDS=2`，滑动后 `TOTAL_FDS=23/SYNC_FDS=3`）；② `DRM_MODE_ATOMIC_NONBLOCK` 解除 ~31 FPS 瓶颈（`atomic_total=884`、`nonblock=882`、`ebusy=0`、`fallback=0`、`over25ms=0`，稳态 `commit=139..157us`，bootanim `121 frames / 2005ms`，滑动 `96` 帧中 `90/96 = 93.8% <=12ms`，`MODE_APPLIED` `60/90/120Hz`）；③ `PANEL_COLD_REINIT off_rc=0 on_rc=0`、`SDE_VSYNC_ERR=0`、`SDE_FENCE_ERR=0`、`HISYSEVENT_ERR=0`、`RCD_ERR=0`、`CLIENT_TARGET_ERR=0`；桌面 `88.87s`、`SCREENLOCK_VERIFIED after=0`、`SNAPSHOT_FILE_READY bytes=264281`、`PHASE4_ORCHESTRATOR_DONE 95.63s`、`SOFTBUS_LATE_END 126.26s`、`RESOURCE_LATE_END 129.52s`、`Focus window: 13`（`SCBDesktop8`）。 | 已闭环，作为批次 B2 基线。 |
| **B2（存储/相册打不开 + 完整模糊 + 微博/音乐 + 主题激活 + NV21 — 已完成 `270d36fa`）** | ① 相册网格 `isLoadImageError: true`（`350x777 format=25` `AllocMem -5`）且大图 `1080x2400 format=25` 被 Turnip `HMOS_NATIVE_INVALID` 拒绝；② 设置存储 `0 GB/0 GB`（`quotactl` `ESRCH` 报错 `13602204` 且缺 `SA 66074` 动态拉起卡死分类计算）；③ 模糊不完整；④ 微博卡启动页（缺 `SA 65926`）、音乐黑内容区（缺 `SA 66054`） | **已在 `vendor_boot_a=270d36fa…`（`boot_id=bf27114b-6d69-4bdc-80e2-a44909b6a1d1`，`boot_a=c83a8468…` 未动）冷启动全量闭环**：① `buffer_vdi.cpp` 按供体 `GetImageLayout` 3 平面 `(Y, U, V)` 与 `hStride->columnStride`/`vStride->rowStride` 对齐并放开奇数 YUV420，配合 Turnip NV21（`format=25` VU→UV 交换）及静音 `HMOS_BIND` 写盘；实机 `/compat/bin/plane_layout_contract` `22/22` 全过（`PLANE_LAYOUT_CONTRACT=0`、`PLANE_CONTRACT_RC=0`），`HMOS_BIND_COUNT=0`、`HMOS_NATIVE_INVALID=0`、`COMPOSER` `total_fds=23/sync_fds=0`、`SDE_VSYNC_ERR=0`、`RCD_ERR=0`，相册网格缩略图与全屏大图（`1970年4月9日 03:08`）全彩清晰无偏绿/条纹/R-B 互换；② `libhmos_ram_no_tee.so` 增加 `quotactl` ext4 空间扫描回退（`QUOTA_ERR_13602204=0`），新增 `index_insert_service-marble.json`（`SA 66074` `"run-on-create": true`）并将 `65926`/`66074` 移出 `sceneboard-optional-sa.list` 解除按需加载拦截（`SAMGR: insert SA:66074_78` 后可闲置 `Stop-SA:66074` 卸载），设置`< 存储>`实测显示 `11%`、`已使用 28.34 GB/256 GB`、7 类分类条目、`清理缓存 330 MB` 及应用大小列表；③ 控制中心整屏高斯模糊连续平滑无接缝；④ `PHASE4_ORCHESTRATOR_DONE procs=397`（`99.40s`）、`SOFTBUS_LATE_END`（`127.74s`）、`RESOURCE_LATE_END`（`131.01s`）、`B2_APPS_LATE_END sa65926=1 sa66054=1 sa66074=1`（`132.65s`），浏览器主界面零回归（主题与微博隐私弹窗进入暗色主页已在前序 B2 候选 `1fbb9d94`/`409128e0` 验证，音乐全程静音未点播放，未复测快手）。 | 已闭环，作为批次 D 基线。 |
| **D（SLPI + 全套传感器 + 自动亮度 + 定位 — 已完成 `8b4c2d35`）** | 自动亮度不行、大部分传感器不工作、定位 | **已在 `vendor_boot_a=8b4c2d356dade02f9305365ef6d82c42485257bc9e41329c872fbcbc69607786`（`boot_id=e48e9f67-7153-4a2c-b2dd-001987437ab7`，`boot_a=c83a8468…` 未动，基线 `270d36fa…`）冷启动全量静音闭环**：① `start-sensors-marble.sh` 在 `boot_slpi` 前加载 `frpc-adsprpc.ko`（`lsmod`：`frpc_adsprpc` + `sensors_ssc`）、`$B pgrep -f '/vendor/bin/sscrpcd'`；② `stage-fastrpc-nodes.sh` 等候最多 2s 并创建 `/dev/adsprpc-smd`（`0664`）与 `adsprpc-smd-secure`（`0644`），`remoteproc3` `running`，`fastrpc_rpmsg_probe`（`dsps`）与 `msm/slpi/sensor_pd` PDR 上线，`sscrpcd`（`pid 4514`）及 `sensor_host`（`uid 3033`，QRTR Service `400`）常驻，`PERSIST=0` 下导入 11 份原厂标定文件；③ `compat/sensors/ssc_wire.h` 修正 `msg==775`（`SNS_STEP_EVENT_CONFIG`）过滤与 `msg==1028`（`SNS_STEP_EVENT`）映射（`tests/test_marble_sensors_d.py` 4/4 通过）；④ `bootevent.hmos_sensor_hdi.ready=true`（`47.70s`），`SA 3601/2802/2803/2804/3301/3308` READY；`native_sensor_client_probe 2` 与 `native_sa_dump 3601 -l` 实测全部 **14 颗真实 SLPI 传感器**（`bmi26x` 加速度/陀螺仪、`tcs3701` ALS、`qmc630x` 磁力计、`proximity`、`orientation`、`gravity`、`rotation vector`、`Game Rotation Vector`、`sns_smd`、`pedometer`、`step_counter`、`sns_geomag_rv`、`device_orient`）在线并实时采样；⑤ 挂载 `isSupportBackSensor=false`，`powermgr`（`uid:5528`）实时订阅 `AMBIENT LIGHT (sensorId=5)` 并在夜间暗室由 `tcs3701 lux=0` 驱动 **62 步 60Hz 平滑自动调光**（`raw=13341/834` → `raw=45/3`）；`COMPOSER_FDS=18`，`d-screen.jpg` 桌面 Wi-Fi 6 清晰无异常；回退 `270d36fa` 见[D 控制脚本](../work/marble-dual-20261123/flash-h20-d-control.sh)。**诚实边界**：① 白天强光自动升亮（`0 lux -> 亮光`）留待白天用户确认；② `locationhub`（`SA 2802/2803/2804`）已就绪，硬件 GNSS 定位（`QMI_LOC` Service `16`）随批次 F MSS 拉起后实测。 | 已闭环，作为批次 E 基线（白天亮光 ALS 与批次 F GNSS 另验）。 |
| **E（蓝牙 A2DP HDI + 扬声器/SCO 静态审计 — 已完成 `2cff13e2`）** | 蓝牙音频不工作；扬声器声音小 | **已在 `vendor_boot_a=2cff13e2544112780506c91c90c7e797f67c3836feaa722c46c556fe7b4bea43`（`boot_id=e3d4b0b1-9f16-47ee-9ef3-3c137ecb9046`，`boot_a=c83a8468…` 未动，基线 `8b4c2d35…`）冷启动全量静音闭环**：① 装配 6-host HCB `d81a353560e0a82cac41ff43096dfdca2894eb8a758e73bff48b374a70113109`（`2381` B，含供体原字节 `audio_bluetooth`：`audio_bluetooth_hdi_service` + `bluetooth_audio_session_service`，`preload=2`）及 `init-bluetooth-marble.cfg` 按需 `a2dp_host`（`-i 5 -n a2dp_host`、`uid=3043`、`once=1`、`ondemand=true`、`stop a2dp_host`），闲置不拉起（`A2DP_IDLE_CNT=0`）；② 自研[A2DP HDI 静音探针](../compat/bluetooth/native_a2dp_hdi_probe.cpp)实测闭环（`PROBE_RC=0`/`A2DP_PROBE_EXIT=0`）：Cycle 1 按需拉起 `a2dp_host`（`pid=9765`、`load_session=0 load_audio=0`）、`StartSessionWithParams`（`SOFTWARE_ENCODING` `48kHz/16b/2ch`）建立 `/dev/ashmem` SMQ（`smq_good=1 smq_size=7705`、`a2dp_host` `30720 B`）、枚举 `bt_a2dp`/`bt_a2dp_fast` 等 7 适配器、`CreateRender(PIN_OUT_SPEAKER=1)`→`Start` 触发异步回调 `StartRender count=1` + `RenderOperationResult(1, 0)`、`3840 B` PCM SMQ 往返全匹配（`write_rc=0 reply_bytes=3840 rx_bytes=3840 exact_match=1`）、优雅停流并通知在线 `audio_server`/`bluetooth_service` 上下线，Cycle 2 复加载 `pid=9786` 成功（`reload_ok=1`）且 `DriverInstallerFullStopDeviceHost a2dp_host 5 0` 干净退出；③ 夜间绝对静音：`marble-pal-server.log` `HMOS_AUDIO_PCM` 始终为 `0`（唯一 495B 增长为后台 `pid=5830` `Diag_LSM_Init error=111`），`libhmos_audio_vdi.so`/`libhmos_pal_server.so` 与 `8b4c2d35` 原字节一致；④ 零回归：`SA 1120/1130/1191/3001/3009/3601/10/1301/2802/3301/3308` 全 READY，`native_bt_host_check_v139` `STATE=1`、`native_sensor_client_probe 2` `count=14`、`remoteproc3=running`、`WLAN0_CARRIER=1`、`COMPOSER_FDS=23`；⑤ **静态审计已定位**：扬声器小声根因是 `audio_vdi.c:224`（`r->volume=0.03f`）与 `pal_server.c:112`（`{1,{3,0.03f}}`）硬编码 `-30.46 dB` 衰减（硬件 `aw882xx_smartpa 2-0034/2-0035` + 双 PA 均为 `0 dB` 满增益、系统 `audio_volume_config.xml` 由 `audio_server` 软件控音且未调主扬声器 `SetVolume()`，**谨遵夜间静音规则未改 `0.03f`**）；BT SCO 内核 `bt_fm_slim.ko`/`slimbus.ko` 已载入、`resourcemanager_ukee_mtp.xml` 路由至 `SLIM-DEV1-RX-7/TX-7`，`audio_vdi.c` 尚仅支持 `PIN_VDI_OUT_SPEAKER`，留待批次 F 与通话一并接入。回退 `8b4c2d35` 见[E 控制脚本](../work/marble-dual-20261123/flash-h20-e-control.sh)。**诚实边界**：① 真实蓝牙耳机配对与 OTA 放音需真实音频外设另验；② 白天恢复 `0.03f` 增益与扬声器外放听感留待白天确认；③ 白天亮光 ALS 自动升亮待白天实测；④ BT SCO/GNSS/基带通话随批次 F 推进。 | 已闭环，作为批次 F 基线（外设耳机/白天外放/SCO 另验）。 |
| **F1（基带 MSS 拉起 + Clean-Room RO->RAM `hmos_rmtfs` + 只读 QMI DMS/UIM/NAS 探针 — 已完成 `830aad72`）** | 基带固件装配、防 panic 保护、Clean-Room EFS 影子服务、UIM 读卡、QMI 服务发现 | **已在 `vendor_boot_a=830aad7227a02490b5290ee0cbde0a83b2e143e9b8a07246da5a05b88fe0fb6f`（`boot_id=2778f2b7-d289-4e61-bffe-c59157576f8a`，`boot_a=c83a8468…` 未动，回退基线 E `2cff13e2…`）冷启动闭环**（[构建脚本](../tools/packaging/build_marble_h20_f1_modem.py)，4 组单测 `43` 项 `OK, skipped=1`）：① 闲置 `remoteproc2=offline`（仅显式手工触发，零改动 `boot_a`；向 `recovery` 写 `enabled` 即可防 `qcom_q6v5_pas` 超时/崩溃 panic）；② 无前置手挂下冷启动单次运行[F1 脚本](../devices/marble/runtime/telephony/start-modem-f1-marble.sh)自动只读绑定 `modem_a` 至 `/compat/original-firmware` 与 `/hmos-root/compat/original-firmware`（`F1_MODEM_FW_COUNT=30`、`F1_MODEM_FW_SYMLINKED=37`），加载 5 个内核模块（`msm_sharedmem`/`heap_mem_ext_v01`/`msm_memshare`/`qsee_ipc_irq_bridge`/`glink_pkt`），装配 `/dev/uio0`（`239:0`，`0xf5700000`/`0x280000`）与 clean-room [hmos_rmtfs](../compat/telephony/hmos_rmtfs.c)（[协议头](../compat/telephony/qmi_rmtfs.h)），仅以 `O_RDONLY -> RAM COW` 影子服务 `modemst1`/`modemst2`/`fsg`/`fsc`（`RMTFS_OPEN` `modem_fs1/fs2`、`RW_IOVEC` 读取 5118 扇区 `rc=0`、零写闪存、`PERSIST_CNT=0`）；③ `4080000.remoteproc-mss` 稳定 `running`（`MSS_REC=enabled`、`CRASH_CNT=0`）；④ 稳态实机运行[只读探针](../compat/telephony/marble_modem_probe.c) `--all` 扫全 **8 个远程 QMI 服务**（`wds 0x1`、`dms 0x2`、`nas 0x3`、`wms 0x5`、`voice 0x9`、`uim 0xb`、`loc 0x10`、`pdc 0x24`，均 `node=0`）：`DMS_INFO`（`rev="MPSS.DE.2.0.c1-00381-WAIPIO_GEN_PACK-1.82267.44"`、`masked=86***02`、`op_mode=3(offline)` 零射频发射）、`UIM_CARD slot=1 present apps=2`（`CSIM` 与 `USIM` 均已就绪 `state=7(ready) pin1=3`，`gw_sec=0x0101`/`1x_sec=0x0100`）、`NAS_SERVING reg=0(not_registered)`；⑤ 全程零回归：`PAL_PCM=0`、`PERSIST_CNT=0`、`REMOTEPROC3=running`、`WLAN0_CARRIER=1`、`COMPOSER_FDS=21`、11 项 SA（`1120/1130/1191/3001/3009/3601/10/1301/2802/3301/3308`）`NATIVE_SA_READY`、`BT_HOST_STATE_RC=0 STATE=1`、14 颗 SLPI 传感器在线；回退 `2cff13e2` 见[F1 控制脚本](../work/marble-dual-20261123/flash-h20-f1-control.sh)。**诚实边界**：① [F1 脚本](../devices/marble/runtime/telephony/start-modem-f1-marble.sh)内固定 `sleep 8` 早于调制解调器完成 8 项 QMI 注册（t+8s `QRTR_REMOTE_COUNT=1`，稳态 t+29s 至 check3 均稳定 `8` 且 UIM `ready`），批次 F2 将改为有限时间 QRTR 就绪轮询；② 当前保持 `op_mode=3(offline)` 零发射；③ 尚未接入 OHOS RIL/Telephony SA、IPA/RMNET 数据、`QMI_LOC` 定位与通话 BT SCO。 | MSS `running` 且 `CRASH_CNT=0`、RO->RAM EFS、8 个 QMI 服务与 UIM `ready` 已闭环；作为 F2 基线。 |
| **F2（下一批 — OHOS RIL QMI 桥 + Telephony SA + `DMS online` / IPA-RMNET + GNSS `QMI_LOC` + BT SCO）** | 信号、网络注册、数据流量、电话、短信；GNSS 硬件定位；通话 BT SCO | 在 F1 基线替换为有限时 QRTR 就绪轮询并保留 RO->RAM EFS；实现 clean-room `libril_vendor_qmi.z.so` 对接 Telephony SA（`4010/4008/4006`），受控切换 `DMS online` 测 NAS 注册/信号，接入 IPA/RMNET 数据、GNSS `QMI_LOC` 与 `audio_vdi.c` BT SCO（`SLIM-DEV1-RX-7/TX-7`）。 | QRTR 轮询即就绪、SIM 注册/信号/数据/通话/短信/GNSS/SCO 分项实测。 |
| **G（相机）** | 相机 | 装配 `camera.ko` + 固件 + CRM/传感器拓扑 + 高通 Camera HAL → OHOS Camera HDI 桥接。 | 前后摄预览连续帧、拍照落盘与重读。 |
| **H（收尾：USB 组合口 COM9+HDC + 真实 SN/UDID + 调试 HAP + 镜像清理）** | 序列号、HDC、调试 HAP、清理旧临时镜像 | 保留 COM9 救援通道前提下适配组合 USB/HDC；补真实硬件 SN/UDID 与调试 HAP；清理 `E:\marble-tmp` 历史冗余整镜。 | COM9 不丢、HDC 授权与哈希传输、真实 SN/UDID、全量单测通过。 |

- **用户最新授权（2026-11-24）**：`boot_a` 和 `vendor_boot_a` 均可按适配需要修改、构建及刷写；包括内核、bootstrap、引导与兼容层。不再把 `boot_a` 固定哈希当成后续刷机前提。
- 保留 userdata；abl/xbl/modem/TEE/vbmeta/super/dsp/bluetooth/persist/recovery 等原厂底层分区继续保持不写。USB 组合引导可以适配，变更 COM9 枚举前必须安排可恢复的访问路径。
- **精简镜像留存**：不为每轮迭代保留大量整镜或重复 Windows 副本。保留当前部署候选和最少必要恢复基线，历史结果优先存 hash、差分、构建方法与日志。构建/测试仍引用的输入、唯一原厂样本、标定、源码和不可再生证据保留；确认依赖替代后再清理可再生旧镜像。此规则不是清空 work 目录。
- 直接推进实现和实机验收；保留镜像尺寸/结构、设备身份、槽位和读回校验，不为重复的同范围操作再次询问。
- 原厂固件、标定、设备身份只能使用实物已有来源；不生成虚假序列号、信号、位置、录音数据或服务成功返回。
- 每批固定现场 `boot_id`、镜像、实际加载库/配置、调用身份。独立验收业务结果，不以 PID、SA 发布或模拟测试代替。
- 一次候选只合入一个子系统的必要差异；先做离线正负对照、镜像往返和回退校验，再上机。

## 当前现场与第一批

<a id="marble-bt-e2e-r47"></a>

### 第46–47轮：批次 A 蓝牙（QCA6490）冷启动自启与真实设置 UI 闭环通过

本批基于动画版基线 `dff03e31…` 构建蓝牙单增量镜像并仅刷写 `vendor_boot_a`，`boot_a=c83a8468955ca6584df29b6e5b7cba27c4b27ae5563a43401566bad301c9cae8` 未动，最终装机 **`vendor_boot_a=0fc0cf63b146ca23775ec26900f20cd098c816ece47491b8b0d3683037bc65a3`**，冷启动 `boot_id=a06e6049-c2bf-465b-945f-604767665d47`。493→501 条 CPIO 记录中 489 条原记录逐字节不变，DTB/bootconfig/AVB 结构不变；直接回退至 `dff03e31…` 用[蓝牙控制脚本](../work/marble-dual-20261123/flash-h20-bt-control.sh)的 `restore <boot_id>` 或 `restore-fastboot`。[打包脚本](../tools/packaging/build_marble_h20_bluetooth.py) · [候选差分报告](/mnt/e/marble-tmp/bt-build-r47/report.json)。

- **A1/A2（零刷机协议闭环）**：反汇编原厂 HAL `android.hardware.bluetooth@1.0-impl-qti.so` 确认 QCA6490 换速回包成功态为 `status=0x01`（`GetVsHciEvent@0x55c2c`）、`TCGETS2`/`BOTHER` 3.2 Mbps（`0x11`）流控切换顺序、TLV 段头 `[0x1e, len]` 与 `hpnv21g.bin` Tag 27 禁用 SIBS（`0101->0100`，`UpdateNewNvmFormat@0x600a0`）。[固件探针](../compat/bluetooth/marble_bt_fw_probe.c)与升级后的[H4 桥接](../compat/bluetooth/marble_bt_bridge.c)在真机实测 `hpbtfw21.tlv`（602 段）+ `hpnv21g.bin`（27 段）加载、`BUILD_INFO=BTFW.HSP.2.1.0-00570-PATCHZ-1`、`HCI_Reset`（BT 5.3）及经 `/dev/unix/socket/hmos.bt.h4` 收到 261 帧真实 BLE 广播（32+ 周边设备）。宿主 6 组共 48 项蓝牙单测全过。
- **A3（冷启动两阶段时序 + 设置融合服务闭环）**：首次试刷 `287f3392…` 暴露两个真机缺口并已同批修复：
  1. 开机早期 `samgr` 提前拉起 `bluetooth_service` 早于 `hmos_bluetooth_hdi` 发布导致 `AdapterManager` 依赖未就绪；改为两阶段参数触发（`marble.debug.start.bluetooth.hdi=true` 先停旧进程并启动 HDI → 等 `PUBLISH_RC=0` → `marble.debug.start.bluetooth=true` 启动 `bluetooth_service` 与 `fusion_connectivity_manager_ext`）并预创建 `hmos-bluetooth-hdi.txt`（UID `3021`）。
  2. 设置蓝牙页（`Setting.Fusion`）调用 `FusionUtil` 依赖 `fusion_connectivity_manager_ext`（SA 1191），缺失时点击开关报 `1021800099`；按 donor 原身份/权限补齐 SA 1191，并加会话断开后 `tcflush + HCI_Reset` 复位（`BRIDGE_SESSION_RESET`）。
- **最终真机冷启动与设置 UI 实测（`boot_id=a06e6049…`）**：
  - 零人工重启冷启动：`SA 1301/4606/1120/5100/1130/1191/3001` 全部 `rc=0`，`wlan0 carrier=1`（`10.21.30.203/24`）；桥接自动接入 `SOCKET_CLIENT pid=6422 uid=3021`（`BRIDGE_SESSION_START sessions=1`），`BluetoothHost` 初始即 `STATE=1`（ON）。`bt-coldboot-0fc0cf63-1795532302.serial.txt`（原始日志未随仓库保留）。
  - 真实设置应用点击进入蓝牙页并点击开关（`953, 407`）做 **开 → 关 → 开**：初始 `IsOn: true / STATE=1`；点击关后 `IsOn: false / STATE=3`、桥接记录 `BRIDGE_SESSION_CLOSED sessions=1` 与 `BRIDGE_SESSION_RESET sessions=1 rc=0`；再次点击开后 `IsOn: true / STATE=1`、桥接重连 `sessions=2`，设置页 `Setting.Fusion.AvailableDeviceGroup` 实时渲染 **12 个真实周边蓝牙设备节点**。`bt-ui-full-e2e-0fc0cf63-1795532379.serial.txt`（原始日志未随仓库保留）。
- **诚实边界**：蓝牙系统开关、冷启动自启、底层 H4/QCA6490、设置 UI 切换与真实周边设备发现已闭环；真实外设配对与 A2DP 蓝牙音频尚未实测，后续独立验收。

<a id="marble-radio-first-check"></a>

### 第45轮：蓝牙控制器首次应答，基带仍未启动

本轮在 A 槽动画版 `dff03e31…`、boot `370caf66-b91a-4a5b-a385-5bd79b8a29b4` 做分离验证，未重启、刷机或写入原厂固件分区。[真机基线](../work/marble-dual-20261123/m15-marble-radio-baseline-1795521879.serial.txt#L2-L46) · [试验后读回](../work/marble-dual-20261123/m15-radio-settled-1795524417.serial.txt#L2-L25)。

- **基带仍是 `offline`。** 驱动已绑定 `qcom,cape-modem-pas`，请求 `modem.mdt`。改良的 QRTR 探针在真机成功完成本地名字服务器查询，WDS、DMS、NAS、UIM、PDC 均未通告，MSS 前后均 `offline`。原厂 `modem_a` 仅只读访问；按当次 MDT 程序头核对了 28 个所需分段的精确长度（含 `b31` 哈希段），均匹配，但内核搜索的 `/compat/firmware` 尚未装配这些 MSS 文件。结构核对不是安全固件认证，[现有 PAS 驱动](../work/marble-kernel-work/kernel/drivers/remoteproc/qcom_q6v5_pas.c#L397-L411)在鉴权／复位失败或当前恢复禁用时等待启动超时，会触发 AP 内核 panic；本轮未触发 MSS，也未尝试安装 donor Balong RIL。后续单独安排具备物理恢复的启动窗口，先做原厂同源完整性与加载路径复核，再看 `running` 和 UIM／NAS 通告，最后单独验读卡、注册、信号、通话与短信。[QRTR 实测](../work/marble-dual-20261123/m15-radio-preflight-1795522345-b1d4371386-qrtr.serial.txt#L2-L12) · [MDT 核对](../work/marble-dual-20261123/m15-mss-original-layout-1795522718.json)。
- **蓝牙已取得真实芯片版本回包，系统蓝牙还不可用。** [实机 tty 绑定](../work/marble-dual-20261123/m15-bt-uart-id-1795523510.serial.txt#L2-L9)确认 `ttyHS0` 属于 `894000` GENI，预检确认原厂分区可读、临时节点可用、无现有 H4 所有者。[独立 ROM 探针](../compat/bluetooth/marble_bt_rom_probe.c)在单所有者窗口短暂上电，按 stock 115200/RTS/CTS 配置并投票 UART 时钟，只发送一次 `0xfc00/0x19`。真机返回完整 Unified Command Complete，`product=0x13 soc=0x400c1211 rom=0x0201 patch=0x38e6`；UART 投票已释放，驱动确认下电、GPIO 回到 0、rfkill 未变。未下载固件、未发 Reset、未起 H4 或 HDI；扫描、配对、音频均未验。[版本实测](../work/marble-dual-20261123/m15-bt-rom-once-1795524160-e97487ea84-query.serial.txt#L23-L48) · [清理与 WLAN](../work/marble-dual-20261123/m15-bt-rom-once-1795524160-e97487ea84-cleanup.serial.txt#L1-L7)。
- **蓝牙下一层按实测版本选固件。** [原厂 HAL](../work/marble-bluetooth-dump/vendor/lib64/hw/android.hardware.bluetooth@1.0-impl-qti.so) SHA `39cde925…` 的 `LoadPatchMaptable@0x516e8` 将该版本键 `0x400c121100130201` 映射为 `HSP2_1_`，普通项为 `hpbtfw21.tlv` 和 `hpnv21g.bin`，另有 XMEM 分支。普通两件已在原厂分区只读核对；旧桥默认的 `hpbtfw20.tlv`／`hpnv20.bin` 不是这个版本键的组合。后续先验原厂换速及匹配回包，再验对应固件下载、HCI Reset、HDI 和真实扫描。源码另外修正了[启动脚本的 chroot 路径](../devices/marble/runtime/bluetooth/start-bluetooth-marble.sh#L22-L24)，未装入当前镜像。宿主定向回归 83 项，82 通过、1 跳过。

<a id="h20-live-animation-and-audio"></a>

### 接机实测：开机动画出帧与音频双向拖动

- **固定音频时间线通过。** 连接后先读回原 H20 `c687adec…`、槽 `a`、boot `b4d02895-b1be-4ef9-9fbb-3a692c130b52`。在私有 RAM 中以 UID 0 的诊断进程运行原 AVPlayer fd 接口，确认实际映射 `libavplayer.so` / `libnative_media_core.so`。WAV 暂停 `777→777 ms`，AAC/M4A 暂停 `817→817 ms`；两者各自的 `SEEKDONE` 和独立查询均准确落在 `4200→1200 ms`，恢复推进、`6000 ms` 自然完成、同步释放均通过；测试文件已清理。这是原生控制链的真机结果，不代替扬声器听感、真实 PCM、音乐 App 或 MP4 界面验收。[接机基线](../work/marble-dual-20261123/m15-reconnect-baseline-1795519045.serial.txt#L120-L145) · [WAV](../work/marble-dual-20261123/m15-h20-avplayer-1795519381-91e79f9e32-tones.wav-run.serial.txt#L17-L46) · [AAC/M4A](../work/marble-dual-20261123/m15-h20-avplayer-1795519381-91e79f9e32-tones.m4a-run.serial.txt#L5-L35) · [清理回执](../work/marble-dual-20261123/m15-h20-avplayer-1795519381-91e79f9e32-cleanup.serial.txt#L1-L3)。
- **开机动画获得实际输出。** 单独刷 `vendor_boot_a=dff03e31…`，`boot_a=c83a8468…` 读回不变。首次启动在 `88.89 s` 请求、以原 graphics UID `1003` 运行；首轮采样只包含启动后 `0.10/1.39 s` 的黑帧，不能据此判断后段。保持同镜像、不再刷写而重启，boot `370caf66-b91a-4a5b-a385-5bd79b8a29b4`，`88.97 s` 请求，原进程 PID 5656。实际 DSI 主平面在 `92.97 / 94.08 / 95.20 s` 依次显示 HUAWEI 字样、淡出与粒子变化，非黑像素 `132 / 624 / 2180`（每帧采样 10,200 像素）。这是硬件扫描缓冲区中的动态像素，不是对面板的外部摄录，也未量化帧率、亮度或开机声音。[帧对照](../work/marble-dual-20261123/m15-h20anim-1795520969-evidence.png) · [原始帧时间与指纹](../work/marble-dual-20261123/m15-h20anim-1795520969.json#L3-L45) · [分区读回](../work/marble-dual-20261123/m15-h20anim-1795520969-readback.serial.txt#L502-L507)。
- **退场与回归。** 两次启动均有原 `started/finished=true`、启动门复位及桌面焦点。第二次在 `156.36 s` 发出清理请求，后续无残留动画进程，原媒体、窗口及数据服务可用。WLAN 自动重连并取得 IPv4，实测网关 ICMP 往返；RenderService 实际映射的渲染库与 Turnip 按 inode 和 SHA 仍与 H20 一致。当前保留 `dff03e31…`；快手 App 的动态视频在此候选上仍待业务复验，不把相同驱动字节当作 App 回归。直接回退原 H20 用[专用回退入口](../work/marble-dual-20261123/flash-h20-bootanim-control.sh)的 `restore <当前 boot_id>`。[首轮退场](../work/marble-dual-20261123/m15-h20anim-final-1795520203.serial.txt#L312-L343) · [第二轮原进程退场](../work/marble-dual-20261123/m15-h20anim-repeat-handoff-1795521142.serial.txt#L416-L443) · [实际映射与网关往返](../work/marble-dual-20261123/m15-h20anim-settled-1795521271.serial.txt#L2-L11)。

<a id="h20-offline-static"></a>

### H20 断开阶段：静态修复与上机门槛

以下是历史离线批次：当时只在主机运行解析、构建及合成测试，H20 `c687adec…` 是当时的实机基线，各改动尚未部署；接机后只部署了上文的动画单增量。不要把更宽的通用 Phase 4 源码或历史样本当成实装。

| 项目 | 已证实及离线改动 | 下次实测的判据 |
| --- | --- | --- |
| 开机动画 | H20 有 launcher、Vulkan 适配和 init 条件，却没有开机流程触发该条件。原 `bootanimation`（`865d7318…`）在 `main.cfi @0x2252c` 等 RenderService SA 10 与 ready，`CheckAndCreateMedia @0x20d28` 等 SA 3002，`Play @0x214fc` 走原播放器与 Surface。原 `cust_bootvideo.mp4`（`66f2b50a…`）在主机无错误解出 480 帧：1256×2760 H.264 Main / 60 fps，另有 AAC-LC。新增[一次性就绪门控及交接](../devices/marble/runtime/boot/start-phase4-marble.sh#L197-L281)，沿用原 init 身份和 `boot.completed`；不等待动画播放结果、不伪造启动或完成事件。 | 冷启动同步看原进程、资源路径、播放器/Surface 首个失败、屏幕连续画面和退场，再复验桌面、WLAN、快手。尺寸映射、播放流畅度和音频均未实测。 |
| GPU / 显示 / 卡顿 | [H20 原日志](../work/marble-dual-20261123/m15-h20-yuv-desktop-1795497699.serial.txt#L488-L517)在约 133 秒出现 SDE VSYNC 收尾超时与 vblank IRQ 禁用 `-22`；同组现象 H19 已见，对应[内核 encoder 关停](../work/marble-kernel-work/kernel/techpack/display/msm/sde/sde_encoder.c#L3167-L3188)，之后同次开机[取得桌面焦点](../work/marble-dual-20261123/m15-h20-yuv-ui-1795497755.serial.txt#L356-L360)。留存采样未证实额外的 KGSL/Turnip 挂死。NV21 等格式在分配与 Vulkan 导入之间仍有面覆盖缺口；没有更改驱动或全局同步/刷新策略。 | 对齐一次正常灭屏/亮屏的 atomic 返回、围栏和 vblank 序号；菜单动作另用 Binder、首帧与输入的同窗时序，不能拿固定等待或闲置帧间隔当卡顿指标。HDR、受保护内容、高刷和其他 YUV 格式单独验收。 |
| 基带 / SIM / 电话短信 | H20 只读挂载原厂固件卷供 ADSP 等已有路径使用，MSS 运行和蜂窝 RIL 未证实；实装没有专用探针和 RIL 接线，[原厂 MSS running](../work/marble-baseline/remoteproc-firmware.txt#L1-L5)只属 stock 对照。修复 [QRTR 探针](../compat/telephony/qmi_probe.c#L130-L285)的本地控制口、消息类型、完整快照和远端选择；严格关联回包，移除设备身份/原始回显。候选启动脚本分开记录 MSS 运行和服务发现。 | 按名称核实 MSS，再核远端 UIM/NAS 通告、本机原生事务及真实读卡/注册/信号。通话、短信、数据分别验；不把 WLAN、远端通告或请求成功当蜂窝业务通过。 |
| 蓝牙 | H20 有内核串口/电源模块，但无桥接或原 HDI 接线。历史 [H18 预检](../work/marble-dual-20261123/m15-h18-bt-readonly-1795487669-result.serial.txt#L2-L14)只验原 `bluetooth_a` 固件可读与节点装配，未上电。修复[桥接握手判定](../compat/bluetooth/marble_bt_bridge.c#L737-L805)：完整匹配的 Reset Command Complete 且 `status=0` 才交给 socket；未经验证的 EDL 字节不记作应答成功。 | 单硬件所有者，按真实节点和固件身份复核后，分 EDL/固件下载、HCI Reset、原 HDI 回调、发现与自有配件配对；音频另验。 |
| 定位 | [定位准备脚本](../devices/marble/runtime/location/start-location-marble.sh#L39-L83)按实物名称唯一识别 SLPI，不依赖编号；默认跳过上电，不再覆写持久定位开关。请求、实际 SA 发布与定位结果分开。 | 在原定位开关及权限下取得真实位置，核来源、精度、关闭行为，不把 `running` 或 SA 2802 当 GNSS fix。 |
| 音视频 | [探针](../compat/codec/native_codec_probe.c)修正回调共享状态、按缓冲容量乱切包、无效输出计数及 EOS/清理误判。当前只接收调用方预切的一个 Annex-B AU，哈希覆盖完整有效载荷；不是 MP4 解复用或 seek。执行时仍需外部进程超时覆盖可能卡住的 SDK 回调与清理调用。 | 独立验固定 MP4/AAC/WAV 的原入口、前进与拖动时戳、可见变化和实际 PCM；保留原快手动态画面回归，硬件解码只凭实际组件与驱动 I/O 认定。 |
| 主题 / WebView / 音乐与微博 | [H20 应用审计](../work/h20-apps-static/REPORT.md)确认 H20 实装 Asset 条目在通用打包源缺失。已[同步原身份及显式启动门](../devices/marble/runtime/init/init-platform-marble.cfg#L216-L259)，[与实际 H20 条目比较](../tests/test_asset_startup_contract.py)通过；未自动打开 gate 或改信任。正负 [Web 夹具](../tests/fixtures/marble-webview/README.md)纳入源码清单，生成脚本锁 boot ID 和目标机环回。原默认长镜头是 ASTC 序列，不等于视频；HuaweiID 异常、cfg-policy 与实际主题激活独立判断。 | 原浏览器与嵌入宿主分别验真实 GET、DOM/Canvas、按钮行为及 renderer。主题先核实际 variant 路由、原 Activate 请求及槽标志，再验画面与恢复。微博停页、音乐首屏/播放仍待同动作业务证据。 |
| 相机及其他 | H20 中 `camcc` 是时钟模块，实装引导链未装配 `camera.ko`，嵌入 DTB 的 CRM/LX7 不证明运行。原厂后端为 Qualcomm Camera HAL，donor `CameraDaemon` 是[不同 ISP 实现](../work/donor-rom/ADY-AL10-207.0.0.111-SP10/fs/chip_prod/etc/camera/hal_repository/system/platform_device.json#L2-L21)；不能靠同名设备节点或复制 HCB 生成预览。存储已有[同轮 quota `ESRCH` 与设置业务失败](../work/marble-dual-20261123/m15-quota-query-result.serial.txt#L4-L9)的历史记录；HDC 与 ACM/COM9 的物理 UDC 归属仍须分开；HAP 的原签名链拒绝与真实 SN/UDID 是两个问题。 | 相机先验同 boot 的模块/固件、CRM 绑定和真实传感器拓扑，再验前后摄连续帧、拍照及重读。设置容量总量和按应用分类分层；HDC 保留恢复通道，主机授权与哈希传输另验。身份/HAP、麦克风听感及实况窗保持原判据。 |

**独立开机动画候选** `dff03e315bd2599efda9ad6fbe03b98b4e6a774ca779e557c0183afc1fbff939` 验证仅启动脚本和清单两条 CPIO 差异，491/493 原始记录不变；YUV/RGB、Turnip、Asset、DTB 和 bootconfig 保持 H20 原件。[候选报告](../work/marble-h20-bootanimation.dRx6BU/report.json) · [冻结输入构建器](../tools/packaging/build_marble_h20_bootanimation.py)。基带、蓝牙和 codec 的新 AArch64 工件另外保存：[编译与来源指纹](../work/marble-h20-static-tools.FyuRlb/diagnostics.json)；未塞进动画候选。通用 Phase 4 构建器从 `--diagnostics-dir`（默认 `--phase4-build`）读取 [诊断构建记录](../tools/packaging/marble_diagnostics.py)，校验对应源码 SHA、AArch64 ELF 标识、产物尺寸和 SHA 后才使用该组实测字节，不再从旧分散路径取这四个工件。该门槛不代表通用镜像已重建、试刷或通过实机验收。

**离线验证**：聚焦回归共 182 项，181 通过、1 跳过；四个诊断件在独立目录重新编译后与已留工件逐字节一致，网页正负样本及生成的串口脚本也已复核。本批未执行目标机程序，未开监听或改写手机状态。接机优先保留 H20 正控并验动画单增量，再分别验证 Web／应用、固定媒体和各硬件链路。

### 第44轮：固定媒体对照与 NV21 导入边界（离线）

- [合成音视频夹具](../tests/fixtures/marble-media/README.md)已生成 WAV、AAC/M4A 和 MP4，均为 6.000 秒。MP4 的 180 帧带三段颜色、帧号和移动标记；三段音频为 400/600/800 Hz。主机分别从 4.2、1.2、3.2 秒重新打开定位，得到第 126/36/96 帧及对应音调；[样本指纹和解码记录](../work/marble-media-fixture.vy4yqP/media-report.json)。反证覆盖同色旧帧、U/V 互换、黑帧、静音及文件替换。这不是单个设备播放器的 seek 结果。
- [原 AVPlayer fd 探针](../compat/codec/native_avplayer_probe.c)与现有单 AU 解码器分离。以 WAV/M4A 各运行一遍，要求新状态回调、播放推进、暂停稳定、4.2→1.2 秒两次 seek 回调及实际落点、恢复推进、EOS 与同步释放。[独立 AArch64 交叉构建](../work/marble-avplayer-final.1Gnkfx/report.json)已复核。donor `libavplayer.so`（`48aa7c24…`）静态确认 `OnSeekDoneCb@0x1ad90` 以 `OH_PLAYER_SEEK_POSITION` 传入位置，`OnStateChangeCb@0x196fc` 以 `OH_PLAYER_STATE` 映射 NDK 状态，`OnEosCb@0x19bf8` 发送独立 EOS，body 允许为空；`GetCurrentTime@0x1f390` 独立转发底层查询。本机回归覆盖单次时间跳跃、旧／缺失 seek、提前 EOS，以及同步释放期间重复事件。探针的 `evidence=timeline_only` 不代替扬声器、真实 PCM、系统音乐界面或视频验收，未进入 H20 包体。
- **NV21 暂不启用。** [H20 的实机追踪](../work/marble-dual-20261123/m15-h20-yuv-import-1795498192.serial.txt#L10-L17)覆盖 ext24/NV12（native `0x8000`）与 RGB（`1`），所查记录未见 ext25。实装 Turnip `de21593b…` 与当前 Meson 库 `ad86b0f5…` 不同。[Allocator](../compat/display/buffer_vdi.cpp#L283-L300)保留格式 25，能描述 Y+VU 两平面的线性几何；现有 [Turnip 源码配方](../compat/graphics/mesa-turnip.patch#L823-L871)还在查询、创建、布局和采样转换中限制格式。单加枚举会留下三平面误配或 U/V 颠倒。后续候选必须保留 ext25 身份直至 Bind，核对真实 native 格式、FD/pitch/范围和对应 conversion，在采样进入矩阵前只交换一次 U/V；ext24 保持不交换。按 BT.601 窄范围，Y/U/V `128/88/176` 用 NV12 或一次纠正后的 NV21 均预期 `(207,107,50)`，错序约 `(67,144,227)`，此为软件公式对照，不是设备色值。实机应逐项留 Create、Bind、view/sampler 和实际像素，坏跨度、错格式及未支持保护路径保持真实报错。

本轮没有设备调用或新镜像。接机后在 H20 正控下先核样本文件指纹，分别记 WAV 与 AAC 的时间线/出声、原播放器的前后 seek/动态画面、Browser 与嵌入 WebView，最后将画面与真实 native 格式对齐，不由 MP4 的 `yuv420p` 元数据推定底层格式。

### 第43轮：H20 修复快手黑视频，真实 YUV 导入及画面通过

旧 RGB 兼容补丁把外部句柄统一设为 `1`，连带破坏了 native YUV 的延迟布局与图像视图契约。原 `CreateDevice` 以直接 `BL` 调用 `SetupDeviceProcAddresses`，同名 preload 原型不作为部署路径；也未找到可验证的代码洞。实际方案使用[原位条件补丁](../patches/donor/ady-al10-111-sp10/librender_service_base.yuv-native-handle.json)：仅在原构造区改 6 条指令，`VkFormat == 0` 恢复原 native `0x8000`，非零格式继续 `1`。原保护标志、尺寸分支、RGB、ELF 布局和 Turnip 保留；照常调用驱动并使用真实返回。[完整导入合同](../work/yuv-handle-fix/REPORT.md) · [独立差分测试](../tests/test_marble_yuv_native_handle_recipe.py)（240 组 / 720 次模拟，覆盖随机栈、保护标志、格式和尺寸边界）。

- **动画试验前的 H20 基线**：`vendor_boot_a=c687adec08b1b917fcbdab6fcc96caebc038aaabca43503dc1def0ceb9953079`，`boot_id=02573073-bdae-414a-b73e-2b1b76e4f774`。仅构建和刷入 `vendor_boot_a` 的渲染补丁脚本及清单增量，491/493 个 CPIO 原始记录不变，DTB/bootconfig 不变。`boot_a=c83a8468…` 未刷；实装渲染库 `b4c7c0b8…`、原 Turnip `de21593b…` 已读回并确认映射。[独立往返报告](../work/marble-h19-yuv-native.1re8uo3y/report.json) · [刷写回执](../work/marble-h19-yuv-native.1re8uo3y/flash.log) · [分区及库读回](../work/marble-dual-20261123/m15-h20-yuv-first-1795497607.serial.txt#L429-L432) · [实际渲染映射](../work/marble-dual-20261123/m15-h20-yuv-ready-1795497645.serial.txt#L459-L464)。
- **真机导入**：快手原图标启动 `com.kuaishou.hmapp`，PID 8841、UID 20020048、窗口 `hmapp0`。同 boot、同 RenderService 线程观测到 YUV `VkFormat=0/external=24/handle=0x8000`，Create Vulkan 返回 `0`（同机 H19 对照 `-11`）、原 Allocate 的布尔结果 `1`、Bind Vulkan 返回 `0`；紧邻 RGB 仍 `format=37/handle=1` 且三步通过。[原真实导入日志](../work/marble-dual-20261123/m15-h20-yuv-import-1795498192.serial.txt#L3-L17)。
- **真机画面**：快手视频区从黑屏恢复。切换一条视频后，在同一条连续播放中分别于 uptime `1054.07s` 和 `1055.84s` 截得同一人物/场景的不同姿势、字幕和进度条；视频中央区域超过 63% 像素的亮度变化大于 12。较早无变化双帧未当播放证明。[采样身份与时间](../work/marble-dual-20261123/m15-h20-kwai-playback-1795498620-capture.serial.txt#L2-L26) · [画面 1](../work/marble-dual-20261123/m15-h20-kwai-playback-1795498620-f1.jpg) · [画面 2](../work/marble-dual-20261123/m15-h20-kwai-playback-1795498620-f2.jpg) · [基础服务/WLAN 复核](../work/marble-dual-20261123/m15-h20-yuv-play-status-1795498500.serial.txt#L2-L12)。
- **复现构建**：H19→H20 单增量使用[固定输入的构建脚本](../work/marble-dual-20261123/build-yuv-native-handle.py)。[通用 Phase 4 构建器](../tools/packaging/build_marble_phase4_images.py)另支持 `--rs-yuv-native-handle`，仅选择该渲染配方、强校验实际生成的补丁语句与解压后的 Turnip，和 `--rs-native-commit` 互斥；默认配方保持不变。通用选项与本轮实刷的 H19 增量构建是两条不同的构建入口。
- **并行下一步**：独立组在宿主完成[离线 Web 内容夹具](../tests/fixtures/marble-webview/README.md)（真实交互改变 JS 状态与 Canvas 色块，附 CSP 禁用脚本负控）。待在 H20 原浏览器实测，Browser 与嵌入式 WebView 分开记。快手测试仅验已测流的真实动态画面，音频听感/音画同步、固定 MP4 拖动和硬解仍待验；主题应用/恢复、微博启动页与音乐空白另行复测。当前运行候选及直接回退 H19 均保留，回退入口 `flash-m15-role-control.sh restore-yuv-native`。


### 第42轮：YUV创建外部handle失配已实际闭合

同tid4169原调用捕获：YUV props.format0/external24、创建tiling0/usage4/**handle1**，返回-11；RGB format37/external0、usage7/handle1返回0。原render_base补丁在offset0x3df000将handle常量0x8000改成1，当前Turnip外部YUV路径要求0x2000或0x8000，所以RGB兼容补丁全局套到YUV是明确的参数合同失配。[同次三事件](../work/marble-dual-20261123/m15-h19-yuv-info-1795495443.serial.txt) · [补丁合同对照](../work/yuv-handle-mismatch.json)。

后续不能全局回滚handle使RGB回归，也不能YUV重标RGBA。下一步按真实external-format分支保留原OHOS/native handle，同时RGB继续opaque FD，分别观察Create/Allocate/Bind/YCbCr与实际视频像素；本轮仅观测/测试，不改创建参数或刷库。


### 第41轮：同调用原Vulkan返回YUV -11 / RGB 0

actual render base加载compat patched SHA`e916d185…`，不是原donor`d1482a82…`；现有补丁位在同CreateVkImage函数，已核recipe不动本轮创建结果观测点。使用实际文件offset0x3def94入口与0x3df0d0调用后普通uprobes，不修改寄存器/返回；首次无事件不当通过，原capture触发表面提交后捕获配对。

同tid4169：YUV1280x720 props.format0/external24实际vkCreateImage返回-11（FORMAT_NOT_SUPPORTED）；紧邻RGB1080x2400 props.format37/external0返回0。不是OOM或无效fd。tracefs事件/挂载已清理。[真实调用配对](../work/marble-dual-20261123/m15-h19-yuv-create-1795495007.serial.txt)。

现在首个硬失败明确是当前加载Vulkan路径拒绝外部YUV格式。actual Turnip hash`de21593b…`需与外部格式支持源码/build匹配，既有render创建external-handle补丁也需逐字段核对；下一阶段定位-11实际driver分支而非强改YUV为RGBA。设备仍H19，本轮未刷库。


### 第40轮：黑视频的GPU导入失败已实际捕获

H19同boot `bbe433d4…`，当前快手消费的960x720 format24 YUV缓冲，stride1024/size1110016，FD fstat有效，但NativeBufferUtils返回BackendTexture valid0/vk0；同窗口原render_service持续报`vkCreateImage failed`。已按相邻import/handle配对12条失败，不把FD有效或decoder RenderOutputBuffer当GPU图像创建成功。[原导入错误](../work/marble-dual-20261123/m15-h19-yuv-import-1795493784.serial.txt) · [独立配对](../work/marble-dual-20261123/m15-h19-yuv-import-1795493784.serial.analysis.json)。

这明确定位一个实际失败层：YUV native buffer→Vulkan图像创建，而非仅“可能GPU”。RGBUI与固定单帧CPU解码正控保留；具体VkResult、multi-plane format/tiling/usage/externalMemory配置仍待观测，不能直接重标YUV为RGBA或fake成功。原CreateVkImage已定位于render_service_base VA0x3dff94：OPTIMAL tiling0、props.format0则usage4、外部handle0x8000，pNext externalMemory→externalFormatOHOS；NV12预期VkFormat0/externalFormat24而非VkFormat24，实际属性待观测。Vulkan PFN被显式resolver缓存，须在准确dispatch位观察真实VkResult，不仅同名preload。下一步参数与支持特征核验，再做最小正确导入修复；本轮未刷图形库。[原ABI与参数](../work/yuv-vk-create-audit/REPORT.md)。


### 第39轮：软件解码整帧hash与参考一致，修正格式误标

同H19 boot `bbe433d4…`固定320x240单帧输出115200字节/errors0/EOS1，整帧乘后xor FNV hash `698649ace4789615`。原探针使用pixel-format枚举1但旧注释误写NV12；SDK证实1实际I420、NV12为2。主机I420参考整帧hash完全相同，NV12参考不同正是布局差异，不是解码错误。已改源码显式SDK枚举，避免后续误判。[实机整帧](../work/marble-dual-20261123/m15-h19-full-frame-1795493227-result.serial.txt) · [格式/参考对照](../work/codec-full-frame.oBslZd/comparison.json)。

这证明该单帧软件解码输出与已知参考全帧非密码hash匹配；尚未回传整帧逐字节或密码hash、Surface显示未验证。快手黑视频仍需固定Surface路径，不能从单帧软解通过推出所有在线媒体已修。


### 第38轮：固定单帧H264软件解码输出通过

原--decode最后payload与EOS分离，EOS改为独立空输入，避免将最后数据丢弃。生成320x240 baseline H264单访问单元彩条，6732字节，主机FFmpeg参考解码115200字节YUV；源sha`288fdd2f…`、参考sha`2551e6bf…`。[样本记录](../work/codec-one-frame.JRTFFl/report.json)。

同H19 boot `bbe433d4…`私有RAM上机原OH_VideoDecoder，实际capability列软件AVC/HEVC等、hw0；固定单帧输出frames1/bytes115200、width320/height240/stride320、errors0、outputEOS1，严格检查exit0，临时文件已清理，基础SA在线。[原输出](../work/marble-dual-20261123/m15-h19-one-frame-1795492918-result.serial.txt)。说明当前框架软件视频解码并非完全缺失，旧inventory“无视频软解插件”判断不适用于该运行态。

输出仅64字节采样hash，尚未与参考整帧像素比对，不证明画面正确；下一步保存本固定样本的解码输出或读back统计并与参考比较，再查Surface/YUV导入。单帧成功不当作MP4/seek/硬解或快手画面完成。


### 第37轮：收紧视频诊断验收，避免空EOS假阳性

- 当前原3011 dump虽然rc0但内容0字节，未取得视频像素证据，不能当codec业务通过。[原dump](../work/marble-dual-20261123/m15-h19-codec-dump-1795492174.serial.txt)。快手黑视频仍开放，尚未定位像素产生或Surface消费首个断点。
- 修正原native_codec_probe：仅非空output计帧、按attr.offset/size/capacity界限取样，不对空EOS缓冲取64字节；decode必须output EOS且无错误、非零bytes/frames，instance必须Start/Stop/Destroy全成功。旧input EOS后200ms提前退出已移除。[源码](../compat/codec/native_codec_probe.c) · [契约测试](../tests/test_codec_output_acceptance.py)。
- 检查器`3383eb18…`编译通过，尚未部署；输入仍按capacity切码流，不能把它当完整Annex-B access-unit/PTS验证器。下一步固定少帧样本配正确输入分包，分离软件解码输出与实际Surface黑帧，本轮不刷图形驱动。


### 第36轮：应用打开、内容执行与黑视频分层

H19同boot `bbe433d4…`，微博仍启动页，但其Asset请求原错误已经从24000001 unavailable变为24000002 asset not found，说明真实服务已接收并返回业务结果；未伪造asset或密钥，停页唯一原因仍未知。[新业务返回](../work/marble-dual-20261123/m15-h19-web-current-1795491159.serial.txt)。ArkWeb engine和nwebspawn存在，无实际renderer/DOM验收，库映射不算WebView完成。

原快手图标进入PID9986/hmapp0，截图已显示feed作者/头像/点赞数等网络内容，但视频区域黑，不能继续简单记为“APP没启动”。原RN/Hermes/ArkWeb与视频client已映射，原JS有Observed未定义caught异常，同时Native播放器XComponent surface创建/清空/重建。错误是否直接造成黑视频需单独验证。[原快手入口](../work/marble-dual-20261123/m15-h19-kwai-icon-1795491304.serial.txt) · [实际黑视频页](../work/marble-dual-20261123/m15-h19-kwai-page-1795491534.jpg) · [异常栈](../work/marble-dual-20261123/m15-h19-observed-stack-1795491452.serial.txt)。

原av_codec_service持续RenderOutputBuffer计数，不能仅由此认定像素非黑/完整解码；Surface消费/像素/屏幕图层才是下一步。本轮没有为黑视频盲改GPU或将软件decoder假标硬件。actual codec service加载原OMX_Core/codec_component_manager_service/hcodec、无video/vcodec fd（只dmabuf），旧“codec host全缺”inventory不能直接覆盖当前运行；组件实际输出像素与Surface链继续查。[实际OMX身份](../work/marble-dual-20261123/m15-h19-omx-identity-1795491961.serial.txt)。Observed原全局资源候选为framework/stateMgmt.abc，但原ACE/VM加载点未定位，不写假decorator或仅复制文件当修复。[Observed审计](../work/observed-runtime-audit/REPORT.md)。[解码输出与Surface](../work/marble-dual-20261123/m15-h19-video-output-1795491662.serial.txt)。


### 第35轮追加：Asset原服务装配已部署，微博仍停页

H19 `b966b9a642f1768730737d92b8c7b3ea4648f36606f2259aea13afa2ca6f5cb7`，boot `bbe433d4-eab5-4d73-9465-db701f019acd`。只补原asset服务角色和原2目录job，库/原profile/密码学检查不改；正常gate装配后8100可取，两目录原UID6226/0711。[现场启动](../work/marble-dual-20261123/m15-h19-asset-start-1795490830.serial.txt) · [原生发布前置](../work/asset-start-preflight/report.md)。发布不代表HUKS/秘密查询解密成功。

微博原图标新PID9059取得前台仍停启动页面，旧24000001 unavailable在此次采样未再出现；统计Histogram service报错不自动当新根因，需原业务返回/页面推进链。截图转输遇base64缺段，未当完整页面证据，原部分数据清理已执行；后续重新捕获。1301/WMS/WLAN在线、carrier1。[后续日志](../work/marble-dual-20261123/m15-h19-weibo-after-asset-1795490933.serial.txt)。本次只证明前置Asset装配修复，应用启动完成和WebView仍未验收。


### 第35轮：主题入口与微博启动页的真实断点

当前H18同镜像、用户重启后boot `779360a3-9956-43bf-a47f-66450e16b8d9`。主题推荐图像与我的页正常渲染；我的→主题点击因原HuaweiID服务异常12300001停留，未取得同次Activate请求，不据此宣称GPU卡死。原theme server按需Publish后约60秒自行卸载符合donor逻辑；空a/b槽和缺描述不能单独归因为硬件解码。[激活审计](../work/theme-activation-audit/report.md) · [原认证失败](../work/marble-dual-20261123/m15-theme-local-failure-1795489564.serial.txt)。

微博和快手BMS均已安装。微博原图标进入PID11528，但停在启动标语页，主线程采样epoll而非内核冻结；原首个业务失败为ASSET服务不可用24000001/SA8100，当前init没有asset_service声明，原数据目录也缺。已离线构建原Asset角色/按需服务与两目录的单platform增量候选`b966b9a6…`，原profile run-on-create=false保持；尚未部署，先核HUKS/初始化强依赖，不吞错或伪造密钥。[候选报告](../work/marble-asset-control.022b22cy/report.json)。[微博实机](../work/marble-dual-20261123/m15-weibo-stall-1795490092.serial.txt) · [实际启动页](../work/marble-dual-20261123/m15-weibo-page-1795490022.jpg) · [Asset装配缺口](../work/marble-dual-20261123/m15-asset-pre-1795490182.serial.txt)。

微博进程实际已映射ArkWeb engine/libffmpeg及仓颉运行时，排除“核心库全未加载”的简单假设，不等于WebView DOM/视频已验。音乐实际bundle是com.huawei.hmsapp.music，不是此前查错的hmos.music；能取得前台且进程存活，业务/页面退出原因仍需同动作证据。主题、Asset、WebView和codec保持独立验证，不把共同告警当统一根因。


### 最新优先级与第34/35轮：主题异常重启后现场

用户最新要求优先主题、音视频解码、WebView和音乐/快手/微博启动，蓝牙握手暂后移；目标已按新要求更新，预算仍120轮。用户应用第三方主题后报告卡住并自行重启，当前boot `779360a3-9956-43bf-a47f-66450e16b8d9`，不要继续使用旧PID/boot脚本。

- 重启后1301/WMS/WLAN可取、内存余量正常，GPU缓冲绑定/后续提交仍运行；当前捕获未见GPU fault/hang，不代表应用主题瞬间没发生解码/GPU问题。原theme A/B槽的app/skin/system为空，SCB报激活ID解析与manifest/description缺失，variant china原资源存在而通用sys_prod/resource/themes缺。[重启图形/资源](../work/marble-dual-20261123/m15-theme-gpu-1795488988.serial.txt) · [槽检查](../work/marble-dual-20261123/m15-theme-slots-1795489040.serial.txt)。
- 原主题图标可进入推荐与我的页面，网络封面/静态缩略图实际渲染；点击“我的→主题”未打开本地列表，同次原日志HuaweiID系统认证失败12300001/120710101。该动作不是GPU冻住证据，不能绕登录或用假主题FLAG标可用。[入口截图](../work/marble-dual-20261123/m15-theme-entry-1795489242.jpg) · [本地入口原失败](../work/marble-dual-20261123/m15-theme-local-failure-1795489564.serial.txt)。
- SA65846按需出现后被samgr卸载；原donor本身ondemand=true，现有UID/权限相符，不先归因“缺声明”。主题激活原文件生成链继续定点分析，待得到最小正确修复，用户异常场景瞬时GPU/decoder仍需另捕获。


### 第33轮：本机蓝牙上电前RAM预检通过

同H18 boot `8ba0b2b2…`，OHOS检查工件`00eaa21f…`分段传输并校验hash，只在独立tmpfs目录创建live sysfs来源tty508:0/power497:0节点、socket父目录和原bluetooth_a只读固件挂载。--check实际选择hpbtfw20.tlv/hpnv20.bin、节点/父目录检查全过；未打开UART或执行power ioctl，rfkill仍0且未创建H4 socket。[实机预检](../work/marble-dual-20261123/m15-h18-bt-readonly-1795487669-result.serial.txt)。

临时挂载/工件/节点/目录清理rc0，1301/4606/1120在线。此结果只覆盖本机装配前置，不是EDL版本请求、固件下载或蓝牙扫描成功；下一阶段单owner有界上电握手，任何错误保留并停止自动重试。


### 第32轮：蓝牙原固件指纹与无副作用check准备

- 原桥接--check调用socket_prepare会mkdir父目录，与“只校验”语义不符；已改成lstat/access检查已有父目录，不在check中创建节点/套接字或上电。正负测试确认父目录缺失时退出而不创建，存在时返回CHECK_RESULT=ok且无socket文件。[副作用测试](../tests/test_bt_check_readonly.py)。
- ARM64静态桥接候选`6568bfc1…`已构建，尚未部署/启动。[构建记录](../work/bt-check-readonly.YTba0o/report.json)。真实EDL/HCI下载尚未测试，编译通过不代表蓝牙可用。
- 本boot只读临时vfat挂载bluetooth_a，确认hpbtfw20.tlv=201560bytes/hash b7fcf643…、hpbtfw21.tlv=146068/hash8ccb6543…、hpnv20.bin=6319/hasha48f5538…，随后卸载成功；未修改firmware/标定/蓝牙电源。[原固件取证](../work/marble-dual-20261123/m15-h18-bt-firmware-audit-1795487393.serial.txt)。下一阶段独立RAM节点与固件检查、单硬件所有者握手。


### 第31轮：当前蓝牙断点是未装配，不混用旧版本自检

H18 boot `8ba0b2b2…`只读核对：btpower/bt_fm_slim/msm_geni_serial已加载，live sysfs ttyHS0=508:0、btpower=497:0，rfkill bt_power状态0；OHOS root内/dev/ttyHS0、/dev/btpower、bt_firmware挂载及桥接bin均缺席，SA1130与hmos.bt.h4消费者不存在。[当前事实](../work/marble-dual-20261123/m15-h18-bt-nodes-1795486915.serial.txt) · [模块/服务](../work/marble-dual-20261123/m15-h18-bt-current-1795486845.serial.txt)。宿主/dev缺节点不能等同内核没有驱动，已分开记录。

源码stage已有蓝牙块但当前镜像基于较窄F路线未装配；历史CHECK_RESULT=ok不能当当前H18通过，更不能当芯片EDL/HCI握手成功。新增[节点预检](../tools/maint/bluetooth_stage_preflight.py)只从live sysfs取major/minor，冲突时hold，不使用旧major或上电。[本boot计划](../work/marble-dual-20261123/bt-current-preflight.json)。

下一阶段先单变量装配只读固件与节点、桥接--check，核实原硬件所有者与串口；随后原厂下载握手和HDI/框架分层。MAC/固件只读真实来源，不随机制造身份。本轮未改蓝牙电源、固件或分区。


### 第30轮：原录音条目的GUI回放与暂停链通过

同H18 boot `8ba0b2b2…`，SoundRecorder PID10543/窗口30，点击刚保存的44秒条目播放。原应用AVPlayer进入playing，后端真实OPEN/START及PCM write成功；非零PCM经VDI提交。暂停原应用回调state6/paused，随后PAL STOP/CLOSE0，统计2,243,520帧、4,140,196个非零样本、peak440。[开始回放](../work/marble-dual-20261123/m15-h18-replay-start-1795486464.serial.txt) · [原应用与后端结果](../work/marble-dual-20261123/m15-h18-replay-result-1795486552.serial.txt)。

再续播/暂停原UI两次调用通过，基础服务/WLAN保持在线。[续播操作](../work/marble-dual-20261123/m15-h18-replay-resume-1795486598.serial.txt)。此证据支持实际保存录音的原播放器解码与PCM输出，不证明用户听觉、音质、声源相关性或全过程音画时序。帧统计包含框架空闲缓冲，不用它反算文件精确时长；旧日志中另有0-duration文件被原应用删除，未混为本次44秒条目失败。


### 第29轮：桌面原录音机启动与GUI采集保存通过

H18同boot `8ba0b2b2…`，实机桌面建议区域出现原录音机图标，触摸(281,1296)从原SCB路径成功进入soundrecorder0/PID10543/窗口30；原应用UID20020081，非root代录。[图标启动](../work/marble-dual-20261123/m15-h18-recorder-icon-1795485892.serial.txt) · [原页面](../work/marble-dual-20261123/m15-h18-recorder-page-1795485933.jpg)。

点击原录音按钮开始、随后停止，真实VDI 48k/1/S16 Create/Start/Stop/Destroy均0；页面新增44秒录音条目。此次因工具调用间隔为44秒，不能标为精确短时延测试。尚未验证保存文件解码、回放听感和受控声源响应。[原开始](../work/marble-dual-20261123/m15-h18-recorder-start-1795486000.serial.txt) · [原停止](../work/marble-dual-20261123/m15-h18-recorder-stop-1795486045.serial.txt) · [保存后UI](../work/marble-dual-20261123/m15-h18-recorder-saved-1795486082.jpg)。基础服务/网络保持在线。

原aa失败已分离为debug工具问题：-u值传入requestCode槽而userId仍-1，10103601是底层EINVAL22的误导文本，不证明BMS缺安装；普通查询会重置calling identity。当前foreground user具体值未实测，不猜成0。[原工具审计](../work/recorder-aa-query/report.md)。不为此改权限/签名或断言应用启动失败，后续以真实图标路径为准。


### 第28轮：原录音机安装成功，启动查询链仍失败

H18 `c387517b5e6ec6e182036b9fd8917536ecdd21da58da63a528030cecb93e560e`，boot `8ba0b2b2-63e9-4309-9434-3bf55a339b8d`，只修helper针对SoundRecorder的目标user100。原BMS正常签名安装返回install bundle successfully；独立dump确认com.huawei.hmos.soundrecorder/version7.0.0.361、UID20020081、enabled=true、singleton=false、MainAbility/module phone。[安装](../work/marble-dual-20261123/m15-h18-recorder-install-system-1795485261.serial.txt) · [包查询](../work/marble-dual-20261123/m15-h18-recorder-dump-1795485503.serial.txt)。

原UID2000 aa启动仍返回10103601 bundle不存在，显式user100与module phone均同样失败；不以BMS存在就宣称应用已启动，不重复安装或绕权限。[完整入口请求](../work/marble-dual-20261123/m15-h18-recorder-module-launch-1795485538.serial.txt)。下一阶段核原AbilityManager查询身份/用户和安装包系统标志链，普通图标启动与GUI采集仍待验。1301/401/4606/1120/1901/1903在线、WLAN carrier1。[末次检查](../work/marble-dual-20261123/m15-h18-recorder-resolve-1795485574.serial.txt)。


### 第27轮：原录音机试装暴露用户参数错误

H17 `355ebac864729c828887e2fd391535cf081fea4851c6f9af3c3cf4bd5d8ba42d`，boot `ae72b091-6561-456f-94ec-d7de4b81e5cb`，仅helper新增原SoundRecorder目录白名单。原UID1000/init令牌调用BMS试装原签名包，返回“zero user can only install singleton app”，尚未安装。[原失败](../work/marble-dual-20261123/m15-h17-recorder-install-system-1795484723.serial.txt)。

原install-system模式此前针对singleton ArkWeb用户0，正常SoundRecorder不能共用该目标用户。已修正只对SoundRecorder使用现有user100，ArkWeb用户0保留，签名/权限/用户合法性检查原样。候选helper`a3549d84…`编译通过，尚未部署；下轮单对象增量再试装，不把helper最终exit0当BMS成功。


### 第26轮：公共录音失败首因已同次闭合

- 同H16 boot `c3279c4e…`重复公共capturer探针，原audio_server明确VerifyClientPermission拒绝UID0/tokenId0的MICROPHONE，随后InitIpcStream失败-62980116、CreateCapturer为空、Generate返回1。这是原调用身份检查，不是mono VDI返回失败；先前预加载VDI Create0不能冒充同probe已通过权限。[同次原日志](../work/marble-dual-20261123/m15-h16-public-recheck-1795484161-result.serial.txt)。不改权限guard，不给无token root伪造app身份。[入口/初始化顺序审计](../work/capture-generate-audit/report.md)明确policy可先准备设备pipe，不能把VDI预初始化与之后客户端令牌校验混为同一成功层。
- 原SoundRecorder包存在于system/app，但原UID2000 aa启动返回10103601 bundle不存在，证明文件存在不等于BMS安装。[原应用启动](../work/marble-dual-20261123/m15-h16-recorder-launch-1795484069.serial.txt)。包名com.huawei.hmos.soundrecorder、MainAbility、MICROPHONE正常请求声明已核对。
- 已在固定安装helper白名单加入原SoundRecorder目录（normal原BMS安装/签名检查不变），编译候选`7035e7cb…`与8项权限/参数测试通过，尚未部署。下一步只改helper的最小镜像增量，安装原签名包、走原应用权限与UI录音，再验证框架链。


### 第25轮：mono VDI/policy已部署，公共采集仍失败

H16 `498bf522b121b7af454c209664f46e8b289646a4a3672ddfc96df963137dcc0a`，boot `c3279c4e-170d-4d69-9785-2f5a33904a7a`，VDI `e48607d6…`、policy `a2d51ac2…`实际读回匹配，镜像仅改两个合同对象和清单。[构建报告](../work/marble-vdi-mono.1ke4bccm/report.json)。

- 新公共OHOS capturer探针正常构造48k/1/S16/MIC/NORMAL，但GenerateCapturer返回1，未取得capturer；不按shell最终0认定采集通过。[原返回](../work/marble-dual-20261123/m15-h16-public-capture-1795483646-result.serial.txt)。
- 同boot音频服务日志已到真实VDI CreateCapture，pins134217729/rate48000/channels1/format2/type0，rc0，随后Stop/Destroy0，表明合同下游能创建，失败在另一个框架/调用身份步骤，尚无唯一根因。不能先假定权限或绕token。[VDI边界](../work/marble-dual-20261123/m15-h16-capture-fail-1795483699.serial.txt)。
- 原MUSIC Start/Stop/Release0、71328帧，1301/401/4606/1120/4700/4802/1901/1903均在线、WLAN carrier1。[基础回归](../work/marble-dual-20261123/m15-h16-accept-1795483769.serial.txt)。下一步普通录音应用原token/权限与public创建链实测，不以root探针等同普通应用录音验收。


### 第24轮：VDI与policy mono能力候选已同步，尚未部署

- 已按实测单麦能力同步输入VDI候选：channelCount1/MONO、OPEN48000/1、GetFrameSize2，客户端采集对齐S16两字节而非固定stereo4，保留真实短读、不补PCM。stripped VDI `e48607d6…`编译通过。线上仍旧VDI，不把候选当已生效。
- policy候选仅把Builtin_Mic_In关联收敛到primary_input 48k mono/1920byte，移除该设备的fast/AI/不支持格式候选，耳机/蓝牙与全部输出pipe保持原参数。[候选policy记录](../work/mono-builtin-policy.0ne_0wpk/report.json) · [合成器](../tools/packaging/mono_builtin_input_policy.py)。
- 输入与输出枚举/帧数正负测试通过；下一阶段将VDI+内置mic policy同一能力合同打包，独立比较输出/其它设备不变，验证原framework capture与GUI域。当前实装仍H15，后端mono通过不替代框架申请与应用权限/声源验收。


### 第23轮：PAL mono后端部署与真实采集通过

当前H15 `ec29d8b611399dd7572f5b5f36d1785f4be6669f3b8c2b4d9ee6d84464ed8306`，boot `9b3796a9-bfd5-4b45-b05f-0bb9d0c060d0`。PAL对象实际读回`22f3350b…`与stripped候选相同，其余线上库/VDI/policy未改。

- Mono探针两次STATUS/OPEN/START/STOP/CLOSE均0；完整启动后采集48000字节/24000帧、22524非零、峰值637，后端累计frames与bytes/2均24000。[就绪后mono](../work/marble-dual-20261123/m15-h15-mono-ready-1795482757-result.serial.txt)。非零与帧数不替代真实声源/音质验收。
- 旧stereo接口探针仍96000字节/24000帧，关闭成功；原MUSIC Start/Stop/Release0、71328帧，自动4700/4802/1901/1903与数据库/WMS/WLAN在线、carrier1。[旧接口](../work/marble-dual-20261123/m15-h15-stereo-regression-1795482813-result.serial.txt) · [播放及调度](../work/marble-dual-20261123/m15-h15-accept-1795482868.serial.txt)。
- 下一阶段同步真实mono输入能力与OHOS VDI/policy，验证GUI录音，不把临时root后端探针提升成原应用域录音通过。


### 第21/22轮：PAL重建失配已定位为遗漏strip

- 原库与去mono差异重建基线的所有allocated ELF段逐字节一致，差异仅.comment/.symtab/.strtab/.shstrtab；对重建件执行相同llvm-strip后，整文件cmp精确恢复实装`054b1530…`。前轮部署hold已解除于构建身份层，不是忽略差异。[strip审计](../work/pal-mono-strip-audit.jYqNjq/report.json)。
- stripped mono后端`22f3350b…`以同工具/同链接输入构建；在H14基础生成单对象镜像候选`ec29d8b611399dd7572f5b5f36d1785f4be6669f3b8c2b4d9ee6d84464ed8306`，只改PAL server和清单，CPIO记录/AVB身份/独立解包往返通过。[镜像报告](../work/marble-pal-mono.4a5xcpt5/report.json)。**尚未部署**，本机仍H14。
- 后续只用已构建mono探针检查48000/1真实采样、frames=bytes/2、停止释放，并复查原stereo/播放回归。框架VDI/policy能力仍未同步，单后端支持不等于GUI录音已修。


### 第20轮：mono探针已构建，后端基线失配阻止直接部署

- 对照已装PAL build-report命令重建去掉mono差异的基线，得到`c9eb3aa9…`而非实装`054b1530…`，逐字节复现检查未过；没有忽略该失配并刷入新库。可能来自源/编译产物身份差异，须独立定位，不能用前轮NDK新编译件替代同工具基线。[同工具候选报告](../work/pal-mono-toolchain.sto0rucj/report.json)。设备仍H14 `76a30a32…`。
- 按已记录线上命令构建mono候选`85137b14…`，仅为待核对工件，未部署。独立mono探针`54e1f0ea…`使用48000/1、目标48000字节，并比较后端累计frame与bytes/2，带真实START/STOP/CLOSE；尚未上机，不以编译成功当mono硬件通过。
- 原音频与模式测试23项通过；下一步定位旧库/重建身份差异后才做镜像单对象增量。框架VDI/policy仍未修改，不宣称mono能力已上线。


### 第19轮：单麦与通话双麦模式分离，准备真实mono诊断

- 原厂ResourceManager对实际选中的speaker LOW_LATENCY明确默认channels=1、speaker-mic→AMIC5；双AMIC1+AMIC5的speaker-dmic-endfire仅在VOIP_TX或voice_call custom dual-mic定义。不能为录音立体声假改流type到通话用例。[模式对照](../work/marble-mic-xml.ijm5fxrv/capture-mode-report.json) · [校验器](../tools/maint/verify_stock_mic_modes.py)。与上轮左右完全相同的单源PCM结果一致。
- 新PAL后端源码候选增加channels1诊断支持，保留旧channels2接口；请求采用实际channel count，读取对齐与frames按协商channels×2计算，避免mono帧数被除4。当前VDI声明与线上仍为原stereo路径，未冒称已对齐整个框架能力。
- Bionic候选`504d696d…`编译通过、原音频测试通过，**尚未部署**。下一轮固定后端旧新构建差异与镜像单对象增量，上机用真实mono请求验证PCM与帧数，再决定VDI/policy能力同步；不伪造/补齐PCM。


### 第18轮：H14采集释放与逐声道反证

- 同H14 boot `9e5d2df5…`麦克风前/中/后getter确认AMIC5/SoundWire（DEC3/SMIC9），DMIC保持关闭，结束后路由恢复；实际96,000字节/24,000帧采集和STOP/CLOSE均0，原Digital Mic2错误未在此次窗口出现，1301/4606在线。[本版路由复测](../work/marble-dual-20261123/m15-h14-mic-snapshot-1795480170-result.serial.txt)。
- 新建仅统计、不保存PCM的逐声道探针，24,000对样本左右**逐对完全相同**，每声道23,108非零、峰值123、DC约-33.2、去DC RMS约24.8。这证明当前单AMIC5路线的立体声缓冲为同源数据，不能声称双麦独立采集。[原始统计](../work/marble-dual-20261123/m15-h14-channel-stats-1795480268-result.serial.txt) · [统计分析](../work/marble-dual-20261123/m15-h14-channel-stats-1795480268-result.serial.analysis.json)。
- 原请求48000/2和实际single-AMIC路由的采集能力语义需后续对齐（正确mono能力或原厂双AMIC路线），不凭名称/非零值声明真实立体声。未有受控外部声源、回放听感和GUI录音证据，本轮未改驱动或样本数据。


### 第17轮：限定动作窗口的菜单帧证据

H14同boot、Settings PID9064/窗口66，实机图像确认首页菜单坐标；采样只读取metrics原长度后的新增记录，不清空现有日志、不改composer。[实际首页](../work/marble-dual-20261123/m15-h14-perf-home-1795479700.jpg)。

- 多设备协同进入/返回动作窗口uptime633.45–639.49秒，新增67帧提交批次起点约633.570秒、终点636.588秒，落在该动作窗口内；最大提交服务耗时12,195µs、>25ms计数0，fence_error/clock_errors均0，网络与1901/1903持续在线。[原记录](../work/marble-dual-20261123/m15-h14-menu-measure-1795479895.serial.txt) · [独立分析](../work/marble-dual-20261123/m15-h14-menu-measure-1795479895.serial.analysis.json)。
- 该批次entry_max为16.225秒，大于整个动作窗口，来自保留的前次entry/idle间隔，不能当菜单等待时间。67帧服务耗时也不等于UI首次绘制/触控延迟；helper每步固定等3秒，未测实际菜单完成时点。已新增[指标边界分析器](../tools/maint/analyze_menu_metrics.py)与正负测试，禁止从idle计数冒称性能修好。
- 本轮未刷镜像；下一阶段需要原Input/RS提交事件时间点或页面内容首变证据，再判断应用主线程/Binder等待，而非继续靠SA数量或聚合平均帧时。


### 第16轮：资源自动晚启已固化并通过首次实机业务复测

H14 `76a30a3251e50e48beeda6032390fb54f746272ee89ad49e5dc2430a83fb2e8a`，boot `9e5d2df5-885b-462e-8ef8-865b112c761d`。仅编排追加[资源晚启脚本](../devices/marble/runtime/boot/late-resource-marble.sh)，原角色/profile/库不变：等待401/501/数据库/WMS/WLAN/SoftBus/DM，父子目录owner/mode核验，有界超时与单boot owner；错误不修复未知权限或盲开门。[构建报告](../work/marble-auto-resource.txtssm67/report.json)。

- 原编排155.87秒完成，SoftBus156.31秒触发、188.57秒结束，资源189.40秒自动触发，1901/1903在191.83秒前就绪，WLAN保持carrier1。外部诊断没有设置这两个gate。[自主启动记录](../work/marble-dual-20261123/m15-h14-auto-check-1795479455.serial.txt)。
- 新bootSettings原图标前台PID9064、窗口66，原resource_record出现该PID/UID的持久TIMER申请；音乐原Start/Stop/Release=0、71328帧，桌面正常、WLAN carrier1。[真实申请](../work/marble-dual-20261123/m15-h14-settings-1795479513.serial.txt) · [播放回归](../work/marble-dual-20261123/m15-h14-accept-1795479483.serial.txt)。旧其他process record仍是持久恢复数据，不以其旧PID当本轮有效进程证明。
- 正负测试覆盖前置SA失败、父目录不符、重复owner不触发；首次自主启动通过，第二次冷启动、菜单延迟和全部CPU/freezer/插件功能仍开放。


### 第15轮：设置退出与重开资源记录复验

同H13 boot `fab3e254…`，原Settings PID13022前台时只有一条app级TIMER持久资源。原UID2000 force-stop后旧进程退出且桌面取得焦点，三秒后存储appResourceRecord仍保留；未自行删除资源文件或把持久记录当泄漏。[退出实测](../work/marble-dual-20261123/m15-h13-resource-life-exit-1795478756.serial.txt)。

从原图标重开后Settings前台PID13947，同UID资源记录PID随之更新，resourceNumber仍4、unit仍一条、参数未重复，1901/1903/1301/4606/1120在线、WLAN carrier1。[重开实测](../work/marble-dual-20261123/m15-h13-resource-life-reopen-1795478812.serial.txt) · [独立JSON比较](../work/marble-dual-20261123/resource-lifecycle-check.json)。原库分析证实OnProcessDied只清PID-key process map；OnAppStopped(uid,bundle,false)清理mask排除TIMER4，所以本例仅TIMER记录保留符合donor设计，不能当泄漏。app重新申请按UID复用、资源mask OR、按resource index更新unit；与实测不重复一致。显式reset/release和菜单性能仍待验。[原生生命周期语义](../work/eff-resource-lifecycle/report.md)。


### 最新继续：原Settings效率资源申请已有业务记录

同H13 boot `fab3e254…`，用户继续指令后从锁屏恢复、保留音乐进程并返回桌面，原UID2000入口仅重启Settings再点击原图标，前台PID13022/窗口66。[实际操作](../work/marble-dual-20261123/m15-h13-settings-efficiency-1795478381.serial.txt)。

- 原服务生成的resource_record明确记录Settings真实UID20020062/PID13022：resourceNumber=4、resourceIndex=2、isPersist=true、timeOut=0、reason=apply。这是效率资源申请到原服务的业务证据，不只SA发布。当前Settings日志可见232条且requestTimerResources/RSS连接失败计数0；缺明确JS成功日志，不以“无错误”单独判成功。[真实服务记录](../work/marble-dual-20261123/m15-h13-efficiency-detail-1795478560.serial.txt) · [文件身份/错误计数](../work/marble-dual-20261123/m15-h13-efficiency-state-1795478533.serial.txt)。
- 1903 dump尝试rc10但内容0字节，保留为未取得dump数据，不把空输出当通过。原Settings传感器/主题缺口仍存在；菜单延迟与资源释放、重启后自动调度尚待验收。
- 原MUSIC播放Start/Stop/Release=0、71328帧，1901/1903/1301/4606/1120在线、WLAN carrier1，受控资源启用未见该轮基础回归。[音频回归/日志可见性](../work/marble-dual-20261123/m15-h13-efficiency-health-1795478490.serial.txt)。


### 第14轮：1901/1903实际发布，业务申请验收仍开放

H13资源试验已部署 `7cc4497aec53b02f8e0021829faa5e085e49cd358db0deaeb53b4ebaacb5d6b2`，boot `fab3e254-b982-4cac-9fc1-37fa8c1c2f67`。实装platform/profile hash与候选匹配，自动SoftBus完成、WLAN carrier1、401/501就绪，之后原init gate启动资源试验。[装机基线](../work/marble-dual-20261123/m15-h13-first-1795477762.serial.txt)。

- 原1901在约2058秒Publish，1903约2060秒Publish；实际进程comm为resource_schedule_trial（profile文件名影响），PID11606，UID/GID1096、组1000/5001、域resource_schedule_service，与原ressched角色相符。pidof resource_schedule_service空不等于退出。[实际启动](../work/marble-dual-20261123/m15-h13-resource-start-1795477794.serial.txt) · [身份/原日志](../work/marble-dual-20261123/m15-h13-resource-post-1795477821.serial.txt)。
- 七数据目录实际原owner/mode正确，cgroup2挂载保持，v1 cpuset/cpuctl/frz未创建；原cgroup/thermal/plugin缺路日志保留，未以插件返回true当硬件调度完成。[作用范围](../work/marble-dual-20261123/m15-h13-resource-effects-1795477920.serial.txt)。
- 1901/1903与4700/4802/1301/4606/1120持续在线、WLAN carrier1；原Settings重新启动尝试发现音乐前台，按现有foreground guard跳过，没有强行结束或切走音乐。故本轮尚未取得Settings效率资源申请成功、菜单延迟或真实播放回归。[前台保护](../work/marble-dual-20261123/m15-h13-settings-resource-1795477852.serial.txt) · [末次检查](../work/marble-dual-20261123/m15-h13-resource-final-1795477981.serial.txt)。门仍非持久自动启动，下一次冷启动需独立就绪流程。


### 第13轮：两SA资源试验镜像已构建，尚未部署

- 1901原OnStart本次调用直接Publish，无executor/1906就绪硬门；executor实际SA1918，缺proxy返回6但上层继续。插件原初始化错误可继续，不代表内核调度成功；真实卡死/崩溃仍可能阻断。cgroup适配保持独立，不复制虚假v1节点。[1901审计](../work/ressched-start-audit/report.md)。
- 在H12基础构建受控候选 `7cc4497aec53b02f8e0021829faa5e085e49cd358db0deaeb53b4ebaacb5d6b2`：原ressched UID/GID/权限/域，独立1901+1903 profile，门`marble.debug.start.resource_trial`默认关闭，仅七个原数据目录。库/原profile/cgroup/freezer不改，不递归chown已有内容。[差分报告](../work/marble-resource-trial.h0x6oymo/report.json)。尚未部署，本机仍H12 boot `b308023e…`。
- 真机401/501就绪，七目录均尚不存在；父public原权限0711。第一次预检错误假设0755而退出23，读回原值后修正预检，不修改父目录，第二次验证通过。[原DAC](../work/marble-dual-20261123/m15-h12-resource-dac-1795475468.serial.txt) · [修正预检](../work/marble-dual-20261123/m15-h12-resource-paths-verified-1795475509.serial.txt)。
- 受控发布实验下一轮继续：先刷并验证新镜像/角色/profile，再等待WLAN与桌面，原init gate启动，分别检查1901/1903就绪、原Settings申请资源、保持网络/音频；只发布SA不算效率资源业务通过。


### 第12轮：定时资源调用缺口明确，暂不盲启整个调度进程

- 当前H12同boot设置原图标前台PID8897/窗口66，4700/4802/1301/4606/1120在线、WLAN carrier1；原Settings仍报requestTimerResources9800004和ResSchedClient连接失败，1901/1903/1906缺席，3702正常。不能将已有时间服务当效率资源服务。[前台](../work/marble-dual-20261123/m15-h12-settings-1795474797.serial.txt) · [真实调用错误](../work/marble-dual-20261123/m15-h12-timer-baseline-1795474842.serial.txt)。
- 原backgroundTaskManager.applyEfficiencyResources的innerkits固定查1903，IPC code14；代理获取失败返回980000402再经NAPI映射，不是timer_service。1903效率资源manager依赖401/501/1901的ready mask全部成立，因此只发布1903不足以完成该功能。Settings ABC v24方法体尚未完整反汇编，原Clock方法体不可代替Settings精确证据。
- 原resource_schedule_service profile有14个run-on-create SA，不为单个timer资源问题全部启用。原厂init还要求cpuset/cpuctl/freezer路径；实机是cgroup2且这些路径缺失，不能直接复制pre-init/init的mount/chown/freezer操作。[原声明前置](../work/marble-resource-preflight.npv2mnmx/report.json) · [实机挂载](../work/marble-dual-20261123/m15-h12-resource-mount-1795474957.serial.txt)。已制作仅保留1903/1901且原SA字段不变的离线profile候选，尚未部署；先查1901自身依赖，再决定实际试装。下一步原角色、真实资源查询对照，冻结/CPU调度路径另批集成，不伪造ready或吞错。[完整原生调用报告](../work/timer-resource-chain/report.md) · [最小profile记录](../work/marble-efficiency-profile.qfh2l00a/report.json)。


### 第11轮：有界自动晚启已部署并通过首次启动

当前H12 `5f096219f33776c587cb06b411c76fccf4fb8d91e337997919c2602935d5a033`，boot `b308023e-09c5-4451-b2b6-80c7e34d98aa`。仅在H11 start编排末尾合入[有界晚启实现](../devices/marble/runtime/boot/late-softbus-marble.sh)，CPIO其余490条原记录不变；服务角色与库保持不变。等待1301/401/4606/1120和WLAN carrier1、单boot owner、60秒有界等待，失败不盲启动/重试。[构建报告](../work/marble-auto-softbus.5l5q4jyl/report.json)。

- 原编排156.16秒完成后，156.61秒才自动触发SoftBus，188.87秒4802就绪，结尾carrier1；全程未用外部param命令设gate。[自动启动实测](../work/marble-dual-20261123/m15-h12-auto-check-1795474573.serial.txt)。
- 新boot桌面1080x2400取得焦点，WLAN carrier1，原MUSIC Start/Stop/Release=0、71328帧，1301/4606/1120/4700/4802均在线。[基础/音频回归](../work/marble-dual-20261123/m15-h12-accept-1795474616.serial.txt)。这里只覆盖首次自动启动和原生音乐调用，第二次冷启动、设置前台/菜单耗时、采集和分布式业务仍待独立复测。
- 离线正负测试确认就绪失败保持gate闭合、已有owner不再触发，脚本语法和依赖/权限契约通过；不把模拟测试当实机失败模式已验收。


### 第10轮：同镜像晚启对照通过基础回归

当前H11 `aa247d8d7b97e0020f728f136a2ebb441a60011b7ca83f2dfb9750d238af2ee7`、新boot `63c4e287-37e0-45b5-a056-935939cb8b69`。同一镜像保持SoftBus门关闭完成启动，166秒时WLAN carrier1/up，1301/401/4606/1120在线，4700/4802尚未发布。原配置不存在自动gate setter，避免把未启对照混成早启。[晚启前基线](../work/marble-dual-20261123/m15-h11-late-baseline-1795473945.serial.txt)。

- 193秒确认桌面/WLAN/BMS与原platform hash后，才通过原init门启动SoftBus。约200秒4700发布，228秒4802及基础服务均在线，WLAN保持carrier1；原DM收到distributeddata/Samgr监听注册，未再次出现H10的DISTRIBUTED_DATASYNC缺权限断点。[受控启动](../work/marble-dual-20261123/m15-h11-late-control-1795473971.serial.txt) · [发布复验](../work/marble-dual-20261123/m15-h11-late-post-1795474007.serial.txt)。
- 设置原图标取得前台PID8608/窗口66，再点击同一多设备协同行。原Settings采样不再有DeviceNameUtils.createDeviceManager错误，但requestTimerResources9800004与缺hiwrite仍在。不是所有菜单等待已修，更不是分布式硬件业务通过。[设置前台](../work/marble-dual-20261123/m15-h11-late-settings-1795474031.serial.txt) · [同菜单对照](../work/marble-dual-20261123/m15-h11-late-menu-1795474056.serial.txt)。
- MUSIC真实调用Start/Stop/Release均0、71328帧，4700/4802/1301/4606/1120同时在线、WLAN carrier1。[音频与应用日志](../work/marble-dual-20261123/m15-h11-late-accept-1795474083.serial.txt)。此次说明晚启可避开之前WiFi回归，不能仅据两次启动证明唯一因果；自动晚启尚未写入镜像，下次重启仍需相同门槛，下一阶段固化可恢复的就绪调度。


### 第9轮：恢复基线完整回归与菜单动作实证

- 恢复H9、新boot `d7ab341b-17b8-451e-b483-0cd57923b739`在206秒时1301/401/4606/1120在线，WLAN carrier1/operstate up，设置原图标进入前台PID8434/窗口66。确认第8轮撤回后的基础功能恢复。[恢复实测](../work/marble-dual-20261123/m15-h9-r9-preflight-1795473204.serial.txt) · [设置前台](../work/marble-dual-20261123/m15-r9-settings-open-1795473249.serial.txt)。
- 实机截图固定“多设备协同”首页行坐标(540,1383)，原UID2000触摸进入真实页面；不是仅WMS窗口名推断。[设置首页](../work/marble-dual-20261123/m15-r9-settings-home-1795473273.jpg) · [目标页面](../work/marble-dual-20261123/m15-r9-multidevice-page-1795473349.jpg)。页面上的“已开启”是存储状态，不证明对应硬件/协同实际可用。
- 该次动作附近composer 60帧、平均提交服务约6.3ms，未出现>25ms服务耗时；指标包含动作前idle间隔，不能把entry_max约12秒当这次菜单阻塞。输入helper固定等3秒也不能当实际页面延迟。[动作/指标](../work/marble-dual-20261123/m15-r9-multidevice-1795473324.serial.txt)。
- 原Settings日志明确DeviceNameUtils.createDeviceManager失败11600101，以及requestTimerResources返回9800004（系统服务未连接）、sensor服务异常；多设备页进入后主线程采样在epoll，不是整机冻结。DM/SoftBus仍缺席，未绑定唯一阻塞根因，不直接从告警数量改源码。[去重日志](../work/marble-dual-20261123/m15-r9-menu-errors-1795473457.serial.txt)。
- 两个自有串口诊断并发曾争抢COM9导致Open访问失败，已补跨进程flock串行化；保留原始错误与解码日志，独立线程重叠负向测试通过。下步菜单点击期间采样首次渲染/原Binder等待，再晚启DM控制，与同菜单正负比较。


### 第8轮：DM依赖与原生角色分层修复

H11实验镜像 `aa247d8d7b97e0020f728f136a2ebb441a60011b7ca83f2dfb9750d238af2ee7` 已因联网回归撤回，现恢复H9 `fe7f0379…`，新boot `d7ab341b-17b8-451e-b483-0cd57923b739`，分区读回已核实，恢复启动复查已确认1120重新发布（H11时缺席），但约132秒carrier仍0、WMS尚未发布，完整关联/应用回归仍待完成；新增SoftBus声明的H10只改platform/清单，再以独立H11仅恢复DM原厂gid/APL/permission/ACL，库与校验函数不改。[H10报告](../work/marble-h9-softbus.d8zrrv6k/report.json) · [H11报告](../work/marble-dm-role.uc_k98s3/report.json)。

- H9虽有DM进程但未声明SoftBus。原DM OnStart只监听4700，收到SoftBus事件后才走Publish4802；原库映射SHA已匹配。[原生调用链](../work/marble-h9-dm-startup-readonly-20261124/report.md)。
- H10原SoftBus发布4700后，DM仍未发布；实测CreateSessionServer失败且原SoftBus权限检查返回DISTRIBUTED_DATASYNC拒绝。故不能仅以4700存在认为DM闭合。[H10失败](../work/marble-dual-20261123/m15-h10-dm-after-sb-1795472263.serial.txt)。
- H11恢复原厂DM角色后，4700/4802均实际发布，原DM dump返回真实帮助内容（dump rc10不代表空输出），原distributeddata周期Load DM失败在采样窗口为0。[DM发布](../work/marble-dual-20261123/m15-h11-dm-final-1795472685.serial.txt) · [实际dump](../work/marble-dual-20261123/m15-h11-dm-complete-1795472723.serial.txt)。未验证分布式配对、实际菜单延迟或动画改善。
- 本次在uptime53秒过早启动SoftBus，WLAN后续1120/1124/1126超时，carrier读取EINVAL；桌面后来发布但联网回归未过。安排同镜像重启，下一次必须等待原WLAN/桌面稳定再开SoftBus，分离早启并发与功能本身；若仍回归，恢复已保留H9。普通busybox reboot和sync后reboot -f均未改变boot_id，不算已重启；已通过原fastboot路径恢复H9且确认新boot_id。下轮从恢复基线稳定启动后重新做晚启实验。SoftBus child目录实际setgid位变化已记录，不改共享database权限。


**第7轮当前装机H9测试安装入口**：`fe7f03794462e4da6d22df06b3dd6780d611f08384e35b8bacd41477c2c2b16d`，boot `478afb06-64d4-49d1-b749-8e8f5196c374`。仅在H8增加本机绑定的自有调试HAP、固定bundle安装/卸载helper命令和stage落点；既有helper UID/APL/权限未改，原签名和设备检查保留。[镜像差分](../work/marble-h8-fixture.bjaie_ox/report.json)。实际安装已越过早期调用者权限错误，到原PKCS7证书链验证失败；详见第7轮，不算HAP安装已修好。

**第6轮H8序列号桥接正控**：`d21df3eaec95317c924224fde8dc692ae0d5da37972d6cda240935e203def0f1`，boot `b358189e-f545-4e16-bf8a-b7635a0be6b7`。仅替换H7探针和清单，491条CPIO中489条原始记录保持一致；去掉新桥接的同工具重建件与实装旧探针逐字节相同，确认不是把全部fresh源码改动塞进候选。[构建报告](../work/marble-h7-serial.j1j2no8h/report.json)。

- 真实bootconfig `androidboot.serialno`已传入OHOS可见RAM cmdline，原native参数`ohos.boot.sn`读回与实物原值一致。原stage30模块自行写入devUdid，`bm get -u`在BMS就绪后成功显示同值。[首轮参数](../work/marble-dual-20261123/m15-h8-serial-1795461795.serial.txt) · [高层就绪复查](../work/marble-dual-20261123/m15-h8-final-1795461931.serial.txt)。早期28/96秒bm失败是服务未就绪，163秒重查通过。
- 该devUdid独立计算与`SHA256(当前manufacturer || 当前model || 真实SN)`大写十六进制相同；manufacturer/model仍是donor的HUAWEI/ohos，不把它当Xiaomi产品元数据已修。桥接不填写预计算UDID、不搬模块到autorun、不修改IMEI或原厂分区。
- PAL/AGM初始化0、真实socket、WLAN carrier1，1301/401/4606/1120在线；H8实际应用前台和声音/录音仍需新boot复测。下一步正常HAP设备绑定与产品字段适配，保留H7最小恢复基线。

### 第7轮：本机调试签名与原生安装入口

- H8同boot复验设置、时钟前台成功，MUSIC播放Start/Stop/Release=0、71328帧，WLAN carrier1。[前台](../work/marble-dual-20261123/m15-h8-apps-1795462203.serial.txt) · [播放](../work/marble-dual-20261123/m15-h8-play-1795462247.serial.txt)。
- 基于官方SDK完整资源/ABC/签名流程重建自有 `org.example.marble.hapfixture.a91e4f62`、entry/EntryAbility，debug profile绑定实测UDID；normal APL、空ACL/受限权限，签名与profile的SDK/OpenSSL验证均0，未签和篡改负控均1。[完整报告](../work/marble-hap-fixture.Ws945r/verified.qj2ftwlf/report.json)。旧by nu目录报告为空，不能替代此轮完整证据。
- 原init UID1000 helper仅允许这一固定bundle的install-fixture/uninstall-fixture，HAP由RAM stage提供root:root0444；拒绝其他bundle与symlink，宿主exec参数正负测试通过。没有给USB角色安装权限，也没有改签名检查。
- H9在BMS就绪后正控query仍显示测试包未安装；实装调用到原PKCS7验证，返回9568257而非早期9568266调用令牌错误。原日志指出签名链的OpenHarmony root不在生效root集合（root数2），虽然trusted app source列表有OpenHarmony。此处缺口是原生信任根链匹配，尚未修改信任配置、补根或跳过校验。[安装结果](../work/marble-dual-20261123/m15-h9-install-fixture-1795463203.serial.txt) · [实际证书错误](../work/marble-dual-20261123/m15-h9-pkcs7-1795463255.serial.txt)。实际root文件SHA与donor一致，OpenHarmony条目的DER SHA也与SDK root一致，因此“仅缺root文件”已被反证。原libhapverify的FindMatchedRoot在内部DevMode=2时对OpenHarmony root有跳过分支；该mode的真实来源和含义仍在分析。当前developermode参数=true不是证明内部DevMode枚举。原bundle扩展在验签前读取const.boot.oemmode，非rd路径经devices_security_interface_service调用真实GetFuseState；仅返回1才设置DEV，否则NON_DEV（内部值2）。H9 oemmode缺失，extension与hapverify实际映射/磁盘SHA匹配；HDI返回尚未实测。没有设置rd、模拟开发fuse或改验签分支，保留原失败。OpenHarmony根DER匹配但原NON_DEV门控可以排除该根，因此SDK demo签名离线通过不等于本手机准入。[运行判定前置](../work/marble-dual-20261123/m15-h9-dev-kind-1795463659.serial.txt) · [完整证书链与原生模式分析](../work/hap-root-chain.report.md)。安装失败后bundle gate已复位、临时请求/日志已清理，数据库/BMS/WMS/WLAN服务与carrier保持在线；没有把失败重试扩成改信任绕过。

**H7历史部署正控**：镜像 `5b4e3db6cbd52676aadc48dfcb3673c508e15c4a88ff12b5e018472ca66b03bd`，boot `9b491ec6-b69f-4fc3-9e28-2bf7d1750a9d`。仅在 H6 基础上修正一个麦克风 XML 路径；当前本轮结果见下方第5轮，H6段落为历史正控。

**第 2 轮结果：H6 音乐 PCM 路由通过原生调用测试。** 已部署 `aec836041ffee5dec7030b9e3b4a5e95c2766fcde2f9b47c47312ac27bcfceac`，boot `4bb1f563-2eca-437a-9cb1-4f8245eac6a2`，策略读回 `1ed92a3943fe71ed76c07ed34dcfc15eca567e88fbe86c2d5814da5149a4ae2a`。本次仅将 H5 的 offload streamProp.supportDevices 再移除 Speaker_Out，原 VDI 保持 type5 不支持，原音乐用途和其它参数不变。原 parser 会从 streamProp.supportDevices 反向构造实际 device pipe map，H5 只改 device.supportPipes 因而无效。[原生解析证据](../work/pcm-parser-followup-20261124.md) · [H5 有效 dump](../work/marble-dual-20261123/m15-h5-pipe-config-1795457457.serial.txt)。

- 两次 MUSIC/NORMAL latency 原生探针均 `Start=0 / Stop=0 / Release=0`，回调与框架帧数分别 66,870 和 71,328；PAL 实际 START 和 PCM write 成功，旧实例 STOP/CLOSE 成功后新实例重新 OPEN/START。[首次](../work/marble-dual-20261123/m15-h6-play-1795457763.serial.txt) · [重开复验](../work/marble-dual-20261123/m15-h6-recheck-1795457814.serial.txt)。此处证明真实 PCM 提交链，不冒称已经由听觉确认扬声器出声、音量键或任意媒体应用全部正常。
- 本次开机 WLAN 自动关联，carrier=1、operstate=up、IPv4 存在；桌面实测 Clock（PID 8524 / 窗口65）与 Settings（PID8894 / 窗口66）取得前台，1301/4606/1120 保持在线。[应用回归](../work/marble-dual-20261123/m15-h6-apps-1795457858.serial.txt)。

### 第 3 轮：录音、音量输入及真实序列号桥接

- H6 本 boot 的采集复验：STATUS/OPEN/START/STOP/CLOSE 全部 0，读取 96,000 字节、24,000 帧、16,916 个非零样本，峰值 8,192。仍有 Digital Mic2 pinctrl -22，非零数据不等于正确麦克风/音质；原厂 static overlay 的同名 handset 路径其实选择 AMIC1+AMIC5，而当前 audioroute shim 只用 base XML。下一步固定真实 mixer getter与逐声道/声源对照。[采集日志](../work/marble-dual-20261123/m15-h6-capture-1795458201-result.serial.txt) · [路由调查](../work/mic-route-audit.md)。
- 用原生 UID2000 输入模拟 VOLUME_UP/DOWN，WMS 出现 SCBVolumePanel24，1301/4606保持在线；此结果仅覆盖软件按键链与面板，实体按键和实际音量另验。[音量输入](../work/marble-dual-20261123/m15-h6-volume-1795458295.serial.txt)。
- 序列号实机断点确认：bootconfig 有真实 androidboot.serialno，OHOS可见 cmdline未携带，ohos.boot.sn/devUdid缺失。原 init stage30已显式加载UDID库，不应搬到autorun。新增[桥接实现](../compat/bootstrap/boot_serial_bridge.h)在native init前保留原值、检查别名冲突，从bootconfig传入RAM cmdline；覆盖重复/冲突/路径型/空白/容量失败对照，AArch64 bootstrap编译通过。**尚未部署该bootstrap**，需先确认相对正在运行入口的完整差分，不把fresh源码构建当最小镜像。[原生初始化证据](../work/udid-chain.md) · [当前身份基线](../work/marble-dual-20261123/m15-h6-serial-baseline-1795458369.serial.txt)。
- 设置停顿采样看到DM加载周期失败，4700/4802/1903未发布；尚未绑定特定菜单动作，不把它们当唯一根因。修正一次诊断脚本语法错误后重新采样完整结束；现有composer闲置帧间隔不作为动画性能结论。[完整采样](../work/marble-dual-20261123/m15-h6-stall-verified-1795458833.serial.txt)。

### 第 4 轮：实际麦克风路由反证与待部署修复

- 固定 H6 PAL PID4698 的真实映射、环境和 XML hash，确认 audioroute singleton 使用原 base XML，static overlay 原件存在于 PAL 私有目录但未被 singleton应用。[有效路径](../work/marble-dual-20261123/m15-h6-mic-route-1795459242.serial.txt)。
- Bionic getter前/中/后三次读回显示：采集中 `TX DEC2 MUX=MSM_DMIC`、`TX DMIC MUX2=DMIC2`、DEC2开，AMIC1/AMIC5均关闭。Digital Mic2仍返回-22；延迟两秒后读到的96,000字节全零，进一步否定之前“非零即正确麦克风”的解释。[快照](../work/marble-dual-20261123/m15-h6-mic-snapshot-1795459972-result.serial.txt)。首个getter误用musl装载Bionic库，已保留错误并改用匹配Bionic工具，不把失败快照当成功。
- 非返回uprobes实际抓到 `audio_route_apply_and_update_path("speaker-mic")`，停止时reset同一路径。它不是先前仅从请求device推断的handset路径。[实际参数](../work/marble-dual-20261123/m15-h6-mic-path-1795460367-result.serial.txt)。所以不能仅替换handset-dmic-endfire。
- 新增[合成器](../tools/packaging/merge_marble_mic_route.py)仅替换一个原厂speaker-mic路径为static overlay规定的`amic5`，复用已验证base AMIC5/SoundWire定义，不动默认控件、speaker输出或其它path。正负对照通过，候选XML hash `03e0171e5d9524e661514151f72df9d588c6aa8241f435985294d8af7f0b610f`，**尚未部署**。下一轮在单独镜像增量中只改singleton的XML输入，再复查真实getter、DAPM、声源响应与播放回归，不吞掉驱动错误。[候选记录](../work/marble-mic-speaker-amic.g848llql/report.json)。

### 第 5 轮：H7 实机 AMIC5 路由修复通过

- H7 对 H6 的解包差分仅为 mixer XML 和清单；芯片、DSP/标定、原库及播放policy保持不变。只将实际调用的speaker-mic按原厂static规则替换为amic5。[候选差分报告](../work/marble-m15-audio-pal.bgxpv8xn/report.json)。
- 两次采集前/中/后getter确认：采集中DEC3=`SWR_MIC`、SMIC3=`SWR_MIC9`、DEC3开、AMIC5开；DMIC MUX2保持ZERO、DEC2关闭。结束后恢复；同窗口不再有H6的Digital Mic2 PRE_PMU/POST_PMD -22。[首次快照](../work/marble-dual-20261123/m15-h7-mic-snapshot-1795460894-result.serial.txt) · [启动完成后复验](../work/marble-dual-20261123/m15-h7-mic-snapshot-1795461043-result.serial.txt)。
- 延迟两秒后仍真实读取96,000字节，非零样本46,388、峰值113，API/停止/关闭均0；不将这些统计提升为声源位置、左右独立性、语音质量或GUI录音通过。下一阶段需受控外部声源与实际录音应用验收。
- 本boot原MUSIC播放Start/Stop/Release=0，帧数71,328；Settings（PID9204 / 窗口66）与Clock（PID8626 / 窗口65）取得前台，WLAN carrier=1，1301/4606/1120均保持在线。[播放](../work/marble-dual-20261123/m15-h7-regression-1795461002.serial.txt) · [应用回归](../work/marble-dual-20261123/m15-h7-apps-1795461128.serial.txt)。

下面 H4/H5 内容为同一调查的历史对照，不再代表当前装机：

最新已刷入 H5 PCM 策略候选 `a0f42900a280ff6441d3ac642c90abadfd8d00960c702ddf04e2716c6f121f90`，boot `36de142f-5f67-44d2-9bca-a7d43900b446`；策略实际读回 `b236bc94…`。本 boot 1301/4606/1120/3001 在线、AGM/PAL 初始化为 0，但原 OHOS 播放仍返回 Start=2，日志仍有 type5 创建请求，说明仅撤销 Speaker_Out 的 offload pipe 关联未解决实际选择链。下一步核对 native policy 的有效设备/route flag 和首次失败点；不将此候选算为播放修好。[H5 实测](../work/marble-dual-20261123/m15-h5-play-1795456638.serial.txt)。下面音频/采集结果来自前一版 H4 `33ea94a21c02b0668029175540b034bde309232a1286d833c529d8fd38a34b7b`、boot `0e31f622-41e8-4ab0-8b71-f23d5912eb87`，不能混写为 H5 已通过。二者均基于 F 的 WLAN/init 基线。

- 已实测 `PAL_SERVER_AGM_INIT_RC=0`、`PAL_SERVER_PAL_INIT_RC=0` 和真实 PAL socket；进程以 linker64 名称运行，`pidof agmplay` 空不代表退出。[初始化结果](../work/marble-dual-20261123/m15-h4-result-1795454891.serial.txt) · [进程/线程](../work/marble-dual-20261123/m15-h4-threads-1795455030.serial.txt)。
- H4 的 OHOS 播放探针能生成 renderer，但 `OH_AudioRenderer_Start` 返回 2。[播放结果](../work/marble-dual-20261123/m15-h4-play-1795454947.serial.txt)。后续直接 PAL 生命周期探针 `STATUS/OPEN/START/STOP/CLOSE` 均为 0，表明后端能实际启动；未提交 PCM，因此不算发声。原生链选择 `type=5`（offload），当前 PCM-only VDI 真实不支持。已根据原 policy 的 normal fallback 制作 H5，只撤销 Speaker_Out 对 offload_output 的关联，保留 VDI 能力边界、原其它 pipe 与参数。[调用链分析](../work/audio-start-chain.md)。
- 麦克风探针的最初 `OPEN rc=-104` 是探针复用了 STATUS 的单次连接；改为新连接后，实际 `OPEN/START/STOP/CLOSE` 均为 0，读取 96,000 字节、24,000 帧，其中 18,278 个非零样本，峰值 8,192。此项证明真实后端返回采样，不等于音质或 GUI 录音验收；内核仍有 Digital Mic2 pinctrl 告警，待测试声源相关性和通道。[采集复查](../work/marble-dual-20261123/m15-h4-capture-reconnect-1795456203-result.serial.txt)。
- WMS 桌面已恢复 1080×2400，数据库在线，WLAN carrier=1；新版本的真实应用前台、声音、录音仍需各自复验。
- 前三次音频候选已分别保留失败证据：宿主路径启动 loader 失败；PAL 读取资源 XML 的根目录别名缺失；mixer XML 缺失导致原库 SIGSEGV。后续只在 PAL 私有 overlay 补原厂路径，不覆盖 OHOS 公共 USB 音频配置。

## 分批顺序及完成门槛（2026-11-24 第46轮重排）

> 以下按截至第 45 轮已实机闭环项（桌面/DPI/状态栏/按键/WLAN/PAL 音频与 AMIC5 录音/YUV 黑视频修复/开机动画出帧）重新收口，按“低风险高收益 → 交互体检 → 高风险独立窗口”顺序推进：

| 批次 | 功能 | 下一步工作 | 完成门槛 |
| --- | --- | --- | --- |
| A（已完成基础闭环） | 蓝牙（QCA6490） | **已完成 A1/A2/A3（`0fc0cf63…`，`boot_id=a06e6049…`）**：冷启动自动 3.2 Mbps + `hpbtfw21.tlv`/`hpnv21g.bin`（SIBS 关）+ HDI + SA 1130 + SA 1191；设置真实 UI 开→关→开与 12 个周边设备实时渲染通过，WLAN/桌面无回归。**留待独立验**：外设配对与 A2DP 音频 | 基础开关/扫描/重连/UI 列表已过；配对与 A2DP 另验 |
| B（当前） | 音视频收尾、WebView、应用与主题、定位 | B1：快手动态画面回归 + 扬声器真实听感/固定 MP4 UI seek + NV21（format 25）Turnip 导入修复；B2：原浏览器与嵌入 WebView 跑离线正负夹具 + 微博/音乐 App 排障；B3：主题 variant/激活/恢复与定位真机 fix | 快手/MP4/NV21 实际画面与音画正常；Browser 与嵌入 WebView DOM/Canvas 交互通过；微博/音乐可正常进入操作；主题可应用并恢复；定位返回真实 fix 且关闭有效 |
| C | 灭屏/亮屏同步、动画卡顿与设置等待 | C1：采集灭屏/亮屏 SDE atomic/fence/vblank 序号定位 `sde_encoder` VSYNC 超时与 `-22`；C2：设置菜单与应用切换同窗采样 Binder/SA 等待、首帧与输入时序，并排查存储 `quota ESRCH` 与实况窗倒计时到期收尾 | 灭屏/亮屏无超时异常；设置/应用数秒卡顿定位并显著改善；存储容量分类与实况窗到期收尾通过 |
| D（高风险窗口） | 基带、SIM、信号、电话、短信 | 复核 `boot_a` PAS 恢复/超时策略与 `/compat/firmware` 同源装配，在具备 fastboot 物理恢复前提下拉起 MSS → 核 QRTR（WDS/DMS/NAS/UIM/PDC）→ 逐级接 RIL/telephony | MSS 稳定 `running` 且不 panic；真实 SIM 识别、网络注册与信号；用户配合验数据/短信/通话（不改 IMEI/标定，不测紧急呼叫） |
| E（高复杂度） | 相机 | 同 boot 核查 `camera.ko`、固件、CRM 绑定与真实传感器拓扑，设计原厂 Qualcomm Camera HAL → OHOS Camera HDI 最小桥接 | 前后摄连续预览帧、拍照保存与重读通过（录像另验） |
| F（收尾） | HDC 共存、序列号/UDID 与 HAP、工件精简 | 在保证 COM9 可恢复前提下验证 USB 组合（ACM+HDC）与主机 `hdc.exe` 哈希往返；补齐真实序列号/UDID 与本机调试签名 HAP 生命周期；清理可再生旧镜像 | `hdc` 主机授权与文件哈希往返通过；原生读回真实身份且唯一测试 HAP 安装/启动/卸载通过；仅保留当前候选与最少恢复基线 |

## 过期工件清理（2026-11-24）

已移除 H1/H2/H3 三个淘汰音频整镜及其 Windows 重复副本、可重新提取的 F 解包缓存，共 11 个文件、680,558,434 字节（约 649 MiB 逻辑大小）。当前 H4、最少 F 恢复基线、报告、冻结构建脚本和原始故障日志保留。清理前后保护件哈希一致。[删除允许表](../work/marble-cleanup.xyt8r9am/plan.json) · [实际结果](../work/marble-cleanup.xyt8r9am/result.json)。已撤下对应失效部署入口；没有清空 work 或删除原厂固件/标定。

## 批次 F2 实机闭环里程碑（2026-11-25 更新）

- **旧现象 → 固定输入 → 最小差异**：在 F1 `830aad72`（仅 MSS+`hmos_rmtfs`+只读 QMI）基础上，固定接入 clean-room [libril_vendor_qmi.c](../compat/telephony/libril_vendor_qmi.c)（`SONAME=libril_vendor.z.so`，按 donor `libhril.z.so` `0x5f940/0x5f94c` 修正 `ReqDataInfo`=`{serial, request, slotId}`；阻塞 `Dial`/`SendGsmSms`/`SendCdmaSms`/`ActivatePdpContext` 返回 `HRIL_ERR_GENERIC_FAILURE`）、[native_ril_hdi_loader.c](../compat/telephony/native_ril_hdi_loader.c)（加载 `ril_service`）、7-host HCB（`2920` B，添加 `riladapter_host` `preload=2`）、门控 [init-modem-f2-marble.cfg](../devices/marble/runtime/telephony/init-modem-f2-marble.cfg)、[telephony-marble.json](../devices/marble/runtime/telephony/telephony-marble.json)（SA `4010/4007/4008`）与 [start-modem-f2-marble.sh](../devices/marble/runtime/telephony/start-modem-f2-marble.sh)（裁剪 `param get` 尾部空格，按需 bind-mount 与加载 IPA/RMNET `.ko`）。构建输出 `/mnt/e/marble-tmp/vendor_boot_h20_f2_telephony_8da1f9b1.img`（`sha256=8da1f9b13497a21012dda4c9f6829248a6ed6de48e7e83cbd60f8571d8e821c9`），`boot_a=c83a8468…` 未动。
- **已过验证层与新版实际结果（`BOOT_ID=f2fac75e-35a7-4186-8d87-80793f992d52`）**：
  - 主机验证：`RIL_VENDOR_QMI_HOST_TEST_OK=1`、`MODEM_PROBE_SELF_TEST_OK=1`、`Ran 48 tests in 58.669s — OK (skipped=1)`。
  - COM9 Check 1（闲置门控）：`REMOTEPROC2=offline`、`REAL_RIL=0`、`HCB_SIZE=2920`、`const.sys.radio.vendorlib.path=/vendor/lib64/libril_vendor.z.so`、`const.telephony.slotCount=2`、`PERSIST_CNT=0`、零新崩溃。
  - COM9 Check 2（`--online`，`ONLINE_RC=0`）：`F2_QRTR_READY=1 count=8 wait_s=3`、UIM 槽位 1 `CSIM`+`USIM` `state=7(ready) pin1=3`、`UIM_EF_ICCID masked=89***31`、`UIM_EF_IMSI mcc=460 mnc=11`、`LOC_REV rev=152`、`original-firmware` 保持 `ro`。
  - COM9 Check 3（`--ipa`，`IPA_RC=0`）：全 6 个 IPA/RMNET 模块加载通过（`missing=none failed=none`）+ `"IPA FW loaded successfully"`、`WLAN0_CARRIER=1`、`DMESG_OOPS=0`。
  - COM9 Check 4（`--telephony`，`TEL_RC=0`）：`F2_RIL_VENDOR_BOUND=1`、`RIL_HDI_LOAD ril_rc=0 svc_ril=1`、`LoadVendor` -> `HRilRegOps completed` -> `servmgr add service ril_service, result is 0`、`riladapter_host` (PID `10100`) 与 `telephony-marble` (PID `10108`) 常驻、SA `4010/4007/4008/4009` 全 READY、`CoreService` 在 `EsimManager::OnInit(8)` 超时结束等待后广播真实 `usual.event.SIM_STATE_CHANGED:1`（slot 0 `NOT_PRESENT`）与 `:4`（slot 1 `SIM_STATE_READY`）及 `SimFileManager`，`OOB_CNT=0`、`RAW_PII=0`。
  - 全量基线零回归：`PAL_PCM=0`、`PERSIST_CNT=0`、`CRASH_CNT=17`（零新崩溃）、`REMOTEPROC3=running`、`WLAN0_CARRIER=1`、`COMPOSER_FDS=22`、`DMESG_OOPS=0`、`BT_HOST_STATE_RC=0 STATE=1 uid=0`、`SENSOR_CLIENT_INFOS rc=0 count=14`、15 项基线 SA + 4 项 Telephony SA 全 READY。
- **未过门槛与回退（留待批次 F3）**：`DMS_SET_MODE online` 仍返回 `err=0x34` / `op_mode=3(offline)`（QRTR `tftp 0x1000` 缺席与无 `/hmos-root/persist` 挂载）；`const.booster.virtual_modem_switch=true` + `persist.distributedmodem.multidevice.enable=true` 触发 8 槽位虚拟卡并导致 `EsimManager::OnInit(8)` 因缺 `SA 66250` 等待约 64s 及 `CellBroadcast Invalid slotId 2..7`；GNSS `QMI_LOC` 与通话 BT SCO 待 F3 接入。回退 F1 `830aad72` 用 [flash-h20-f2-control.sh](../work/marble-dual-20261123/flash-h20-f2-control.sh) `restore <BOOT_ID>`。

## 批次 F3 实机闭环里程碑（2026-11-25 更新）

- **旧现象 → 固定输入 → 最小差异**：在 F2 `8da1f9b1` 基础上，固定合入：① RAM 叠加参数修正（`const.booster.virtual_modem_switch=false`、`persist.distributedmodem.multidevice.enable=false`、`const.ril.esim_type=0`、`vendor.hw_mc.ril.esim.close_esim=false`）及 Option A [telephony-f3-marble.json](../devices/marble/runtime/telephony/telephony-f3-marble.json)（替换镜像内 `graphics/telephony-marble.json` 并补入 `SA 66250`，不改源 `telephony-marble.json` 与 `test_marble_modem_f2.py`）；② clean-room RO->RAM [hmos_tqftpserv.c](../compat/telephony/hmos_tqftpserv.c)（3 路自动绑定 QRTR `0x1000` `inst=0x1/0x101/0x0`、`persist` `ro,noload` 瞬态拷贝至 `/run/marble-rfs-ram` 后立即卸载、拒绝 `..` 及 `/readonly/` 写请求）与 [marble_modem_probe.c](../compat/telephony/marble_modem_probe.c)（新增 `--dms-raw` / `--loc-probe`）；③ 纯 C（`NEEDED=[libc.so]`）[native_gnss_hdi_publisher.c](../compat/location/native_gnss_hdi_publisher.c) 与门控 [init-modem-f3-marble.cfg](../devices/marble/runtime/telephony/init-modem-f3-marble.cfg)（`uid=gps`、`u:r:location_host:s0`，SA 5100 发布 `gnss_interface_service` V2.0 + 80B `LocationInfoV2_0` + `QMI_LOC` 工作线程）。[F3 构建脚本](../tools/packaging/build_marble_h20_f3_rf_gnss.py)产出 `/mnt/e/marble-tmp/vendor_boot_h20_f3_rf_gnss_e4043df4.img`（`sha256=e4043df42d9c703c064cae8b900ee6305e5f16665330eb2cd31a2fc379cb15d5`），`boot_a=c83a8468…` 未动，8 项保护成员零改动。
- **已过验证层与新版实际结果（`BOOT_ID=280717b4-ec52-447f-8e41-c33fff923f65`）**：
  - 主机验证：`TQFTPSERV_SELF_TEST_OK=1`、`MODEM_PROBE_SELF_TEST_OK=1`、`GNSS_HDI_HOST_TEST_OK=1`，6 组单测共 `Ran 54 tests in 70.349s — OK (skipped=1)`。
  - COM9 Check 1（闲置门控 & 参数）：`KEY_PROCS=8`、`bootevent.boot.completed=true`、`RPROC2=offline`、`PID_TQFTP=none`、`PID_GNSS=none`、`PERSIST_MOUNTED=0`、`TEL_66250=1`、`CRASH_CNT=17`，4 项新 boot 参数全部实机读回生效。
  - COM9 Check 2（`--tftp` + F1 + `--dms-raw`/`--loc-probe`）：`F3_PERSIST_UNMOUNTED=1`、`F3_TQFTP=ready pid=9802`、`F1_MSS_STATE=running`、`QRTR_WAIT_S=0 COUNT=8`；实机首次跑通 `WRQ /readwrite/server_check.txt blk=7680 wsize=10 opts=1 -> bytes=5 rc=0` 与 `/readwrite/mcfg.tmp`，并精准捕获 MPSS 请求的缺失路径（`/readwrite/ota_firewall/ruleset`、`/readonly/firmware/image/modem_pr/{so/980_0_0.mbn,mcfg/configs/.../mbn_{hw,sw}.dig}`）。`DMS_SET_MODE online` 仍 `err=0x34` / `op_mode=3(offline)`、`DMS_CAP tx=0 rx=0 data=4 sim=2 ifs=0:[]`、`PDC_LIST err=0x11`、`LOC_REV rev=152 err=0x0`（offline 下 `LOC_REG_EVENTS err=0x30`、`LOC_START err=0x2e`、`LOC_STOP err=0x0`）。
  - COM9 Check 3（`--ipa`）：`IPA_RC=0`，全部 6 项 IPA/RMNET 模块及固件加载完成。
  - COM9 Check 4（`--telephony`，`TEL_SH_RC=0`）：`SA 4010/4007/4008/66250` 在 **`wait_s=1`s** 内全部 `NATIVE_SA_READY`（消除 ~64s 等待），`SIM_STATE_CHANGED:1`/`:4`、`SimFileManager` 与 `CellularData Slot1` 正常，`OOB_CNT=0`、`SA66250_FAIL=0`、`SLOT3_7_COUNT=0`。
  - COM9 Check 5（`--gnss`，`GNSS_SH_RC=0`）：`pid=10427` `uid=1021` `GNSS_HDI_PUBLISH_RC=0 service=gnss_interface_service`，跨进程 `--check` 通过 **`GNSS_HDI_CHECK rc=0:0:0:0 maj=2 min=0 cb_caps=1(0x1) cb_ws=4(last=4)`**。
  - 全量基线零回归：`PAL_PCM=0`、`PERSIST_MOUNTED=0`、`PERSIST_PARAM_CNT=0`、`CRASH_CNT=17`、`REMOTEPROC3=running`、`WLAN0_CARRIER=1`、`COMPOSER_FDS=22`、`DMESG_OOPS_STRICT=0`、`NATIVE_SA_READY id=1130`、`SENSOR_CLIENT_INFOS rc=0 count=14`、21 项实测 SA 全 READY（`402` 同前基线不可用）。
- **未过门槛与回退（留待批次 F4）**：① MPSS 请求 `/readwrite/` 应映射 `rfs/msm/mpss/` 且 `modem_pr/{so,mcfg/configs}` 缺失消化/SO 文件，`DMS_CAP` 呈 `tx=0 rx=0 ifs=0`、`DMS_SET_MODE online` 仍 `err=0x34`，致真正蜂窝注册/通话/短信/GNSS 真实定位及 BT SCO 留待 F4；② `SignalStatus-SignalModel` / `CellBroadcast` 仍残留 `slotId 2`（槽 3–7 已清零）。回退 F2 `8da1f9b1` 用 [flash-h20-f3-control.sh](../work/marble-dual-20261123/flash-h20-f3-control.sh) `restore <BOOT_ID>`。

## 批次 F4 实机闭环里程碑（2026-11-25 更新）

- **旧现象 → 固定输入 → 最小差异**：在 F3 `e4043df4` 基础上，固定合入：① 修复 [hmos_tqftpserv.c](../compat/telephony/hmos_tqftpserv.c) 中 `--ro-dir` 压缩时的重叠 `snprintf` 归零缺陷（抽取 `compact_ro_dirs`），补齐 `/readwrite/`→`rfs/msm/mpss/`、`/shared/`→`rfs/shared/`、`/hlos/`→`hlos_rfs/` 虚拟路径映射、修复 `rsize=0` 与 `timeoutms` / `OACK` 响应及发布后真实 QRTR 端口输出；② 扩展 [marble_modem_probe.c](../compat/telephony/marble_modem_probe.c)（`--services-all`、`DMS_BAND_CAP` `0x0045`、`DMS_ACT_STATE` `0x002f`、`NAS_RF_BAND`、`NAS_SYS_INFO`、`PDC_GET_SELECTED_CONFIG` `0x0021`、带 `0x10` token 的 `PDC_LIST`、`QMI_LOC_INFORM_CLIENT_REVISION` `0x0020` 与掩码降级及错误解码）；③ 更新纯 C [native_gnss_hdi_publisher.c](../compat/location/native_gnss_hdi_publisher.c) 发送 `QMI_LOC_INFORM_CLIENT_REVISION` 并记录 `LOC_REG`/`LOC_START` 响应错误码；④ 新增 [start-modem-f4-marble.sh](../devices/marble/runtime/telephony/start-modem-f4-marble.sh) 与 [F4 构建脚本](../tools/packaging/build_marble_h20_f4_rf_mcfg.py)，在 `STAGE_F4_BLOCK` 中向 `/sys_prod/variant/region_comm/china/etc/param/hw_defaults.para` RAM 叠加设置 `const.telephony.vsimModemCount=0`。构建产出 `/mnt/e/marble-tmp/vendor_boot_h20_f4_rf_mcfg_7d92e2a0.img`（`sha256=7d92e2a0b438e9b630391523cea2e952e5423ba31d2a163f82462fc9b1f77530`），`boot_a=c83a8468…` 未动，10 项保护成员逐字节不变。
- **已过验证层与新版实际结果（`BOOT_ID=efc809e8-81b5-458c-999f-9d6ebe2eef8e`）**：
  - 主机验证：`TQFTPSERV_SELF_TEST_OK=1`、`MODEM_PROBE_SELF_TEST_OK=1`、`GNSS_HDI_HOST_TEST_OK=1`，7 组单测（新增 [test_marble_modem_f4.py](../tests/test_marble_modem_f4.py)）共 `Ran 60 tests in 60.895s — OK (skipped=1)`。
  - COM9 Check 1（闲置门控 & 参数）：`KEY_PROCS=8`、`bootevent.boot.completed=true`、`RPROC2=offline`、`RPROC3=running`、`PERSIST_MOUNTED=0`、`F3_PARAMS` + `F4_PARAMS vsim=0` 全生效。
  - COM9 Check 2（[start-modem-f4-marble.sh](../devices/marble/runtime/telephony/start-modem-f4-marble.sh) `--modem`）：`TQFTPSERV_READY ports=16450,16451,16452`、`TQFTP_RO_MCFG_OK=6`、`TQFTP_RO_MCFG_REJECT=0`（成功传输 `980_0_0.mbn`、32 B `mbn_hw.dig` 与 32 B `mbn_sw.dig`），解锁 **`DMS_CAP tx=316800 rx=316800 data=4 sim=2 ifs=5:[1,4,5,8,10]`**、`DMS_BAND_CAP`（`rc=0 err=0x0`）、`DMS_ACT_STATE`（`rc=0 err=0x0`）、`NAS_SYS_INFO`（`rc=0 err=0x0`）与 `DMS_SET_MODE req=online rc=0 err=0x0(none)`，基带随后稳定进入 **`op_mode=0(online)`**；`UIM_CARD slot=1` `CSIM`+`USIM` `ready`（`ICCID masked=89***31`, `IMSI mcc=460 mnc=11`），`QRTR_ALL_COUNT=76`。
  - COM9 Check 3/4/5 & 全量回归：`IPA_RC=0`；Telephony `wait_s=1`、`SA 4010/4007/4008/4009/66250` 全 `NATIVE_SA_READY`、`CellularData slotId=1 numeric=46011 opkey=46011`、`SignalStatus-SignalModel` `slotId 2` 归零（仅余 1 条 `CellBroadcast StaticSubscriber: Invalid slotId 2`，`SLOT2_COUNT=1`、`SLOT3_7_COUNT=0`）；`gnss_hdi_marble`（`pid=11666`, `uid=1021`）`GNSS_HDI_PUBLISH_RC=0`，`GNSS_HDI_CHECK rc=0:0:0:0`；`PERSIST_MOUNTED=0`、`CRASH_CNT=17`、`REMOTEPROC3=running`、`WLAN0_CARRIER=1`、`COMPOSER_FDS=25`、`DMESG_OOPS_STRICT=0`、`NATIVE_SA_READY id=1130`、`SENSOR_CLIENT_INFOS rc=0 count=14`、**21/21** 实测 SA 全 READY；夜间静音约束未改 `0.03f` 且未做外放播放测试。
- **未过门槛与回退（留待批次 F5/G/H）**：① `NAS_SERVING` 仍 `reg=0(not_registered) cs=2 ps=2`（`WRQ /readwrite/mcfg.tmp ... unlink=0` 末包超时 `rc=-110`、`PDC_LIST err=0x11`、`PDC_SELECTED err=0x39`）；② `QMI_LOC_REG_EVENTS` (`0x0107`/`0x0007`/`0x0003`) 返回 `err=0x30(invalid_arg)`、`LOC_START` 返回 `err=0x2e(invalid_op)`（期间收到 `pos=1` 位置指示）；③ 仍余 1 条 `CellBroadcast StaticSubscriber: Invalid slotId 2`；④ BT SCO（`SLIM-DEV1-RX-7/TX-7`）、扬声器 `0.03f` 增益恢复及白天音频/ALS 留待后续。回退 F3 `e4043df4` 用 [flash-h20-f4-control.sh](../work/marble-dual-20261123/flash-h20-f4-control.sh) `restore <BOOT_ID>`。

## 批次 U1.7 实机闭环里程碑（2026-11-25 用户实测确认）

- **用户已确认工作**：关于手机 OEM/版本/CPU/HarmonyOS 动画、华为账号登录、天气与微博联网、屏蔽 OTA 及消除红点、三方应用权限、指南针/SLPI 传感器、图库不再卡死（仍略微卡顿）；核心产出 `/mnt/e/marble-tmp/vendor_boot_h20_u1_ux_security_7689087f.img`（[build_marble_h20_u1_ux_security.py](../tools/packaging/build_marble_h20_u1_ux_security.py) + [db_key.c](../compat/framework/database/db_key.c) + `push_manager_service` `SA 65728` + `privacy_service` 21 权限/6 ACL + 移除 `SA 3006` 与 OTA hosts 屏蔽）。
- **边界核查**：Stock OTA（[extraction-report.json](../work/donor-rom/ADY-AL10-207.0.0.111-SP10/extracted/base/extraction-report.json)、`update_full_cust`、`update_full_preload`）均不含 `anco_hmos.img`。华为账号登录通过不等于设备证明或商店业务通过。`com.huawei.hmos.location` 的 `OUCS 631008 / 12000001` 是定位链单独观察到的错误，不作为商店业务码；商店证据见下方[专项核查](#marble-store-attestation)。

## 重排后后续修复计划（2026-11-25 更新）

1. **Batch U2.1（已完成构建与 COM9 实机启动验收）**：原生 HDF `location_host`（`libmarble_gnss_hdi_driver.z.so` + 8-host `hdf_default_phase4.hcb` + `init-u2-location-marble.cfg`）、稳态 MPSS `op_mode=1(low_power)`（消除 `nr5g_rrc` SSR 崩溃）与 `marble_location.para`（`const.location.*`）、GPU `surface_post` 热路径消抖减日志、高通 Venus VPU 固件与 `msm_video.ko`（`/dev/video32..33`）基础就绪。
2. **Batch U3（拆分为 U3.1 / U3.2 推进）**：
   - **Batch U3.1（已完成构建与 COM9 实机验收 `10ddf24b`）**：原生 HDF `codec_host`（host index `8`，`codec_image_service` HDI 2.1 + CLEAN-ROOM ARM64 NEON `libjpeg_vdi_impl.z.so` 桥接 `libturbojpeg.z.so`，支持 post-SOS scan 重建 JPEG 头与 DMA-BUF 同步）、`composer_host` 内置 `displayengine_device` V1.1（`libdisplayengine_module.so`）与原 `display_engine_service`（`SA 65962` 就绪并移出可选表）、及只读 `native_venus_v4l2_probe` 实机探测。
   - **Batch U3.2a（已完成构建与 COM9 实机全链路验收 `fd7a6b4a`）**：高通 Venus V4L2（`/dev/video32..33` `H264/HEVC`）→ OHOS OpenMAX / Codec HDI 4.0（`libOMX_Core.z.so` + `codec_omx_idl_device` + `media_codec_capabilities` HCB，`OMX.marble.video.{decoder,encoder}.{avc,hevc}`）+ `libdisplay_buffer_vdi_impl.z.so` 视频编码/解码 usage。
   - **Batch U3.2b（已完成构建与 COM9 实机全链路验收 `d5299691`）**：clean-room `/vendor/lib64/libheifdecode_vdi_impl.z.so`（Venus `/dev/video32` HEVC 单帧/`grid` 硬解与 `V4L2_BUF_FLAG_LAST` 排空）、`HDF_Image_hw_dec_heif_marble` HCB、`native_heif_probe`（DMA NDK 3x7=21 例全过且零 `msm_vidc` 错误）及图库 `RecFFRTWorker` 卡顿定位结案。
3. **Batch F6**：蜂窝 NAS 注册（`46011` MCFG/PDC）、通话/短信/移动数据与蓝牙通话 SCO。
4. **Batch G → H**：相机（`camera.ko` + Camera HAL→HDI）→ 仅在用户明确允许的白天窗口校准扬声器音量（夜间严格保持 `0.03f` 不改）与 USB `COM9+HDC` 调试生命周期。

## 批次 U2.1 实机闭环验收里程碑（2026-11-25 更新）

- **旧现象 → 固定输入 → 最小差异**：针对 U2 (`d25c9255`) 定位失败的 3 个根因，在 U1.7 `7689087f` 基础上固定合入：① 将 [native_gnss_hdi_publisher.c](../compat/location/native_gnss_hdi_publisher.c) 扩展为标准 uhdf2 驱动 `libmarble_gnss_hdi_driver.z.so`（导出 `driverDesc`、`NEEDED=[libc.so]`，共用 `QMI_LOC` + `UTC` 注入分发逻辑），更新 [build_marble_phase4_images.py](../tools/packaging/build_marble_phase4_images.py) `build_phase4_hdf_hcb(..., location_host=True)` 新增第 8 个宿主 `location_host`（host index `7`、`preload=2`、`gnss_interface_service`），并由 [init-u2-location-marble.cfg](../devices/marble/runtime/telephony/init-u2-location-marble.cfg) 定义按需启动的 `/vendor/bin/hdf_devhost -i 7 -n location_host`（`uid=location_host`），停止独立进程 `gnss_hdi_marble`；② 更新 [start-u2-postboot-marble.sh](../devices/marble/runtime/telephony/start-u2-postboot-marble.sh) 改用稳态 `DMS op_mode=1(low_power)`（实测在 5G 未联调下规避 `nr5g_rrc_misc.c:639` SSR 崩溃且 `QMI_LOC` 正常收发）；③ 于 [build_marble_h20_u2_loc_media.py](../tools/packaging/build_marble_h20_u2_loc_media.py) `STAGE_U2_BLOCK` 写入纯 LF `/system/etc/param/marble_location.para`（`const.location.gps.support=true`、`nlp_service_name`/`geocode_service_name=com.huawei.hmos.location`、`supl_mode=2`），同时保留 U2 `surface_post` 热路径优化与 Venus VPU `/dev/video32..33`。[build_marble_h20_u2_loc_media.py](../tools/packaging/build_marble_h20_u2_loc_media.py) 产出 `/mnt/e/marble-tmp/vendor_boot_h20_u2_loc_media_974b7b75.img`（`sha256=974b7b7528d2a6d9f9669cf7bc19c2862cdfeb77192e71daf6d75331a1d534aa`），`boot_a=c83a8468…` 未动，16 项核心保护成员逐字节不变；已清理旧 U2 `d25c9255` 镜像，保留 U1.7 `7689087f` 与 F5 `34b842d0` 作回退。
- **已过验证层与新版实际结果（`BOOT_ID=9c718080-9b1b-4109-9543-a0a58048ac60`）**：
  - 主机验证：6 组单测（[test_marble_u2_loc_media.py](../tests/test_marble_u2_loc_media.py) + `u1` + `huks_soft` + `f5` + `f2` + `phase4`）共 **`Ran 52 tests in 26.871s — OK`**。
  - COM9 实机验收（开机 >255s）：`BOOT_COMPLETED=true`、`PERSIST_MOUNT_CNT=0`、`PAL_PCM=0`、`WLAN0=10.21.30.203/24`、`CRASH_CNT=17`、`DMESG_OOPS=0`、`VENUS_DEVS=2`、`RENDER_LOG_SZ=34.9K`；`RPROC2=running`、**`FATAL_MSS_CNT=0`**（`op_mode=1(low_power)` 稳定无崩溃）；4 项 `const.location.*` 参数均读取成功；`STANDALONE_GNSS_PID=none`，`location_host` 按需拉起（`LOC_HOST_PID=4882`）、`BAD_HOST_CNT=0`、`BAD_DEV_CNT=0`、`BAD_NLP_CNT=0`，`hilog` 显示 `locationhub/GnssAbility: ConnectGnssHdi success`，`gnss-hdi/status.log` 已完成 `GNSS_HDF_DRIVER_BIND_OK` → `GNSS_HDF_DRIVER_INIT_OK` → `GNSS_QMI_LOC_SVC_READY` → `GNSS_HDI_ENABLE` → `GNSS_QMI_LOC_REG_OK mask=0x187` → `GNSS_HDI_START` → `GNSS_QMI_LOC_START_OK` → `GNSS_QMI_LOC_IND type=pos` → `GNSS_HDI_LOC_DISPATCH rc=0` 并更新 `GnssAbility` 时间基准。
- **留待人工测试与后续（U3/F6）**：请人工在窗边/室外（GPS 冷启动需要天空信号；室内纯网络定位仍受 Huawei TEE `OUCS 12000001` 限制）复测地图/天气定位、GPS 位置回传与基础回归，确认无误后进入 Batch U3（Venus/JPEG/Image HDI + `display_engine_service` + 图库 `RecFFRTWorker` 卡死定位）。回退 U1.7 `7689087f` 用 [flash-h20-u2-control.sh](../work/marble-dual-20261123/flash-h20-u2-control.sh) `restore <BOOT_ID>`。

## 批次 U3.1 实机闭环验收里程碑（2026-11-25 更新）

- **旧现象 → 固定输入 → 最小差异**：针对 U3 基线中 `codec_image_service` 缺失（`ExtDecoder: Init jpeg hardware decoder failed`）、`SA 65962` 缺失及高通 Venus V4L2 能力未知问题，在 U2.1 `974b7b75` 基础上合入：① [jpeg_vdi_impl.c](../compat/codec/jpeg_vdi_impl.c)（`libjpeg_vdi_impl.z.so`，`NEEDED=[libc.so]`，导出 `GetCodecJpegHwi`，支持 `/dev/dma_heap/system` + `DMA_BUF_IOCTL_SYNC`、OHOS post-SOS scan 字节流自动重建 JFIF 头与 `RGBA_8888`/`NV21`/`NV12` 解码，桥接 `/system/lib64/chipset-sdk/libturbojpeg.z.so`）；② [displayengine_module.cpp](../compat/display/displayengine_module.cpp)（`libdisplayengine_module.so`，`NEEDED=[libc++.so, libc.so]`，导出与原 `libdisplay_engine_wrapper_service_1.1.z.so` `UND` 完全一致的 6 个 `DisplayEngineWrapperOH` 符号）接入 `composer_host` 内置 `displayengine_device` 与原生 `display_engine_service`（`SA 65962`，从 `sceneboard-optional-sa.list` 移除）；③ [build_marble_phase4_images.py](../tools/packaging/build_marble_phase4_images.py) 扩展 9 宿主 HCB（`displayengine=True, codec_image_host=True`）、[init-u3-codec-display-marble.cfg](../devices/marble/runtime/codec/init-u3-codec-display-marble.cfg) 与 [build_marble_h20_u3_media_display.py](../tools/packaging/build_marble_h20_u3_media_display.py)（含 `persist.multimedia.image.hardwaredecode.enabled=true` 与自包含 `u3_bind()`）产出 `/mnt/e/marble-tmp/vendor_boot_h20_u3_media_display_10ddf24b.img`（`sha256=10ddf24b89dd348bc3541f8c499325acad9b8a3faf500e2564d993c16755179e`），`boot_a=c83a8468…` 未动，18 项保护成员逐字节不变；④ [native_venus_v4l2_probe.c](../compat/codec/native_venus_v4l2_probe.c) 只读探测 Venus `/dev/video32..33`。首版 `9cdba258` 因误调未定义的 `bind_once` 导致 stage 中断，已修复（改用独立 `u3_bind()` 并去除未定义的 `skipped` 调用）并清理旧镜像，保留 `974b7b75` / `7689087f` / `34b842d0` 作回退。
- **已过验证层与新版实际结果（`BOOT_ID=9b08fab5-7960-486c-9fc7-64b0425cef17`，`UPTIME=132.86`）**：
  - 主机验证：7 组单测（[test_marble_u3_media_display.py](../tests/test_marble_u3_media_display.py) + `u2` + `u1` + `huks_soft` + `f5` + `f2` + `phase4`）共 **`Ran 56 tests in 30.090s — OK`**。
  - COM9 实机验收：`BOOT_COMPLETED=true`、`HW_JPEG_PARAM=true`、`PERSIST_MOUNT_CNT=0`、`PAL_PCM=0`、`WLAN0=10.21.30.203/24`、`CRASH_CNT=0`、`DMESG_OOPS=0`、`FATAL_MSS_CNT=0`、`OP_MODE=op_mode=1(low_power)`、`location_host=4933`；`composer_host`（4237）成功注册 `display_engine_wrapper_service`（`displayengine.log` 已有 `max_bl=4095`、`GetSupported` 10 项与 `SetHardwareEffect` 7 项调用）；`display_engine_service`（4489）就绪并发布 `SA:65962`（`NATIVE_SA_READY id=65962`，`OPT_SA_65962_CNT=0`）；`codec_host`（4488）成功注册 `codec_image_service`，`jpeg-vdi.log` 已验证多张真实开机 JPEG 硬件解码 `ret=0`（`3120x3120 -> 390x390 sample=8`、`3120x3120 -> 3120x3120 sample=1`、`876x876 -> 438x438 sample=2`，`rebuilt=1`）。
  - Venus V4L2 实机探测（`VENUS_PROBE_OK_DEVS=2`）：`/dev/video32`（`msm_vidc_decoder`，OUT=`H264/HEVC/VP90/HEIC`，CAP=`Q12C/NV12/NV21`）、`/dev/video33`（`msm_vidc_encoder`，OUT=`Q12C/NV12/NV21`，CAP=`H264/HEVC/HEIC`），已具备 Batch U3.2 硬件视频/HEIC 对接全部输入。
- **留待人工测试与回退**：请人工测试图库浏览与大图/缩略图解码流畅度、亮度/护眼等显示调节与系统基础回归；回退至 U2.1 `974b7b75` 使用 [flash-h20-u3-control.sh](../work/marble-dual-20261123/flash-h20-u3-control.sh) `restore <BOOT_ID>`。

## 批次 U3.2a 实机闭环验收里程碑（2026-11-25 更新）

- **旧现象 → 固定输入 → 最小差异**：针对原厂 `OMX.hisi.*` 缺失/失效、AVCodec NDK 缺少硬件视频编解码以及 SM7475 `msm_video.ko`（`/dev/video32..33`）与 donor `libhcodec.z.so` ABI 匹配需求，在 U3.1 `10ddf24b` 基线上合入：① [buffer_vdi.cpp](../compat/display/buffer_vdi.cpp) 的 `SUPPORTED_USAGE` 增加 `HBM_USE_VIDEO_ENCODER (1ULL << 14)` 与 `HBM_USE_VIDEO_DECODER (1ULL << 15)`（保留 `-DACE5_LINEAR_STRIDE_ALIGNMENT=128` 与受保护 usage 拒绝），产出 `libdisplay_buffer_vdi_impl.z.so`（`167080` B、`sha256=7af217ae…`）与 `plane_layout_contract`（`35328` B、`sha256=c86fafb4…`）；② [venus_v4l2.h](../compat/codec/venus_v4l2.h) 与 [venus_v4l2.c](../compat/codec/venus_v4l2.c) 补全 `venus_nv12_to_i420_dst()` / `venus_i420_src_to_nv12()`（对齐 `buffer_vdi.cpp` `GetImageLayout` 格式 `28` 及 `CropStore` 尾端边界），将 `venus_ctx_start()` 改为解码器先仅启动 `OUTPUT_MPLANE` 并延后到 `venus_ctx_handle_source_change()` 再分配/启动 `CAPTURE_MPLANE`，同时修正 `venus_ctx_send_eos()` 在 `!streaming_out` 返回 `-EAGAIN` 与 `venus_ctx_flush()` `was_in`/`was_out` 状态恢复；③ [omx_il_minimal.h](../compat/codec/omx_il_minimal.h) 与 [omx_core_venus.c](../compat/codec/omx_core_venus.c) 实现 `libOMX_Core.z.so`（`55952` B、`sha256=dd89daf6…`），导出 OHOS `GraphicPixelFormat`（`24`/`25`/`28`）到 `OMX_PARAM_PORTDEFINITIONTYPE.format.video.eColorFormat` 以匹配 `libhcodec.z.so` `g_pixelFmtTable`，码流口（`dec 0`/`enc 1`）通告 `CODEC_BUFFER_TYPE_AVSHARE_MEM_FD (0x2)`、解码输出口（`dec 1`）通告 `CODEC_BUFFER_TYPE_HANDLE (0x4)`（`stride=128`），支持 `0x6FA00010..0x6FA00036` 厂商参数（`0x6FA00033` 维持 `OMX_ErrorUnsupportedIndex`）并通过 `c->pending_eos` 延后跨 `SOURCE_CHANGE` 的 EOS；④ 更新 [build_marble_phase4_images.py](../tools/packaging/build_marble_phase4_images.py)、[build_marble_h20_u32_venus_codec.py](../tools/packaging/build_marble_h20_u32_venus_codec.py) 与 [test_marble_u32_venus_codec.py](../tests/test_marble_u32_venus_codec.py)（`supportPixelFmts=[24, 25, 28]`、`codec_omx_idl_device` 与 `media_codec_capabilities` HCB），打包 `/mnt/e/marble-tmp/vendor_boot_h20_u32_venus_codec_fd7a6b4a.img`（`sha256=fd7a6b4a727f7b1d1f24409c988d760794b16c431342f3afc4e538f5369b5620`，`100663296` B，`548->551` 条目，`24` 项保护成员逐字节不变，`boot_a=c83a8468…` 未动）。已清理过期候选 `e0bc99dc`、`189d8929`，保留 `10ddf24b` / `974b7b75` / `7689087f` / `34b842d0` 作回退。
- **已过验证层与新版实际结果（`BOOT_ID=493f6158-06e3-41df-b2c7-b2eb336aa1eb`，`UPTIME=131.31`）**：
  - 主机验证：`VENUS_V4L2_HOST_TEST_OK`、`OMX_VENUS_HOST_TEST_OK`，8 组单测（[test_marble_u32_venus_codec.py](../tests/test_marble_u32_venus_codec.py) + `u3` + `u2` + `u1` + `f5` + `d` + `phase4` + `codec`）共 **`Ran 68 tests in 29.801s — OK`**。
  - COM9 基线：`BOOT_COMPLETED=true`、`PERSIST_MOUNT_CNT=0`、`PAL_PCM=0`、`FATAL_MSS_CNT=0`、`OMX_MOUNT=1`、`WLAN0=10.21.30.203/24`、`FAULTLOG_CNT=17`（未增）、无 oops/BUG，`codec_host=4504`、`display_engine_service=4505`、`composer_host=4252`、`location_host=4951`、`av_codec_service=6622`、`jpeg-vdi.log` `DoJpegDecode ret=0` 正常。
  - VDI 布局契约：[plane_layout_contract](../tests/display/plane_layout_contract.cpp) `PLANE_LAYOUT_CONTRACT=0 cases=22 dma_allocations=0 physical_commits=0`。
  - V4L2 环回：[native_venus_decode_test](../compat/codec/native_venus_decode_test.c) `--dump-au /data/local/tmp/marble-codec`：`VENUS_TEST_H264_ENC_OK aus=4 len0=400`、`VENUS_TEST_H264_DEC_OK frames=4 crop=256x256 mae_y=0.28 mae_uv=0.00 ystride=256 size=98304`、`VENUS_TEST_HEVC_ENC_OK aus=4 len0=842`、`VENUS_TEST_HEVC_DEC_OK frames=4 crop=256x256 mae_y=0.37 mae_uv=0.00 ystride=256 size=98304`、`VENUS_LOOPBACK_ALL_OK`。
  - OHOS AVCodec NDK 端到端（[native_codec_probe](../compat/codec/native_codec_probe.c)）：能力枚举 `CODEC_PROBE_SUMMARY entries=16 hw=4 sw=12 decode=10 encode=4`（含 `OMX.marble.video.{decoder,encoder}.{avc,hevc}`）；实例生命周期 `--instance video/avc 1280 720` 与 `--instance video/hevc 1280 720` 均 `create=0 configure=0 prepare=0 start=0 stop=0 destroy=0`；真实硬解 `--decode video/avc`（`frames=1 bytes=98304 hash=eb9cb80f4c6585da errors=0 size=400 width=256 height=256 pixel_format=1 input_eos=1 stop=0 destroy=0 stage=ok`、`CODEC_OUTPUT_EOS=1`）与 `--decode video/hevc`（`frames=1 bytes=98304 hash=f0bf959f1e05231d errors=0 size=842 width=256 height=256 pixel_format=1 input_eos=1 stop=0 destroy=0 stage=ok`、`CODEC_OUTPUT_EOS=1`）；`omx-venus.log` 确认 H.264/HEVC 均走 `deferred EOS sent after SOURCE_CHANGE` → `pix_fmt=28 stride=256 lines=256 filled=98304 copy_rc=0` → `output EOS delivered` → `venus session stopped`，`dmesg vidc` 零 `msm_vidc_allow_last_flag` / `START_INPUT` / `START_OUTPUT` 错误。
- **未过门槛与回退**：AVCodec NDK 硬件编码、Surface 直出播放与应用侧视频实放留待后续批次；回退至 U3.1 `10ddf24b` 使用 [flash-h20-u32-control.sh](../work/marble-dual-20261123/flash-h20-u32-control.sh) `restore <BOOT_ID>`。

### 批次 U3.2b 里程碑（已构建并完成 COM9 实机全链路验收：`vendor_boot_h20_u32b_heif_codec_d5299691.img`，`2026-11-25`）
- **实现**：
  - clean-room [heif_vdi_impl.c](../compat/codec/heif_vdi_impl.c)（导出 `GetCodecHeifDecodeHwi` / `ICodecHeifDecodeHwi::DoHeifDecode`，通过高通 Venus `/dev/video32` `V4L2_PIX_FMT_HEVC` -> `NV12` 执行单帧 `hvc1` 与 `grid` 切片硬件解码，输出 RGBA/RGBX/BGRA/BGRX/RGB565/NV12/NV21/I420，并在每个 tile 会话排空尾包 `V4L2_BUF_FLAG_LAST` 后再 `STREAMOFF`/关闭句柄以消除 `msm_vidc_allow_last_flag: not allowed in state START_OUTPUT`）；
  - [build_marble_phase4_images.py](../tools/packaging/build_marble_phase4_images.py) 新增 `HDF_Image_hw_dec_heif_marble` HCB 节点；[build_marble_h20_u32b_heif_codec.py](../tools/packaging/build_marble_h20_u32b_heif_codec.py) 打包 `vendor_boot_h20_u32b_heif_codec_d5299691.img`（仅写 `vendor_boot_a`，保持 `boot_a=c83a8468...` 及 29 个受保护成员字节一致）；
  - [native_heif_probe.c](../compat/codec/native_heif_probe.c) 通过 `/compat/bin/native_heif_probe` 调用 OHOS ImageSource DMA NDK 端到端验证。
- **离线验证**：`HEIF_VDI_HOST_TEST_OK`、`HEIF_PROBE_HOST_TEST_OK`、`tests.test_marble_u32b_heif_codec` 与 8 个回归测试套件共 72 项单测全过（`OK`）。
- **COM9 实机验收（`NEW_BOOT=c914d0a0-a137-46a7-adbb-f5ca31f2f3d1`）**：
  - 基线：`bootevent.boot.completed=true`、`boot_a=c83a8468...`、`vendor_boot_a=d5299691...`、`PERSIST_MOUNT_CNT=0`、`PAL_PCM=0`、`FATAL_MSS_CNT=0`、`OOPS_BUG_CNT=0`、`HEIF_MOUNT=1 OMX_MOUNT=1 JPEG_MOUNT=1`、`WLAN0=10.21.30.203/24`、`FAULTLOG_CNT=17`（零新增崩溃）、`persist.multimedia.image.heifhardwaredecode.enabled=true`、`codec_host=4513`/`display_engine_service=4514`/`composer_host=4269`/`location_host=4962`/`av_codec_service=6640` 全在；
  - HEIF 端到端压测与硬件闭环：连续 3 轮执行 `/compat/bin/native_heif_probe` 全部通过（`HEIF_PROBE_ALL_OK cases=7`，覆盖单帧与 2x2 grid 及 400x360 裁剪，RGBA/NV12/NV21），`heif-vdi.log` 新增 21 条 `DoHeifDecode ret=0`（`grid=0 tiles=1/1` 与 `grid=1 tiles=4/4`），内核 `dmesg | grep -iE 'vidc|venus'` 零 `msm_vidc: err` / `msm_vidc_allow_last_flag` / `START_OUTPUT` 错误；
  - 零回归：真机照片 `/data/service/el2/100/hmdfs/account/files/Photo/3/IMG_1795583199_003.jpg` 解码通过（`ok=1`，`jpeg-vdi.log` `DoJpegDecode ret=0`），`plane_layout_contract` `22/22` 通过，`native_codec_probe --decode` `video/avc`（`hash=eb9cb80f4c6585da`）与 `video/hevc`（`hash=f0bf959f1e05231d`）均 `errors=0` 且 `CODEC_OUTPUT_EOS=1`。
- **图库 `RecFFRTWorker` 卡顿定位结案**：确认来源于图库 `PhotoBrowser.hsp` -> `librecommended.so` 的常驻 FFRT 工作线程（`RecFFRTWorker_0/1/2`）在 `ffrt_cond`（`wchan=futex_wait_queue_me`）上空闲等待新任务时触发良性 30 秒 FFRT 协程看门狗告警（`ReportTaskTimeout:596`），非死锁或主线程卡死；回退至 U3.2a `fd7a6b4a` 使用 [flash-h20-u32b-control.sh](../work/marble-dual-20261123/flash-h20-u32b-control.sh) `restore <BOOT_ID>`。

## 批次 U4 实机闭环验收里程碑（2026-11-25 更新：`vendor_boot_h20_u4_dcm_soft_tee_cbdd3cf9.img`）

- **旧现象 → 固定输入 → 最小差异**：`DeviceSecurityKit::getDeviceToken=-10002`，原 `dcm_service` 依赖的 teecd/设备证书 HDI 在移植环境缺失且 `65866` 仍在可选 SA 屏蔽表中。在 accepted **U3.2b `d5299691`** 上重基打包 `hmos_dcm_service`、`hmos_dcm_token_probe` 与 [init-dcm-marble.cfg](../devices/marble/runtime/dcm/init-dcm-marble.cfg)（35 个既往 F/U1/U2/U3.1/U3.2a/U3.2b 保护成员逐字节不变，`553→556` 条目），patch `hmos-init-debug.cfg`、`stage-phase4-marble.sh`（移出 `sceneboard-optional-sa.list` 中的 `65866` 并授权 `3562:3562` 目录）与 `start-phase4-marble.sh`（触发 `marble.debug.start.u4_dcm=true`），产出 `/mnt/e/marble-tmp/vendor_boot_h20_u4_dcm_soft_tee_cbdd3cf9.img`（`sha256=cbdd3cf9fa4105de526e85d21e2b69f41aedae5d93708d85c59cecb4bb51b713`，`boot_a=c83a8468…` 未动）。
- **关键修复与实现**：① 从 `DeviceCertManagerService` 确认 14 个 code 映射，code 1/5/9 返回本地 P-256 CA 证书链、7/10 返回本地可用状态 `state=1`、code 3 签发本地 `hmosdcm1.*` HMAC-SHA256 token，其余 2/4/6/8/11/12/13/23 返回 `-10009`；② 修正 `LoadSamgr()` 使用 `ISystemAbilityManager` `+112` 子对象调整 thunk `_ZThn112_N4OHOS25SystemAbilityManagerProxy16AddSystemAbility...`（首版 `76ca2757` 解析非 thunk 符号导致 `PeerHolder::Remote()` 偏移错读返回 `rc=38`），同步修正重试循环中 `sptr<IRemoteObject> stubRef` 生命周期以防 `AddSystemAbility` 失败提早释放 stub；③ `DispatchAttestIds`/`DispatchCertState` 补齐 `ReadInterfaceToken()` 校验与惰性 `EnsureFacts()`；④ [init-dcm-marble.cfg](../devices/marble/runtime/dcm/init-dcm-marble.cfg) 增加 `post-fs-data` 与 `param:marble-u4-dcm` 启动触发；⑤ `hmos_dcm_token_probe` 新增 code 1/5/9/7/10 协议校验及 `--sdk-state`（直接调用原厂 `libdevice_cert_mgr_sdk.z.so`）。
- **已过验证层（离线 + COM9 实机 `NEW_BOOT=3f474e17-b0b6-4e26-ba28-73f2c58f8c79`）**：
  - 离线：10 组单测（[test_marble_dcm_soft_tee.py](../tests/test_marble_dcm_soft_tee.py) + `u32b` + `u32` + `u3` + `u2` + `u1` + `f5` + `d` + `phase4` + `codec`）共 **`Ran 94 tests in 30.614s — OK`**。
  - COM9 基线：`BOOT_COMPLETED=true`、`boot_a=c83a8468…`、`vendor_boot_a=cbdd3cf9…`、`PERSIST_MOUNT_CNT=0`、`PAL_PCM=0`、`FATAL_MSS_CNT=0`、`OOPS_BUG_CNT=0`、`OPT_SA_65866_CNT=0`、`WLAN0=10.21.30.203/24`、`FAULTLOG_CNT=17`（零新增崩溃）、`DCM_START_PROP=true`、`hmos_dcm_service=4573`/`codec_host=4537`/`display_engine_service=4538`/`composer_host=4285`/`location_host=4987`/`av_codec_service=6665` 全在。
  - DCM SA 65866 与原厂 SDK：`native_sa_check 65866` → `NATIVE_SA_READY id=65866`；`hmos_dcm_token_probe` code 3 通过（`TOKEN_LEN=236`、`TOKEN_DIGEST=058d8447e97d2a17`、`cb_rc=0`）；`--code 7/10` 返回 `state=1`、`--code 1/5/9` 返回 `blob_len=808 cert_count=2`、`--code 2/23` 返回 `-10009`；`--sdk-state`（原厂 `libdevice_cert_mgr_sdk.z.so`）输出 `SDK_CERT_STATE rc=0 state=1` 与 `SDK_PROV_STATUS rc=0 status=1`；`hmos-dcm-service.log` 记录 `ADD_SA id=65866 rc=0` 及开机已有系统调用方 `uid=7023`/`uid=1024` 成功请求；`local-ca/` 生成 `root.pem`/`device.pem` 与 `0600` 私钥（属主 `3562:3562`）。
  - 零回归：`native_heif_probe` `HEIF_PROBE_ALL_OK cases=7`（`dmesg vidc/venus` 零错误）、`plane_layout_contract` `22/22`、`native_codec_probe --decode` `video/avc` 与 `video/hevc` 均 `errors=0` 且 `CODEC_OUTPUT_EOS=1`。
- **边界、未过门槛与回退**：`hmos_dcm_service` 签发的 `hmosdcm1.*` token 与 P-256 证书链（`NOT Huawei`）仅解决本地 SA 65866 / API `-10002` 可用性，**并非华为官方云端签发/硬件 TEE 证明**；应用商店/主题商店 UI 侧云端认证业务效果及非本地签发分支（`2/4/6/8/11/12/13/23`）不在本地模拟范围内。已清理过期 U4 试制镜像（`76ca2757`/`545c34a8`/`fce5ce4f`/`c20241d6`），回退 U3.2b `d5299691` 使用 [flash-h20-u4-control.sh](../work/marble-dual-20261123/flash-h20-u4-control.sh) `restore <BOOT_ID>`。

## 批次 F6 + 开机动画与花屏根治实机闭环里程碑（2026-11-25 更新：`vendor_boot_h20_f6_cellular_nas_c3bc219b.img`）

- **基线与控制脚本**：在实机在线基线 U4 [`vendor_boot_h20_u4_dcm_soft_tee_778fb2a2.img`](file:///mnt/e/marble-tmp/vendor_boot_h20_u4_dcm_soft_tee_778fb2a2.img)（`sha256=778fb2a28ba24be140e4909d1b7269f2b09ce44ac8cbf0ed47269d40f6a712da`）上叠加打包 [`build_marble_h20_f6_cellular_nas.py`](../tools/packaging/build_marble_h20_f6_cellular_nas.py)，仅刷写 `vendor_boot_a`（`boot_a=c83a8468955ca6584df29b6e5b7cba27c4b27ae5563a43401566bad301c9cae8` 未动，37 个前序受保护成员逐字节不变），产出 `/mnt/e/marble-tmp/vendor_boot_h20_f6_cellular_nas_c3bc219b.img`（`sha256=c3bc219bb741e7b21fcd74919abe91121566f264ad438d5780ba31e958d03f4d`）；回退 `778fb2a2` 使用 [flash-h20-f6-control.sh](../work/marble-dual-20261123/flash-h20-f6-control.sh)。已清理覆盖旧试验镜像 `ea588ded` / `88a80b58`。
- **修复内容 1 — 开机动画硬解恢复（[start-phase4-marble.sh](../devices/marble/runtime/boot/start-phase4-marble.sh)、[omx_il_minimal.h](../compat/codec/omx_il_minimal.h)、[omx_core_venus.c](../compat/codec/omx_core_venus.c)）**：
  - 原因：原流程 `t≈69s` 在 `audio_media` 后即启动 `bootanimation`（调用 `OMX.marble.video.decoder.avc` 硬解 `cust_bootvideo.mp4` `1256x2760` H.264 60fps），而 `start-venus-marble.sh` 直到 `t≈105s` 的 `start-u2-postboot-marble.sh` 才加载 `videocc-waipio.ko` / `msm_video.ko` 并创建 `/dev/video32..33`，导致 `venus_ctx_open fail rc=-2` → `OMX_ErrorHardware` → `HiPlayer` 报错退出无画面；另 [omx_il_minimal.h](../compat/codec/omx_il_minimal.h#L213-L214) 将 `OMX_IndexConfigCommonInputCrop`/`OutputCrop` 误定为 `0x07000008`/`0x07000007`（OpenMAX IL 1.1.2 与 `libhcodec.z.so` 实为 `0x0700000E`/`0x0700000F`），导致 `GetCropFromOmx` 报 `0x8000101a`。
  - 修复：在 `start-phase4-marble.sh` 的 `# BEGIN marble native boot animation` 前先加载 `start-venus-marble.sh`（生成 `/compat/bin` 与 `/dev/video32..33`）；修正 Crop Index 为 `0x0700000E`/`0x0700000F` 并在 `OMX_GetHandle` 增加 `/dev/video32`/`/dev/video33` 可访问性检查。
- **修复内容 2 — 间歇性花屏根因排查与多点修复（[mesa-turnip.patch](../compat/graphics/mesa-turnip.patch)、[mesa-zink-msaa-guard.patch](../compat/graphics/mesa-zink-msaa-guard.patch)、[composer_vdi.cpp](../compat/display/composer_vdi.cpp)）**：
  - **Turnip KGSL 同步缺陷**：Mesa 25.0.1 release 构建关闭了 `assert()`，`tu_knl_kgsl.cc` 中的 `kgsl_syncobj_merge()` 在双 `TS` 跨队列合并与 `TS+FD` 合并分支未转换 `ret`（且 `TS+FD` 误将 `STATE_FD` 的 `sync` 传给 `kgsl_syncobj_ts_to_fd(sync)` 导致读脏指针/丢栅栏）；`kgsl_syncobj_wait_any()` 在纯 FD 等待 (`queue==NULL`) 时仍调 `timestamp_to_fd(NULL, 0)` 且 `ret != 0` 与 `VK_TIMEOUT`/`VK_SUCCESS` 返回值写反；`kgsl_queue_submit()` 空提交跨多 signal 时未 `dup` FD 且未销毁 `wait_sync`，失败路径空指针解引用 `u_trace_submission_data`；`tu_FreeMemory` 调整 `OH_NativeBuffer_Unreference` 至 `tu_bo_finish` 之后。已同步重编译 `libvulkan_freedreno.so.gz`。
  - **Zink MSAA 递归保护实装**：重新编译并打包含 `mesa-zink-msaa-guard.patch`（`ZINK_MSAA_EXPAND` / `ZINK_MSAA_EXPAND_CAP`）的 `zink-runtime-marble.tar.gz`（U4 旧包内未包含此补丁）。
  - **Composer 模式切换与 AOD 30Hz 过滤**：`composer_vdi.cpp` `Initialize()` 过滤 `<60Hz` 的小米 AOD 模式（保留当前默认模式），`ChangeMode()` 在 `ALLOW_MODESET` 前等待 `lastPresentFence` 并在提交前后调用 `PauseVsyncForModeset()`/`ResumeVsyncAfterModeset()`；实机已重编译 4 项 display/contract 工件与 `libdisplay_composer_vdi_impl.z.so`。
- **修复内容 3 — Batch F6 蜂窝 NAS + 纯净 8-KO IPA 顺序（[marble_modem_probe.c](../compat/telephony/marble_modem_probe.c)、[libril_vendor_qmi.c](../compat/telephony/libril_vendor_qmi.c)、[start-modem-f6-marble.sh](../devices/marble/runtime/telephony/start-modem-f6-marble.sh)、[start-u2-postboot-marble.sh](../devices/marble/runtime/telephony/start-u2-postboot-marble.sh)）**：
  - 按精确依赖顺序加载 `ipa_fmwk -> gsim -> ipam -> rndisipam -> ipa_clientsm -> [IPA ready] -> ipanetm -> [rmnet_ipa0] -> rmnet_ctl -> rmnet_core` 后再拉起 MSS 与 `op_mode=0(online)`，支持双卡槽位自测与 Sub 1 电信 LTE（`46011`）QMI NAS 注册及增量指示，并拉起 Telephony SA（`4010/4007/4008/4009/66250`）与 `riladapter_host`。
- **已过验证层（主机 + COM9 实机 `BOOT_ID=1aa15755-930b-4225-ab72-651c378f9335`）**：
  - 主机验证：`VENUS_V4L2_HOST_TEST_OK`、`OMX_VENUS_HOST_TEST_OK`、`MODEM_PROBE_SELF_TEST_OK=1`、`RIL_VENDOR_QMI_HOST_TEST_OK=1`；10 组单测全绿（`81/81 OK`，`c3bc219b` 重打包后复测 `tests.test_marble_modem_f6` `5/5 OK`）。
  - COM9 基线：`boot_a=c83a8468…`、`vendor_boot_a=c3bc219b…`、`BOOT_COMPLETED=true`、`PERSIST=0`、`OOPS=0`、`PAL_PCM=0`、`wlan0=10.21.30.203/24`。
  - 开机动画冷启动实证：`marble-phase4-autostart.log` 于 `uptime=68.20` 输出 `VENUS_EARLY_READY=1`（`marble-venus-early.log`：`VENUS_NODES_READY count=2`，`/dev/video32` `video33`），先于 `uptime=69.13 BOOTANIM_START_REQUESTED=1`；`omx-venus.log` 末尾完整记录 `OMX.marble.video.decoder.avc 1256x2760 fps=60` → `SOURCE_CHANGE rc=0 new=1256x2760 min_buf=8` → `first output delivered pix_fmt=24 stride=1280 lines=2760 filled=5299200 copy_rc=0` → `output EOS delivered` → `venus session stopped`，`BOOTANIM_STOP_REQUESTED=1 native_finished=true`（`uptime=103.44`），`PLAYER_ERR=0 CROP_ERR=0`。
  - 显示/GPU 实证：`KGSL_FAULT=0`、`DMESG_GPU=0`、`REL_LOST=0`、`M30=0`、`EBUSY=0`、`FBK=0`、`FERR=0`、`BIND=0`、`INV=0`、`GERR=0`，`MODE_APPLIED` 仅在 `60/90/120Hz` 间切换，设备内 6 个兼容库 SHA256 与本次编译产物完全一致；实机动态使用过程中是否彻底消除偶发花屏仍需用户在机旁目视确认。
  - Batch F6 蜂窝实证：8 个 IPA/RMNET 模块冷启动一次性干净加载（`F6_IPA_READY=1 rmnet_ipa0=1`、`IPA_WARN=0`），`RPROC2=running`、`FATAL_MSS=0`（`uptime=350.15s` 在线稳定）、`op_mode=0(online)`、`NAS_HOME mcc=460 mnc=11 desc="CT"`、`NAS_SYS_INFO_DEC lte_srv=2 domain=2 cell_id=0x6532503 tac=0x7e23 plmn=46011 ims_voice=1`、`NAS_SIG_LTE rssi=-74 rsrq=-12 rsrp=-113 snr=28`；`SA_4010/4007/4008/4009/66250` 均就绪、`riladapter_host` 已加载 `libril_vendor.z.so`、`CellularData` 与 `DataStorage` 已识别 `slotId = 1, opkey = 46011`。
  - 零回归冒烟：`SA_65866=1` / `hmos_dcm_token_probe` `TOKEN_CALLBACK=ok errCode=0 token_len=236`、`native_codec_probe` `entries=16 hw=4`、`native_heif_probe` `HEIF_PROBE_ALL_OK cases=7`、`location_host` / `codec_host` / `hmos_dcm_service` 全部常驻运行。

## 批次 F7 蜂窝 RIL 上报修复 + IPA v5.0 / QMAPv5 数据面实机闭环里程碑（2026-11-26 更新：`vendor_boot_h20_f7_cellular_data_460bd64f.img`）

- **旧现象 → 固定输入 → 最小差异**：Batch F6 已打通 8-KO IPA 加载与电信 `46011` NAS 注册，但仍缺：① `riladapter_host` / `CoreService` 上报补齐（每 5 秒 `BuildNeighboringCellList failed`、每卡槽 `RadioState`、信号滞回与 NITZ/PS 通知 ID）；② `rmnet_data0` 数据面未打通（QMI DPM/WDA/WDS + IPA v5.0 QMAPv5 帧收发）。在 accepted **F6 `c3bc219b`** 上叠加打包 [`build_marble_h20_f7_cellular_data.py`](../tools/packaging/build_marble_h20_f7_cellular_data.py)（43 个前序保护成员逐字节不变，`557→558` 条目），仅刷写 `vendor_boot_a`（`boot_a=c83a8468955ca6584df29b6e5b7cba27c4b27ae5563a43401566bad301c9cae8` 未动），产出 `/mnt/e/marble-tmp/vendor_boot_h20_f7_cellular_data_460bd64f.img`（`sha256=460bd64f705b491ecd858d3e1ec7f3c5f2d4509076065f97a4c4f45379003af2`）。
- **关键根因定位与 4 项修复**：
  1. **IPA v5.0 `qcom,wan-use-skb-page` 下行软中断死锁（`21s` 看门狗重启）**：marble 设备树启用了 `qcom,wan-use-skb-page`，V1 `RMNET_IOCTL_SET_INGRESS_DATA_FORMAT` 未配置 `buff_size` 导致 `ipa_client_apps_wan_cons_agg_gro=false`，首包 RX 在 `ipa3_wan_rx_pyld_hdlr()` 对 0 字节线性页回收 SKB 死循环。在 [marble_modem_probe.c](../compat/telephony/marble_modem_probe.c) 切换至 IPA v5.0 `RMNET_IOCTL_EXTENDED_V2`（`0x89FE`，`buff_size=8192`、`agg_byte_limit=7500`、`agg_time_limit=500`、`agg_pkt_limit=16`、`int_modt=32`、`int_modc=1`），并在拉起 `rmnet_ipa0`/`rmnet_data0` 前关闭 IPv6。
  2. **冷启动 `t≈121s` MPSS `ipa_ul.c:5104` 断言 SSR**：`--ipa-setup` 过早于 `QMI_IPA_INIT_MODEM_DRIVER_REQ_V01 response received` 触发且拉起网口时发出未请求 IPv6 RS。在 [start-modem-f7-marble.sh](../devices/marble/runtime/telephony/start-modem-f7-marble.sh) 新增 `wait_qmi_ipa_init()` 等待该响应并先禁用 IPv6，调整 `do_online()` 顺序（`ensure_mss_qrtr` → `do_ipa_data` → `--set-mode online` → `--dpm-open`）。
  3. **QMAPv5 8B 下行校验和头与 `WDA_DL=9` 精确对齐**：IPA v5.0 V2 ingress 无条件启用 `IPA_ENABLE_CS_DL_QMAP`，仅设 `cs_offload_en=0`（4B 头）会覆写 IPv4 `TOS` 导致 `Ip: InHdrErrors`；设 `ing_p.cs_offload_en=1`（8B `40 01 ... 04 80 00 00`）+ `rmnet_data0` netlink `RMNET_PRIV_FLAGS_INGRESS_MAP_CKSUMV5`（`BIT(5)`）后，`WDA_SET_DATA_FORMAT` 仍用 `dl=5` 会使首字节 `next_hdr` 为 `0x00` 导致 `rmnet_core` 误按 4B 剥离。将 [marble_modem_probe.c](../compat/telephony/marble_modem_probe.c) 与 [libril_vendor_qmi.c](../compat/telephony/libril_vendor_qmi.c) 统一改为 `WDA_UL=5 (QMAP)` + `WDA_DL=9 (QMAPv5)`，实机抓包确认 `40 01 00 54 04 00 00 00` 完整剥离为 `proto=0x0800` 纯净 IPv4。
  4. **`riladapter_host` 邻区空列表指针校验**：反汇编 `libhril.z.so` 确认 `BuildNeighboringCellList` 先判 `items == NULL` 再判 `itemNum < 1`；在 [libril_vendor_qmi.c](../compat/telephony/libril_vendor_qmi.c) 改为返回静态非空 64B 占位指针配合 `itemNum=0`，同步补齐双卡槽 `RadioState`、`-dBm` 滞回、NITZ/PS 通知 ID 及 `ActivatePdpContext`/`DeactivatePdpContext`/`GetPdpContextList`/`GetLinkCapability`。
- **已过验证层（离线 + COM9 无干预冷启动 `BOOT_ID=febe3c02-2634-4c59-9adf-8e10259f4dad`）**：
  - 离线：`MODEM_PROBE_SELF_TEST_OK=1`、`RIL_VENDOR_QMI_HOST_TEST_OK=1`，12 组回归测试套件（含加固后的 [test_marble_modem_f7.py](../tests/test_marble_modem_f7.py)）共 **`Ran 94 tests in 45.132s — OK`**。
  - COM9 冷启动 Part A：`boot_a=c83a8468…`、`vendor_boot_a=460bd64f…`、`BOOT_COMPLETED=true`、`F7_QMI_IPA_INIT=1 wait_s=1`、`IPA_SETUP_IOCTL if=rmnet_ipa0 llp=0 ing=0(v2=1,st=0) egr=0(v2=1,st=0) mux=0`、`set rx_buff_sz config from netmngr 8192`、`dev(rmnet_data0) register to IPA`、`F7_MODEM_ONLINE=1 attempt=1`、`DPM_OPEN_PORT ep=4 iface=1 rx=2 tx=23 rc=0`、`F7_TELEPHONY_READY=1`、`U2_POSTBOOT_DONE=1`、`plmn=46011`（冷启初驻 5G NR SA `nr5g_srv=2`，稳态 LTE `lte_srv=2 rssi=-76 rsrp=-110`）、`RIL_PID=10853` `RIL_ERR_CNT nb_fail=0 nb_inv=0 sig_inv=0 nitz_err=0`。
  - COM9 蜂窝数据面 Part B：`WDA_GET_FMT llp=2 ul=5 dl=9`、`WDS_START apn="ctnet" rc=0`、`WDS_SETTINGS ip=10.9.90.75/29 gw=10.9.90.76 dns1=202.96.128.86 mtu=1500`；实机通过 `rmnet_data0` 双向公网 Ping **`119.29.29.29` `4/4 (0% packet loss, min=25.967ms)`**、**`223.5.5.5` 最低 `25.645ms`**，两轮合计 `Icmp: InEchoReps +14`、`rmnet_data0 tx=16 rx=14 txb=1344 rxb=1176`、`Ip: InHdrErrors=0`（增量 0）、`IpExt: InCsumErrors=0`（增量 0）、`WDS_STOP rc=0`，且 `wlan0=10.21.30.203/24` 未受干扰。
  - COM9 安全与零回归 Part C：开机动画硬解正常（`VENUS_EARLY_READY=1` → `BOOTANIM_STOP_REQUESTED=1 native_finished=true`）、6 个核心 SA（`4010/4007/4008/4009/65866/66250`）与 5 个常驻服务全在、`hmos_dcm_token_probe` `TOKEN_CALLBACK=ok errCode=0 token_len=236`、`native_heif_probe` `HEIF_PROBE_ALL_OK cases=7`、`PAL_PCM=0`、`PERSIST=0`、`KGSL_FAULT=0`、`FATAL_MSS=0`、`OOPS=0`。
- **边界、未过门槛与回退**：OHOS 系统设置中 `CellularData` 默认关闭且 `/cust` 运营商 `pdp_profile.json` 尚未注入（当前由 `marble_modem_probe` 验证 QMI WDS + IPA v5.0 QMAPv5 数据面完整闭环，`libril_vendor_qmi.c` 亦已同步 `ul=5/dl=9`）；遵循夜间静默约束未测试语音通话与短信。已清理过期 F7 试验镜像（`273b9898`/`34d38ad4`/`904d9ad4`/`d451183c`），回退 F6 `c3bc219b` 使用 [flash-h20-f7-control.sh](../work/marble-dual-20261123/flash-h20-f7-control.sh) `restore <BOOT_ID>`。

## 批次 F8 OHOS CoreService 4G/5G 制式修复 + `/cust/global` 运营商 APN 注入 + V1_3/V1_4 `HRilDataReq` 实机闭环里程碑（2026-11-26 更新：`vendor_boot_h20_f8_ohos_cellular_090f2239.img`）

- **旧现象 → 固定输入 → 最小差异**：Batch F7 打通了 IPA v5.0 / QMAPv5 数据面，但 OHOS 上层仍存在三处断点：① `hril_vendor_abi.h` 使用 Android 枚举值（`LTE=13`/`NR=17`），被 `libtel_core_service.z.so` 映射为 `0 (UNKNOWN)`，导致 `CellularData` 报 `radioTech is 0`；② `/cust/global` 目录缺失使 `const.cust.config_dir_layer` 跳过 `/cust/global`，`net.db` `pdp_profile` 为空导致 `matchedApns is empty`；③ OHOS 5.x `libhril.z.so` 在 `version > 12` 时调用 `HRilDataReq` slot 12（`ActivatePdpContextWithApnTypes`）与 slot 18（`ActivatePdpContextWithApnTypesforSlice`），F7 中两槽位均为 `NULL`。在 accepted **F7 `460bd64f`** 上叠加打包 [`build_marble_h20_f8_ohos_cellular.py`](../tools/packaging/build_marble_h20_f8_ohos_cellular.py)（46 个前序保护成员逐字节不变，`558→559` 条目），仅刷写 `vendor_boot_a`（`boot_a=c83a8468955ca6584df29b6e5b7cba27c4b27ae5563a43401566bad301c9cae8` 未动），产出 `/mnt/e/marble-tmp/vendor_boot_h20_f8_ohos_cellular_090f2239.img`（`sha256=090f22390290b6bdf13e0c0592b33c56c78d6910f55cde2ce52fa6265072267e`）。
- **关键根因与 3 项修复**：
  1. **OHOS `HRilRadioTech` 枚举精确对齐（[hril_vendor_abi.h](../compat/telephony/hril_vendor_abi.h)）**：反汇编 `libtel_core_service.z.so`（`0xbf0ac..0xbf134`）并与 SDK `telephony_radio_type.h` 交叉核验，修正为 `GSM=1, CDMA1X=2, WCDMA=3, HSPA=4, HSPAP=5, TD_SCDMA=6, EVDO=7, EHRPD=8, LTE=9, LTE_CA=10, IWLAN=11, NR=12`。
  2. **OHOS 5.x `HRilDataReq` 全槽位与单路 mux 守护（[hril_vendor_abi.h](../compat/telephony/hril_vendor_abi.h)、[libril_vendor_qmi.c](../compat/telephony/libril_vendor_qmi.c)）**：补齐 `HRilLinkBandwidthInfo`（36B）及 `HRilDataReq` slot 4（`GetLinkBandwidthInfo`）、slot 7（`SendDataPerformanceMode`）、slot 8（`SendDataSleepMode`）、slot 12（`ActivatePdpContextWithApnTypes`）、slot 18（`ActivatePdpContextWithApnTypesforSlice`），并在单路 `rmnet_data0` 上拒绝 `ims`/`xcap`/`emergency` 等次级 APN 请求覆盖已激活的互联网承载。
  3. **确定性中国运营商 `/cust/global` 配置注入（[build_marble_h20_f8_ohos_cellular.py](../tools/packaging/build_marble_h20_f8_ohos_cellular.py)）**：从 donor 打包 `carrier-cn-marble.tar.gz`（`32` 目录：`46000`–`46099`、`network`、`rule`，共 `111` 文件、`59,577` 字节），在 `stage-phase4-marble.sh` 启动 `hmos_init` 解析 `baselayer.cfg` 之前解压至 `$R/cust/global/etc/carrier`，使系统冷启动自动激活 `/cust/global` 配置层。
- **已过验证层（离线 + COM9 无干预冷启动 `BOOT_ID=3ada9d7e-361f-4a70-aaf9-56847c68febf`）**：
  - 离线：`RIL_VENDOR_QMI_HOST_TEST_OK=1`，13 组离线回归测试套件（含 [test_marble_modem_f8.py](../tests/test_marble_modem_f8.py)）共 **`Ran 98 tests in 45.557s — OK`**。
  - COM9 冷启动 Part A：`boot_a=c83a8468…`、`vendor_boot_a=090f2239…`、`BOOT_COMPLETED=true`、`const.cust.config_dir_layer` 包含 `/cust/global`（`CARRIER_DIRS=32`）、`OPKEY1=46011`；`DataStorage` 自动完成首次导入：`telephony_data_preferences.xml` 写入 `opkey_conf_checksum=1712747305` 与 `apn_conf_checksum46011=3444470569`，`net.db` 自动入库 **3 条电信 APN**（`CTNET`/`CTWAP`/`ims`），`opkey.db` 入库 **2294 条**规则；`telephony-marble`（PID `10926`）稳定上报 **`regStatus=REG_STATE_HOME_ONLY(1) radioTechnology=9 roam=0 slotId:1`**（`cfgTech:9` = LTE），**`MATCHED_EMPTY_CNT=0`**、**`RADIOTECH0_CNT=0`**；`riladapter_host`（PID `10925`）`RIL_ERR_CNT nb_fail=0 nb_inv=0 sig_inv=0 nitz_err=0`。
  - COM9 蜂窝数据面 Part B：`WDA_GET_FMT llp=2 ul=5 dl=9`、`WDS_START apn="ctnet" rc=0`、`ip=10.24.52.101/30 gw=10.24.52.102 dns1=202.96.128.86`；通过 `rmnet_data0` 双向公网 Ping **`119.29.29.29` `4/4 (0% loss, min=35.201ms)`** 与 **`223.5.5.5` `4/4 (0% loss, min=23.023ms)`**，`rmnet_data0 rx=8/672B tx=8/672B`、`Icmp: InEchoReps 0→8 (+8)`、`Ip: InHdrErrors=0`、`IpExt: InCsumErrors=0`、`WDS_STOP rc=0`，且 `wlan0=10.21.30.203/24` 未受干扰。
  - COM9 安全与零回归 Part C：开机动画硬解正常（`VENUS_EARLY_READY=1` → `BOOTANIM_STOP_REQUESTED=1 native_finished=true`）、6 个核心 SA（`4010/4007/4008/4009/65866/66250`）与 5 个常驻进程全部在位、`hmos_dcm_token_probe` `TOKEN_CALLBACK=ok errCode=0 token_len=236`、`native_heif_probe` `HEIF_PROBE_ALL_OK cases=7`、`PAL_PCM=0`、`PERSIST=0`、`KGSL_FAULT=0`、`FATAL_MSS=0`、`OOPS_EXACT=0`。
- **边界、后续待解项（转入 Batch F9）与回退**：
  1. `sim.db` 中 Slot 1 的 `is_main_card=0` / `is_cellular_data_card=0`（尽管 `persist.telephony.MainCard.Iccid` 已匹配电信卡 ICCID SHA256），反汇编 `libhril.z.so` 已定位为 `HRilSimReq` slot 19（`SetRadioProtocol`）缺失与 `GetRadioProtocol` 双卡槽 `modemId` 均返回 `0`；
  2. `telephony-marble` 日志存在 `CellInfo::ProcessCurrentCellInfo cellSize:0 error` 与 `CellInfo::GetCellLocation is null`，需在 `GetCurrentCellInfo` 中上报真实 QMI NAS 服务小区（LTE/NR）结构体；
  3. 遵循夜间静默约束未测试语音通话与短信；回退 F7 `460bd64f` 使用 [flash-h20-f8-control.sh](../work/marble-dual-20261123/flash-h20-f8-control.sh) `restore <BOOT_ID>`。

## 批次 F9 主卡无线协议协商 `SetRadioProtocol` + 5G NR / 4G LTE 服务小区上报实机闭环里程碑（2026-11-26 更新：`vendor_boot_h20_f9_sim_cell_d01c4581.img`）

- **旧现象 → 固定输入 → 最小差异**：Batch F8 已恢复 `CoreService` `radioTechnology=9/12` 与 `/cust/global` APN 自动入库，但存在两项剩余断点：① `HRilSimReq` slot 19（`SetRadioProtocol`）为 `NULL` 且 `GetRadioProtocol` 对双卡槽均返回 `modemId=0`；② `GetCurrentCellInfo` 返回 `itemNum=0`，导致 `telephony-marble` 持续报 `CellInfo::ProcessCurrentCellInfo cellSize:0 error`、`CellInfo::GetCellLocation is null` 与 `OperatorName::GetCurrentLac location is nullptr`。在 accepted **F8 `090f2239`** 上叠加打包 [`build_marble_h20_f9_sim_cell.py`](../tools/packaging/build_marble_h20_f9_sim_cell.py)（全部 48 个前序保护成员含 `carrier-cn-marble.tar.gz` 与 `stage-phase4-marble.sh` 逐字节不变，`559→559` 条目，仅更新 `libril_vendor_qmi.z.so`），仅刷写 `vendor_boot_a`（`boot_a=c83a8468955ca6584df29b6e5b7cba27c4b27ae5563a43401566bad301c9cae8` 未动），产出 `/mnt/e/marble-tmp/vendor_boot_h20_f9_sim_cell_d01c4581.img`（`sha256=d01c4581674ea5f4e87ebf773c25c613e5822532b1cac2c29848168c6256aa3f`）。
- **关键修复与反汇编核验（[hril_vendor_abi.h](../compat/telephony/hril_vendor_abi.h)、[libril_vendor_qmi.c](../compat/telephony/libril_vendor_qmi.c)）**：
  1. **`HRilSimReq` slot 19 (`SetRadioProtocol`) + 分槽 `modemId`**：反汇编 `libhril.z.so`（`0x8a1cc` / `0x864bc` / `0x87274`）与 `libtel_core_service.z.so`（`0x1727c0` / `0x1472c4`），确认 `SetRadioProtocol` 为 slot 19（偏移 `152`），返回 20B `HRilRadioProtocol`（`status=1` 为 `SUCCESS`，`2` 为 `FAIL`），`UPDATE (phase=2)` 后触发 `HNOTI_SIM_RADIO_PROTOCOL_UPDATED (1208)`；`GetRadioProtocol` 按活跃主卡分槽返回独立 `modemId`（活跃卡槽为 `0`，空槽为 `1`）。同时反汇编 `SavePrimarySlotId (0x1472c4)` 证实 HarmonyOS 5.x 主卡状态持久化于 `persist.telephony.MainCard.Iccid`（已精确匹配 Slot 1 ICCID SHA256 `9BAC9B54...`），并非旧版 `sim.db` `is_main_card` 字段。
  2. **56 字节 `HRilCurrentCellInfoVendor`（LTE `ratType=5` / NR `ratType=6`）真实服务小区上报**：交叉反汇编 `libhril.z.so`（`BuildCurrentCellList_V1_2` `0x78ec8`）与 `libtel_core_service.z.so`（`ProcessCurrentCellInfo` `0x9bf8c..0x9cb20`）逐字节验证 56B 布局及 `rsrp`/`rsrq` 正幅值约定（CoreService 内做 `neg` 取反），在 `TASK_NET_GET_CURRENT_CELL` 中整合 `QMI_NAS_GET_SYS_INFO (0x004d)`、`QMI_NAS_GET_SIGNAL_INFO (0x004f)` 与 `QMI_NAS_GET_CELL_LOCATION_INFO (0x0043)`（解析 LTE TLV `0x13`/`0x1c` 的 `tac`/`cell_id`/`earfcn`/`pci` 及 NR TLV `0x22`/`0x23` 的 `tac`/`nci`/`pci`/`nr_arfcn`）。
- **已过验证层（离线 + COM9 无干预冷启动 `BOOT_ID=f05f972c-0965-4784-af76-82944a239bd4`）**：
  - 离线：`RIL_VENDOR_QMI_HOST_TEST_OK=1`（含第 8 节 `SetRadioProtocol` + `HNOTI=1208` + LTE/NR `CurrentCellInfoVendor` 自测），14 组离线回归测试套件（含 [test_marble_modem_f9.py](../tests/test_marble_modem_f9.py)）共 **`Ran 102 tests in 48.533s — OK`**。
  - COM9 冷启动 Part A（5G NR SA + 4G LTE 双小区与 APN 匹配实证）：`boot_a=c83a8468…`、`vendor_boot_a=d01c4581…`、`BOOT_COMPLETED=true`、`CUST_LAYER` 含 `/cust/global`、`OPKEY1=46011`、`net.db` 3 条电信 APN（`CTNET`/`CTWAP`/`ims`）；`telephony-marble`（PID `10967`）实时完整打印：
    - 5G NR SA 驻网：`NotifyStateChange finish slotId:1, regStatus:1, cfgTech:12`、`CellularData: Slot1: radioTech is 12`、**`CellInfo::ProcessCurrentCellInfo cell size:1, slotId:1`**、**`CellInfo::ProcessCurrentCellNr slotId:1 rsrp:-119`**；
    - 4G LTE 驻网：`CellularData: Slot1: radioTech is 9`、**`CellInfo::ProcessCurrentCellInfo cell size:1, slotId:1`**、**`CellInfo::ProcessCurrentCellLte arfcn:<private> pci:<private> rsrp:-111 slotId:1`**；
    - `cellSize:0 error=0`、`GetCellLocation is null=0`、`GetCurrentLac location is nullptr=0`、`SetRadioProtocol failed=0`；`ApnManager::InitApnHolders`（共 14 个 holder）仅 10 个次级可选类型为空，`default (CTNET)`/`supl`/`mms (CTWAP)`/`ims` 4 类在 5G NR 与 4G LTE 下均命中；`riladapter_host`（PID `10966`）`RIL_ERR_CNT nb_fail=0 nb_inv=0 sig_inv=0 nitz_err=0`。
  - COM9 蜂窝数据面 Part B：`WDA_GET_FMT llp=2 ul=5 dl=9`、`WDS_START apn="ctnet" rc=0`、`ip=10.4.63.42/30 gw=10.4.63.41 dns1=202.96.128.86`；通过 `rmnet_data0` 双向公网 Ping **`119.29.29.29` `4/4 (0% loss)`** 与 **`223.5.5.5` `4/4 (0% loss, min=22.215ms)`**，`rmnet_data0 rx=8/672B tx=8/672B`、`Icmp: InEchoReps 0→8 (+8)`、`Ip: InHdrErrors=0`、`IpExt: InCsumErrors=0`、`WDS_STOP rc=0`，且 `wlan0=10.21.30.203/24` 未受干扰。
  - COM9 安全与零回归 Part C：开机动画硬解正常（`VENUS_EARLY_READY=1` → `BOOTANIM_STOP_REQUESTED=1 native_finished=true`）、6 个核心 SA（`4010/4007/4008/4009/65866/66250`）与 5 个常驻进程全在、`hmos_dcm_token_probe` `TOKEN_CALLBACK=ok errCode=0 token_len=236`、`native_heif_probe` `HEIF_PROBE_ALL_OK cases=7`、`PAL_PCM=0`、`PERSIST=0`、`KGSL_FAULT=0`、`FATAL_MSS=0`、`OOPS_EXACT=0`。
- **边界与回退**：当前因 `wlan0` 优先在线，`NetConnManager` 保持 Wi-Fi 为默认路由（用户白日关闭 Wi-Fi 即可由 OHOS `CellularData` 自动经 `ActivatePdpContextWithApnTypes` 拉起 `ctnet`/`rmnet_data0`）；夜间仍禁测语音通话与短信。回退 F8 `090f2239` 使用 [flash-h20-f9-control.sh](../work/marble-dual-20261123/flash-h20-f9-control.sh) `restore <BOOT_ID>`。

## 批次 G1 SM7475 相机内核栈 + LX7 ICP 固件 + 4 颗物理传感器 CCI 硬件探测里程碑（2026-11-26 更新：`vendor_boot_h20_g1_camera_hw_0e768215.img`）

- **旧现象 → 固定输入 → 最小差异**：此前 F9 尚未加载 SM7475 相机内核栈（`camera.ko` 等 5 个模块）与 `CAMERA_ICP` 固件，`/dev/media0`、`/dev/video0..1` 与 `/dev/v4l-subdev0..23` 均不存在。在 accepted **F9 `d01c4581`** 上叠加打包 [`build_marble_h20_g1_camera_hw.py`](../tools/packaging/build_marble_h20_g1_camera_hw.py)（47 个前序保护成员逐字节不变，`stage-phase4-marble.sh` 与 `start-u2-postboot-marble.sh` 唯一可逆拼接，`559→562` 条目），仅刷写 `vendor_boot_a`（`boot_a=c83a8468955ca6584df29b6e5b7cba27c4b27ae5563a43401566bad301c9cae8` 未动），产出 `/mnt/e/marble-tmp/vendor_boot_h20_g1_camera_hw_0e768215.img`（`sha256=0e7682159759264f2f904bda12acc4290fdbfe98eff7a98ff0a43d0ff68af30a`）。
- **新增最小实现**：
  1. **确定性 LX7 ICP 固件包 `camera-icp-fw.tar.gz`（490,891 B，23 文件）**：经 `/sys/firmware/devicetree/base/soc/qcom,cam-icp/icp_use_pil` 与 `qcom,lx7/fw_name=CAMERA_ICP` 核验仅需 PIL MDT 分段（`CAMERA_ICP.mdt` + `CAMERA_ICP.b00..b21`），无需 4.7 MB 非安全 ELF；冷启动阶段解压至 `/compat/firmware` 与 `$R/compat/firmware`。
  2. **冷启动相机内核驱动与节点脚本（[start-camera-g1-marble.sh](../devices/marble/runtime/camera/start-camera-g1-marble.sh)）**：在 `start-u2-postboot-marble.sh` Step C（支持 `marble-skip-stages` 写入 `camera` 单步回退）按依赖序加载 `qcom_ipc_lite.ko`、`synx-driver.ko`、`leds-qti-flash.ko`、`camcc-waipio.ko`、`camera.ko`，将 `/dev/video0..1`、`/dev/media0..1`、`/dev/v4l-subdev0..23` 同步映射至 `$R/dev`（`0660`）并自动运行硬件探测。
  3. **净室 SM7475 相机 UAPI 硬件探针（[marble_camera_probe.c](../compat/camera/marble_camera_probe.c)，16 KB OHOS arm64 ELF，`NEEDED=[libc.so]`）**：枚举 `/dev/media0`（27 实体）、查询 `CPAS/ISP/ICP/JPEG/CSIPHY/TPG/SENSOR/EEPROM/ACTUATOR/FLASH` 能力，并经 `/dev/video0` CRM 分配 DMA-BUF 构造 `CAM_SENSOR_PROBE_CMD`（按 DT 原厂电压上电 → CCI I2C 实读硬件芯片 ID → 逆序断电清理）。
- **已过验证层（离线 + COM9 无干预冷启动 `BOOT_ID=f0a41e1f-f840-4e66-99fd-56307cba7a04`）**：
  - 离线：`MARBLE_CAMERA_PROBE_SELF_TEST_OK=1`（`-Wall -Wextra -Werror` 零告警），[test_marble_camera_g1.py](../tests/test_marble_camera_g1.py) 5/5 通过且 15 组批次离线回归套件全部退出码 `0`。
  - COM9 冷启动相机全栈实证：`boot_a=c83a8468…`、`vendor_boot_a=0e768215…`、`ICP_FW_ROOT=23`、5 个 `.ko` 自动加载、`CAMERA_NODES_READY count=28 wait_steps=0`、`CAMERA_G1_DONE=1`；
    - `CPAS`：`family=2 cam_ver=6.8.0 cpas_ver=1.1.0`；`ISP`：`3x IFE`、`2x IFE-Lite`、`3x CSID v6.8.0`、`2x CSID-Lite v6.8.0`（`iommu_ns=0x366e`）；`JPEG`：`1x ENC + 1x DMA`（`iommu_ns=0x158ba`）；`5x CSIPHY v0x213`；`3x TPG v0x10000003`；
    - **LX7 ICP 固件安全启动实证**（打开 `/dev/video0` 初始化 `cam_mem_mgr` 后查询 `/dev/v4l-subdev22`）：**`ICP v4l-subdev22 rc=0 fw=18.18.18 api=19.19.19 num_ipe=2 num_bps=1 iommu_ns=0x2cd9a`**；
    - **4/4 物理相机传感器真实 CCI I2C 芯片 ID 探测全绿**：
      - Slot 0（后置 64MP 广角，`v4l-subdev8`, `csiphy=2`, `yaw=180`）：**OV64B40**（`0x20`, `reg=0x300a` → **`0x5664`**, `rc=0`）；
      - Slot 1（前置 16MP，`v4l-subdev9`, `csiphy=3`, `yaw=0`）：**S5K3P9**（`0x20`, `reg=0x0000` → **`0x3109`**, `rc=0`）；
      - Slot 2（后置 8MP 超广角，`v4l-subdev10`, `csiphy=0`, `yaw=180`）：**IMX355**（`0x34`, `reg=0x0016` → **`0x0355`**, `rc=0`）；
      - Slot 3（后置 2MP 微距，`v4l-subdev11`, `csiphy=1`, `yaw=180`）：**GC02M1**（`0x6e`, `reg=0x00f0` → **`0x02e0`**, `rc=0`）。
  - COM9 零回归与安全：`rmnet_data0`（`10.20.197.159/26`）公网 Ping `119.29.29.29` **`4/4 (0% loss, min=31.791ms)`** 与 `223.5.5.5` **`4/4 (0% loss, min=22.994ms)`**、`Icmp: InEchoReps +8`、`InHdrErrors=0`、`InCsumErrors=0`、`WDS_STOP rc=0`；`VENUS_NODES_READY count=2`、6 个核心 SA（`4010/4007/4008/4009/65866/66250`）与 5 个常驻守护进程全在、`DCM` `errCode=0 token_len=236`、`HEIF` `7/7 OK`、`PAL_PCM=0`、`PERSIST=0`、`KGSL_FAULT=0`、`FATAL_MSS=0`、`OOPS_EXACT=0`。
- **边界、转入 Batch G2 与回退**：`marble_camera_probe --all` 在扫描 `cam-icp` 时须先保持 `/dev/video0` 打开以初始化 `cam_mem_mgr`（已在 COM9 验证 `icp=1 fw=18.18.18`，将在 Batch G2 一并合入）；下一步转入 **Batch G2**（接入 OHOS `camera_host` HDI + `SA 3008` `camera_service` 与实机帧采集流）。回退 F9 `d01c4581` 使用 [flash-h20-g1-control.sh](../work/marble-dual-20261123/flash-h20-g1-control.sh) `restore <BOOT_ID>`。

## 批次 G2 OHOS Camera HDI (`camera_host` host-9 + 净室 VDI) + `SA 3008` (`camera_service`) 端到端闭环里程碑（2026-11-26 更新：`vendor_boot_h20_g2_camera_hdi_a9b087d7.img`）

- **旧现象 → 固定输入 → 最小差异**：G1 已在硬件层打通 4 颗物理传感器与 LX7 ICP，但 OHOS 上层仍未启用 `camera_host`（HDF host 9）、`SA 3008`（`camera_service` 仍在 `sceneboard-optional-sa.list` 屏蔽表中）与 SM7475 相机 VDI。在 accepted **G1 `0e768215`** 上叠加打包 [`build_marble_h20_g2_camera_hdi.py`](../tools/packaging/build_marble_h20_g2_camera_hdi.py)（47 个前序保护成员逐字节不变，`hdf_default_phase4.hcb` 9→10 host，`hmos-init-debug.cfg`/`stage-phase4-marble.sh`/`start-u2-postboot-marble.sh` 唯一可逆拼接，`562→567` 条目），仅刷写 `vendor_boot_a`（`boot_a=c83a8468…` 未动），产出 `/mnt/e/marble-tmp/vendor_boot_h20_g2_camera_hdi_a9b087d7.img`（`sha256=a9b087d72c389efaeb719a73ee6986dbe05b20f8b581efbea081ebc4a68c3c53`）。
- **新增最小实现**：
  1. **净室 SM7475 OHOS Camera VDI 1.0（[marble_camera_vdi_impl.cpp](../compat/camera/marble_camera_vdi_impl.cpp)，79 KB `libmarble_camera_vdi_impl.z.so`）**：以 `-DOHOS_PLATFORM` 对齐 donor `RefBase::CanPromote()`，三个 VDI 类虚表尺寸与槽位（224B/232B/256B）与 donor `libcamera_external_usb_camera.so` 逐槽一致；修复 [surface.h](../compat/camera/include/surface.h) `OHOS::Surface` 非虚继承 `public RefBase`（匹配 donor `libsurface.z.so` 偏移 0）；通过未改动 donor `libcamera_host_service_1.0.z.so` 的 `HdfLoadVdi` 安全路径校验（经 `g2_bind` 绑定覆盖 `/vendor/lib64/libcamera_external_usb_camera.so` 与 `/vendor/etc/hdfconfig/camera_host_config.hcb`），暴露 4 路物理相机 `lcam001..lcam004`、`CameraMetadata` 能力块、`OH_NativeWindow_*` 预览 NV21（`format=25`）与拍照基线 JPEG（`format=12` 线性 DMA-BUF 承载有效 JFIF）流式/单拍采集，并内置客户端失联自动止停保护。
  2. **HDF host-9 + `SA 3008` 冷启动编排（[build_marble_phase4_images.py](../tools/packaging/build_marble_phase4_images.py)、[init-g2-camera-marble.cfg](../devices/marble/runtime/camera/init-g2-camera-marble.cfg)、[start-camera-g2-marble.sh](../devices/marble/runtime/camera/start-camera-g2-marble.sh)）**：从 `sceneboard-optional-sa.list` 移出 `3008`，为 28 个相机 `/dev` 节点授予 `root:dev_camera (0:3090) 0660`，由 `marble.camera.ready=true` 触发拉起 `camera_host`（`-i 9`）与 `camera_service`（`SA 3008`）；并合入更新版 `marble_camera_probe`（保持 `/dev/video0` 打开实现单次冷启动同时 `icp=1` + `probed=4`）。
  3. **端到端 Binder/ConsumerSurface 探针（[native_camera_hdi_probe.cpp](../compat/camera/native_camera_hdi_probe.cpp)，53 KB）**：采用 donor 安全 `IPCObjectStub` 虚表克隆（`+200` 偏移）+ `OH_ConsumerSurface_Create` / `OH_NativeWindow_WriteToParcel` 实测 `SA 3008`、`GetCameraIds`、`GetCameraAbility`、`OpenCamera`、`GetStreamOperator`、`CreateStreams`、`AttachBufferQueue`、`CommitStreams`、`Capture`（预览 NV21 + 拍照 JPEG）、`OnCaptureStarted/OnFrameShutter/OnCaptureEnded` 回调、`ReleaseStreams` 与 `Close`。
- **已过验证层（离线 + COM9 无干预冷启动 `BOOT_ID=96942d7e-948b-44aa-9c9c-ff207264e7bd`）**：
  - 离线：`MARBLE_CAMERA_VDI_HOST_TEST_OK=1`、`MARBLE_CAMERA_HDI_PROBE_SELF_TEST_OK=1`、[test_marble_camera_g2.py](../tests/test_marble_camera_g2.py) 4/4 通过，跨批次 115 项离线回归测试全部 OK。
  - COM9 冷启动单次自启实证：`boot_a=c83a8468…`、`vendor_boot_a=a9b087d7…`；
    - G1 硬件单次全绿：`ICP v4l-subdev22 rc=0 fw=18.18.18 api=19.19.19 num_ipe=2 num_bps=1` + `MARBLE_CAMERA_PROBE_SUMMARY ... icp=1 ... probed=4 (ov64b40/s5k3p9/imx355/gc02m1)`；
    - G2 服务零等待就绪：`CAMERA_G2_DEV_CHOWN=28`、`CAMERA_G2_PROCS host_pid=11873 sa3008_pid=11874 wait_steps=0`、`CAMERA_G2_DONE=1`、`SA_3008=NATIVE_SA_READY id=3008`；
    - **连续两轮端到端真机采集全绿（`RUN1_RC=0` & `RUN2_RC=0`）**：每轮均得 `sa3008=1 host=1 cam_count=4 abilities_ok=4 open=1 preview_frames=3 preview_nonzero=1 jpeg_ok=1 jpeg_bytes=10590 started=2 shutter=4 ended=2 errors=0 close=1`，落盘 JPEG 头尾 `FF D8 FF E0 00 10 JFIF ... FF D9`（10590 B），两轮前后 `camera_host=11873`、`camera_service=11874` PID 不变、零崩溃、零泄漏。
  - COM9 零回归与安全：7 个核心 SA（`3008/4010/4007/4008/4009/65866/66250`）与全部守护进程在线；`DCM` `errCode=0 token_len=236`、`HEIF` `7/7 OK`、`WLAN0=10.21.30.203/24`、`VENUS=2`、`CAM_MODS=5`；蜂窝 `WDS_START apn="ctnet" rc=0`、`rmnet_data0=10.21.218.66/30`、`119.29.29.29` `4/4 (0% loss, min=27.645ms)`、`223.5.5.5` `min=23.659ms`、`WDS_STOP rc=0`；`PAL_PCM=0`、`PERSIST=0`、`KGSL_FAULT=0`、`FATAL_MSS=0`、`OOPS_EXACT=0`。
- **边界、转入 Batch G3 与回退**：当前 VDI 输出为确定性彩条 NV21/JFIF 帧（证明全链路 HDI/VDI/Surface/DMA-BUF 闭环）；下一步转入 **Batch G3**，在 SM7475 硅片上打通 `cam-tpg` / 物理 `IMX355`（Slot 2）→ `CSIPHY0` → `CSID` → `IFE RDI0` → DMA-BUF 真实硬件像素采集并接入 `libmarble_camera_vdi_impl.z.so`。回退 G1 `0e768215` 使用 [flash-h20-g2-control.sh](../work/marble-dual-20261123/flash-h20-g2-control.sh) `restore <BOOT_ID>`。

## 批次 G3 SM7475 真硅片像素采集（TPG0 + 物理 Sony IMX355 真实光学）接入 Camera VDI 与 2D DCT JPEG 里程碑（2026-11-26 更新：`vendor_boot_h20_g3_camera_capture_454adf32.img`）

- **旧现象 → 固定输入 → 最小差异**：G2 已完成 OHOS Camera HDI (`camera_host` host-9) 与 `SA 3008` (`camera_service`) 端到端 Binder/Surface/DMA-BUF 闭环，但图像内容仍为软件合成彩条与 DC-only JPEG。在 accepted **G2 `a9b087d7`** 上叠加打包 [`build_marble_h20_g3_camera_capture.py`](../tools/packaging/build_marble_h20_g3_camera_capture.py)（全部 54 个 G2/G1/F1–F9/U2–U4 保护成员逐字节不变；仅替换 `libmarble_camera_vdi_impl.z.so` 与 `native_camera_hdi_probe`，新增独立硬采工具 `marble_camera_hw_capture` 并单行拼接至 `stage-phase4-marble.sh`，`567→568` 条目），仅刷写 `vendor_boot_a`（`boot_a=c83a8468…` 未动），产出 `/mnt/e/marble-tmp/vendor_boot_h20_g3_camera_capture_454adf32.img`（`sha256=454adf324c6b6b0e9f7878f69be708894013c106afab2816cceaba9f7b8e866c`）。
- **新增最小实现**：
  1. **净室 SM7475 真硅片采集与 DSP/JPEG 引擎（[marble_isp_capture.h](../compat/camera/marble_isp_capture.h)、[marble_isp_capture.c](../compat/camera/marble_isp_capture.c)）**：
     - 完整打通高通 SM7475 Spectra/Titan camera-kernel 3 槽 DMA-BUF 环形持续流协议（`CAM_REQ_MGR_SCHED_REQ` 先于 `CAM_CONFIG_DEV(UPDATE)`、`op_code` 偏移 `-1` 适配 `cam_ife_hw_mgr.c:11008`、单设备 ISP `CRM_LINK` `pd=1` 即时下发、TPG v1.3 `skip_pattern=0xFFFFFFFF` 解除 `tpg_throttle` 阻塞、同句柄 `CAM_SENSOR_PROBE_CMD` 规避 `cam_sensor_shutdown` 关句柄重置 `is_probe_succeed`）；
     - 同时支持 **Stage G3-A**（`cam-tpg0` v1.3 → `CSID-Lite` → `IFE-Lite RDI0` → IOMMU → DMA-BUF + `cam_sync`）与 **Stage G3-B**（Slot 2 物理 **Sony IMX355** 19.2 MHz MCLK `1640×1232` / `820×616` → `CSIPHY0` 4-lane D-PHY 720 Mbps/lane → `CSID-Lite` → `IFE-Lite RDI0` → IOMMU → DMA-BUF + `cam_sync`），退流/关闭时逆序 `STREAMOFF` 并干净断电；
     - 内置零外部依赖 MIPI RAW10 解包、RGGB 2×2 Bayer 去马赛克、黑电平扣减（`64`）、灰世界 AWB + 98 分位自动增益 + sRGB Gamma、NV21/RGBA 转换，以及完整 **2D 8×8 Baseline DCT + Annex K DC/AC Huffman YCbCr 4:2:0 JFIF 编码器**（零 `libm`/`libjpeg` 依赖）。
  2. **Camera VDI 接入真硅片实时流与双摄探针升级（[marble_camera_vdi_impl.cpp](../compat/camera/marble_camera_vdi_impl.cpp)、[native_camera_hdi_probe.cpp](../compat/camera/native_camera_hdi_probe.cpp)、[marble_camera_hw_capture.c](../compat/camera/marble_camera_hw_capture.c)、[build_camera_g3.sh](../compat/camera/build_camera_g3.sh)）**：
     - `MarbleStreamOperatorVdi` 保持 `224/232/256` B 虚表与 donor 100% 一致：`lcam003`（Slot 2）实时拉起物理 **IMX355 真实光学流**（`imx355_optical`），`lcam001/002/004` 拉起 **SM7475 硬件 TPG0 硅片流**（`sm7475_tpg`），硬件不可用时自动优雅降级至软件彩条；`stopAllCaptures()/ReleaseStreams()/Close()` 保证安全停流并断电传感器；
     - `native_camera_hdi_probe` 默认同轮串行验证 `lcam001`（`sm7475_tpg`）与 `lcam003`（`imx355_optical`）经真实 OHOS Binder + `ConsumerSurface` + DMA-BUF 的 NV21 预览（3 帧 + 逐帧校验和变化检测）与 Baseline DCT JPEG 单拍。
- **已过验证层（离线 + COM9 无干预冷启动 `BOOT_ID=7130bb5e-62aa-4e1c-93aa-377288987664`）**：
  - 离线：宿主 `--self-test` + Python Pillow 端到端 JPEG 解码精度 ±1 LSB；[test_marble_camera_g3.py](../tests/test_marble_camera_g3.py) 3/3 通过，G3/G2/G1/F1–F9/U2–U4 共 **155/155** 项离线回归测试全部 OK。
  - COM9 冷启动实机双摄闭环验证（`boot_a=c83a8468…` 未动、`vendor_boot_a=454adf32…`、`BAT=100`、`BIND_VDI=1`、`HOST_PID=11746`、`CAM_SERVICE_PID=11757`）：
    - **连续两轮双摄 OHOS HDI 端到端实采全绿（`RUN1_RC=0` & `RUN2_RC=0`）**：
      - `lcam001`（SM7475 TPG0 硅片）：两轮均 `open=1 preview_frames=3 preview_nonzero=1 jpeg_ok=1 jpeg_bytes=6340 hw_src=sm7475_tpg hw_frames=5 raw10_mean=511 close=1`；宿主机拉取该 HDI 产出的 `6340 B` JPEG 经 Pillow 解码得 `size=(640, 480) mode=RGB`，8 彩条中心 RGB 为 `(255,255,255), (255,255,1), (1,255,255), (1,255,0), (255,0,255), (254,0,0), (0,0,254), (0,0,0)`（与理论 SMPTE 彩条误差 ≤1 LSB）；
      - `lcam003`（物理 Sony IMX355 真实光学 `1640×1232`）：两轮均 `open=1 preview_frames=3 preview_nonzero=1 preview_csum_changes=2 jpeg_ok=1 jpeg_bytes=67302/68982 hw_src=imx355_optical hw_frames=6 raw10_mean=63/64 close=1`（原生 10-bit 光学黑电平 64 + 逐帧独立传感器噪声校验和），头尾 `FF D8 FF E0 00 10 JFIF ... FF D9`，另经 Pillow 实测解码 `160×120` 光学 JPEG 通过（`OPT_IMX355_JPEG_PIL_OK bytes=5917`）；
      - 汇总：`MARBLE_CAMERA_HDI_PROBE_SUMMARY sa3008=1 host=1 cam_count=4 abilities_ok=4 open=1 preview_frames=6 preview_nonzero=1 jpeg_ok=1 started=4 shutter=11 ended=4 errors=0 close=1 tpg_hw=1 imx355_hw=1`，两轮前后 `camera_host` PID `11746` 恒定不变；硬件中断实证 `CSIPHY0=2`、`cci0=24`、`cpas-cdm=46`、`csid-lite=486`；独立 CLI `/compat/bin/marble_camera_hw_capture --imx355 --frames 2` 亦得 `frames_ok=2/2 csum_changes=1`。
  - COM9 零回归与安全：`DCM` `errCode=0 token_len=236`、`HEIF_PROBE_ALL_OK cases=7`、`WLAN0=10.21.30.203/24`、`VENUS=2`、蜂窝 `WDS_START apn="ctnet" rc=0` & `WDS_STOP rc=0`、`PAL_PCM=0`、`PERSIST=0`、`KGSL_FAULT=0`、`FATAL_MSS=0`、`OOPS_EXACT=0`。
- **边界、转入 Batch G4 与回退**：当前 `lcam003`（后置超广角 IMX355）已全链路真光学实采，`lcam001/002/004` 使用 SM7475 硬件 TPG0 硅片流；下一步转入 **Batch G4**（解锁更多物理光学传感器如 `OV64B40`/`S5K3P9`/`GC02M1` + 验证 `SA 3008` `HCameraService` 框架级客户端接入）。回退 G2 `a9b087d7` 使用 [flash-h20-g3-control.sh](../work/marble-dual-20261123/flash-h20-g3-control.sh) `restore <BOOT_ID>`。

## 批次 G4 全 4 颗物理光学镜头（OV64B40 C-PHY / S5K3P9 / IMX355 / GC02M1）接入 Camera VDI 与公共 `libohcamera.so` 框架闭环里程碑（2026-11-26 更新：`vendor_boot_h20_g4_all_cameras_cc78daec.img`）

- **旧现象 → 固定输入 → 最小差异**：G3 仅在 `lcam003`（后置超广角 IMX355）打通真实光学采集，主力后摄 `lcam001`（OV64B40）、前摄 `lcam002`（S5K3P9）与微距 `lcam004`（GC02M1）仍走硬件 TPG0 彩条，且未验证公共 OHOS NDK `libohcamera.so` → `SA 3008` 链路。在 accepted **G3 `454adf32`** 上叠加打包 [`build_marble_h20_g4_all_cameras.py`](../tools/packaging/build_marble_h20_g4_all_cameras.py)（全部 54 个前序保护成员逐字节不变；仅更新 `libmarble_camera_vdi_impl.z.so`、`native_camera_hdi_probe`、`marble_camera_hw_capture`，新增 `ohcamera_fwk_probe` 并单行拼接至 `stage-phase4-marble.sh`，`568→569` 条目），仅刷写 `vendor_boot_a`（`boot_a=c83a8468…` 未动），产出 `/mnt/e/marble-tmp/vendor_boot_h20_g4_all_cameras_cc78daec.img`（`sha256=cc78daec5ed49b6af6fe63cfdc929cc0137221f31127fa1f0fd1129a48307bd5`）。
- **新增最小实现**：
  1. **全 4 颗物理传感器 SM7475 硬件驱动与 4 序 Bayer ISP（[marble_sensor_tables.h](../compat/camera/marble_sensor_tables.h)、[marble_isp_capture.h](../compat/camera/marble_isp_capture.h)、[marble_isp_capture.c](../compat/camera/marble_isp_capture.c)、[marble_camera_hw_capture.c](../compat/camera/marble_camera_hw_capture.c)）**：
     - 解码 Xiaomi `marble` 官方 QTI Parameter Parser V3.4.0 传感器配置，完整支持 **Slot 0 后置主摄 OV64B40**（`0x20`, `CSIPHY2` **3-trio C-PHY** `csiphy_3phase=1` `lane_assign=0x0210` `data_rate=3.84 Gbps/trio`, `1920×1080` / `4624×3472` RAW10, `RGGB`）、**Slot 1 前置 16MP S5K3P9SP04**（`0x20`, `CSIPHY3` 4-lane D-PHY `545 Mbps/lane`, 16-bit I2C + `0x6f12` 1398-word `CAM_CMD_I2C_CONT_WR` T&P 固件突发写入, `2304×1728` RAW10, `GRBG`）、**Slot 2 后置超广角 IMX355**（`0x34`, `CSIPHY0` 4-lane D-PHY `720 Mbps/lane`, `1640×1232` RAW10, `RGGB`）与 **Slot 3 后置微距 GC02M1**（`0x6e`, `CSIPHY1` 1-lane D-PHY `672 Mbps/lane`, `1600×1200` RAW10, `GBRG`）；
     - 新增 `marble_bayer_to_rgb24_resize_ex` 支持全部 4 种 Bayer 排列（`RGGB/GRBG/BGGR/GBRG`）与独立断电/清理。
  2. **Camera VDI 全 4 路真光学切换、元数据修复与双层探针（[marble_camera_vdi_impl.cpp](../compat/camera/marble_camera_vdi_impl.cpp)、[native_camera_hdi_probe.cpp](../compat/camera/native_camera_hdi_probe.cpp)、[ohcamera_fwk_probe.cpp](../compat/camera/ohcamera_fwk_probe.cpp)、[build_camera_g4.sh](../compat/camera/build_camera_g4.sh)）**：
     - `MarbleStreamOperatorVdi` 保持 `224/232/256` B 虚表一致，将 `lcam001..lcam004` 全部切至各自物理光学传感器（单路失败自动降级至 SM7475 TPG0 再至软件彩条）；修复 `OHOS_CONTROL_AE_COMPENSATION_STEP` 为标准 `camera_rational_t`（`count=1`）并补齐 `OHOS_ABILITY_AE_COMPENSATION_*`；
     - `native_camera_hdi_probe` 默认串行实测全部 4 路物理光学相机经 Binder + `ConsumerSurface` + DMA-BUF 的 NV21 预览与 Baseline DCT JPEG；`ohcamera_fwk_probe` 通过公开 OHOS NDK `libohcamera.so` 实测 `SA 3008`（`camera_service`）相机枚举、前后摄能力集与手电筒支持查询（严格遵守 OHOS AccessToken 权限边界，不做任何绕过）。
- **已过验证层（离线 + COM9 无干预冷启动 `BOOT_ID=dd904335-2ca5-4b50-83e1-a7d6977bcebd`）**：
  - 离线：宿主 `--self-test` + Pillow 验证；[test_marble_camera_g4.py](../tests/test_marble_camera_g4.py) 3/3 通过，跨批次 G4/G3/G2/G1/F1–F9/U2–U4 共 **158/158** 项离线回归测试全部 OK。
  - COM9 冷启动全 4 摄闭环验证（`boot_a=c83a8468…` 未动、`vendor_boot_a=cc78daec…`、`BAT=100`、`libmarble_camera_vdi_impl.z.so=5f6f38de…`、`HOST_PID=11724`、`CAM_SVC_PID=11725`）：
    - **公开 `libohcamera.so` → `SA 3008` 框架探测全绿（`FWK_RC=0`）**：`OH_CAMERA_MGR rc=0`、`OH_TORCH_SUPPORTED rc=0 supported=1`、`OH_SUPPORTED_CAMERAS rc=0 count=2`（`CAM[0] id=lcam001 pos=1 BACK`、`CAM[1] id=lcam002 pos=2 FRONT`，各 `CAP rc=0 prev=6 photo=3 video=4`），`MARBLE_OHCAMERA_FWK_PROBE_SUMMARY ok=1 cams=2 caps_ok=2 back=1 front=1`；
    - **连续两轮全 4 物理光学镜头 OHOS HDI Binder/DMA-BUF 实采全绿（`RUN1_RC=0` & `RUN2_RC=0`）**：
      - `lcam001`（后置主摄 OV64B40 C-PHY 3-trio）：`hw_src=ov64b40_optical hw_frames=5 preview_csum_changes=2 jpeg_bytes=61073/60324 close=1`，宿主机校验 SHA256 与 Pillow 解码 `640×480 RGB` OK；
      - `lcam002`（前置 16MP S5K3P9 4-lane）：`hw_src=s5k3p9_optical hw_frames=5 preview_csum_changes=2 jpeg_bytes=74640/73886 close=1`，宿主机校验 SHA256 与 Pillow 解码 `640×480 RGB` OK；
      - `lcam003`（后置超广角 IMX355 4-lane）：`hw_src=imx355_optical hw_frames=6 preview_csum_changes=2 jpeg_bytes=70504/70508 close=1`，宿主机校验 SHA256 与 Pillow 解码 `640×480 RGB` OK；
      - `lcam004`（后置微距 GC02M1 1-lane）：`hw_src=gc02m1_optical hw_frames=5 preview_csum_changes=2 jpeg_bytes=27640/27564 close=1`，宿主机校验 SHA256 与 Pillow 解码 `640×480 RGB` OK；
      - 汇总：`MARBLE_CAMERA_HDI_PROBE_SUMMARY ... ov64b40_hw=1 s5k3p9_hw=1 imx355_hw=1 gc02m1_hw=1 optical_cams=4 errors=0`，两轮共 8 次硬件开关流前后 `camera_host` PID `11724` 恒定不变；硬件中断实证 `CSIPHY0=2`、`CSIPHY1=4`、`CSIPHY2=4`、`CSIPHY3=2`、`cci0=174`、`cci1=24`、`cpas-cdm=90`、`csid-lite=247`，`CAM_ERR_CNT=0`。
  - COM9 零回归与安全：`DCM errCode=0 token_len=236`、`HEIF_PROBE_ALL_OK cases=7`、`WLAN0=10.21.30.203/24`、`VENUS=2`、`PAL_PCM=0`、`PERSIST=0`、`KGSL_FAULT=0`、`FATAL_MSS=0`、`OOPS_EXACT=0`。
- **边界与回退**：全部 4 颗物理光学传感器（主摄/前摄/超广角/微距）均已通过 OHOS Camera HDI 真实光学出图并验证 `SA 3008` 框架接入；后续可按需扩展闪光灯手电筒（`cam-flash` / `SetFlashlight`）、对焦马达（`cam-actuator`）、自适应自动曝光（AE）或转向硬件视频编解码 / 外设子系统。回退 G3 `454adf32` 使用 [flash-h20-g4-control.sh](../work/marble-dual-20261123/flash-h20-g4-control.sh) `restore <BOOT_ID>`。

## 批次 H1 SM7475 硬件 NFC（NXP SN110T NCI 2.0 `/dev/nq-nci` + `nfc_host` host-10 + Clean-Room OHOS NFC HDI v1.1）里程碑（2026-11-26 更新：`vendor_boot_h20_h1_nfc_1f028cce.img`）

- **旧现象 → 固定输入 → 最小差异**：SM7475 内核 `nfc_i2c` 已在启动时探测到物理 NXP SN110T（`1-0028: sn-nci`，`/sys/class/qti-nfc/nq-nci/dev=507:0`），但 HarmonyOS chroot 中尚未创建 `/dev/nq-nci`、HDF `hdf_default_phase4.hcb` 仅配置至 host-9（`camera_host`）、缺 `nfc_host`（host-10）与适配 SM7475 `/dev/nq-nci` 的 NFC HDI v1.1 实现。在 accepted **G4 `cc78daec`** 上叠加打包 [`build_marble_h20_h1_nfc.py`](../tools/packaging/build_marble_h20_h1_nfc.py)（全部 55 个前序保护成员逐字节不变；更新 `hdf_default_phase4.hcb` 新增 `nfc_host` host-10 与 `stage-phase4-marble.sh` 单行接入，新增 `libmarble_nfc_interface_service_1.1.z.so`、`marble_nfc_hw_probe`、`native_nfc_hdi_probe`、`init-h1-nfc-marble.cfg`、`start-nfc-h1-marble.sh`，`569→574` 条目），仅刷写 `vendor_boot_a`（`boot_a=c83a8468…` 未动），产出 `/mnt/e/marble-tmp/vendor_boot_h20_h1_nfc_1f028cce.img`（`sha256=1f028cce85853bfb4ea0f59adf1634752b056e6786b1f40b8a110283273cc248`）。
- **新增最小实现**：
  1. **NXP SN110T 独立硬件探针与 Clean-Room OHOS NFC HDI v1.1（[marble_nfc_hw_probe.c](../compat/nfc/marble_nfc_hw_probe.c)、[marble_nfc_hdi_impl.cpp](../compat/nfc/marble_nfc_hdi_impl.cpp)、[native_nfc_hdi_probe.cpp](../compat/nfc/native_nfc_hdi_probe.cpp)、[build_nfc_h1.sh](../compat/nfc/build_nfc_h1.sh)）**：
     - Clean-room 实现 `libmarble_nfc_interface_service_1.1.z.so`（SONAME `libnfc_interface_service_1.1.z.so`，导出 `NfcInterfaceImplGetInstance`，`_ZTVN4OHOS3HDI3Nfc4V1_17NfcImplE` 虚表大小精确 `296 B` 与 donor `100%` 一致，`NfcVendorConfig` 各字段偏移经 `static_assert` 对齐 donor `libnfc_stub_1.1.z.so` 反汇编）；直接通过 `/dev/nq-nci` ioctl（`NFCC_GET_INFO`/`NFC_SET_PWR`/`NFC_GET_IRQ_STATE`）+ 独立 IRQ 轮询读线程与 SN110T Standby 自动重试驱动真硅片 NCI 2.0，并在 `Close()`/`Shutdown()` 时干净断电 VEN；
     - `native_nfc_hdi_probe` 链接未改动的 donor `libnfc_proxy_1.1.z.so` + `libnfc_proxy_1.0.z.so`，经真实 OHOS Binder `INfcInterface::Get(false)` + `INfcCallback` 回调验证完整 NCI 2.0 握手。
  2. **HDF host-10 宿主配置与启动编排（[build_marble_phase4_images.py](../tools/packaging/build_marble_phase4_images.py)、[init-h1-nfc-marble.cfg](../devices/marble/runtime/nfc/init-h1-nfc-marble.cfg)、[start-nfc-h1-marble.sh](../devices/marble/runtime/nfc/start-nfc-h1-marble.sh)）**：
     - `hdf_default_phase4.hcb` 新增 host-10 `nfc_host`（`libnfc_hdi_driver.z.so` → `nfc_interface_service`）；`start-nfc-h1-marble.sh` 启动期动态读取 `/sys/class/qti-nfc/nq-nci/dev` 创建 `/hmos-root/dev/nq-nci`（`3042:1027 0660`）、bind-mount `libmarble_nfc_interface_service_1.1.z.so` 并拉起 `nfc_host`。
- **已过验证层（离线 + COM9 无干预冷启动 `BOOT_ID=f9f1fc4e-25a8-4e80-9d01-acdd7df1cc50`）**：
  - 离线：[test_marble_nfc_h1.py](../tests/test_marble_nfc_h1.py) 3/3 通过，跨批次 H1/G4–G1/F1–F9/U2–U4 共 **161/161** 项离线回归测试全部 OK（`84.245s`）。
  - COM9 冷启动实机双轮 Binder/NCI 2.0 闭环验证（`boot_a=c83a8468…` 未动、`vendor_boot_a=1f028cce…`、`BAT=100`、`/hmos-root/dev/nq-nci=507:0 3042:1027 0660`、`NFC_BIND=1`、`libnfc_interface_service_1.1.z.so=5c934cd2…`、`NFC_HOST PID=12053 UID=3042`）：
    - **连续两轮真实 OHOS Binder NFC HDI v1.1 实采全绿（`RUN1_RC=0` & `RUN2_RC=0`）**：
      - `NFC_HDI_VERSION rc=0 ver=1.1`、`NFC_VENDOR_CONFIG rc=0 st=0 apdu=65535 prop_cfg=9 algo=1 ok=1`、`NFC_OPEN rc=0 st=0 evt_ok=1`；
      - 经 Binder 回调真实回传硬件 NCI 2.0 帧：`BINDER_NCI_RESET_RSP=40000100`、`BINDER_NCI_RESET_NTF=60000a020020040501a4011068`（`nci_ver=0x20`、`mfg=0x04` NXP、`chip=0xa4` SN110T、`fw=16.104`）、`BINDER_NCI_INIT_RSP`（33 B）、`BINDER_NCI_GET_CFG_RSP=40030600010002f401`、`NFC_CLOSE rc=0 st=0 evt_ok=1`（VEN 干净断电）；
      - 汇总：`MARBLE_NFC_HDI_PROBE_SUMMARY sa1140=0 hdi_ver=1.1 cfg_ok=1 open=1 nci_reset=1 nci_ntf=1 nci_init=1 post_init=1 pre_disc=1 get_cfg=1 close=1 nci_ver=0x20 mfg=0x04 chip=0xa4 fw=16.104 all_ok=1`，两轮前后 `nfc_host` PID `12053`（线程 `T12056`）恒定不变，`NFC_DMESG_CNT=26`。
  - COM9 零回归与夜间静音安全：G4 四摄（`CAM_HOST=11950`、`CAM_SVC=11951`、`ohcamera_fwk_probe FWK_RC=0 cams=2 caps_ok=2`、`native_camera_hdi_probe CAM_RC=0 optical_cams=4 errors=0`）、`DCM`（`DCM_RC=0 errCode=0 token_len=236`）、`HEIF`（`HEIF_PROBE_ALL_OK cases=7`）、`VENUS=2`（`VENUS_PROBE_OK_DEVS=2`）、`WLAN0=10.21.30.203/24`、`PAL_PCM=0`、`PERSIST=0`、`KGSL_FAULT=0`、`FATAL_MSS=0`、`OOPS_EXACT=0`、`CAM_ERR_CNT=0`。
- **边界与回退**：NXP SN110T 硬件与 OHOS NFC HDI v1.1 服务（`nfc_host` host-10）已全链路打通并通过双轮冷启动实测；`SA 1140`（`nfc_service`）当前为 ondemand 且 `persist.nfc.switch.state=1`（关闭态），可于后续批次验证系统框架级 `SA 1140` 开关与标签发现。回退 G4 `cc78daec` 使用 [flash-h20-h1-control.sh](../work/marble-dual-20261123/flash-h20-h1-control.sh) `restore <BOOT_ID>`。

## 批次 G5 相机实时闭环自动曝光（AE）、后置主摄 GT9764 对焦马达（AF）与 `libohcamera.so` 零元数据告警里程碑（2026-11-26 更新：`vendor_boot_h20_g5_camera_3a_70227667.img`）

- **旧现象 → 固定输入 → 最小差异**：G4 四颗物理光学传感器均采用固定室内曝光/增益（强光下易过曝、弱光下动态范围受限），后置 64MP 主摄 `OV64B40` 配套的物理 GT9764 VCM 对焦马达（`v4l-subdev7`）未初始化驱动，且公共 NDK `libohcamera.so` 的 `GetSupportedCameraOutputCapability` 因缺少 `OHOS_ABILITY_STATISTICS_DETECT_TYPE`（tag `0x1f`）每次查询输出 `Invalid metadata type size.`（`meta=0`）。在 accepted **H1 `1f028cce`** 上叠加打包 [`build_marble_h20_g5_camera_3a.py`](../tools/packaging/build_marble_h20_g5_camera_3a.py)（全部 60 个前序保护成员含 H1 NFC / G1–G2 / F1–F9 / U1–U4 逐字节不变，零 init/stage/HCB 脚本改动；仅原位替换 4 个相机产物，`574→574` 条目），仅刷写 `vendor_boot_a`（`boot_a=c83a8468…` 未动），产出 `/mnt/e/marble-tmp/vendor_boot_h20_g5_camera_3a_70227667.img`（`sha256=7022766795ee51311f0fe8f6d52d0af57cee4c7ffefd53d33b10bcc48e65b39c`）。
- **新增最小实现**：
  1. **全 4 颗物理传感器实时闭环 AE 与后置主摄 GT9764 VCM 对焦马达（[marble_isp_capture.h](../compat/camera/marble_isp_capture.h)、[marble_isp_capture.c](../compat/camera/marble_isp_capture.c)、[marble_camera_hw_capture.c](../compat/camera/marble_camera_hw_capture.c)）**：
     - 实现宿主可单测的定点闭环自动曝光控制器 `marble_ae_compute_next`（去黑电平目标亮度 `220`、防高光裁剪 `clip_permille`、曝光优先增益其次、死区防抖与 1 帧流水线冷却），利用内核 `CAM_SENSOR_PACKET_OPCODE_SENSOR_INITIAL_CONFIG` 在推流态无状态扰动地经 CCI I2C 实时下发曝光/模拟增益（覆盖 `OV64B40` 组 0 原子更新、`S5K3P9` 16-bit、`IMX355` 组保与 `GC02M1` 页 0），同步输出中心 ROI 锐度指标 `sharpness`；
     - 按 Xiaomi `marble` 官方 `com.qti.sensormodule.marble_ofilm_ov64b40_wide_i.bin` 寄存器表驱动 `/dev/v4l-subdev7`（`CAM_ACTUATOR_DEVICE_TYPE` slot 0，GT9764 `0x18` FAST_PLUS、VAF 供电与 5 步 AAC3 静音消振初始化 → 超焦距 DAC `370`），暴露 `marble_hw_session_set_focus` 与 `--focus`/`--af-sweep`，并在会话关闭时平滑回位 DAC `512` 后断电 `VAF`。
  2. **Camera VDI 元数据对齐与框架探针升级（[marble_camera_vdi_impl.cpp](../compat/camera/marble_camera_vdi_impl.cpp)、[ohcamera_fwk_probe.cpp](../compat/camera/ohcamera_fwk_probe.cpp)、[build_camera_g5.sh](../compat/camera/build_camera_g5.sh)）**：
     - 在 `buildCameraAbilityVec` 补齐 `OHOS_ABILITY_STATISTICS_DETECT_TYPE`（`OHOS_CAMERA_HUMAN_FACE_DETECT`），保持 `224/232/256 B` VDI 虚表不变；遥测文件扩展输出 `ae_exp/ae_gain/ae_upd/act_ok/focus_dac/sharp`；`ohcamera_fwk_probe` 新增 `metas_ok` 校验。
- **已过验证层（离线 + COM9 无干预冷启动 `BOOT_ID=b5330856-b83a-41e0-94c2-904dcaf0092c`）**：
  - 离线：宿主 `--self-test` 覆盖暗光/强光/裁剪/死区 4 场景 AE 收敛 + Bayer/JPEG 解码；[test_marble_camera_g5.py](../tests/test_marble_camera_g5.py) 3/3 通过，全部活跃跨批次（G5/H1/G4–G1/F1–F9/U1–U4）离线回归测试通过。
  - COM9 冷启动全量验收（`boot_a=c83a8468…` 未动、`vendor_boot_a=70227667…`、`BAT=100`、`libmarble_camera_vdi_impl.z.so=cadfa428…`、`CAM_HOST=11772`、`CAM_SVC=11774`、`NFC_HOST=11869`）：
    - **公共 `libohcamera.so` → `SA 3008` 零元数据告警（`FWK_RC=0`, `META_ERR_CNT=0`）**：`CAM[0]`（`lcam001`）与 `CAM[1]`（`lcam002`）均返回 `CAP rc=0 prev=6 photo=3 video=4 meta=1`，`MARBLE_OHCAMERA_FWK_PROBE_SUMMARY ok=1 cams=2 caps_ok=2 metas_ok=2 back=1 front=1`，`hilog` 中 `Invalid metadata type size.` 计数归零；
    - **连续两轮全 4 物理镜头 OHOS HDI 闭环与 GT9764 AF 扫焦全绿**：`CAM1_RC=0` & `CAM2_RC=0`（`optical_cams=4 errors=0 ae_upd=1`，两轮前后 `camera_host` PID `11772` 不变）；`marble_camera_hw_capture --ov64b40 --af-sweep --frames 6` 得 `OV_AF_RC=0`、`act=1`、`dac=320→370→450→550→370→320`、`ae_exp=9952 ae_gain=3968 ae_upd=2`、`frames_ok=6/6`、`CAM_ERR_CNT=0`（另四摄单测 `OV64B40/S5K3P9/IMX355/GC02M1` 全部实证在线 CCI I2C 曝光/增益生效且 `0 CAM_ERR`）；
    - **全子系统零回归与夜间静音安全**：H1 NFC（`NFC_RC=0 all_ok=1 chip=0xa4 fw=16.104`，`nfc_host` PID `11869` 不变）、`DCM`（`DCM_RC=0 errCode=0 token_len=236`）、`HEIF`（`HEIF_PROBE_ALL_OK cases=7`）、`VENUS=2`、`WLAN0=10.21.30.203/24`、`PAL_PCM=0`、`PERSIST=0`、`KGSL_FAULT=0`、`FATAL_MSS=0`、`OOPS_EXACT=0`、`CAM_ERR_CNT=0`。
- **边界与回退**：四摄实时闭环 AE、后置 64MP 主摄 GT9764 静音对焦驱动及 `libohcamera.so` 零元数据告警均已通过冷启动实测；后续可进一步在 VDI 中按帧自动触发 CDAF 连续对焦或在白天验证闪光灯手电筒。回退 H1 `1f028cce` 使用 [flash-h20-g5-control.sh](../work/marble-dual-20261123/flash-h20-g5-control.sh) `restore <BOOT_ID>`。

## 批次 H2 SM7475 硬件热管理（Thermal HDI v1.1 + `thermal_host` host-11）里程碑（2026-11-26 更新：`vendor_boot_h20_h2_thermal_b990d6a1.img`）

- **旧现象 → 固定输入 → 最小差异**：SM7475 内核 `/sys/class/thermal/thermal_zone*` 实时暴露 CPU/SoC/GPU/电池/USB/Modem/Camera 全套温区，但 HarmonyOS `hdf_default_phase4.hcb` 仅配置至 host-10（`nfc_host`），缺少 `thermal_host`（host-11）与适配 SM7475 硬件温区的 `libthermal_interface_service_1.1.z.so`。在 accepted **G5 `70227667`** 上叠加打包 [`build_marble_h20_h2_thermal.py`](../tools/packaging/build_marble_h20_h2_thermal.py)（全部 60 个前序保护成员含 G5 四摄 3A / H1 NFC / F1–F9 / U1–U4 逐字节不变；更新 `hdf_default_phase4.hcb` 从 11 宿主扩至 12 宿主新增 `thermal_host` host-11、`hmos-init-debug.cfg`、`stage-phase4-marble.sh` 与 `start-u2-postboot-marble.sh`，新增 `libmarble_thermal_interface_service_1.1.z.so`、`native_thermal_hdi_probe`、`init-h2-thermal-marble.cfg`、`start-thermal-h2-marble.sh`，`574→578` 条目），仅刷写 `vendor_boot_a`（`boot_a=c83a8468…` 未动），产出 `/mnt/e/marble-tmp/vendor_boot_h20_h2_thermal_b990d6a1.img`（`sha256=b990d6a1126103c0cfdcaee9638aead2b084696ee6abb055d66bf7919ca8b875`）。
- **新增最小实现**：
  1. **Clean-Room SM7475 Thermal HDI v1.1 与端到端 Binder 探针（[marble_thermal_hdi_impl.cpp](../compat/thermal/marble_thermal_hdi_impl.cpp)、[native_thermal_hdi_probe.cpp](../compat/thermal/native_thermal_hdi_probe.cpp)、[build_thermal_h2.sh](../compat/thermal/build_thermal_h2.sh)）**：
     - 严格按 donor `libthermal_stub_1.1.z.so` 反汇编对齐 `ThermalZoneInfo`（`32 B`）与 `HdfThermalCallbackInfo`（`24 B`）内存布局及 `ThermalInterfaceImpl` 虚表（精确 `272 B`），导出 `ThermalInterfaceImplGetInstance`，仅依赖 `[libutils.z.so, libc++.so, libc.so]`；实时扫描 SM7475 `/sys/class/thermal/thermal_zone*` 聚合输出 9 个标准 OHOS 温区（`cpu`/`soc`/`gpu`/`battery`/`charger`/`pa`/`camera`/`shell`/`ambient`，单位 m°C），支持后台异步 `IThermalCallback::OnThermalDataEvent` Binder 推送与只读安全控制 RPC；
     - `native_thermal_hdi_probe` 链接未改动的 donor `libthermal_proxy_1.1.z.so`，经真实 Binder 验证 `GetVersion`、`GetThermalZoneInfo`、`Register`/`OnThermalDataEvent`/`Unregister`、`RegisterFanCallback`/`UnregisterFanCallback` 与控制 RPC。
  2. **HDF host-11 宿主编排（[build_marble_phase4_images.py](../tools/packaging/build_marble_phase4_images.py)、[init-h2-thermal-marble.cfg](../devices/marble/runtime/thermal/init-h2-thermal-marble.cfg)、[start-thermal-h2-marble.sh](../devices/marble/runtime/thermal/start-thermal-h2-marble.sh)）**：
     - `hdf_default_phase4.hcb` 新增 host-11 `thermal_host`（`libthermal_driver.z.so` → `thermal_interface_service`，`uid=power_host` 3025）；启动期 bind-mount `libmarble_thermal_interface_service_1.1.z.so` 并通过 `marble.thermal.ready=true` 拉起 `thermal_host`。
- **已过验证层（离线 + COM9 无干预冷启动 `BOOT_ID=59890f83-4f70-4c62-938a-7d63e192d69c`）**：
  - 离线：[test_marble_thermal_h2.py](../tests/test_marble_thermal_h2.py) 3/3 通过，跨批次活跃回归测试共 **134/134** 项全绿（H2/G5/H1/G4–G1/F1–F9/U2–U4/Phase4）。
  - COM9 冷启动双轮 Binder/HDI 闭环验证（`boot_a=c83a8468…` 未动、`vendor_boot_a=b990d6a1…`、`BAT=100`、`THERMAL_BIND=1`、`libthermal_interface_service_1.1.z.so=d2fadc11…`、`THERMAL_HOST PID=11929 UID=3025`）：
    - **连续两轮 `native_thermal_hdi_probe` 全绿（`RUN1_RC=0` & `RUN2_RC=0`）**：`THERMAL_HDI_VERSION rc=0 ver=1.1`，实时读出全部 9 路硬件温区且两轮动态变化（Run 1 `cpu=61000 soc=49500 gpu=45100 battery=29600 charger=29000 pa=45500 camera=43200 shell=30800 ambient=29000` → Run 2 `cpu=58200 soc=48700 ... pa=45100`），异步回调 `THERMAL_CALLBACK reg_rc=0 cb_ok=1 cb_events=1 cb_zones=9 unreg_rc=0`，控制 RPC `ctrl_ok=1`，`all_ok=1`，两轮前后 `thermal_host` PID `11929` 恒定不变。
  - COM9 全子系统零回归与夜间静音安全：G5 四摄（`camera_host=11807`、`FWK_RC=0 metas_ok=2`、`CAM_RC=0 optical_cams=4`）、H1 NFC（`nfc_host=11911`、`NFC_RC=0 all_ok=1 chip=0xa4 fw=16.104`）、`DCM`（`DCM_RC=0 errCode=0 token_len=236`）、`HEIF`（`HEIF_PROBE_ALL_OK cases=7`）、`VENUS=2`、`WLAN0=10.21.30.203/24`、`PAL_PCM=0`、`PERSIST=0`、`KGSL_FAULT=0`、`FATAL_MSS=0`、`OOPS_EXACT=0`、`CAM_ERR_CNT=0`。
- **边界与回退**：SM7475 硬件热管理 HDI v1.1 与 `thermal_host`（host-11）已全链路打通并通过冷启动双轮实测；所有控制 RPC 采用只读安全守卫以免误触发调频/断充。回退 G5 `70227667` 使用 [flash-h20-h2-control.sh](../work/marble-dual-20261123/flash-h20-h2-control.sh) `restore <BOOT_ID>`。

## 批次 H3 SM7475 硬件电源管理（Power HDI v1.3 + `power_host` host-12）里程碑（2026-11-26 更新：`vendor_boot_h20_h3_power_436e32e2.img`）

- **旧现象 → 固定输入 → 最小差异**：系统 `SA 3301`（`powermgr`）已在运行并查找 `power_interface_service`（`IPowerInterface` v1.3），但 `hdf_default_phase4.hcb` 仅配置至 host-11（`thermal_host`），缺少 `power_host`（host-12）与面向 SM7475 `/sys/power/{wake_lock,wake_unlock,state}` 的 `libpower_interface_service_1.3.z.so`。在 accepted **H2 `b990d6a1`** 上叠加打包 [`build_marble_h20_h3_power.py`](../tools/packaging/build_marble_h20_h3_power.py)（全部 64 个前序保护成员含 H2 Thermal / G5 四摄 3A / H1 NFC / F1–F9 / U1–U4 逐字节不变；更新 `hdf_default_phase4.hcb` 从 12 宿主扩至 13 宿主新增 `power_host` host-12、`hmos-init-debug.cfg`、`stage-phase4-marble.sh` 与 `start-u2-postboot-marble.sh`，新增 `libmarble_power_interface_service_1.3.z.so`、`native_power_hdi_probe`、`init-h3-power-marble.cfg`、`start-power-h3-marble.sh`，`578→582` 条目），仅刷写 `vendor_boot_a`（`boot_a=c83a8468…` 未动），产出 `/mnt/e/marble-tmp/vendor_boot_h20_h3_power_436e32e2.img`（`sha256=436e32e276302c11d242426ffc82e02fc2d3ba3afc52a17adc5ccd6941e10ce6`）。
- **新增最小实现**：
  1. **Clean-Room SM7475 Power HDI v1.3 与端到端 Binder 探针（[marble_power_hdi_impl.cpp](../compat/power/marble_power_hdi_impl.cpp)、[native_power_hdi_probe.cpp](../compat/power/native_power_hdi_probe.cpp)、[build_power_h3.sh](../compat/power/build_power_h3.sh)）**：
     - 严格按 donor `libpower_stub_1.2.z.so` 反汇编对齐 `RunningLockInfo`（`40 B`）布局与 `PowerInterfaceImpl` 虚表（精确 `360 B`），导出 `PowerInterfaceImplGetInstance`，仅依赖 `[libutils.z.so, libc++.so, libc.so]`；实现真实内核 `/sys/power/wake_lock` & `/sys/power/wake_unlock` 引用计数锁管理与释放清理、`PowerDump` 实时诊断、v1.2 `IPowerHdiCallback` + v1.3 `IPowerHdiCallbackExt`（含 tag）回调分发及安全非 S3 休眠/唤醒状态机（永不读取阻塞的 `/sys/power/wakeup_count`，永不中断 COM9/USB）；
     - `native_power_hdi_probe` 链接未改动的 donor `chipset-sdk/libpower_proxy_1.3.z.so` & `libpower_proxy_1.2.z.so`，经真实 Binder 验证 `SA 3301`、`GetVersion==1.3`、内核 wakelock 持有/可见/释放/清空、v1.2+v1.3 回调、`GetWakeupReason` 与 `SetPowerConfig`/`GetPowerConfig`。
  2. **HDF host-12 宿主编排（[build_marble_phase4_images.py](../tools/packaging/build_marble_phase4_images.py)、[init-h3-power-marble.cfg](../devices/marble/runtime/power/init-h3-power-marble.cfg)、[start-power-h3-marble.sh](../devices/marble/runtime/power/start-power-h3-marble.sh)）**：
     - `hdf_default_phase4.hcb` 新增 host-12 `power_host`（`libpower_driver.z.so` → `power_interface_service`，`uid=power_host` 3025，含 `DAC_OVERRIDE`/`DAC_READ_SEARCH` 以访问 `/sys/power/wake_{lock,unlock}`）；启动期 bind-mount `libmarble_power_interface_service_1.3.z.so` 并通过 `marble.power.ready=true` 拉起 `power_host`。
- **已过验证层（离线 + COM9 无干预冷启动 `BOOT_ID=5b68df12-8e24-4355-b326-dfeb226d2f6e`）**：
  - 离线：[test_marble_power_h3.py](../tests/test_marble_power_h3.py) 3/3 通过，跨批次活跃回归测试共 **137/137** 项全绿（H3/H2/G5/H1/G4–G1/F1–F9/U2–U4/Phase4）。
  - COM9 冷启动双轮 Binder/HDI 闭环验证（`boot_a=c83a8468…` 未动、`vendor_boot_a=436e32e2…`、`BAT=100`、`POWER_BIND=1`、`libpower_interface_service_1.3.z.so=561512ac…`、`POWER_HOST PID=12082 UID=3025`、`POWERMGR PID=5845`）：
    - **连续两轮 `native_power_hdi_probe` 全绿（`RUN1_RC=0` & `RUN2_RC=0`）**：`SA3301_REGISTERED=1`、`POWER_HDI_VERSION rc=0 ver=1.3`、`POWER_WAKELOCK hold=0 unhold=0 visible=1 cleared=1 state_ok=1 rl_cb_msgs=2`（`/sys/power/wake_lock` 测试前后均干净为空）、`POWER_CALLBACKS reg=0 ext_reg=0 susp=0 wake=0 cb=1/1 ext=1/1 tag=marble_h3_tag ok=1`、`POWER_CONFIG wakeup_rc=0 reason=boot_cold cfg=balanced sys_state=[freeze mem] ok=1`、`all_ok=1`，两轮前后 `power_host` PID `12082` 恒定不变。
  - COM9 全子系统零回归与夜间静音安全：H2 Thermal（`THERMAL_RC=0 zones=9 valid=9 cpu=49900 gpu=40000 battery=29800 cb_ok=1 all_ok=1`）、H1 NFC（`NFC_RC=0 all_ok=1 chip=0xa4 fw=16.104`）、G5 四摄（`FWK_RC=0 metas_ok=2`、`CAM_RC=0 optical_cams=4 jpeg_bytes=31882 errors=0`）、`DCM`（`DCM_RC=0 errCode=0 token_len=236`）、`HEIF`（`HEIF_PROBE_ALL_OK cases=7`）、`VENUS=2`、`WLAN0=10.21.30.203/24`、`PAL_PCM=0`、`PERSIST=0`、`KGSL_FAULT=0`、`FATAL_MSS=0`、`OOPS_EXACT=0`、`CAM_ERR_CNT=0`。
- **边界与回退**：SM7475 硬件电源管理 HDI v1.3 与 `power_host`（host-12）已全链路打通并通过冷启动双轮实测；真实 S3/mem 深度休眠有意保持禁用守卫以保障调试期 COM9/USB 与系统稳定性。回退 H2 `b990d6a1` 使用 [flash-h20-h3-control.sh](../work/marble-dual-20261123/flash-h20-h3-control.sh) `restore <BOOT_ID>`。

## 批次 H4 SM7475 硬件 USB Type-C 端口（USB Port HDI v2.0 + `usb_host` host-13）里程碑（2026-11-26 更新：`vendor_boot_h20_h4_usb_port_d350cddf.img`）

- **旧现象 → 固定输入 → 最小差异**：SM7475 内核 `/sys/class/typec/port0/{power_role,data_role}` 与 `/sys/class/power_supply/usb/*` 实时暴露 Type-C 双角色状态（`source [sink]`、`host [device]`、`3.0A`、`a600000.dwc3`），但 `hdf_default_phase4.hcb` 仅配置至 host-12（`power_host`），缺少 `usb_host`（host-13）及面向 SM7475 sysfs 的 `libusb_port_interface_service_2.0.z.so`。在 accepted **H3 `436e32e2`** 上叠加打包 [`build_marble_h20_h4_usb_port.py`](../tools/packaging/build_marble_h20_h4_usb_port.py)（全部 68 个前序保护成员含 H3 Power / H2 Thermal / G5 四摄 3A / H1 NFC / F1–F9 / U1–U4 逐字节不变；更新 `hdf_default_phase4.hcb` 从 13 宿主扩至 14 宿主新增 `usb_host` host-13、`hmos-init-debug.cfg`、`stage-phase4-marble.sh` 与 `start-u2-postboot-marble.sh`，新增 `libmarble_usb_port_interface_service_2.0.z.so`、`native_usb_port_hdi_probe`、`init-h4-usb-port-marble.cfg`、`start-usb-port-h4-marble.sh`，`582→586` 条目），仅刷写 `vendor_boot_a`（`boot_a=c83a8468…` 未动），产出 `/mnt/e/marble-tmp/vendor_boot_h20_h4_usb_port_d350cddf.img`（`sha256=d350cddf2fa3cbc17d1c4180304278b6b20f5d0d5bccf61061c33e01dae32ddc`）。
- **新增最小实现**：
  1. **Clean-Room SM7475 USB Port HDI v2.0 与端到端 Binder 探针（[marble_usb_port_hdi_impl.cpp](../compat/usb/marble_usb_port_hdi_impl.cpp)、[native_usb_port_hdi_probe.cpp](../compat/usb/native_usb_port_hdi_probe.cpp)、[build_usb_h4.sh](../compat/usb/build_usb_h4.sh)）**：
     - 严格按 donor `libusb_stub_2.0.z.so` / `libusb_port_driver.z.so` 反汇编对齐结构尺寸（`UsbPort=24 B`、`PortInfo=24 B`、`UsbPortStatus=16 B`、`USBDeviceInfo=16 B`）、`UsbPortImpl::device_` 精确偏移 `+8`、`vbase_offset=0x10` 与精确 `240 B` 虚表，同时导出 `UsbPortInterfaceImplGetInstance` 与 `UsbPortImpl::UsbdEventHandle`，仅依赖 `[libutils.z.so, libc++.so, libc.so]`；实时只读解析 `/sys/class/typec/portN/{power_role,data_role,port_type}` 输出双角色端口状态与异步 Binder `IUsbdSubscriber::PortChangedEvent` 回调，并在 `SetPortRole` 强制只读安全守卫（永不改写 `/sys/class/typec` 或 `UDC` 以防断开 COM9/USB）；
     - `native_usb_port_hdi_probe` 链接未改动的 donor `chipset-sdk-sp/libusb_proxy_2.0.z.so`，经真实 Binder 验证 `GetVersion==2.0`、`QueryPort`、`QueryPorts`、异步 `BindUsbdPortSubscriber`/`PortChangedEvent`/`UnbindUsbdPortSubscriber` 与 `SetPortRole` 守卫。
  2. **HDF host-13 宿主编排（[build_marble_phase4_images.py](../tools/packaging/build_marble_phase4_images.py)、[init-h4-usb-port-marble.cfg](../devices/marble/runtime/usb/init-h4-usb-port-marble.cfg)、[start-usb-port-h4-marble.sh](../devices/marble/runtime/usb/start-usb-port-h4-marble.sh)）**：
     - `hdf_default_phase4.hcb` 新增 host-13 `usb_host`（`libusb_port_driver.z.so` → `usb_port_interface_service`，`uid=usb_host` 3023）；启动期 bind-mount `libmarble_usb_port_interface_service_2.0.z.so` 并通过 `marble.usb_port.ready=true` 拉起 `usb_host`。
- **已过验证层（离线 + COM9 无干预冷启动 `BOOT_ID=2f57699b-33bd-4c4e-8241-9f6d290a39a0`）**：
  - 离线：[test_marble_usb_port_h4.py](../tests/test_marble_usb_port_h4.py) 3/3 通过，跨批次活跃回归测试共 **140/140** 项全绿（H4/H3/H2/G5/H1/G4–G1/F1–F9/U2–U4/Phase4）。
  - COM9 冷启动双轮 Binder/HDI 闭环验证（`boot_a=c83a8468…` 未动、`vendor_boot_a=d350cddf…`、`BAT=100`、`USB_BIND=1`、`libusb_port_interface_service_2.0.z.so=01c20962…`、`USB_HOST PID=12026 UID=3023`、`HDF_HOSTS=13`）：
    - **连续两轮 `native_usb_port_hdi_probe` 全绿（`RUN1_RC=0` & `RUN2_RC=0`）**：`USB_PORT_HDI_VERSION rc=0 ver=2.0`、`USB_PORT_QUERY rc=0 port_id=1 power_role=2 data_role=2 mode=1`（对应实时 `source [sink]` + `host [device]`）、`USB_PORT_QUERY_PORTS rc=0 count=1 p0_id=1 sup=3 mode=1 power=2 data=2 match=1`、`USB_PORT_CALLBACK bind=0 unbind=0 events=1 ev_port=1 ev_power=2 ev_data=2 ev_mode=1 ev_sup=3 ok=1`、`USB_PORT_SET_ROLE same=0 bad_port=-3 bad_role=-3 ok=1`、`all_ok=1`，两轮前后 `usb_host` PID `12026` 恒定不变。
  - COM9 全子系统零回归与夜间静音安全：H3 Power（`POWER_RC=0 sa3301=1 all_ok=1`）、H2 Thermal（`THERMAL_RC=0 zones=9 cpu=49900 gpu=39600 battery=29500 all_ok=1`）、H1 NFC（`NFC_RC=0 all_ok=1 chip=0xa4 fw=16.104`）、G5 四摄（`FWK_RC=0 metas_ok=2`、`CAM_RC=0 optical_cams=4 jpeg_bytes=32331 errors=0`）、`DCM`（`DCM_RC=0 token_len=236`）、`HEIF`（`HEIF_PROBE_ALL_OK cases=7`）、`VENUS=2`、`WLAN0=10.21.30.203/24`、`PAL_PCM=0`、`PERSIST=0`、`KGSL_FAULT=0`、`FATAL_MSS=0`、`OOPS_EXACT=0`、`CAM_ERR_CNT=0`。
- **边界与回退**：SM7475 硬件 USB Type-C Port HDI v2.0 与 `usb_host`（host-13）已全链路打通并通过冷启动双轮实测；角色切换保持安全只读守卫避免断开 COM9/USB。回退 H3 `436e32e2` 使用 [flash-h20-h4-control.sh](../work/marble-dual-20261123/flash-h20-h4-control.sh) `restore <BOOT_ID>`。

## 批次 H5 SM7475 硬件内存追踪（MemoryTracker HDI v1.0 + `memorytracker_host` host-14）里程碑（2026-11-26 更新：`vendor_boot_h20_h5_memorytracker_9a05fcb7.img`）

- **旧现象 → 固定输入 → 最小差异**：系统 `SA 1909`（`memmgrservice`）已在运行，SM7475 内核 `/sys/kernel/dmabuf/buffers` 实时暴露 89+ 块 Adreno/Display/Venus/Camera DMA-BUF 缓冲区及每进程 `/proc/<pid>/{fdinfo,smaps,smaps_rollup}`，但 `hdf_default_phase4.hcb` 仅配置至 host-13（`usb_host`），缺少 `memorytracker_host`（host-14）与适配 SM7475 的 `passthrough/libmemory_tracker_interface_service_1.0.z.so`。在 accepted **H4 `d350cddf`** 上叠加打包 [`build_marble_h20_h5_memorytracker.py`](../tools/packaging/build_marble_h20_h5_memorytracker.py)（全部 72 个前序保护成员含 H4 USB Port / H3 Power / H2 Thermal / G5 四摄 3A / H1 NFC / F1–F9 / U1–U4 逐字节不变；更新 `hdf_default_phase4.hcb` 从 14 宿主扩至 15 宿主新增 `memorytracker_host` host-14、`hmos-init-debug.cfg`、`stage-phase4-marble.sh` 与 `start-u2-postboot-marble.sh`，新增 `libmarble_memory_tracker_interface_service_1.0.z.so`、`native_memorytracker_hdi_probe`、`init-h5-memorytracker-marble.cfg`、`start-memorytracker-h5-marble.sh`，`586→590` 条目），仅刷写 `vendor_boot_a`（`boot_a=c83a8468…` 未动），产出 `/mnt/e/marble-tmp/vendor_boot_h20_h5_memorytracker_9a05fcb7.img`（`sha256=9a05fcb7a600359938f1eff98d0ee351da324048bbab2d775b17b91f2cad0b93`）。
- **新增最小实现**：
  1. **Clean-Room SM7475 MemoryTracker HDI v1.0 与端到端 Binder 探针（[marble_memorytracker_hdi_impl.cpp](../compat/memorytracker/marble_memorytracker_hdi_impl.cpp)、[native_memorytracker_hdi_probe.cpp](../compat/memorytracker/native_memorytracker_hdi_probe.cpp)、[build_memorytracker_h5.sh](../compat/memorytracker/build_memorytracker_h5.sh)）**：
     - 严格按 donor `libmemorytracker_stub_1.0.z.so` 反汇编对齐 `MemoryRecord`（`16 B`）布局与 `MemoryTrackerInterfaceImpl` 虚表（精确 `208 B`），导出 `MemoryTrackerInterfaceImplGetInstance`，仅依赖 `[libutils.z.so, libc++.so, libc.so]`；同时支持 `passthrough` 直调与 HDF Binder IPC，100% 只读采集 `/sys/kernel/dmabuf/buffers/*/{size,exporter_name}`、`/proc/<pid>/fdinfo/*`、`/proc/<pid>/smaps*` 与 `/proc/meminfo`；
     - `native_memorytracker_hdi_probe` 链接未改动的 donor `libmemorytracker_proxy_1.0.z.so`，经真实 Binder 验证 `SA 1909`、`GetVersion==1.0`、全局 `GRAPH`/`GL`/`MM`/`CAM`/`OTHER` 与进程级 `GetDevMem` 及非法参数拦截。
  2. **HDF host-14 宿主编排（[build_marble_phase4_images.py](../tools/packaging/build_marble_phase4_images.py)、[init-h5-memorytracker-marble.cfg](../devices/marble/runtime/memorytracker/init-h5-memorytracker-marble.cfg)、[start-memorytracker-h5-marble.sh](../devices/marble/runtime/memorytracker/start-memorytracker-h5-marble.sh)）**：
     - `hdf_default_phase4.hcb` 新增 host-14 `memorytracker_host`（`libmemorytracker_driver.z.so` → `memory_tracker_interface_service`，`uid=memmgr` 1111，含 `DAC_READ_SEARCH`/`SYS_PTRACE`）；启动期 bind-mount `passthrough/libmemory_tracker_interface_service_1.0.z.so` 并通过 `marble.memorytracker.ready=true` 拉起 `memorytracker_host`。
- **已过验证层（离线 + COM9 无干预冷启动 `BOOT_ID=53dcc0ee-4dbe-42ec-873c-80dd413c2752`）**：
  - 离线：[test_marble_memorytracker_h5.py](../tests/test_marble_memorytracker_h5.py) 3/3 通过，跨批次活跃回归测试共 **143/143** 项全绿（H5/H4/H3/H2/G5/H1/G4–G1/F1–F9/U2–U4/Phase4）。
  - COM9 冷启动双轮 Binder/HDI 闭环验证（`boot_a=c83a8468…` 未动、`vendor_boot_a=9a05fcb7…`、`BAT=100`、`MT_BIND=1`、`libmemory_tracker_interface_service_1.0.z.so=621e9dc0…`、`MT_HOST PID=12121 UID=1111`、`MEMMGR PID=4629`、`DMABUF_CNT=89`、`HDF_HOSTS=14`）：
    - **连续两轮 `native_memorytracker_hdi_probe` 全绿（`RUN1_RC=0` & `RUN2_RC=0`）**：`SA1909_REGISTERED=1`、`MEMORYTRACKER_HDI_VERSION rc=0 ver=1.0`、实测实时 SM7475 DMA-BUF 池 `graph=118370304`（~112.9 MiB）、`gl=118370304`、全局内核 `other=739495936→739639296`、自进程 `self=3042304→3028992`、非法参数 `bad_type=-3 bad_pid=-3 ok=1`、`all_ok=1`，两轮前后 `memorytracker_host` PID `12121` 恒定不变。
  - COM9 全子系统零回归与夜间静音安全：H4 USB Port（`USB_RC=0 all_ok=1`）、H3 Power（`POWER_RC=0 sa3301=1 all_ok=1`）、H2 Thermal（`THERMAL_RC=0 zones=9 cpu=51900 gpu=40800 battery=29900 all_ok=1`）、H1 NFC（`NFC_RC=0 chip=0xa4 fw=16.104 all_ok=1`）、G5 四摄（`FWK_RC=0 metas_ok=2`、`CAM_RC=0 optical_cams=4 jpeg_bytes=32680 errors=0`）、`DCM`（`DCM_RC=0 token_len=236`）、`HEIF`（`HEIF_PROBE_ALL_OK cases=7`）、`VENUS=2`、`WLAN0=10.21.30.203/24`、`PAL_PCM=0`、`PERSIST=0`、`KGSL_FAULT=0`、`FATAL_MSS=0`、`OOPS_EXACT=0`、`CAM_ERR_CNT=0`。
- **边界与回退**：SM7475 硬件内存追踪 HDI v1.0 与 `memorytracker_host`（host-14）已打通 passthrough 与 Binder 双路径并通过冷启动双轮实测。回退 H4 `d350cddf` 使用 [flash-h20-h5-control.sh](../work/marble-dual-20261123/flash-h20-h5-control.sh) `restore <BOOT_ID>`。

## 批次 H6 SM7475 硬件视频编码器（`OH_VideoEncoder` H.264/HEVC `DYNAMIC_HANDLE` EOS 修复 + 硬件编解码环回探针）里程碑（2026-11-26 更新：`vendor_boot_h20_h6_venc_eos_c1cab391.img`）

- **旧现象 → 固定输入 → 最小差异**：通过 HarmonyOS 公共 AVCodecKit NDK（`libnative_media_venc.so` → `SA 3011 av_codec_service` → `codec_host` → `libOMX_Core.z.so` → SM7475 Venus `/dev/video33`）实测硬件 H.264/HEVC 编码时，发现 `CodecDynaBuffer::EmptyThisBuffer` 在存在 `bufferhandle` 时会无条件将 `filledLen` 覆写为 `sizeof(DynamicBuffer)=16`，导致 `omx_core_venus.c` 原判定 `(nFlags & OMX_BUFFERFLAG_EOS) && nFilledLen == 0` 失效，在每次编码结束送入空 EOS 缓冲区时错误多编入 1 帧陈旧/未初始化幽灵帧（4 进 5 出）。在 accepted **H5 `9a05fcb7`** 上叠加打包 [`build_marble_h20_h6_venc_eos.py`](../tools/packaging/build_marble_h20_h6_venc_eos.py)（全部 78 个前序保护成员含 HCB、`hmos-init-debug.cfg`、`start-u2-postboot-marble.sh` 及 H1–H5 / G1–G5 / F1–F9 / U1–U4 逐字节不变；仅更新 `graphics/libOMX_Core.z.so`、新增 `graphics/native_venc_probe` 并在 `stage-phase4-marble.sh` 拷贝探针，`590→591` 条目），仅刷写 `vendor_boot_a`（`boot_a=c83a8468…` 未动），产出 `/mnt/e/marble-tmp/vendor_boot_h20_h6_venc_eos_c1cab391.img`（`sha256=c1cab391ee7817505dc41fbaf91def0782608dd267a583222a26b4ab620e15fb`）。
- **新增最小实现**：
  1. **OpenMAX Venus 编码器 `DYNAMIC_HANDLE` EOS 修复与宿主单测（[omx_core_venus.c](../compat/codec/omx_core_venus.c)、[build_venc_h6.sh](../compat/codec/build_venc_h6.sh)）**：
     - 在 `omx_worker_main` 对 `VENUS_ROLE_ENCODER` 且 `ohos_buf_type == CODEC_BUFFER_TYPE_DYNAMIC_HANDLE` 的 `OMX_BUFFERFLAG_EOS` 输入缓冲区直接按纯 EOS 处理，调用 `venus_ctx_send_eos` 而不多编码幽灵帧；同步在宿主单测 `OMX_VENUS_HOST_TEST` 新增用例 #4 验证 2 帧 + `filledLen=16` EOS 精确输出 2 数据帧 + 1 EOS（`fbd_cnt==3`）；保持 `libOMX_Core.z.so` 纯 C11 且仅依赖 `[libc.so]`。
  2. **公共 AVCodecKit 硬件编解码闭环探针（[native_venc_probe.c](../compat/codec/native_venc_probe.c)）**：
     - 链接公共 NDK `libnative_media_venc.so` + `libnative_media_vdec.so`，端到端测试 `OMX.marble.video.encoder.avc` 与 `OMX.marble.video.encoder.hevc`（`640x480` NV12、4 帧、第 2 帧动态 `OH_MD_KEY_REQUEST_I_FRAME`、严格校验 `out_pkts==in_frames==4` 与 `keyframes>=2`），并将硬件编码出的 Annex-B 码流直灌 `OH_VideoDecoder`（`/dev/video32`）逐帧解码校验亮度均值误差 `y_diff<=20`。
- **已过验证层（离线 + COM9 无干预冷启动 `BOOT_ID=04fb8e77-0017-4f1d-8195-925fddf5b38e`）**：
  - 离线：`omx_venus_host_test`（含用例 #4）通过，[test_marble_venc_h6.py](../tests/test_marble_venc_h6.py) 3/3 通过，跨批次活跃回归测试共 **146/146** 项全绿（H6/H5/H4/H3/H2/G5/H1/G4–G1/F1–F9/U2–U4/Phase4）。
  - COM9 冷启动双轮全硬件编解码闭环验证（`boot_a=c83a8468…` 未动、`vendor_boot_a=c1cab391…`、`BAT=100`、`OMX_BIND=1`、`libOMX_Core.z.so=e134cf3b…`、`CODEC_HOST PID=4718`、`AVCODEC PID=6850`、`HDF_HOSTS=14`）：
    - **连续两轮 `native_venc_probe` 精确 4 进 4 出全绿（`RUN1_RC=0` & `RUN2_RC=0`）**：
      - **H.264 (`OMX.marble.video.encoder.avc`)**：`in_frames=4 out_pkts=4 keyframes=2 total_bytes=7584 nals=8 param=1 idr=1 annexb=1 eos=1 err=0 ok=1` → 硬件解码环回 `dec_frames=4 dec_bytes=1843200 mean_y=126 target_y=127 y_diff=1 eos=1 ok=1`；
      - **HEVC (`OMX.marble.video.encoder.hevc`)**：`in_frames=4 out_pkts=4 keyframes=2 total_bytes=6576 nals=10 param=1 idr=1 annexb=1 eos=1 err=0 ok=1` → 硬件解码环回 `dec_frames=4 dec_bytes=1843200 mean_y=127 target_y=127 y_diff=0 eos=1 ok=1`；
      - 两轮前后 `codec_host`（`4718`）与 `av_codec_service`（`6850`）PID 均恒定不变。
  - COM9 全子系统零回归与夜间静音安全：H5 MemoryTracker（`MT_RC=0 sa1909=1 graph=105816064 all_ok=1`）、H4 USB Port（`USB_RC=0 all_ok=1`）、H3 Power（`POWER_RC=0 sa3301=1 all_ok=1`）、H2 Thermal（`THERMAL_RC=0 zones=9 all_ok=1`）、H1 NFC（`NFC_RC=0 chip=0xa4 fw=16.104 all_ok=1`）、G5 四摄（`FWK_RC=0 metas_ok=2`、`CAM_RC=0 optical_cams=4 jpeg_bytes=32543 errors=0`）、`DCM`（`DCM_RC=0 token_len=236`）、`HEIF`（`HEIF_PROBE_ALL_OK cases=7`）、`VENUS=2`、`WLAN0=10.21.30.203/24`、`PAL_PCM=0`、`PERSIST=0`、`KGSL_FAULT=0`、`FATAL_MSS=0`、`OOPS_EXACT=0`、`CAM_ERR_CNT=0`。
- **边界与回退**：HarmonyOS 公共 AVCodecKit 硬件 H.264/HEVC 编码（含动态请求关键帧与精确 EOS）与解码环回均已通过冷启动双轮实测。回退 H5 `9a05fcb7` 使用 [flash-h20-h6-control.sh](../work/marble-dual-20261123/flash-h20-h6-control.sh) `restore <BOOT_ID>`。

## 批次 H7 HarmonyOS 原生 NFC 框架服务（`SA 1140 nfc_service` + Clean-Room `libnci_native_vendor.z.so`）与批次 F10（5G/5A 信号显示 + `QRTR 0x302` IMSDCM VoLTE/VoNR/IMS-SMS + QMI WMS 短信 + QMI VOICE 通话 + `SA 4005/4006`）合批实机闭环里程碑（2026-11-26 更新：`vendor_boot_h20_f10_5g_ims_call_sms_20723451.img`）

- **旧现象 → 固定输入 → 最小差异**：
  1. **5G(5A) 信号未显示**：① [libril_vendor_qmi.c](../compat/telephony/libril_vendor_qmi.c) 原解析 `QMI_NAS_GET_SYS_INFO (0x004d)` 时误读 `TLV 0x4e` 且反转 `dcnr = endc ? 1 : 0`，导致 `CoreService::UpdateNrState` 始终得到 `isDcNrRestricted=1`、`NrState=1(NOT_SUPPORT)`、`cfgTech=9(LTE)`；② `parse_nas_signal_info` 未过滤 QMI NR `-32768 (0x8000)` 哨兵值；③ 供体 `sys_prod/etc/param/hw_defaults.para` 设 `const.telephony.efl_forbidden=true` 与 `const.telephony.show_efl_mobile_type=false`，抑制状态栏与设置的 5G/5A 图标及开关。
  2. **电话与短信不可用**：① `libril_vendor_qmi.c` 此前将拨号/挂断/接听/拒接/短信发送全部占位拦截，且未绑定双卡槽子订阅（电信卡在 Slot 1）；② 缺少 `SA 4005`（`libtel_call_manager.z.so`，被 `foundation-app-core.json` 遗漏且被 `sceneboard-optional-sa.list` 拦截）与 `SA 4006`（`libtel_cellular_call.z.so`，被 `telephony-marble.json` 遗漏）；③ 中国电信 `46011` 为纯 PS 驻网（`ims_voice=1`），SM7475 MPSS 要求宿主侧发布 `QRTR 0x302 (QMI_SERVICE_IMSDCM)` 服务以建立独立 `IMS` PDN（`profile=2, mux_id=2`, IPv6）后才注册 VoLTE/VoNR/IMS-SMS。
  3. **基线与最小差异**：在已通过离线验收的 **H7 NFC 基线 `663bdf7f`**（[`build_marble_h20_h7_nfc_service.py`](../tools/packaging/build_marble_h20_h7_nfc_service.py)，将原生 `SA 1140 nfc_service` + `libnci_native_vendor.z.so` (`sn110u`) 叠加于 H6 `c1cab391`）上叠加打包 [`build_marble_h20_f10_5g_ims_call_sms.py`](../tools/packaging/build_marble_h20_f10_5g_ims_call_sms.py)：全部 84 个前序保护成员（含 `hdf_default_phase4.hcb`、`libhmos_audio_vdi.so`、`libhmos_pal_server.so`、H1–H7 NFC、G1–G5 相机、U1–U4、F1–F7）逐字节不变；仅更新 `graphics/libril_vendor_qmi.z.so`、`graphics/telephony-marble.json`（源 [telephony-f10-marble.json](../devices/marble/runtime/telephony/telephony-f10-marble.json)，保留 F2/F3 JSON 不动）、`graphics/foundation-app-core.json`（补 `SA 4005`）与 `graphics/stage-phase4-marble.sh`（移出 `sceneboard-optional-sa.list` 中的 `4005` 并原位更新已绑定挂载的 `u1-sysprod-hw-defaults.para` 为 `efl_forbidden=false` / `show_efl_mobile_type=true`，保持 inode 不变），`597→597` 条目；仅刷写 `vendor_boot_a`（`boot_a=c83a8468…` 未动），产出 `/mnt/e/marble-tmp/vendor_boot_h20_f10_5g_ims_call_sms_20723451.img`（`sha256=207234511e21e7c4656bc7cca93a38aca5a0c091a5986f91a429e8ba8db5b835`）。
- **新增实现**：
  1. **ABI 与 QMI 5G/SMS/Call/IMSDCM 实现（[hril_vendor_abi.h](../compat/telephony/hril_vendor_abi.h)、[libril_vendor_qmi.c](../compat/telephony/libril_vendor_qmi.c)）**：
     - 修正 `0x4f`（`endc_available`）/ `0x4e`（`dcnr_restriction`）映射与 NR `-32768` 过滤，实现 `SetNrOptionMode`、`Get/SetPreferredNetwork`（`QMI_NAS 0x0033/0x0034`）与 `GetPhysicalChannelConfig`（`QMI_NAS_GET_RF_BAND_INFO 0x0031`）。
     - 内置无绑定 `QRTR 0x302 (SVC_IMSDCM)` 服务端线程（按 `81voltd` 协议实现 `0x0020 START_CONN` 独立 `WDS mux_id=2` IPv6/IPv4 承载、`0x0021 STOP_CONN` 拆链 + `0x04 IND 0x0020 err_code=13` 及非阻塞两阶段 `IMSS 0x008f` VoLTE/VT/IMS/SMS 激活）。
     - 实机验证并接入双卡槽绑定 `QMI_WMS_BIND_SUBSCRIPTION (0x004c, u8=slot)` 与 `QMI_VOICE_BIND_SUBSCRIPTION (0x0044, u8=slot)`：实现真实 SMS 收发（`QMI_WMS_RAW_SEND/SEND_ACK/GET_SMSC/SET_SMSC` + `EF_SMSP 0x6F42` 回退 + 双槽 `QMI_WMS_EVENT_REPORT_IND` → `HNOTI_SMS_NEW_SMS=1101`）与真实通话控制（`QMI_VOICE` 拨号/挂断/接听/拒接/保持/恢复/切换/DTMF/通话列表 + 双槽 `QMI_VOICE_ALL_CALL_STATUS_IND` → `HNOTI_CALL_STATE_UPDATED=1001`，`voiceDomain=0` 适配 `SA 4006 CSControl`）。
  2. **系统能力与 5G/5A 显示参数（[telephony-f10-marble.json](../devices/marble/runtime/telephony/telephony-f10-marble.json)、[build_marble_h20_f10_5g_ims_call_sms.py](../tools/packaging/build_marble_h20_f10_5g_ims_call_sms.py)）**：启用 `SA 4005`（`libtel_call_manager.z.so`）与 `SA 4006`（`libtel_cellular_call.z.so`），放开 `const.telephony.efl_forbidden=false` 与 `const.telephony.show_efl_mobile_type=true`。
- **已过验证层（离线 + COM9 无干预冷启动 `BOOT_ID=ca2fb901-ba95-4618-8e5a-4fa7fb63076d`）**：
  - 离线：宿主 `ril_host_test`（含 ENDC/DCNR、NR 哨兵、EF_SMSP、WMS/VOICE 编解码、IMSDCM 0x0020 响应/指示、SetNrOptionMode/SendSmsAck）`-Wall -Wextra -Werror` 通过（`RIL_VENDOR_QMI_HOST_TEST_OK=1`）；[test_marble_modem_f10.py](../tests/test_marble_modem_f10.py) 7/7 + F3/F7/F8/F9/F10 26/26 全绿，且保持 `libril_vendor_qmi.z.so` `NEEDED=[libc.so]`、`SONAME=libril_vendor.z.so`。
  - COM9 冷启动实测（`boot_a=c83a8468…` 未动、`vendor_boot_a=20723451…`、`PERSIST=0`、`hdf_hosts=17`）：
    - **VoLTE / VoNR / IMS-SMS 全自动冷启动注册成功**：`QRTR_IMSDCM_0x302=1(1:16487)`、`RIL_FDS=16`（IMS WDS 承载常驻）、**`IMSA_SUB1 reg=2 rat=1 sms=2 voice=2 vt=2 ut=2`**（较初版 `02037951` 修复 `0x0021` 断链指示与两阶段 `IMSS` 激活后全部达 FULL_SERVICE）。
    - **5G(5A) 信号链就绪**：`const.telephony.efl_forbidden=false`、`const.telephony.show_efl_mobile_type=true`、`NAS_SUB1 lte_srv=2 endc=1 dcnr=0`，`CoreService` 实测上报 `NotifyStateChange finish slotId:1, regStatus:1, cfgTech:12`（NR 5G）。
    - **SMS 与 Call 协议栈就绪**：`WMS_SUB1 bind_err=0 smsc_err=0 smsc=+8613334262200`、`VOICE_SUB1 bind_err=0 calls_err=0`、`OPT_SA_HAS_4005=0`、`foundation` 已加载 `libtel_call_manager.z.so`（`FND_4005=4`）、`telephony-marble` 已加载 `libtel_cellular_call.z.so`（`TEL_4006=4`）。
    - **H7 NFC 冷启动闭环**：`nfc_service`（`pid=12336`）与 `nfc_host` 常驻，`MARBLE_NFC_FWK_PROBE_SUMMARY sa1140=1 avail=1 state=1 state_name=OFF chip=sn110u nfcc=1 ce=1 tag=1 ext=1 proxy=1 all_ok=1`。
    - **零回归**：`wlan0`（`10.21.30.203/24`）、`rmnet_data0` (`UP,LOWER_UP`)、`camera_host`/`location_host`/`thermal_host`/`power_host`/`usb_host`/`foundation`/`sceneboard` 均正常，无新增崩溃，`libhmos_audio_vdi.so`/`libhmos_pal_server.so`（`0.03f` 安全增益）逐字节未动。
- **诚实边界与回退**：
  - 本批在零人工干扰下已完成 5G `cfgTech=12`、VoLTE/VoNR/IMS-SMS 满服务注册（`reg=2 sms=2 voice=2 vt=2 ut=2`）、SMSC 与双槽 QMI 绑定及 `SA 4005/4006/1140` 实机冷启动验证；出于不擅自向外网号码拨打/发信约束，真实通话接通后的听筒/麦克风 ADSP 通话语音通路（`PAL_STREAM_VOICE_CALL`）与实际拨打 `10001`/收发短信留待用户实测反馈，如通话接通后无声则下一批在不触碰媒体 `0.03f` 增益前提下单独补接 `PAL_STREAM_VOICE_CALL`。
  - 回退 H6 `c1cab391` 使用 [flash-h20-f10-control.sh](../work/marble-dual-20261123/flash-h20-f10-control.sh) `restore <BOOT_ID>`。

## 批次 F11 通话 ADSP 语音通路（`PAL_STREAM_VOICE_CALL` + 听筒/免提/蓝牙 SCO 路由 + 白天扬声器音量恢复）里程碑（2026-11-26 更新：`vendor_boot_h20_f11_voice_audio_00d7c1a6.img`）

- **旧现象 → 固定输入 → 最小差异**：
  1. **通话接通后缺少 ADSP 语音通路与听筒/免提/SCO 路由**：[audio_vdi.c](../compat/audio/audio_vdi.c) 原将 `UpdateAudioRoute`、`ReleaseAudioRoute`、`SetVoiceVolume`、`SetExtraParams` 及 donor 6.1 扩展槽 `donorExtensions[5]`（`offset 168`，反汇编 `libaudio_primary_impl_vendor.z.so::AudioSetPhoneCallSceneVdi` 与 `libhdiadapter_new.z.so::AudioRenderSink::SetAudioScene` 确认）全部占位为 `-ENOTSUP`，且 `CreateRender` / `SelectScene` 仅接受 `PIN_VDI_OUT_SPEAKER`；[pal_server.c](../compat/audio/pal_server.c) 尚未接入 `PAL_STREAM_VOICE_CALL` 与 `pal_stream_set_device`。
  2. **扬声器音量过小**：此前为夜间静音在 `audio_vdi.c` 与 `pal_server.c` 硬编码 `0.03f`（`-30.46 dB`），叠加在 HarmonyOS `audio_server` 自身软件音量曲线（`audio_volume_config.xml`，`-27..0 dB`）之上导致白天外放极小声。
  3. **关键 ABI 核查与最小差异**：反汇编实机 `libar-pal.so`（`StreamPCM::StreamPCM@0x19ad1c` 与 `Stream::switchDevice@0x1921e8`）证实 SM7475 原厂 `sizeof(struct pal_device) == 220 B`（无尾部 `custom_config[256]`，区别于开源头 476 B），`sizeof(struct pal_stream_attributes) == 224 B`；同时确认设备运行态 `const.multimedia.audio.has_earpiece=true` 已开启，无需改动系统 `.para`。在 accepted **F10+H7 `20723451`** 上叠加打包 [`build_marble_h20_f11_voice_audio.py`](../tools/packaging/build_marble_h20_f11_voice_audio.py)：全部 85 个非音频保护成员（含 `libril_vendor_qmi.z.so`、`telephony-marble.json`、`foundation-app-core.json`、`hdf_default_phase4.hcb`、H1–H7 NFC/Thermal/Power/USB/MemTracker/VENC、G1–G5 相机、U1–U4、F1–F7）逐字节不变；仅替换 `graphics/libhmos_audio_vdi.so` 与 `graphics/audio-runtime/lib64/libhmos_pal_server.so`，新增 `graphics/native_voice_audio_probe` 并通过 `stage-phase4-marble.sh` 复制至 `/compat/bin/native_voice_audio_probe`（`597→598` 条目）；仅刷写 `vendor_boot_a`（`boot_a=c83a8468…` 未动），产出 `/mnt/e/marble-tmp/vendor_boot_h20_f11_voice_audio_00d7c1a6.img`（`sha256=00d7c1a6b10e8511dac820990bd99bda809354eea67c3376b477570d92d8fbec`）。
- **新增实现**：
  1. **协议与 Bionic PAL 通话/音量后端（[protocol.h](../compat/audio/protocol.h)、[pal_server.c](../compat/audio/pal_server.c)、[pal_capture_backend.inc](../compat/audio/pal_capture_backend.inc)、[pal_capture_worker.inc](../compat/audio/pal_capture_worker.inc)）**：
     - 保持 24 B 定长线协议完全向后兼容，新增 `HMOS_PAL_SET_DEVICE` 与 `HMOS_PAL_VOICE_{OPEN,START,STOP,CLOSE,SET_DEVICE,VOLUME,MUTE,STATUS}`，修正采集路由分流区间 `HMOS_PAL_CAPTURE_OPEN..HMOS_PAL_MIC_GET_MUTE`。
     - 以 `_Static_assert` 锁定 `struct pal_device_v220 == 220 B` 与 `struct pal_stream_attributes == 224 B`，实现双设备（RX+TX）`PAL_STREAM_VOICE_CALL`（`PAL_AUDIO_INPUT_OUTPUT`、默认 Slot 1 `VOICEMMODE2=0x11DC5000` 并支持 Slot 0 `VOICEMMODE1=0x11C05000`、`HANDSET+HANDSET_MIC` ↔ `SPEAKER+SPEAKER_MIC` ↔ `BLUETOOTH_SCO+BLUETOOTH_SCO_HEADSET` 原位 `pal_stream_set_device` 热切与失败自动重开回退、`pal_stream_set_volume` 通道掩码适配及客户端异常断开自动释放 ADSP 语音图）。
     - 默认外放硬件增益改为 `HMOS_AUDIO_DEFAULT_VOL = 1.0f`（由系统软件音量曲线正常控音），同时保留 `HMOS_AUDIO_QUIET_VOL = 0.03f`（创建 `/data/local/tmp/marble-audio-quiet` 即可随时切回 `-30.46 dB` 夜间静音档）。
  2. **HarmonyOS VDI 路由/通话场景/采集扩展与探针（[audio_vdi.c](../compat/audio/audio_vdi.c)、[audio_capture_vdi.inc](../compat/audio/audio_capture_vdi.inc)、[native_voice_audio_probe.c](../compat/audio/native_voice_audio_probe.c)）**：
     - 实现 `UpdateAudioRoute`、`ReleaseAudioRoute`、`SetVoiceVolume`、`SetExtraParams`（支持 `sim_slot=0/1`、`vsid=...`、`call_state=...`）以及 donor 6.1 `donorExtensions[5]`（`offset 168` `SetPhoneCallScene`）；`CreateRender` / `SelectScene` 支持 `PIN_VDI_OUT_SPEAKER`、`PIN_VDI_OUT_EARPIECE`、`PIN_VDI_OUT_BLUETOOTH_SCO` 与 `AUDIO_VDI_IN_CALL`；采集端同步放开 `AUDIO_VDI_IN_COMMUNICATION`/`AUDIO_VDI_IN_CALL` 与 `PIN_VDI_IN_BLUETOOTH_SCO_HEADSET`。
- **已过验证层（离线 + COM9 无干预冷启动 `BOOT_ID=ddebaa7a-04a6-4243-accb-b33a3763c780`）**：
  - 离线：宿主 mock `SOCK_SEQPACKET` PAL 端到端契约测试 + F11/F10/F9/F3/U1/H6 定向回归共 **30/30** 全绿（[test_marble_audio_f11.py](../tests/test_marble_audio_f11.py) 含白天 `1.0f` 默认音量、听筒/免提/BT SCO 创建与热切、`SetPhoneCallScene`、`SetVoiceVolume`、`VOICEMMODE1/2`、`SelectScene`、VoIP/SCO 采集与 85 项镜像守卫）。
  - COM9 冷启动实测（`boot_a=c83a8468…` 未动、`vendor_boot_a=00d7c1a6…`、`PERSIST=0`、`HDF_HOSTS=17`、`NEW CRASHES=0`）：
    - **PAL 服务与白天音量恢复**：`PAL_SERVER_READY=1 pid=6724 socket=@hmos.pal.pcm.v1 voice_api=1 default_vol=1.00`，`audio_server`（`pid=6810`）冷启动开启主扬声器 `PAL_SERVER_OPEN dev=3 vol=1.0000`，双 Awinic SmartPA（`2-0034`/`2-0035`）正常加载。
    - **真实 SM7475 ADSP 通话语音通路与听筒↔免提热切闭环**：`/compat/bin/native_voice_audio_probe /compat/lib64/libhmos_audio_vdi.so` 返回 `VOICE_PROBE_RC=0`：
      - `VOICE_PROBE_RENDER spk_rc=0 spk_vol=1.0000 want_vol=1.00 vol_ok=1 ear_rc=0`
      - `PAL_SERVER_VOICE_OPEN rx=2(HANDSET) tx=23(HANDSET_MIC) vsid=0x11dc5000 rc=0` → `PAL_SERVER_VOICE_START ... rc=0` → `VOICE_PROBE_MMODE2 call_on_rc=0 state=2 rx=2 tx=23 vsid=0x11dc5000 ok=1`
      - `VOICE_PROBE_VOL rc=0`（`PAL_SERVER_VOICE_VOL vol=0.6000 mask=1/3 rc=0`）
      - `PAL_SERVER_VOICE_DEVICE rx=3(SPEAKER) tx=24(SPEAKER_MIC) mode=switch rc=0` → `PAL_SERVER_VOICE_DEVICE rx=2(HANDSET) tx=23(HANDSET_MIC) mode=switch rc=0`（原位 `pal_stream_set_device` 热切零重开！）
      - `VOICE_PROBE_MMODE1 extra_rc=0 on_rc=0 off_rc=0 vsid=0x11c05000 ok=1`（双卡槽 `VOICEMMODE1` & `VOICEMMODE2` 均实测通过）
      - `VOICE_PROBE_SCENE call_rc=0(state=2) media_rc=0(state=0) ok=1`、`MARBLE_VOICE_AUDIO_PROBE_SUMMARY ... all_ok=1`
    - **全系统零回归**：SceneBoard 实测 5G-A 状态上报 `SignalStatus-SignalModel: slotId: 1, regState value: 1, cfgTech value: 13`（`const.telephony.efl_forbidden=false`、`const.multimedia.audio.has_earpiece=true`）、`QRTR_REMOTE_COUNT=9`、`riladapter_host`/`telephony-marble`/`hmos_rmtfs` 常驻；H7 NFC `MARBLE_NFC_FWK_PROBE_SUMMARY sa1140=1 ... all_ok=1`；`wlan0`（`10.21.30.203/24`）在线；无新增崩溃。
- **诚实边界与回退**：
  - 本批已在真机闭环验证 SM7475 ADSP `PAL_STREAM_VOICE_CALL`（`VOICEMMODE1` / `VOICEMMODE2`）、听筒↔免提原位热切、通话音量、白天扬声器 `1.0f` 增益与 HarmonyOS VDI 全套通话路由接口；真实外网拨打/接听语音听感与蓝牙 SCO 耳机通话需用户使用真实号码与蓝牙耳机实测验证。
  - 回退 F10 `20723451` 使用 [flash-h20-f11-control.sh](../work/marble-dual-20261123/flash-h20-f11-control.sh) `restore <BOOT_ID>`（或 `restore-fastboot`）。

## 批次 F12 电话呼出/挂断/短信发送与状态栏信号图标四项实机修复里程碑（2026-11-26 更新：`vendor_boot_h20_f12_tel_fixes_7af88ce0.img`）

- **旧现象 → 固定输入 → 最小差异**：
  1. **状态栏完全不显示信号图标与运营商标签**：反编译实机 `SceneBoard.hap`（`modules.abc`）与 `libtel_core_service_api.z.so` 确认根因：`SignalIcon::aboutToAppear` 与 `SignalOperatorInfo::aboutToAppear` 在桌面首帧仅调用一次 `sim.getMaxSimCount()` 并缓存至 `this.simCount`；此前 `telephony_sa` 晚于 `SceneBoard` 启动，导致 `CoreServiceClient::GetMaxSimCount()` 返回 `0`，`SignalIcon` 渲染分支直接 `returnundefined`（`StatusBarLayoutVm` 录得 `actual: 0.00 0.00`、`status_bar_operator width -1 invlid`）。
  2. **能接电话不能拨出（MO Dial）**：`QMI_VOICE_DIAL_CALL` 仅带 `TLV 0x01` 未传 `TLV 0x10`（`CALL_TYPE_VOICE_IP=0x08`），在纯 VoLTE/VoNR 电信 46011 网络上被调制解调器拒呼；同时 NR SA 下未解析 `QMI_NAS_GET_SYS_INFO` `TLV 0x56`（`nr5g_ims_voice_avail`），且缺少独立的 `SA 4014`（`ImsCoreService`）将真实 `QMI_IMSA (0x21)` 注册状态同步给 `CoreService`。
  3. **挂断后通话界面仍卡在“通话中”**：`HRilCallReq` 缺槽位 12 `CallSupplement`（`offset 12*8`）与槽位 34 `GetCallFailReason`（`offset 34*8`）；同时挂断回调在 SIP BYE 尚未落盘时 0ms 过早推送 `HNOTI_CALL_STATE_UPDATED`（或缺少 `QMI_VOICE_ALL_CALL_STATUS_IND` 挂断后轮询兜底），导致 `CellularCall` 状态机未收到终态断开事件与结束原因码。
  4. **能收短信不能发短信（MO SMS）**：中国电信 46011 SIM 卡下框架走 `SendCdmaSms` 入口，而 `libril_vendor_qmi.c` 此前仅实现 GSM `SendGsmSms`、未接 `SendCdmaSms` 且未处理 VoLTE/IMS 下 3GPP2 C.S0015 到 3GPP TS 23.040 GSM `SMS-SUBMIT`（`format=0x06` `sms_on_ims=1`）转码。
  5. **最小差异打包**：在 accepted **F11 `00d7c1a6`** 上叠加 [`build_marble_h20_f12_tel_fixes.py`](../tools/packaging/build_marble_h20_f12_tel_fixes.py)，全部 85 个非 F12 保护成员（含 F11 音频 VDI/PAL/探针、`telephony-marble.json`、`foundation-app-core.json`、`hdf_default_phase4.hcb`、H1–H7、G1–G5、U1–U4、F1–F7）逐字节不变；新增 `telephony/hmos_ims_core_service`（19 KB 独立 `SA 4014` 进程）与 `telephony/init-modem-f12-marble.cfg`，替换 `telephony/libril_vendor_qmi.z.so`，微调 `hmos-init-debug.cfg`、`stage-phase4-marble.sh` 与 `start-phase4-marble.sh`（在 `Foundation`/`SceneBoard` 之前预启动 `hmos_ims_core_service` + `telephony_sa` 使 `simCount=2`，待 `F7_TELEPHONY_READY=1` 后对 `telephony_sa` 执行一次两阶段干净重拉以让 `CellularData` 正确加载 APN 与 DataShare）；仅刷写 `vendor_boot_a`（`boot_a=c83a8468…` 未动），产出 `/mnt/e/marble-tmp/vendor_boot_h20_f12_tel_fixes_7af88ce0.img`（`sha256=7af88ce035cb72d97b7801140ff8960859cdddad98ebd22a3db80098a9094de6`，`598→600` 条目）。
- **新增与修改实现**：
  1. **独立净室 `SA 4014`（`ImsCoreService`）QMI_IMSA 桥接守护进程（[ims_core_wire.h](../compat/telephony/ims_core_wire.h)、[hmos_ims_core_service.cpp](../compat/telephony/hmos_ims_core_service.cpp)）**：直连高通 `QMI_IMSA (0x21)` 解析 `IMSA_REG_STATUS` / `IMSA_SRV_STATUS` TLV（空卡槽 0 返回 `reg_err=0x46` 时仅视为 `reg=0` 不破坏 QRTR 句柄），通过 `ImsCoreServiceCallback` 按序先推 8 字节 `ImsServiceStatus`（`cb_cmd=0`）再推 `ImsRegistrationStatus`（`cb_cmd=1`）；`GetProxyObjectPtr`（`code=101`）返回空对象以保持通话/短信继续直走本机 QMI RIL，支持 `--probe` 只读诊断模式。
  2. **QMI RIL 呼出/挂断/短信增强（[hril_vendor_abi.h](../compat/telephony/hril_vendor_abi.h)、[libril_vendor_qmi.c](../compat/telephony/libril_vendor_qmi.c)）**：
     - 补齐 `HRilCallReq` 槽位 12 `CallSupplement` 与槽位 34 `GetCallFailReason`（`_Static_assert` 锁定结构体 43×8 字节 ABI）；实现 `v_CallSupplement`（`QMI_VOICE_MANAGE_CALLS` + 活跃呼叫 `END_CALL` 兜底）与 `v_GetCallFailReason`（解析 `QMI_VOICE_ALL_CALL_STATUS_IND` `TLV 0x16` 挂断原因，默认 `16=NORMAL_CLEARING`）；移除 `TASK_CALL_HANGUP` 0ms 抢跑上报并在 `ind_event_thread` 加入 350ms 终态轮询看门狗。
     - `build_voice_dial_tlvs` 在 VoLTE/VoNR 网络下优先携带 `TLV 0x10=0x08`（`CALL_TYPE_VOICE_IP`），失败依次回退 `0x00` 与裸 `TLV 0x01`；`parse_nas_sys_info` 新增 NR SA `TLV 0x56`（`nr5g_ims_voice_avail`）解析并联动 `g_ims_voice_avail`/`g_ims_registered` 提升 CS 注册状态；`poll_nas_delta_tick` 在晚连场景下同步补发 `HNOTI_SIM_STATUS_UPDATED`。
     - 接入 `v_SendCdmaSms` 与纯 C 3GPP2 C.S0015 → 3GPP TS 23.040 GSM `SMS-SUBMIT` 转码器（支持 4-bit DTMF / 8-bit ASCII 目的号码、ASCII 7-bit `0x02`、中文 UTF-16BE `0x04`、Latin `0x08`、GSM7 `0x09` 及多段 UDH），在 IMS SMS 就绪时优先走 `format=0x06`（`sms_on_ims=1`→`0`），失败回退原生 `format=0x00`。
  3. **两阶段启动编排与构建测试（[init-modem-f12-marble.cfg](../devices/marble/runtime/telephony/init-modem-f12-marble.cfg)、[build_marble_h20_f12_tel_fixes.py](../tools/packaging/build_marble_h20_f12_tel_fixes.py)、[test_marble_modem_f12.py](../tests/test_marble_modem_f12.py)、[flash-h20-f12-control.sh](../work/marble-dual-20261123/flash-h20-f12-control.sh)）**。
- **已过验证层（离线 + COM9 无干预冷启动 `BOOT_ID=a8eb5509-073e-493f-b76a-248e040b6eca`）**：
  - 离线：`HRIL_VENDOR_HOST_TEST` 第 10 节（`-std=c11 -Wall -Wextra -Werror`，`RIL_VENDOR_QMI_HOST_TEST_OK=1`）+ F12/F11/F10/F9/F3/U1/H6 定向回归共 **36/36** 全绿。
  - COM9 冷启动实测（`boot_a=c83a8468…` 未动、`vendor_boot_a=7af88ce0…`、`PERSIST=0`、`RMTFS=10728`、`FAULTLOGS=17` 零新增崩溃）：
    - **启动时序与 8 个核心 SA 全就绪**：`IMS=7741`（先于 `SCB=8137`）→ `RIL=11357` → `TEL=11923`（在 RIL/DataShare 就绪后干净重拉）；SA `4014`、`4010`、`4005`、`4006`、`4007`、`4008`、`66250`、`1140` 全部在线。
    - **SA 4014 QMI_IMSA 真实回传**：`HMOS_IMS_CORE IMSA_PUSH slot=1 reg=1 voice=1 video=1 sms=1 tech=1 rc0=0 rc1=0`（`--probe` 实测 `slot=0 reg_err=0x46 reg=0`，`slot=1 reg=1 voice=1 sms=1 tech=1`），实机 `marble_modem_probe --sub 1 --nas-ext` 确认 5G NR SA `TLV 0x4a=020000` 与 `TLV 0x56=01000000`。
    - **状态栏信号图标实机渲染闭环**：`SignalComponent-SignalIcon --> refreshCelluarDisplay: 1, 13, false, false, 4, 125836478`（Slot 1、`cfgTech=13` 5A、4 格满信号），`StatusBarLayoutVm: signalCanUseSpace 100 actual: 24.68 22.91`（此前 F11 为 `0.00 0.00`），且 `status_bar_operator` 不再报 `width -1 invlid`。
    - **CellularData 与零回归**：`telephony_sa` 两阶段重拉后零 `matchedApns is empty` 报错；F11 语音通路 `MARBLE_VOICE_AUDIO_PROBE_SUMMARY ... all_ok=1`；H7 NFC `MARBLE_NFC_FWK_PROBE_SUMMARY ... all_ok=1`；`wlan0` 在线。
- **诚实边界与回退**：
  - 状态栏信号图标渲染、`SA 4014` QMI_IMSA 推送、5G NR SA `TLV 0x56` 识别、CellularData 洁净初始化及 F11/H7 回归均已在真机冷启动验证；出于不擅自向外网号码拨打/发短信的安全原则，**真实外网呼出（MO Dial）、挂断界面退回与真实短信发送（MO SMS）** 留待用户亲自拨打测试号码与互发短信最终验收。
  - 回退 F11 `00d7c1a6` 使用 [flash-h20-f12-control.sh](../work/marble-dual-20261123/flash-h20-f12-control.sh) `restore <BOOT_ID>`（或 `restore-fastboot`）。

## 批次 F13–M3 全功能分批闭环里程碑（2026-11-26 更新：`vendor_boot_h20_m3_codec_pin_anco_f54237e5.img`）

- **批次 F13（`vendor_boot_h20_f13_tel_loc_cutout_41368e8d.img`）**：
  - 修复状态栏挖孔对齐与状态栏高度（[display_manager_config.xml](../devices/marble/runtime/init/display_manager_config.xml)：前置挖孔 `x=508, y=18, w=65, h=67` 对应实测上 Y=18 / 下 Y=85 / 左 X=508 / 右 X=573）。
  - 修复 GNSS `TLV 0x23` 非卫星低精度基站定位（`tech=0x2`）误报过滤（[marble_gnss_hdi_driver.cpp](../compat/location/marble_gnss_hdi_driver.cpp)）；修复蜂窝数据接口名对齐 OHOS `NetsysBpfStats` 白名单（`rmnet0`）以恢复状态栏网速统计。
- **批次 M1（`vendor_boot_h20_m1_tel_vib_disp_4f8809cb.img`，`BOOT_ID=30ebfe13-fffa-42c5-b151-64490f3c93e3` 冷启动通过）**：
  - **根治 `rmnet0` 偶发 `0 RX` 与流量断流竞态**：在 [marble_modem_probe.c](../compat/telephony/marble_modem_probe.c) `--dpm-open` 与 [libril_vendor_qmi.c](../compat/telephony/libril_vendor_qmi.c) `imsdcm_open_wds` 双层锁定 `QMI_WDA_SET_DATA_FORMAT(llp=2, ul=5, dl=9)`，彻底消除 IMS PDN 抢跑锁死 `ul=0, dl=0` 导致的 QMAPv5 丢包；实机 `ping -I rmnet0 223.5.5.5` 3/3 0% 丢包，实时收发字节正常增长。
  - **线性马达震动（Vibrator HDI v2.0）**：新增 `vibrator_host`（HDF host-15）与净室 [libmarble_vibrator_interface_service_2.0.z.so](../compat/vibrator/) 驱动 `qcom-hv-haptics`，并在 [sensors-marble.json](../devices/marble/runtime/sensors/sensors-marble.json) 恢复 `SA 3602`（`libmiscdevice_service.z.so`）；实机探针 `MARBLE_VIBRATOR_HDI_PROBE_SUMMARY sa3602=1 hdi_ver=2.0 ... all_ok=1`。
  - **色彩调节/色温、护眼模式、熄屏显示（AOD）与双击亮屏（DT2W）**：在 [composer_vdi.cpp](../compat/display/composer_vdi.cpp) 接入 SM7475 `SDE_DSPP_PCC_V4` 144 字节硬件矩阵与 LP1/LP2 AOD 状态切换，在 [displayengine_module.cpp](../compat/display/displayengine_module.cpp) 桥接 `DE_FEATURE_XCC`/`DE_FEATURE_GMP`，新增 [marble_touch_wakeup.c](../compat/touch/marble_touch_wakeup.c) 监听 `xiaomi_touch` `POLLPRI` 并通过 `/dev/uinput` 注入 `KEY_WAKEUP`。
- **批次 M2（`vendor_boot_h20_m2_cam_ims_softap_chg_531b5ebd.img`，`BOOT_ID=d68498a9-bbb9-422c-96d8-4f851647aa2b` 冷启动通过）**：
  - **桌面动画偶发卡顿根治**：在 [displayengine_module.cpp](../compat/display/displayengine_module.cpp) 过滤 `GMP subType=0xa1` 场景元数据提示并对 `disp_param` 写操作去重，同时在 [composer_vdi.cpp](../compat/display/composer_vdi.cpp) 对相同矩阵去重并移除 `-EBUSY` 下的阻塞式 `drmModeAtomicCommit(..., 0, nullptr)`，改为延后至下一帧 `PresentDisplay` 原子提交。
  - **相机 HDI V1_2 与手电筒（Torch）**：新增 [libmarble_camera_host_service_1.2.z.so](../compat/camera/) 与 V1_2 `SetFlashlight_V1_2` / `OH_TORCH` 适配，补齐 `NORMAL_PHOTO`/`NORMAL_VIDEO` 模式能力；实机 `native_camera_hdi_probe`（`host_ver=1.2 torch_v12_ok=1 cam_count=4 jpeg_bytes=31954 optical_cams=4`）与 `ohcamera_fwk_probe`（`OH_TORCH_TOGGLE on_rc=0 off_rc=0 ok=1 photo_mode_ok=2 video_mode_ok=2`）全绿。
  - **快充 UI 适配与小米原厂温控（`mi_thermald`）**：升级 [battery_charge_adapter.cpp](../compat/battery/battery_charge_adapter.cpp) 与 `native_battery_hdi_publisher_v131`，集成 `mi_thermal_interface.ko` + 32 份 marble 温控配置 + [native_thermal_launcher_marble.c](../compat/power/thermal/native_thermal_launcher_marble.c)；实机 `mi_thermal=insmod_ok sysfs_msg=1`，`mi_thermald` 常驻并读取实时板温。
  - **个人热点（Wi-Fi SoftAP `hostapd_host` host-16）**：在 HCB 增加第 17 个 host `hostapd_host`（`-i 16`），开启 `net.ipv4.ip_forward=1`，初始化 `nl80211` `hostapd.conf`；实机 `hostapd_host` 常驻且 `wlan0` STA 零回归。
- **批次 M3（`vendor_boot_h20_m3_codec_pin_anco_f54237e5.img`，`BOOT_ID=5831882c-d62c-4c0d-96de-a33e1a8fd725` 冷启动通过）**：
  - **电信 VoLTE/VoNR IMS 短信接口闭环（`OHOS.Telephony.ImsSmsInterface`）**：在 [ims_core_wire.h](../compat/telephony/ims_core_wire.h) 与 [hmos_ims_core_service.cpp](../compat/telephony/hmos_ims_core_service.cpp) 依据 `libtel_sms_mms.z.so` / `libtel_cellular_call.z.so` 二进制实证修正 `IMS_PROXY_TYPE_CALL=0` / `IMS_PROXY_TYPE_SMS=1`，实现 `ImsSmsInterface`（`6000/6100/6101/6500`）直连 `QMI_WMS_RAW_SEND`；实机确认 `IMSA_PUSH slot=1 reg=1 voice=1 video=1 sms_wire=1(mpss=1)`、`GET_PROXY type=1(IMS_SMS) -> ImsSmsStub ok=1`、`IMS_SMS_REG_CB ok=1`、`GET_PROXY type=0 -> nullptr`。
  - **Venus 音视频硬解码增强（[libOMX_Core.z.so](../compat/codec/)）**：补齐解码器 `DYNAMIC_HANDLE`、Seek 后 CSD 保留、OMX 颜色格式归一化及错误上报；实机 `native_venc_probe` H.264/HEVC 编解码环回 `all_ok=1`（`y_diff<=1`）。
  - **锁屏密码（UserIAM `adapter41` + 纯用户态 `pin_auth_host` host-17 + `SA 941`）**：HCB 扩展至 18 个 host（`pin_auth_host` `-i 17`），新增纯用户态 OpenSSL `pin_auth` v3.0 实现并在 [useriam-core-marble.json](../devices/marble/runtime/pinauth/useriam-core-marble.json) 启用 `SA 941`，同时严格保持 `libhmos_ram_no_tee.so` 不变（EL2 userdata 不加密，防止重启丢数据）；实机 `pin_auth_host`、`hmos_userauth_host`、`useriam` 及 `SA 941/901/921/931` 全部在线，零新增故障（`FAULTS=17`）。
  - **卓易通容器（DroiTong ANCO/RGM 运行时）**：挂载 `/dev/binderfs` 与 `/mnt/isula/var`，拉起 `rgm_manager`、`rgm_engine_plugin`、`virt_device`、`hmos_fusion_manager`、`anco_audio` 全部 5 个进程（诚实边界：实机原盘与 donor 均不含华为签发的 `agi.img`/`anco_hmos.img` 容器镜像，卓易通检测异常进一步诊断中）。
  - **诚实硬件边界**：指纹传感器（`fpc1540`/`goodix_3626`）受限于 QUPv3 SPI 归属 QSEE TrustZone 且缺小米签名 TA 与 Bionic HAL，保持透明边界不作伪装；回退 M2 `531b5ebd` 或 M1 `4f8809cb` 使用 [flash-h20-m3-control.sh](../work/marble-dual-20261123/flash-h20-m3-control.sh)。

<a id="marble-store-attestation"></a>
## 应用商店与主题商店：设备证明核查（2026-11-25）

- **应用商店，U1.6 历史采样**：AppGallery 的 `hiappevent` 分别记录了 `DeviceSecurityKit::getDeviceToken=-10002` 和多次 `AccountKit::AuthorizationWithHuaweiID=0`。二者来自不同进程，不能合并为一次点击的结果。`AppGalleryKit::registerSource` 在线程采样中有过失败，稍后的记录也有成功；单个 SDK 的返回不足以定位全部商店业务错误。[应用商店事件](../work/marble-dual-20261123/u16-diag-fb4-1795598872.serial.txt#L2-L15) · [稍后采样](../work/marble-dual-20261123/u16-diag-fb3-1795598802.serial.txt#L4-L20)。
- **主题商店，U1.6 历史采样**：ThemeManager 同一 PID 先记到 `AuthorizationWithHuaweiID=0` 和 `RemoteCommunicationKit::fetch=0`，不到一秒后两次 `getDeviceToken=-10002`。这证明当时 SDK 授权、部分请求成功及本地取令牌失败同时存在；`fetch=0` 不等于主题业务通过，也未取得和提示同次的服务端业务码。内置主题资源的加载、应用、恢复与云端主题目录分开验收。[同进程事件](../work/marble-dual-20261123/u16-hiappevent-1795598647.serial.txt#L51-L55)。
- **当前 U2.1 真机只读复查**：COM9 读回 `vendor_boot_a=974b7b75…`、boot `9c718080-9b1b-4109-9543-a0a58048ac60`、WLAN carrier `1`。原 `dcm_service` profile、服务库及 `libteec_vendor` 存在，但未见 `teecd` 进程、其系统 init 配置或 `/dev/tc_ns_client`，检查的两处原设备证书 HDI 实现均缺席。应用商店服务进程仍在，`65866` 仍在缺席时快速返回的可选表。U1 构建明确从移植运行时遮蔽了该 HDI；早期实际拉起过 `dcm_service`，其 `libteec_vendor` 曾报连接拒绝及初始化失败。只恢复一个文件、取消可选项或观察到 SA 注册，均不能证明设备认证已经可用。[U2.1 现场回执](../work/marble-dual-20261123/marble-stores-readonly-1795603419.serial.txt#L2-L24) · [早期原服务日志](../work/marble-dual-20261123/u11-apps.serial.txt#L61-L71) · [构建侧门控](../tools/packaging/build_marble_h20_u1_ux_security.py#L58-L105)。
- **验收边界**：U2.1 本次未重新触发两家店的操作；当日两个 EL2 日志末尾在按运行时长估算的开机窗口内没有新增上述三种样本。没有采集令牌、账号数据或响应正文，没有改变服务、用户数据和分区。序列号是标识符，不是受信的设备证明；若商店业务要求华为签发或信任的设备凭证，须走官方支持的设备与凭证路径。下次复测须按应用和点击时间分别记录 SDK 错误与业务结果，并用目录、下载/安装或主题应用的实际结果验收，不能只看返回值。本地签名应用的安装及本地主题应用/还原另验。[本次事件范围](../work/marble-dual-20261123/marble-stores-events-1795603568.serial.txt#L2-L8)。

## 交付规则

每项记录：旧现象 → 固定输入 → 最小差异 → 已过验证层 → 新版实际结果 → 未过门槛 → 回退。未定位的问题保持开放，不将日志告警归零冒充功能修好。上机批次必须复查桌面、数据库、WLAN 和 COM9，出现基础回归优先恢复。
