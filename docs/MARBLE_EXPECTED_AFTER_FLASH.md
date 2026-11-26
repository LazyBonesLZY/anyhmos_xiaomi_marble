# 刷机后期望值 / Expected after flashing

> **授权更新（2026-11-24）**：boot_a 与 vendor_boot_a 均可按适配需要修改和刷写；每批按实际目标及读回指纹核验。精简工件规则见[分批计划](MARBLE_BATCH_PLAN.md)。

<a id="current-marble-m5"></a>

## 当前装机：Batch M5 综合缺陷修复（2026-11-26）

冷启动验收 boot：`f93f2eb6-4e82-49d9-ae5d-92fbd94b289b`，槽位 `a`。仅写 `vendor_boot_a`，`boot_a`、`super`、`userdata` 及原厂底层分区均保持不变。

| 对象 | 真机读回 SHA-256 |
| --- | --- |
| `vendor_boot_a` | `c4b6a64f857fac5f144662b7240ef147c0687a77f87c0e60b6fac6fbf23a2f07` |
| `boot_a`（保持原样） | `c83a8468955ca6584df29b6e5b7cba27c4b27ae5563a43401566bad301c9cae8` |
| `/compat/lib64/libpin_auth_interface_service_3.0.z.so` | `26b60bfff1d298298ba145c43d62f0ea75868d79fb4c7683a966403fe6b36377` |
| `/compat/bin/marble_touch_wakeup` | `fb5804954c7f4254f5332cb06814c52e017e9a1f00e3f93b6997584be773ff17` |
| `/compat/lib64/libmarble_motion_interface_service_1.1.z.so` | `e258e557ef096287b43dbcc6ff3e43428350e9edd22b6dcd3b30301e1ec7db87` |
| `/compat/lib64/libmarble_vibrator_interface_service_2.0.z.so` | `4c96e1f6f107ebc2266bfe0a7e8caa88484abc7e1eea2b0167a1ac02918638ba` |
| `/compat/bin/hmos_ims_core_service` | `5ca7f0ad00385b183207bdd4690caf4653351cfa8b118766273cf5d498698667` |
| `/compat/lib64/libril_vendor_qmi.z.so` | `6b0ce00d089355e45a8bb77ca92db4bec2e527a7a22a245b2c1517cb21de9e9c` |
| `/compat/lib64/libmarble_camera_vdi_impl.z.so` | `468d41bd209cf712f43de2f56c315ee668527f68de666868e7948ebbcaab4b7e` |

实机关键回执：`BOOT_COMPLETED=true`、`IMS_SMS_SEND fmt=0x06 ims=1 rc=0 qerr=0x0`、`IMS_SMS_CB_DONE ok=1 ref=193`、`ImsCallStub` VoLTE/VoNR 在线（`HILOG_IMSCALL_NULL=0`）、`EV6=3034:3034` 与 `SceneBoard IsSupportEffect state:1`、`MMI_EV10=1`、`SA_3203/3204/65840/4006=UP`、`HILOG_BAD_PIN/VIB_FAIL/AOD_TO/REM_FAIL/LV_TO=0`、`WLAN0_CARRIER=1`、`DMESG_OOPS=0`。直接回退 M4c 基线为 `dcd8797f4542aa8543e4f6c34fbc545bd5719826c3c523020ca0aea3a35ddab8`。

<a id="current-marble-h20-animation"></a>

## 历史装机：H20 开机动画单增量

第二次验收 boot：`370caf66-b91a-4a5b-a385-5bd79b8a29b4`，槽位 `a`。只写 `vendor_boot_a`，userdata 保留。

| 对象 | 真机读回 SHA-256 |
| --- | --- |
| `vendor_boot_a` | `dff03e315bd2599efda9ad6fbe03b98b4e6a774ca779e557c0183afc1fbff939` |
| `boot_a`（保持原样） | `c83a8468955ca6584df29b6e5b7cba27c4b27ae5563a43401566bad301c9cae8` |
| `/graphics/start-phase4-marble.sh` | `8751460c96ca76ddd6e68ede7432938661447d947975ac056de44771cc931885` |
| RenderService 实际映射的 `librender_service_base.z.so` | `b4c7c0b86a42fd3e329fae5444bb5ff17f176aaff5313873d53bd6d17c17157a` |
| RenderService 实际映射的 Turnip | `de21593bf75316332f112ad8ae746d9583daffaa195cb12c16eddf6a936d1c35` |

