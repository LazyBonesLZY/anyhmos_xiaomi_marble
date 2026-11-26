# marble 负向对照 / Negative controls（2026-11-23）

每条修复都要能回答：**改之前是什么现象？** 没有旧现象记录的"修复"不算证据。
下表左列是旧行为的原文证据（来自 `docs/VERIFICATION.md` 与真机采样），右列是新行为判据。

| 修复 | 旧现象（证据原文） | 新判据 | 验证层 |
| --- | --- | --- | --- |
| `/vendor/etc` 音频配置不可写 | `/vendor` 是只读 erofs；`mount -t overlay` 报 `filesystem on '…/compat/vendor-etc-upper' not supported as upperdir` | `MARBLE_VENDOR_ETC_OVERLAY xml=14 kv=1` | 真机（运行时常量） |
| chroot 内 `#!/busybox sh` 失效 | 子系统脚本 `exit 127`（`/busybox` 在 chroot 内不存在） | `MARBLE_CHROOT_BUSYBOX=ok` | 真机 |
| AGM 找不到 ACDB 目录 | `session_obj_init: 1339 Error:22 initializing graph`；反编译定位 `graph_init.cfi @0x156d4` 硬编码 `/vendor/etc/acdbdata/Forte` | 日志出现 `Load file: …/Forte/MTP_ukee_acdb_cal.acdb`，错误推进到 `gsl_init` 层 | 真机 |
| GSL 模块注册被 DSP 拒绝 | `Register modules cmd 0x100101e failure:9`（`ar_log` 拦截） | `PAL_SERVER_AGM_INIT_RC` 不再是 `-114` | 待新镜像 |
| 亮度被服务覆盖 | 真机 `BRIGHT saved=40 now=3` | `apply` 返回 `ok want=40 observed=40 previous=3`（运行时时序修复仍需开机验） | 真机（工具级） |
| hisysevent sink 静默死亡 | 真机 `pidof` 空、`/dev/unix/socket` 仍留 2 个僵尸 socket | `HISYSEVENT_SINK=<pid> sockets=2`、`HISYSEVENT_ERRNO2=0` | 真机（可拉起）+ 镜像（监督块） |
| 主机路径启动 musl PIE | `timeout 3 /hmos-root/compat/bin/hmos_hisysevent_sink` → `rc=127 can't execute: No such file or directory` | `chroot $R /compat/bin/…` → `pidof` 返回 pid | 真机 |
| `if/fi` 嵌套写坏 | `11e` 里亮度 watcher 被嵌进 sink 分支，`else` 绑到内层 `if`（`bash -n` 仍通过） | 现在 `if=30 / fi=30` 配平，且拆成 `11e`/`11f` 两块 | 静态 |
| `$ART` 未定义导致桌面不来 | start `6a` 用了 `$ART` 才赋值，`set -u` 直接中止 → `6b–9` 全不执行，桌面不出现 | `ART` 在首次使用前定义；`MARBLE_PAL_READY` 等标记连续输出 | 静态（历史事件） |
| 蓝牙补丁"无效" | 真机只有一条 `HCI_TIMEOUT opcode=0xfc00`，无任何 `EDL_VERSION_ATTEMPT`；设备二进制 `1f4d9f9b…` ≠ 重建版 `62b09485…` | 新镜像应出现 `EDL_VERSION_ATTEMPT n=1..3` 与 `CHECK_RESULT=ok` | 待新镜像 |
| HAP 安装被拒 | `bm install` → `code:9568266`；hilog `MSG_ERR_INSTALL_PERMISSIO…` / `error: install permission denied` | 原生入口安装回调成功，独立包查询返回目标版本，真实启动 | 4101 调度和时钟包只读查询已过；实装待验 |
| WLAN 开关/扫描失败 | D 业务返回 `1`，未到 WPA；E 业务返回 `0`，设备管理端 `LoadDevice` 返回 `-202` | F 恢复芯片 HDI 和 WPA 按需调度，设置实际列出附近 AP；G 保留相同 Wi-Fi 配置，连接后有地址、默认路由，网关和公网各回包 2/2 | 真机扫描、关联与 IP 收发；DHCP、DNS/HTTPS、冷启动重连待验 |
| 服务权限丢失 | `disk_manager` / `StorageSpaceMgr` / `device_manager` / `hiview` 的 `permission` 集合比 donor 少 11/12/26/50 条 | `tests/test_marble_service_parity.py` 集合相等断言 | 静态 |
| 启动块静默丢失 | `11d`、蓝牙块、USB/HDC 块先后整段消失（`grep` 0 命中） | `tests/test_marble_boot_blocks.py` 21 个标记 + 顺序断言 | 静态 |
| 镜像里装的是旧二进制 | 旧镜像 `hmos_bt_bridge_marble=1f4d9f9b…`，源码已 `62b09485…` | `ImageBinaryFreshnessTests` 断言镜像内 sha 等于本地构建产物 | 静态 |

## 使用方式

刷机前先跑 `sh /hmos-root/compat/verify/marble_identity.sh` 存一份基线；
刷机后再跑一次，与 `docs/MARBLE_EXPECTED_AFTER_FLASH.md` 的期望值逐行 diff。
**没有基线就没有对照**，事后回忆不算证据。