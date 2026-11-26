# Xiaomi marble（Redmi Note 12 Turbo / POCO F5）交接文档 / Handoff（2026-11-24）

面向下一个接手会话。只记录**已实机验证的事实**、**当前装机状态**、**避坑清单**与**可执行下一步**。

> **当前总目标已扩展**：用户新增蓝牙、音视频解码、基带/SIM/信号/电话/短信、相机、应用启动与菜单停顿、主题、开机动画、定位、麦克风、动画卡顿、序列号及其它实际问题，要求分批适配。此前仅对齐 sheng v146 的限制已被新任务取代；实际完成程度仍按证据逐项标注。见[分批计划与验收门槛](MARBLE_BATCH_PLAN.md)。
> **最新刷写授权（2026-11-24）**：boot_a 与 vendor_boot_a 均可按适配需要修改和刷写，旧 boot_a 固定约束已撤销；保留用户数据及其它原厂底层分区。不再逐轮大量留存整镜，仅保留当前部署候选、最少恢复基线与构建/测试依赖的唯一输入。USB 引导可改，COM9 重枚举前安排可恢复访问。见[授权与工件规则](MARBLE_BATCH_PLAN.md#目标与边界)。

> **最新现场优先于下方历史记录（2026-11-25 更新 — 批次 F4 已闭环，下一步批次 F5/G）**：
> - **当前 A 槽装机（批次 F4 — 修复 `hmos_tqftpserv` MCFG/虚拟路径 + 解锁基带 `DMS_CAP` 5 制式与 `op_mode=0(online)` + `vsimModemCount=0` + 扩展 RF/PDC/LOC 探针）**：**`vendor_boot_a=7d92e2a0b438e9b630391523cea2e952e5423ba31d2a163f82462fc9b1f77530`**（基于 F3 `e4043df4`，[F4 构建脚本](../tools/packaging/build_marble_h20_f4_rf_mcfg.py)，7 组离线单测 `60` 项 `OK (skipped=1)` + `TQFTPSERV_SELF_TEST_OK=1` + `MODEM_PROBE_SELF_TEST_OK=1` + `GNSS_HDI_HOST_TEST_OK=1`，10 项保护成员含 `libhmos_audio_vdi.so`/`libhmos_pal_server.so` 逐字节不变），`boot_a=c83a8468955ca6584df29b6e5b7cba27c4b27ae5563a43401566bad301c9cae8` 未动，冷启动 **`BOOT_ID=efc809e8-81b5-458c-999f-9d6ebe2eef8e`**。实机验证结果：
>   - **Check 1（冷启动闲置门控 & 参数）**：`KEY_PROCS=8`、`bootevent.boot.completed=true`、`RPROC2=offline`、`RPROC3=running`、`PERSIST_MOUNTED=0`、`PID_TQFTP=none`、`PID_GNSS=none`、`F3_PARAMS vmodem=false mdev=false esim=0 close_esim=false`、`F4_PARAMS vsim=0`（`const.telephony.vsimModemCount=0`）、staged 二进制 SHA-256 全部吻合。
>   - **Check 2（[start-modem-f4-marble.sh](../devices/marble/runtime/telephony/start-modem-f4-marble.sh) `--modem` + 修正后 [hmos_tqftpserv.c](../compat/telephony/hmos_tqftpserv.c)）**：
>     - 修复 `--ro-dir` 压缩时的重叠 `snprintf(dst, cap, "%s", dst)` 归零缺陷（`compact_ro_dirs`），补齐 `/readwrite/`→`rfs/msm/mpss/`、`/shared/`→`rfs/shared/`、`/hlos/`→`hlos_rfs/` 虚拟路径映射、修正 `rsize=0`/`timeoutms` 与 `OACK` 响应并打印真实 QRTR 端口（`ports=16450,16451,16452`）：实机 `TQFTP_RO_MCFG_OK=6`、`TQFTP_RO_MCFG_REJECT=0`，成功传输 `modem_pr/so/980_0_0.mbn`（5 次分片读全过）、`mbn_hw.dig`（`32 B rc=0`）、`mbn_sw.dig`（`32 B rc=0`），并服务 `/readwrite/server_check.txt` 与 `/readwrite/mcfg.tmp`。
>     - **基带能力与运行模式突破**：`DMS_CAP` 从 F3 的 `tx=0 rx=0 ifs=0:[]` 跃升为 **`tx=316800 rx=316800 data=4 sim=2 ifs=5:[1,4,5,8,10]`**，`DMS_BAND_CAP`（`0x0045`）与 `DMS_ACT_STATE`（`0x002f`）、`NAS_SYS_INFO`（`0x004d`）均返回 `rc=0 err=0x0`；`DMS_SET_MODE req=online` 首次返回 **`rc=0 err=0x0(none)`**，随后实时 **`DMS_INFO ... op_mode=0(online)`** / **`DMS_OP_DETAIL mode=0(online) offline_valid=0 hw_valid=0`**。
>     - `UIM_CARD slot=1 state=1(present)` `CSIM`+`USIM` `state=7(ready) pin1=3`，`UIM_EF_ICCID masked=89***31`，`UIM_EF_IMSI mcc=460 mnc=11`。`QRTR_ALL_COUNT=76`。
>   - **Check 3（`--ipa`）**：`IPA_RC=0`（6 项 IPA/RMNET 内核模块 + `"IPA FW loaded successfully"`）。
>   - **Check 4（`--telephony` + `vsimModemCount=0`）**：`wait_s=1`、`SA 4010/4007/4008/4009/66250` 全 `NATIVE_SA_READY`，`CellularData slotId=1 numeric=46011 opkey=46011`；`SignalStatus-SignalModel` 不再报 `slotId 2`，全 `hilog` 仅余 1 条 `CellBroadcast StaticSubscriber: Invalid slotId 2`（`SLOT2_COUNT=1`、`SLOT3_7_COUNT=0`）。
>   - **Check 5（`--gnss` & 全量回归）**：`gnss_hdi_marble`（`pid=11666`, `uid=1021` `gps`）`GNSS_HDI_PUBLISH_RC=0`，`GNSS_HDI_CHECK rc=0:0:0:0 maj=2 min=0 cb_caps=1(0x1) cb_ws=4(last=4)`；`PERSIST_MOUNTED=0`、`CRASH_CNT=17`（零新崩溃）、`REMOTEPROC3=running`、`WLAN0_CARRIER=1`、`COMPOSER_FDS=25`、`DMESG_OOPS_STRICT=0`、`NATIVE_SA_READY id=1130`、`SENSOR_CLIENT_INFOS rc=0 count=14`、全部 **21/21** 实测 SA READY；音频 `libhmos_audio_vdi.so`/`libhmos_pal_server.so`（`0.03f` 夜间静音规则）逐字节未动，本轮未做外放播放测试（`marble-pal-server.log` 仅记录开机前期系统音写帧 `pid=5870`，非 F4 测试触发）。
>   - **诚实边界 & F5/G/H 接续项**：① `NAS_SERVING` 仍为 `reg=0(not_registered) cs=2 ps=2`（且 `WRQ /readwrite/mcfg.tmp ... unlink=0` 末包超时 `rc=-110`、`PDC_LIST err=0x11`、`PDC_SELECTED err=0x39`），需在 F5 排查 `unlink=` 选项、PDC MCFG SW 激活（`China/CT`）及 RIL 射频上电/注册；② `QMI_LOC` 下 `LOC_INFORM_REV rc=0 err=0x0`，但 `LOC_REG_EVENTS` (`0x0107`/`0x0007`/`0x0003`) 仍报 `err=0x30(invalid_arg)` 且 `LOC_START` 报 `err=0x2e(invalid_op)`（同时收到 1 帧 `pos=1` 位置指示），需核对 `QMI_LOC` TLV 与 `gps_service` PD；③ 仍余 1 条 `CellBroadcast StaticSubscriber: Invalid slotId 2`；④ BT SCO（`SLIM-DEV1-RX-7/TX-7`）、扬声器 `0.03f` 增益恢复与外放、真实蓝牙耳机、强光 ALS 留待白天/后续窗口。回退 F3 `e4043df4` 见[F4 控制脚本](../work/marble-dual-20261123/flash-h20-f4-control.sh)。
> - **历史 F3 / F2 / F1 / E / D / B2 基线**：批次 F3（`e4043df42d9c703c064cae8b900ee6305e5f16665330eb2cd31a2fc379cb15d5`，`boot_id=280717b4-ec52-447f-8e41-c33fff923f65`，[F3 构建脚本](../tools/packaging/build_marble_h20_f3_rf_gnss.py)，回退 `8da1f9b1` 见[F3 控制脚本](../work/marble-dual-20261123/flash-h20-f3-control.sh)）；批次 F2（`8da1f9b13497a21012dda4c9f6829248a6ed6de48e7e83cbd60f8571d8e821c9`，`boot_id=f2fac75e-35a7-4186-8d87-80793f992d52`，[F2 构建脚本](../tools/packaging/build_marble_h20_f2_telephony.py)，回退 `830aad72` 见[F2 控制脚本](../work/marble-dual-20261123/flash-h20-f2-control.sh)）；批次 F1（`830aad7227a02490b5290ee0cbde0a83b2e143e9b8a07246da5a05b88fe0fb6f`，`boot_id=2778f2b7-d289-4e61-bffe-c59157576f8a`，[F1 构建脚本](../tools/packaging/build_marble_h20_f1_modem.py)，回退 `2cff13e2` 见[F1 控制脚本](../work/marble-dual-20261123/flash-h20-f1-control.sh)）；批次 E（`2cff13e2…`，`boot_id=e3d4b0b1-9f16-47ee-9ef3-3c137ecb9046`，[构建脚本](../tools/packaging/build_marble_h20_e_bt_audio.py)，回退 `8b4c2d35…` 见[E 控制脚本](../work/marble-dual-20261123/flash-h20-e-control.sh)）；批次 D（`8b4c2d35…`，`boot_id=e48e9f67…`）；批次 B2（`270d36fa…`，`boot_id=bf27114b…`）。
>
> **下一步推进顺序（夜间仍静音不测外放、不复测快手）**：
> 1. **批次 F5（蜂窝网注网 / PDC MCFG 激活 / `QMI_LOC` 实定位闭环 / `unlink=` 兼容）**：排查 `hmos_tqftpserv` `unlink=0` 与 `mcfg.tmp` 交互、`PDC` 配置激活及 `NAS_SERVING` 注册/信号/数据，`QMI_LOC_REG_EVENTS`/`LOC_START` 真实搜星 NMEA，并消除 `CellBroadcast` 末条 `slotId 2`。
> 2. **批次 G（相机）** → **批次 H（USB 组合口 COM9+HDC / 真实 SN+UDID / 调试 HAP / `E:\marble-tmp` 冗余镜像清理）** + **白天窗口（扬声器 `0.03f` 增益恢复与外放、BT SCO `SLIM-DEV1-RX-7/TX-7`、真实蓝牙耳机放音、强光 ALS 升亮）**。详见[重排后的分批计划](MARBLE_BATCH_PLAN.md#按根因因果链重排的修复批次2026-11-24-2352-更新)。

---

## 0. 当前实机状态

**历史快照（2026-11-24 00:15，UTC+8）：当时装机为 M15 G**（`de7d8b6d…`，早期记录 boot `e85209ff-82f2-48be-b6e0-0bc0ecb7114e`，最新实测 boot `5c9288b4-01de-4507-9e63-e5652a2dc9e0`）。G 是 F 加一个惰性的音频诊断归档，WLAN 和 init 不变，没有自动启动 PAL。只刷了 `vendor_boot_a`；`boot_a`、`abl/xbl`、基带等底层分区保持原样，用户数据未格式化或迁移。见[装机指纹](MARBLE_EXPECTED_AFTER_FLASH.md#历史装机m15-g)与[真机验收](VERIFICATION.md#m15-g真机应用启动与-wlan-联网)。

> **本轮离线已备妥、尚未打包刷机的 M15 H（音频单变量）产物**：
> - 已用 `tools/build/build_current.py` 重新编译 8 个音频目标到 `work/marble-audio-ohos-build/`（含 `libhmos_audio_{vdi,vdi_loader,manager_service}.so`、`native_audio_{hdi_publisher,service_launcher}`、`native_system_audio_check`、`libhmos_pal_{server,lifecycle}.so`），并编译了独立录音验证探针 `work/marble-audio-ohos-build/pal_capture_probe`（源码 [pal_capture_probe.c](../work/marble-dual-20261123/pal_capture_probe.c)）。
> - 已编写离线打包脚本 [build-m15-audio-pal.py](../work/marble-dual-20261123/build-m15-audio-pal.py)（基于 M15 F `ffd33f03…` 单变量合入音频栈：`unshare -m` + `overlay` 隔离 `/vendor/etc`、原厂 `Forte` ACDB、去掉 `agmipc-service`、补齐 `/dev/snd/*` 枚举与 `audio_host`/`audio_server` 启动包装）。**尚未运行打包，尚未扩展刷机脚本，也未刷入设备。**

| 项目 | 已验证 | 仍待完成 |
| --- | --- | --- |
| 应用启动 | G 上从桌面点击，设置和时钟分别取得 WMS 前台焦点。开机约 44 分钟仍是首发 `distributeddata` PID 3238、SA 1301 在线。 | 未逐一验收所有内置或第三方应用；早期 `311b` 的数据库超时触发点未完全定位。 |
| WLAN | 恢复芯片 HDI 和 WPA 按需加载后，F 扫描通过；G 重启后自动重连（`wlan0 up / carrier=1`、`table 2014` 默认路由），原生 `getaddrinfo` DNS 与 OpenSSL TLS 校验 + HTTPS（204/200）在 `UID 0` 与 `UID 20020092`（附带 `netsys_socket` `1097`）均通过。[联网日志](../work/marble-dual-20261123/m15-g-wlan-connected-1795437468.serial.txt#L282-L307) · [HTTPS 实测](../work/marble-dual-20261123/m15-g-net-probe-1795441303-exec-supp.serial.txt#L1-L18) | 地址动态续租（DHCP 交互）与 GUI 应用内 HTTPS 仍待验；保留当前连接。 |
| 存储 | `/data` 为 ext4 `rw`；`8640/8650/5000` + `libhmos_ram_no_tee.so` `quotactl` 回退 + `SA 66074`（`index_insert_service`）动态拉起，`270d36fa` 设置`< 存储>`实测显示 `已使用 28.34 GB/256 GB`（`11%`）、7 类分类条目、缓存 `330 MB` 与应用大小列表，`QUOTA_ERR_13602204=0`。 | ext4 下 `/sys/fs/hmfs/userdata/main_blkaddr` 仍按预期跳过（`errno=2`），不影响设置存储展示。 |
| 通知 / 实况窗 | F 一分钟倒计时能显示桌面胶囊；到期发布成功。 | 到期后胶囊仍留，显示数字随超时继续增大；待核对到期更新和取消结果。提示音另验。 |
| HAP | F 原生 SA 4101 按需加载通过，UID 1000 的 init helper 以 user 100 查询到已安装的 Clock；独立调试 HAP 通过 SDK 离线签名验证。 | 实测 `const.product.udid`、`const.product.devUdid` 与 `ohos.boot.sn` 均返回 `errNum:106`，`bm get -u` 失败（`libudidmodule.z.so` 未在 `autorun/`）；待补通 UDID 与 helper 白名单后再验安装/启动/卸载。 |
| 声音 / 录音 | G 上 ADSP、真实声卡已识别；`linker64 --list` 通过。在 `5c9288b4` 冷态 ADSP 下补齐 `/proc/asound/cards` 后，单 owner 原厂 `Forte` `agm_init_probe` 加载 `Forte_acdb_cal.acdb` + `Forte_workspaceFileXml.qwsp`，完成 `GPR INIT START` 与 `spf_core_apm_close_all`（`op 1001013 status 0`），返回 **`AGM_PROBE INIT rc=0`、`AGM_PROBE DEINIT rc=0`（`AGM_TRIAL_APP_RC 0`）**。[冷态通过结果](../work/marble-dual-20261123/m15-g-agm-cards-5c9288b4-forte-1795442842-result.serial.txt#L171-L219) | 下一候选镜像接入完整 PAL/AGM 服务并配置原厂 `Forte` ACDB，再验收实际播放、音量键与麦克风录音。 |
| USB / HDC | Windows SDK 客户端已实跑；宿主端点与 UDC 保护有离线测试。 | 当前 COM9 就是物理 USB 的 ACM 救援口。G 未部署 HDC 启动配置，未验授权和哈希往返；保留 COM9，不自动切换。 |

[历史验收和待办背景见下文](#2-当前装机镜像与已验证项)。当前优先做音频及 HAP 的独立验证，保留已连接的 WLAN。

---

## 1. 硬件与分区边界（严格遵守）

- **目标机**：Xiaomi `marble`（SM7475 `ukee`），序列号 `5030e45e`，内核 `5.10.274-Bouquet-anyhmos`，单 DSI 1080×2400 6.67" OLED，无 `init_boot`。
- **Donor ROM**：`ADY-AL10 207.0.0.111(SP10)`（本地解包在 `work/donor-rom/ADY-AL10-207.0.0.111-SP10/fs`）。
- **严格分区安全**：
  - **允许刷 `boot_a` 和 `vendor_boot_a`**，按适配需要更新内核、bootstrap、引导和兼容载荷。
  - **刷完必须立刻执行 `fastboot.exe -s 5030e45e set_active a < /dev/null`**，防止 `slot-retry-count` 归零掉 fastboot。
  - **绝不触碰**：`super`、`userdata`、`abl`、`xbl`、`vbmeta*`、`modem*`、`dsp*`、`bluetooth*`、`persist`、`recovery*`。
- **只读原厂分区（机内完好未动）**：
  - `modem_a` (`/dev/block/sde7`, vfat RO)：含 `image/adsp2.{mdt,b*}`、`image/*.jsn`（7 个 PD 描述文件）、`image/qca6490/{amss20.bin,Data20.msc,bd_l*.elf,...}`。
  - `bluetooth_a` (`/dev/block/sde8`, vfat RO)：含 `image/{hpbtfw20.tlv,hpbtfw21.tlv,hpnv20.bin,...}`。
  - `dsp_a` (`/dev/block/sde12`, ext4 RO) 与 `persist` (`/dev/block/sdf7`, ext4 `ro,noload`)。
  - `recovery_a` (`/dev/block/sde27`, RO)：Android boot v4 镜像（51.5 MiB LZ4 legacy ramdisk，offset `0x1000` 魔数 `02 21 4c 18`），内含 `marble` 原厂 Bionic `linker64`、`liblog.so` 等系统库（主机已编好 690 KB 静态解压工具 `/tmp/unlz4_legacy`）。
- **原厂 vendor 备份**：`work/marble-stock-vendor/marble-stock-vendor.tar.gz`（366 MB，刷 `super` 前备份），含完整 `vendor/lib64/{libar-pal.so,libagm.so,libar-gsl.so,libar-acdb.so,libar-gpr.so,liblx-ar_util.so,liblx-osal.so,libats.so,libagm_*_plugin.so,libaudioroute_ext.so,libtinycompress.so,...}`、`vendor/etc/acdbdata/ukee_mtp/*`、`vendor/etc/audio/sku_ukee/{mixer_paths_ukee_mtp.xml,resourcemanager_ukee_mtp.xml}`、`vendor/etc/{card-defs.xml,kvh2xml.xml,usecaseKvManager.xml}`、`vendor/firmware/aw882xx_acf.bin`、`vendor/etc/wifi/qca6490/WCNSS_qcom_cfg.ini`。

---

## 2. 当前装机镜像与已验证项

下表保留历史 A–G 身份索引，不是永久保留所有整镜的要求。当前 H 现场见顶部；后续 boot 更新核对新候选 hash。仅保留当前部署产物、最少恢复基线和仍被构建/测试使用的输入，重复整镜在依赖替代后淘汰。

| 分区 | 镜像路径 | SHA-256 |
| --- | --- | --- |
| `boot_a` | `work/marble-phase4-m15.BTQ5GO/boot_phase4.img`（与前几版完全一致） | `c83a8468955ca6584df29b6e5b7cba27c4b27ae5563a43401566bad301c9cae8` |
| `vendor_boot_a`（**当前装机 G**） | [M15 音频诊断层](../work/marble-m15-agm-layer.ZCVqd9/vendor_boot_m15_F_agm_payload_only.img) | `de7d8b6d2af89a6aa4a4d0e7b907c2ac1dd0638a958db5da48f24d64510d520f` |
| `vendor_boot_a`（直接回退 F） | [M15 WPA 按需镜像](../work/marble-m15-wpa-ondemand.gy37bj3j/vendor_boot_m15_wpa_ondemand_control.img) | `ffd33f03f4ab6e690e65e95c2813972e1f58c4f1ab1c1eb8ab37a255998505fd` |
| `vendor_boot_a`（回退 E） | [M15 芯片 HDI 对照镜像](../work/marble-m15-chip-hdi.7ubhdbv5/vendor_boot_m15_chip_hdi_control.img) | `3c458ced7f013ab7698a7a274bba380a76189a6faae7046634b325964d18ae50` |
| `vendor_boot_a`（回退 D） | [M15 通知对照镜像](../work/marble-m15-notify-control.LeNOUk/vendor_boot_m15_notify_3203_control.img) | `c1619de2e43fb8ed29e1064c4468b15a73b93bccd82ae17c22ca4b604e251bd7` |
| `vendor_boot_a`（回退 C） | [M15 调度与存储控制镜像](../work/marble-m15-role-storage.1v2ynS/vendor_boot_m15_samgr_storage_control.img) | `0e710df5091df335e4c0bf73bb6e354a32317b507d808def3235379c201e3716` |
| `vendor_boot_a`（回退 B） | [仅保留原厂调度角色的 M15](../work/marble-m15-role-control.MXDrsj/vendor_boot_m15_samgr_role_control.img) | `86ab6421ea962d8ab03dc2794674eaec91b725964e01d4d64eb6641516c29afa` |
| `vendor_boot_a`（回退 A） | [M15 原件](../work/marble-phase4-m15.BTQ5GO/vendor_boot_phase4.img) | `9f64479d8567706f6a6b1cd375475b26916a22f6b6a997debe34ec372f28646a` |

直接回退 F 的 Windows 副本为 `E:\marble-tmp\vendor_boot_m15_wpa_ondemand_ffd33f03.img`，已核对完整 SHA-256。仅在需要回退时使用[带身份校验的刷写脚本](../work/marble-dual-20261123/flash-m15-role-control.sh)的 `restore-wpa-ondemand` 模式；回退后重新读回分区哈希及 `boot_id`。

### 历史真机验收背景（`verifym15b` / `verifym1` / `diagm1b`）

1. **显示 + GPU + 触摸 + 桌面自动解锁**：
   - 1080×2400 单 DSI OLED（`PANEL_COLD_REINIT`）+ Adreno 725 Turnip + Goodix 触摸。
   - `native_screenlock_request` 适配了 ADY-AL10 `libipc_single.z.so` 偏移 `+0x9c`（原 `0x98` 为 `gem-w00`），实机返回 `SCREENLOCK_VERIFIED after=0 callback=0 elapsed=2`，成功解锁进 `SCBDesktop8` 并生成 `display0.jpeg`（133 KB）。
2. **显示 DPI / 状态栏 / 居中挖孔 / 亮屏超时**：
   - `devices/marble/runtime/init/display_manager_config.xml` 绑定挂载覆盖 `/sys_prod/etc/window/resources/display_manager_config.xml`：`<dpi>440</dpi>`、`<cutoutArea>504 28 72 72</cutoutArea>`、`<defaultDisplayCutoutPath>M504 28 L576 28 v 72 h -72 Z</defaultDisplayCutoutPath>`。
   - 实机 WMS 窗口矩形：`SCBDesktop8 [0 0 1080 2400]`、`SCBStatusBar18 [0 0 1080 159]`（原 195px）、`SCBGestureNavBar16 [0 2323 1080 77]`、`SCBGestureTopBar15 [0 0 1080 99]`。
   - `default_settings.json` 中 `settings.display.screen_off_timeout` 由 `30000` 改为 `120000`（RAM overlay 绑定挂载）。
3. **物理按键与输入节点**：
   - `reboot-mode.ko` + `qcom-pon.ko` 已持久化，启动阶段即就绪 `event0..9`：`gpio-keys`、`pmic_pwrkey`、`pmic_resin`、`pmic_pwrkey_bark`、`pmic_pwrkey_resin_bark`、`goodix_ts`、`qcom-hv-haptics`、`ukee-mtp-snd-card Headset/Button/USB_3_5 Jack`。用户已确认物理电源键正常。
4. **消除 `hmos_ipc` 刷屏与全局 App 防卡死**：
   - `compat/ipc/access_tokenid.c` 静默处理 `0x5413` (`TIOCGWINSZ`) / `0x5401` (`TCGETS`) 并对未知 ioctl 限流，用与内核完全一致的 **Clang 23.1.6** 重编 `work/marble-kernel-artifacts/modules/hmos_ipc.ko`，实机 `CMD21523_COUNT=0`。
   - `compat/framework/fallbacks/sceneboard_optional_sa.cpp` 放开到 `appspawn` 派生的所有应用（仍先执行真实 `CheckSystemAbility` IPC，仅对返回空且在白名单中的缺席 SA 快速退出重试循环），实机新启动周期仅剩 1 次 `FusionSearch` 3s 告警，设置页可正常进入不卡死。
5. **ADSP + `hmos-pd-mapper` + 真实声卡 + 真实电池充电**：
   - `stage-phase4-marble.sh` 挂载只读 `modem_a`，将 `adsp2.*`、`*.jsn`、`aw882xx_acf.bin` 同时拷入 `/compat/firmware` 与 `/hmos-root/compat/firmware`，启动 `hmos-pd-mapper`（发布全部 7 个 PD 映射含 `avs/audio` 与 `charger_pd`），加载 `qcom_pil_info.ko`、`smp2p.ko`、`qpnp-pbs.ko`、`qcom-hv-haptics.ko` 及全套 `ukee` 音频/声卡模块后拉起 ADSP。
   - 实机确认：
     - `remoteproc0 (3000000.remoteproc-adsp) state=running`
     - `/proc/asound/cards`：`0 [ukeemtpsndcard ]: ukee-mtp-snd-ca - ukee-mtp-snd-card`
     - `/sys/class/power_supply/battery`：`cap=100 status=Charging volt=4411000`
     - `/sys/class/aud_pasthru_adsp/aud_pasthru_adsp`、`/sys/class/msm_audio_ion/msm_audio_ion` 就绪。
6. **QCA6490 Wi-Fi 驱动与 Netstack + Media/Audio 基础 SA**：
   - `devices/marble/runtime/wifi/start-wifi-marble.sh` 完成固件拷贝、`printf '/compat/firmware'`（无换行）、`cnss/recovery=1`、`cnss/fs_ready=1`、`qca_cld3_qca6490.ko`、创建 `/dev/wlan` 并写入 `ON`，实机 `WIFI_START_EXIT=0 STAGE=complete`、`WIFI_NETDEV_WAIT_SECONDS=0`，`wlan0` 网卡创建成功（MAC `00:97:b7:c2:78:b6`）。
   - 5-host HDF HCB（`allocator_host`、`composer_host`、`hmos_userauth_host`、`wifi_host`、`wpa_host`）已生效，`wifi_host`、`wpa_host`、`wifi_manager_service`、`netsysnative`、`netmanager`、`download_server` 均正常运行。
   - 在线 SA 已实测 `READY`：`10`、`1126`、`1151`、`1158`、`1301`、`1909`、`2902`、`3001`、`3002`、`3009`、`3010`、`3011`、`3701`、`3702`、`3706`。
7. **蓝牙硬件驱动节点就绪**：
   - `btpower.ko` + `msm_geni_serial.ko` 实测就绪 `/sys/class/bt-dev/btpower/dev` 与 `/sys/class/tty/ttyHS0/dev`。

---

## 3. 操作环境与关键避坑清单

### 临时目录（用户明确要求：不要放 C 盘，统一用 E 盘！）
- **Windows 临时目录**：`E:\marble-tmp`（WSL 路径 `/mnt/e/marble-tmp`）。
- 串口执行工具：`E:\marble-tmp\com_exec.ps1`（C 盘旧目录已清理）。
- **WSL 调用 `powershell.exe` / `fastboot.exe` 必须加 `< /dev/null`**，否则会挂死等待 stdin！
- 串口执行示例（`COM9`，单次脚本少于 2800 UTF-8 字节；`-Id` 只接受 1–16 个 ASCII 字母或数字 `[A-Za-z0-9]{1,16}`，连字符和下划线都会触发 `CMD invalid identifier`）：
  ```sh
  powershell.exe -NoProfile -ExecutionPolicy Bypass \
    -File 'E:\marble-tmp\com_exec.ps1' -Port COM9 -Id check1 \
    -ScriptFile 'E:\marble-tmp\check.sh' -TimeoutSec 25 < /dev/null
  ```
- 重启进 fastboot 命令（BusyBox 位于 `/busybox` 且没有 `reboot` applet，必须用 OHOS 的 `/system/bin/reboot`）：
  ```sh
  /busybox sync
  /busybox chroot /hmos-root /system/bin/reboot bootloader
  ```

### 构建与打包命令

当前实装是[已实测的 H20 动画单增量](MARBLE_BATCH_PLAN.md#h20-live-animation-and-audio)。已保留独立动画镜像及原 H20 回退，动画镜像以[单增量构建器](../tools/packaging/build_marble_h20_bootanimation.py)生成。下面的通用构建另含其他差异，不作为这两个镜像的替代。

- 每次新 shell 设置环境变量：
  ```sh
  export HMOS_SDK=$PWD/work/ohos-sdk/installed/native \
         HMOS_UPSTREAM=$PWD/work/upstream \
         HMOS_STAGE=$PWD/work/marble-stage \
         HMOS_DONOR_LINK=$PWD/work/marble-donor-link
  ```
- 编译 `compat` 目标到当前 build 目录（`work/marble-phase4-connection.bl5xLy/build`）：
  ```sh
  python3 tools/build/build_current.py --out work/marble-phase4-connection.bl5xLy/build <target1> <target2>
  ```
- 重编内核模块（如 `hmos_ipc.ko`）**必须使用绝对路径的 Clang 23.1.6**：
  ```sh
  export PATH="$PWD/work/marble-kernel-work/toolchain/llvm-23.1.6-x86_64/bin:$PWD/work/marble-kernel-work/kernel/build-tools:$PATH:/usr/sbin:/sbin"
  ```
  切忌用相对路径导致回退到 Clang 18（会触发 CFI/LTO 版本不一致警告）。
- 通用 Phase 4 主机构建（不是 H20 增量；仅在另行需要通用候选时执行）：
  ```sh
  python3 -B -m unittest tests.test_marble_phase4 tests.test_marble_diagnostics
  batch=$(mktemp -d work/marble-phase4-next.XXXXXX)
  python3 -B tools/packaging/marble_diagnostics.py --out "$batch/diagnostics"
  python3 -B tools/packaging/build_marble_phase4_images.py \
    --phase4-build work/marble-phase4-build \
    --diagnostics-dir "$batch/diagnostics" --out "$batch/images"
  ```
  构建器验证四个诊断件及对应当前源码，预构件不一致时停止。动画单增量直接以 H20 为输入，不走这条命令。达到该候选自己的回退和实机前置条件后才安排转移及刷写。
- 历史刷写示例（非本轮操作）：
  ```sh
  fastboot.exe -s 5030e45e flash vendor_boot_a 'E:\marble-tmp\vendor_boot_next.img' < /dev/null && \
  fastboot.exe -s 5030e45e set_active a < /dev/null && \
  fastboot.exe -s 5030e45e reboot < /dev/null
  ```

### 踩过的关键坑（别再踩）
1. **固件路径的双重命名空间**：PID 1 (`ohos init`) chroot 到 `/hmos-root` 后，内核 `request_firmware()` 实际在 `/hmos-root/compat/firmware` 查找固件；因此所有固件必须**同时拷到 `/compat/firmware/` 和 `/hmos-root/compat/firmware/`**，且写 `/sys/module/firmware_class/parameters/path` **必须用 `printf '/compat/firmware'`**（不能用 `echo`，换行符会导致 `-2 ENOENT`）。
2. **CNSS2 panic 保护**：写 `/sys/kernel/cnss/fs_ready` 之前必须先 `echo 1 > /sys/kernel/cnss/recovery`，否则任何固件查找失败都会直接 kernel panic。
3. **QCA6490 创建 `wlan0` 的触发条件**：加载 `qca_cld3_qca6490.ko` 后必须创建 char 设备 `/dev/wlan`（major 从 `/proc/devices` 或 `dmesg` `wlan_hdd_state wlan major(...)` 动态解析，实测 493 或 499）并写入 `echo ON > /dev/wlan`，`wlan0` 才会立即出现。
4. **声卡 `ukee-mtp-snd-card` 依赖 `qcom-hv-haptics` 在 ADSP 启动前加载**：`asoc-codec-names` 包含 `swr-haptics` (`pm8350b-swr-haptics.1f0170220`)，必须在 `boot_adsp` 之前先加载 `qpnp-pbs.ko` + `qcom-hv-haptics.ko` 与全部音频模块，否则 `swr-haptics` 枚举失败会导致声卡一直 deferred。
5. **不要在串口上对 155 MB 的 `marble-p4-hilog.txt` 直接 `grep`**：会超时锁死 `hmos-probe`。改用 `$B chroot /hmos-root /system/bin/hilog -x` 或 `$B tail -c 500000`。

---

## 4. 下一步执行路线（可直接按顺序做）

### 步骤 1：保留 WLAN 连接，补应用层验收
- G 已通过真实 AP 关联与 IP 收发。先在当前连接下核验原生 DNS 和应用内 HTTPS；只有安排冷启动复测时再验自动重连及 DHCP 租约。

### 步骤 2：直接打包、刷写并验收 M15 H（音频单变量），再推进 HAP、实况窗与存储
- 音频（当前立即衔接点）：
  1. 运行 `python3 work/marble-dual-20261123/build-m15-audio-pal.py`，确认离线 round-trip、AVB footer 及 CPIO/LZ4 大小均低于 `311b` 上限，并已复制到 `/mnt/e/marble-tmp/`。
  2. 在 [flash-m15-role-control.sh](../work/marble-dual-20261123/flash-m15-role-control.sh) 中增加 `audio-pal`（`before=de7d8b6d…` → M15 H）与 `restore-from-audio-pal`（回退 F `ffd33f03…`），刷写 `vendor_boot_a` 并执行 `set_active a`。
  3. 通过 `COM9` 依次验收：桌面/SA 1301/WLAN 自动重连零回归、`/vendor/etc/usb_audio_policy_configuration.xml` 未被污染、`marble-pal-server.log`（`AGMIPC_SHIM_INIT_OK direct=48`、`PAL_SERVER_AGM_INIT_RC=0`、`PAL_SERVER_PAL_INIT_RC=0`、`PAL_SERVER_READY=1`）、`hmos-audio-{host,server}.txt`、`native_system_audio_check` 扬声器播放、音量键、以及临时上传 `work/marble-audio-ohos-build/pal_capture_probe` 验收麦克风录音；随后更新 `docs/VERIFICATION.md` / `docs/MARBLE_EXPECTED_AFTER_FLASH.md` / `docs/MARBLE_HANDOFF.md` 并跑全量单元测试。
- HAP：使用绑定测试设备的调试配置及原生授权安装入口，验收独立包的安装、版本、启动和卸载；不替换已有用户包。实况窗：对比同一通知的到期更新和取消结果，检查应用在前台与后台时是否一致。
- 存储：容量统计与服务调度分别验收；保留现有 userdata，配额迁移仅在有独立备份和离线验证后另行安排。
- HDC：先验证不占用物理 UDC 的 FunctionFS 端点；只有在具备独立恢复手段后再验组合 USB 枚举、授权和传输。

### 步骤 3：蓝牙（对标 `sheng` v139：开关与附近设备扫描）
- 内核节点 `/dev/btpower` 与 `/dev/ttyHS0` 已确认存在；只读 `bluetooth_a` (`/dev/block/sde8`) 含 `hpbtfw20.tlv`、`hpnv20.bin`。
- 参照 `devices/sheng/runtime/bluetooth/`（`init-bluetooth-v136.cfg`、`start-bluetooth-v136.sh`、`bluetooth_service_v136.json`）和 `compat/bluetooth/`，复用已独立验证匹配的 Bionic 运行时与 `marble` 蓝牙库拉起 `hmos.bt.h4` 桥接、`native_bt_hdi_publisher_v136` 和 `bluetooth_service` (`SA 1130`)，从 `sceneboard-optional-sa.list` 移除 `1130`，验证蓝牙开关与扫描发现。
- 注意确认 `persist` 中蓝牙 MAC 路径（之前检查 `/compat/original-persist/bluetooth/` 为空，需检查 `persist` 其他路径或从 `bluetooth_a`/默认方案确认）。

### 步骤 4：BootControl（`mark-boot-success`）与最终收尾
- 参照 `devices/sheng/runtime/boot/mark-boot-success-v135.sh` 与 `compat/bootcontrol/bootcontrol_readonly.c`，从 `recovery_a` 提取 `marble` 对应的 `bootctrl` HAL（检查 `recovery_a` 中 `bootctrl.*.so` 名称），在桌面解锁成功 + 截图完成后调用 `mark_boot_successful()`，解决用户“强制重启后又进不去”的 A 槽 retry 耗尽隐患。
- 运行全量收尾校验：
  ```sh
  python3 tools/maint/refresh_manifest.py && python3 tools/maint/verify_sources.py
  python3 -m unittest discover -s tests -p "test_*.py" -v
  ```
- 诚实更新 `docs/VERIFICATION.md` 和 `README.md`，记录所有实机证据与 SHA-256，不夸大任何未验证功能。

---

## 5. 早期 Phase 4 改动索引（历史记录）

- `compat/ipc/access_tokenid.c`：静默 `0x5413`/`0x5401` 并限流未知 ioctl 日志，已用 Clang 23.1.6 重编 `hmos_ipc.ko`。
- `compat/framework/fallbacks/sceneboard_optional_sa.cpp`：缓存 optional-SA 白名单并扩展到 `appspawn` 全部应用防主线程死等。
- `compat/services/native_screenlock_request.cpp`：兼容 `ADY-AL10` `+0x9c` 偏移，已重编进 `work/marble-phase4-connection.bl5xLy/build/native_screenlock_request`。
- `devices/marble/runtime/init/display_manager_config.xml`：新增 `marble` 专用 DPI=440 + 居中挖孔配置。
- `devices/marble/runtime/init/init-wifi-marble.cfg`：新增 `wifi_host`/`wpa_host`/`wifi_manager_service` 启动配置。
- `devices/marble/runtime/wifi/start-wifi-marble.sh`：新增 QCA6490 固件/驱动/`/dev/wlan ON`→`wlan0` 拉起脚本。
- `devices/marble/runtime/init/hmos-init-phase4.cfg`：导入 `init-wifi-marble.cfg`，移除 `appspawn` 的 `libhmos_wifi_ui_unavailable.so`。
- `devices/marble/runtime/init/init-platform-marble.cfg`：新增 `netsysnative`、`netmanager`、`download_server`。
- `devices/marble/runtime/init/init-hardware-marble.cfg`：新增 `audio_server`、`media_service`、`av_session`、`av_codec_service`。
- `devices/marble/runtime/boot/stage-phase4-marble.sh`：持久化 `qcom-pon.ko`、DPI/默认亮屏超时覆盖、`modem_a` 只读挂载、`aw882xx_acf.bin`、`hmos-pd-mapper`、ADSP/电池/全套声卡模块、Wi-Fi 配置与 `/dev/rfkill`/`/dev/net/tun`。
- `devices/marble/runtime/boot/start-phase4-marble.sh`：串联 audio/media + Wi-Fi/netstack 阶段与 SA 等待。
- `tools/packaging/build_marble_phase4_images.py` & `tests/test_marble_phase4.py`：升级为 5-host HDF HCB 并打包上述全部产物，11/11 单元测试通过。
- 主机临时产物（不入 git）：`work/marble-wifi-config/vendor/etc/wifi/qca6490/WCNSS_qcom_cfg.ini`、`work/marble-audio-fw/vendor/firmware/aw882xx_acf.bin`、`/tmp/unlz4_legacy`、`/tmp/marble-audio-all-build/*`。
