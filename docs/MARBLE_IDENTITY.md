# marble 四个世界 / Four worlds（2026-11-23）

规范：只有四个世界的 SHA256 一致时，才允许说「设备在跑这个改动」。
任何一层没对上，结论就只停留在那一层。

> 下方是早期样本的历史记录。最新装机与验收见[交接进度](MARBLE_HANDOFF.md)和[读回指纹](MARBLE_EXPECTED_AFTER_FLASH.md#当前装机h20-yuv-native)。

## 1. 当时源码（历史快照）

| 文件 | SHA256 |
| --- | --- |
| `devices/marble/runtime/boot/stage-phase4-marble.sh` | `0e982d547c3d6146bdcb213c03584d00bb0686003b5790a7e9eaebf236be252d` |
| `devices/marble/runtime/boot/start-phase4-marble.sh` | `2ccb7e0d138d8f2fd50c0c8f1ec831081604e65c12e85923502e062a8c700a82` |
| `devices/marble/runtime/init/init-platform-marble.cfg` | `8c5e65cfec794041cf7bf53fd8ed77021e1006aeefbcfebff020c6916d89eac4` |
| `devices/marble/runtime/init/foundation-marble.json` | `e7098d3d071b9cff3bd3cdec729107b60d52277f3dccdfa8b6441d6a458b3d1d` |
| `compat/verify/marble_verify_all.sh` | `ff34ba857b794ed0fd21b666277ca0b5291f203c42df735fd4f03dd38e69d8e4` |
| `work/marble-bluetooth-build/hmos_bt_bridge_marble` | `62b09485f3bed3234d963931ab5bae68182cacf4efa29811ff11817202effcf7` |

## 2. 离线构建候选（`work/marble-phase4-*/images/vendor_boot_phase4.img`）

| SHA256 | 备注 |
| --- | --- |
| `70a8919ad3d7b5a9c38a2830811263ee23a43b146398500e85d2b5460917e1fc` | **当时候选**（sink 修好、chroot 启动、蓝牙/USB 块恢复、dsp 前移） |
| `9daddbae13a6464cfc9bc6d02782185a19f1d3094b4c117a92911ce6f0cede46` | 服务权限对齐版 |
| `77eaa57dd626ffca27b71996027ef1fdcb2aa3a9759442c37cfde5ba2a94b1e8` | 蓝牙/USB 块恢复版 |

## 3. ROM 集成目录

`work/marble-phase4-sinkfix.EU03g9/images/`（上表第一行即其产物），
payload 187 项，`roundtrip_verified: true`。

## 4. 设备实际加载（`adb`/串口实测，boot_id `46451fba`，uptime 17312s）

| 对象 | 设备上的值 | 与源码/候选比 |
| --- | --- | --- |
| `vendor_boot_a` | `fd576a135714ea07…` | **旧镜像**（早于 sink、早于权限对齐、早于 dsp 前移） |
| `/compat/bin/hmos_bt_bridge_marble` | `1f4d9f9b7c6eee06…` | **旧二进制**（无 EDL 重试；重建版是 `62b09485…`） |
| `/compat/etc/init-platform-marble.cfg` | `8ffbb2b13a8273a8…` | 旧配置（现源码 `8c5e65cf…`） |
| `/compat/stage-phase4-marble.sh` | 不存在于该路径 | 该镜像里 stage 走的是别的落点 |

## 结论（本轮最重要的一条）

设备在**四个维度上都落后于源码**，所以：

- 「蓝牙补丁无效」其实是**旧二进制没换**（本轮真机日志没有 `EDL_VERSION_ATTEMPT`，只有一条 `HCI_TIMEOUT opcode=0xfc00`）；
- 「sink 静默死掉」的旧二进制里根本没有 chroot 启动逻辑；
- 任何"已修复"的说法，在刷入 `70a8919a…` 之前都只能记在**源码/候选**层。

## 防复发

`tests/test_marble_boot_blocks.py::ImageBinaryFreshnessTests` 断言：镜像里的
`hmos_bt_bridge_marble`、`hmos_hisysevent_sink` 的 SHA256 必须等于本地构建产物，
避免再次「源码改了、镜像里还是旧的」。

## 验证分层记账（每个功能只记到它真正通过的层）

| 层 | 已通过的例子 |
| --- | --- |
| 源码 / 配置断言 | 484 项单元测试（服务权限、路径存在性、启动块顺序、镜像内容） |
| 构建与审计 | `roundtrip_verified: true`、`verify_sources` 通过、payload 187 |
| 宿主（x86） | `soft_huks` selftest（仅宿主，未进镜像） |
| 原生 ARM64 / 真机（旧镜像） | 面板/桌面/触摸、`hisysevent` sink 可 chroot 启动、亮度 `apply`（3→40 回读一致）、蓝牙 `CHECK_RESULT=ok`、HDC `ffs_ready=1`、`tee_backend_probe` backend=soft、`native_codec_probe` rc=0 |
| 真机（新镜像） | **尚无**——待刷 `70a8919a…` |
| 整机功能与感官 | 面板点亮/触控/桌面（旧镜像）；声音、录音、通知、安装、WiFi 均未达成 |
## 设备身份实测（`compat/verify/marble_identity.sh`，boot `46451fba`，uptime 17456s）

```
ID vendor_boot=fd576a135714ea07   boot_a=c83a8468955ca658
ID bin/hmos_bt_bridge_marble=1f4d9f9b7c6eee06      ← 旧（重建版 62b09485）
ID bin/hmos_hisysevent_sink=288eb923e508fbb7       ← 与构建产物一致
ID bin/hmos_brightness_persist=437eb9c8e3fd351d    ← 一致
ID bin/tee_backend_probe=3f3088a5e7fe9b16          ← 一致
ID bin/native_codec_probe=bf49ecd5cce2e967
ID cfg/init-platform=8ffbb2b13a8273a8              ← 旧（源码 8c5e65cf）
ID cfg/foundation=e7098d3d071b9cff                 ← **与源码一致**
ID cfg/stage= (空)  cfg/start= (空)                ← 该镜像里这两个脚本不在 /compat 下
ID audio/ukee-policy=d3f67c44c4411eca
ID audio/runtime-libs=50  agmshim=e56edabbc2d2577b ← 一致
ID nodes gpr=yes(手工 mknod 占位)  snd=43(运行时手建)  dsp=1(手工挂载)
```

结论：**只有 `vendor_boot_a`、`hmos_bt_bridge_marble`、`init-platform-marble.cfg` 三项确实落后**；
其余产物与当前源码/候选一致，说明它们在旧镜像之后没有再次改动。