原动画进程启动并有 DSI 主平面动态像素，随后退出且桌面接管；WLAN 获得 IPv4、原网关回包。保留启动初段黑帧记录，动画出帧不等于从上电开始全程可见。图形库保持 H20，原快手 App 回归另验。[真机记录](MARBLE_BATCH_PLAN.md#h20-live-animation-and-audio) · [回退工具](../work/marble-dual-20261123/flash-h20-bootanim-control.sh)：装机退回 `c687adec…` 用 `restore <当前 boot_id>`。

<a id="当前装机h20-yuv-native"></a>

## 回退基线：H20 YUV native

快手验收 boot：`02573073-bdae-414a-b73e-2b1b76e4f774`，槽位 `a`。接机后的固定音频真机正控为同镜像的 boot `b4d02895-b1be-4ef9-9fbb-3a692c130b52`。

| 对象 | 真机读回 SHA-256 |
| --- | --- |
| `vendor_boot_a` | `c687adec08b1b917fcbdab6fcc96caebc038aaabca43503dc1def0ceb9953079` |
| `boot_a`（本批保持） | `c83a8468955ca6584df29b6e5b7cba27c4b27ae5563a43401566bad301c9cae8` |
| RenderService 实际映射的 `librender_service_base.z.so` | `b4c7c0b86a42fd3e329fae5444bb5ff17f176aaff5313873d53bd6d17c17157a` |
| RenderService 实际映射的 Turnip | `de21593bf75316332f112ad8ae746d9583daffaa195cb12c16eddf6a936d1c35` |

同一快手视频的动态画面和紧邻的 YUV/RGB 导入成功均有实机证据；同镜像另通过固定 WAV 和 M4A 的原生播放时间线与双向拖动。实际 WebView、固定 MP4 界面拖动和扬声器出声各自独立。[刷写和验收证据](MARBLE_BATCH_PLAN.md#第43轮h20-修复快手黑视频真实-yuv-导入及画面通过)。直接回退 H19 `b966b9a6…`，使用[固定身份的回退脚本](../work/marble-dual-20261123/flash-m15-role-control.sh)的 `restore-yuv-native` 模式。

## 历史装机：M15 G

此节为历史 G 验收快照；后续双分区候选以自己的目标 hash 核验。

设备 `boot_id`：`e85209ff-82f2-48be-b6e0-0bc0ecb7114e`。

| 对象 | 真机读回 SHA-256 |
| --- | --- |
| `vendor_boot_a` | `de7d8b6d2af89a6aa4a4d0e7b907c2ac1dd0638a958db5da48f24d64510d520f` |
| `/graphics/marble-agm-init-only-v1.tar.gz` | `bcb1582fc4fc3dc43ccc1fe8d721090d147c470f1e7c7a7d6300f4ed1fb737db` |
| `boot_a`（未刷） | `c83a8468955ca6584df29b6e5b7cba27c4b27ae5563a43401566bad301c9cae8` |
| PID 1 生效的 `/system/etc/init.cfg` | `a67f7a8923c5a5ccb292ae819e773d4f54820bacc3b4cd20d0c0899d70ea2bdc` |
| `/compat/etc/init-platform-marble.cfg` | `af41e99e66a2085142cfd49cc599f44eabd65575f3f9ef4bc444fc365b87b8c8` |
| `/compat/etc/init-wifi-marble.cfg` | `84923a40c2e57740e71c22a1ae1d8ea60f70a675ceb99343a812d15bce697a8d` |
| `/chip_prod/etc/hdfconfig/hdf_default.hcb` | `a38b5846848c4cdbe9507fde2cdf75f4571543f2893e62787b98f50345274168` |
| `/system/profile/foundation.json` | `e6b3e8dd26bbad669634ac7b90ff30e690f99d638729f17f963cc811486971b1` |

G 已验设置与时钟前台、WLAN 真实联网和基础 Bionic 装载；其中 WLAN 验收是同一次开机实测的 IP 收发，不代表应用内 HTTPS 或重启后重连。G 的 WLAN/HCB/init 与 F 一致，只额外带一个不自启的诊断归档。`storage_trial` 非持久；4101 空闲时不发布并非故障。直接回退 F 的 SHA-256 为 `ffd33f03f4ab6e690e65e95c2813972e1f58c4f1ab1c1eb8ab37a255998505fd`，详见[当前装机与回退](MARBLE_HANDOFF.md#2-当前装机镜像与已验证项)。

## 历史候选：`311b642b5af845c6…` 与基于它的 WLAN 集成

以下两份不是当前装机版。`311b642b…` 曾通过四项按需加载检查，但因 1301 不稳定，已回退至 M15。后续以 M15 为基线逐项对照。见[真机对照记录](VERIFICATION.md#2026-11-23-应用回退对照及按需服务真机验收)。

| 镜像 | 完整 SHA-256 | 状态 |
| --- | --- | --- |
| [原调度修复候选](../work/marble-phase4-samgr-app.eKC4oH/images/vendor_boot_phase4.img) | `311b642b5af845c6db9d25b4ed42eda461c97d65b125f4beaddae2cdd861818d` | 已试装、已回退 |
| [WLAN 配置集成候选](../work/marble-phase4-wlan-stage.cykCHh/images/vendor_boot_phase4.img) | `5e779fa6949f15e0e5f247b4e32dd916add4ce215046390f48098f7142b28bcf` | 基于 `311b642b…`，仅离线往返验证，不作为桌面恢复镜像 |

若重测该候选，刷完 `vendor_boot_a` 后，从引导根执行 `/busybox sh /hmos-root/compat/bin/marble_identity.sh`。以打包清单和实机读回核对下表。
不一致的行就是「镜像没生效」或「启动路径把文件写到了别处」。

| 对象 | 期望值（前 16 位） | 说明 |
| --- | --- | --- |
| `ID vendor_boot` | `311b642b5af845c6` | 候选镜像完整 sha `311b642b5af845c6db9d25b4ed42eda461c97d65b125f4beaddae2cdd861818d` |
| `ID boot_a` | `c83a8468955ca658` | 历史仅刷 vendor_boot 试验的 boot 指纹；后续使用新候选目标 hash |
| `ID bin/hmos_bt_bridge_marble` | `9dfaef7a716f8560` | 与当前本地构建产物一致（历史镜像曾为 `62b09485…` / `1f4d9f9b…`） |
| `ID bin/hmos_hisysevent_sink` | `288eb923e508fbb7` | 与构建产物一致 |
| `ID bin/hmos_brightness_persist` | `437eb9c8e3fd351d` | 与构建产物一致 |
| `ID bin/tee_backend_probe` | `3f3088a5e7fe9b16` | arm64 探针 |
| `ID bin/native_codec_probe` | `bf49ecd5cce2e967` | 编解码探针 |
| `ID cfg/init-platform` | `e6aa082a87c11972` | 匹配当前 `devices/marble/runtime/init/init-platform-marble.cfg` |
| `ID cfg/foundation` | `e7098d3d071b9cff` | 12 个 SA，含 3203/3204（M5 打包阶段将 ramdisk `foundation-app-core.json` 中 3204 置为 `run-on-create: true`） |
| `/hmos-init-debug.cfg` 与 `/proc/1/root/system/etc/init.cfg` | `b21a5a81a1b68e03` | 用 `sha256sum` 单独核对；两个文件应一致。原版 samgr 权限和 ACL 从 donor 恢复 |

旧版身份脚本检查 `/compat` 下的 stage / start，但真正的脚本在引导根的 `/graphics/`。`ID cfg/stage` / `ID cfg/start` 为空不表示启动脚本没生效。先核对 init 配置哈希，再从桌面启动两款内置应用，检查目标窗口拿到前台焦点；只看进程存在或 `aa` 退出码不算通过。回滚前后的记录须各自绑定 `boot_id`。

## `marble_verify_all.sh` 期望

| 字段 | 期望 |
| --- | --- |
| `SA 3203` | `ready`（ANS；实况窗依赖它） |
| `SA 3505` / `SA 66090` | `ready`（隐私服务与指示器） |
| `SA 1301` | 前台应用启动前后均可查询；若服务按需被回收，真实调用能再次加载并返回 |
| `SA 5000` / `SA 8640` / `SA 8650` | 单独触发原生按需加载再查 `ready`；空闲时单次查不到不等于失败。设置页总量/可用量由 SA5003 路径单独验收 |
| `SA 4101` | 按需加载不再出现 `StartSA` 请求权限失败；它上线不等于 `bm` 的调用方已获得安装权限 |
| `SA 4700` / `1901` / `801` | `ready`（softbus / ressched / 无障碍） |
| `SINK_PID` | `alive` |
| `HISYSEVENT_ERRNO2` | `0` |
| `BRIGHT` | `saved=40 now=40`（旧镜像实测 `now=3`） |
| `SND` | `total=43 capture=20`，且**不是**手工 mknod 产生 |
| `GPR` | 若仍为 `missing`，说明内核没有该节点（不是我们没建） |
| `PAL` | 期望不再是 `PAL_SERVER_AGM_INIT_RC=-114` |
| `HDC` | 单独检查端点和 UDC 绑定；主机实际授权后做已知文件的哈希往返，不能只看 `ffs_ready` |
| `WIFI` | 实际在设置开启 WLAN 后验证扫描、连接、地址和流量；`WpaMainThread` 只能作阶段证据 |
| `VERDICT`（新版脚本） | `scope=readiness end_to_end=untested`；`pass` 只表示指定常驻 SA 和 socket 的就绪检查通过。按需调用、存储汇总、录音、WLAN 和 HDC 传输各自留证 |
