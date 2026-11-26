# marble DCM 软 TEE：让 `getDeviceToken` 在本地返回设备 token

本文记录 Batch U4：用 clean-room 服务替代华为硬件 TEE 支撑的 SA 65866
（`dcm_service`），使 `DeviceSecurityKit::getDeviceToken` 在 marble 上不再返回
`-10002`，而是返回一个由**本机真实设备值**（序列号 / 型号 / 可选 UDID）派生的
本地 token。

> **边界（必须同时阅读）**：该 token 由本机 HMAC 密钥签名，**不是华为签发的设备
> 证明**，华为云端（AppGallery / 主题商店的 HSDR 业务）不会认可。它只解决
> “本地 API 失败”这一层；商店服务端的设备认证需要官方设备凭证路径。
> 本项目不伪造华为根签名的证书链，也不把本地 token 记作“华为证明通过”。

## 1. 现场事实与调用链

U1.6 / U2.1 的 `hiappevent` 采样（`docs/MARBLE_BATCH_PLAN.md`）显示：

- `AccountKit::AuthorizationWithHuaweiID` 与 `RemoteCommunicationKit::fetch` 返回 `0`；
- `DeviceSecurityKit::getDeviceToken` 返回 `-10002`；
- 设备上没有 `teecd` 进程、`/dev/tc_ns_client` 或可用的设备证书 HDI 实现。

从 donor ROM `ADY-AL10 207.0.0.111(SP10)` 逆向得到的真实链路：

```
DeviceSecurityKit (应用/NAPI)
  libdevicecertificate_napi.z.so
    -> libdevice_cert_mgr_sdk.z.so        DcmGetDeviceToken()
      -> SAMGR SA 65866 (dcm_service)
        libdevice_cert_mgr_service.z.so    DeviceCertManagerService
          -> DeviceCertTokenHsdrConnection -> 华为云端（需要设备证书/TEE 证明）
          -> libdevice_certificate_manager_proxy_1.1.z.so -> vendor HDI -> teecd/TEE
```

原厂 `DcmServiceGetDeviceToken()` 先检查“模拟器”设备类型（返回 `-10003`），再取调用方
HAP 信息、`PrepareConn()` 连接 HSDR 云端、`GenerateDeviceTokenRequest()` 组装请求，
最终由 `GetDeviceTokenFromCloud()` / `HandleDeviceTokenCloudResponse()` 回填 token，
通过回调 `OnDeviceTokenFetched(requestId, errCode, token)` 返回。

在 marble 上硬件 TEE 与华为设备证书缺失，云端路径不可能成功；因此 U4 直接在
SA 65866 这一层提供本地 token。

## 1.1 应用商店登录的真实断点（2026-11-25 现场）

现场 `vendor_boot_a=cbdd3cf9…`、boot `3f474e17…`。DCM 服务与 SA 65866 正常，
`getDeviceToken` 已成功（`TOKEN_ISSUED ... cb_rc=0`），本地证书链/状态也在被调用。
但 AppGallery 登录仍失败：

```
[UcsTokenManager]: updateCredential error 631008 : get anon attestation failed: 12000001
[AuthManager]: signInWithCode error: 630102
[AccountManager]: manualLogin signInWithCode fail: 630102
HUKS: AttestKeyCoreOp: HuksAttestKey fail, ret = -4.
HUKS: HksAnonAttestKey: leave AnonAttestKey, result = -4
```

登录走的是 **HUKS 匿名证明**，不是 DCM 的 `getDeviceToken`：
`HuksEccSigner.getAnonAttestation` → `HksAnonAttestKey` → `HksServiceAttestKey`
→ `HuksHdiAttestKey`（软 HUKS 原本直接返回 -4）→ 若成功才进入
`DcmGenerateCertChainInAttestKey` → `DcmAnonymousAttestKey`（SDK）→ SA 65866 code 2。

对应修复（源码已合并，未打包）：

- `compat/huks/soft_huks_engine.c` 的 `soft_attest_key` 不再返回
  `HKS_ERROR_NOT_SUPPORTED`，改为成功并保留调用方给的 `certChain` 容量（不写内容），
  由后续 `DcmGenerateCertChain` 用同一缓冲写入本地链；
- DCM code 2（`HandleAnonymousAttestRemoteRequest`）返回本地链：
  reply `int32 0 + uint64 requestId`，回调 descriptor
  `ohos.security.devicecertmanager.dcmAnonymousCallback`、code 1，
  数据为 `requestId + errCode + errInfo + certCount(2) + [len + DER]×2`
  （DER 用 `Parcel::WriteUnpadBuffer`，与 SDK 的 `ReadUnpadBuffer` 对称）。

本地链不会被华为服务器接受；“本地 API 成功”与“云端登录成功”仍需分开记录。

## 1.2 云端门禁在哪：UCS/TSMS 侧逆向结论（2026-11-25 补查）

上一节只说明了「本地为什么会失败」。这一节回答「本地过了之后云端会不会过」，
证据全部来自 donor ROM 里的真实二进制，不靠推测。

### 1.2.1 客户端实现位置

`fs/system/system/lib64/platformsdk/libcloud_app_identity_auth_api.z.so`
（sha256 `7ea335d0…9107102`，79 448 B）就是日志里 `OUCS` / `[APPAUTH]` 的来源，
导出 `OHOS::UCS::UcsClient::{Init,ApplyCredential,SignWithCredential,
VerifyWithCredential,EncryptWithCredential,DecryptWithCredential,
GetAccessKeyWithCredential}`。依赖只有
`libcjson` / `libcrypto_openssl` / `libhukssdk` / `libsec_shared` / `libffrt` / `libhilog`。

**关键事实**：该库的动态符号里没有任何 `X509_*` / `d2i_*` / cert-framework 引用，
OpenSSL 侧只用到 `SHA256`、`EVP_EncodeBlock`、`EVP_DecodeBlock`（base64）。
即 **匿名证明证书链在客户端从不做验签/链校验**，只有 HUKS（`HksAnonAttestKey`）
产生链、`HksSign` 产生签名。链的裁决完全在服务端。

### 1.2.2 ApplyCredential 的实际报文（`UcsClient::ApplyCredential` @ `0xbee0`）

下表地址均为 `sub_BEE0`（`UcsClient::ApplyCredential`）内的实际引用点：

| 步骤 | 地址 | 内容 |
| --- | --- | --- |
| JWS header `alg` | `0xbfe4` / `0xbfec` | `alg = "ED256"`（ECDSA-P256-SHA256） |
| header `cty` | `0xc000` / `0xc008` | `cty = "anon_cert"`（硬编码，无模拟器分支） |
| `x5c` 数组 | `0xc01c` | 链逐张 base64 塞入 |
| 取链 | `0xc700` | `HksAnonAttestKey(&paramSet, inData, &certChain)`；失败日志在 `0xccac` |
| 链转 x5c | `0xcbac` / `0xcc2c` | `get anon certs` → `x5cArray add anon cert` |
| 载荷 `len` | `0xc398` | `len = 32` |
| `kid` | `0xc474` | header `kid` |
| 载荷算法域 | `0xc1a4` | `alg=6`，另有 `kekAlg=2` / `dkAlgLen=1` / `akskVersion=2` |
| `pbk` | `0xeb2c` | 设备公钥（`HksExportPublicKey` @ `0xc2f4`） |
| 签名 | `0xca50` | `HksSign`（用证明私钥签 `header.payload`） |
| host / path | `0xcf30` / `0xd044` | `com.huawei.tsms` + `/tsms/v2/credentials` |
| 请求头 | `0xd0cc` / `0xd114` / `0xd144` / `0xd170` | `Content-Type`、`X-ApplyCredential-Terminal`、`X-Request-ID`、`X-Service-Name` |
| body | `0xd1cc` | `{"request": "<JWS compact>"}` |
| 回读 | `0xd460` | 解析 `errorCode`/`errorMsg`，失败即打 `tsms response get error code` |

### 1.2.3 信任锚是华为根，且不在本地

`resources/rawfile/ucs/cbg_root.pem`（Push / HiCar）、
`resources/rawfile/grs_config/config/cbg_root.pem`（AppGallery 等几十个包）内容一致：

```
subject = C=CN, O=Huawei, OU=Huawei CBG, CN=Huawei CBG Root CA
sha256  = 3A:A8:1A:D7:B0:8F:01:58:FF:D2:AD:D7:60:2F:A1:B0:27:E4:91:D0:63:C8:42:37:2C:D3:FB:DA:9D:6B:8D:A0
```

这份根只用于 TLS/GRS 校验，不参与 x5c 链校验（见 1.2.1）。本地 CA
（`CN=marble local DCM root (NOT Huawei)`）不接在这条链上，
所以 `x5c` 里的两张自签证书对 TSMS 而言是“陌生根”。

### 1.2.4 端点现场可用性（2026-11-25 实测）

从开发机直连（经本机代理出网）：

```
GET  https://tsms-drcn.platform.dbankcloud.com/          -> 403（elb，正常）
POST https://tsms-drcn.platform.dbankcloud.com/tsms/v2/credentials
     Content-Type: application/json;charset=UTF-8
     {}                                                  -> HTTP 500 {"errorCode":"tsms.1000"}
```

结论：通道与错误码回传路径都在；请求体不合格时服务端会给出 `tsms.*` 码。
**判断“本地链能不能过”的最终证据，只能来自服务端返回的那一行
`tsms response get error code`**，本地无法预先判定。

### 1.2.5 唯一可能的“云端合法”分支：emulator credential

`sub_61DC`（凭据解析）校验 `cty` 只能是 `anon_cert` 或 `emulator_anon_cert`；
ArkTS 侧常量同名（`DEFAULT_ANON_CERT="anon_cert"`,
`EMULATOR_ANON_CERT="emulator_anon_cert"`, `SYSINTEGRITY_CERT="sysIntegrity"`，
见 `sys_prod/app/PetalMaps/PetalMaps.hap: ets/modules.abc`）。
另有 GRS 键 `TSMS_URL` / `HA_URL` / `KEY_COMPONENT_URL` / `CDN`。

说明华为侧存在“模拟器凭证”这一类型（服务端会签发 `emulator_anon_cert` 凭据）。
若 DevEco 模拟器镜像里带有一套**华为签发、被 TSMS 接受**的软 HUKS 匿名证书
（含私钥），那才是让云端会话真正跑通的路径；这需要单独去挖模拟器镜像，
与本地自签 CA 无关。

### 1.2.6 小结

- 能静态证明的：链不做本地校验 → 本地必然“通过”；裁决在 TSMS → 自签链预期被拒。
- 不能静态证明的：TSMS 对自签链返回的具体 `tsms.*` 码，以及 UCS 在该码下是否降级。
- 因此 `66d82fe1` 的验收判据不是“登录成功”，而是日志里 UCS 从
  `apply credential by tsms failed` 往前推进到 `tsms response get error code`，
  并把服务端码抄回来。拿到那个码，问题才算定性。

## 1.3 COM9 现场实测（2026-11-25 23:35–23:44，未刷机）

通道是 **COM9（USB ACM 串口）**，不是 adb；宿主侧用 `host-tools/Invoke-ProbeCommand.ps1`
（115200 8N1，`cmd <id> <base64>` 协议，脚本上限 2800 B）。

现场读数（`vendor_boot_a=778fb2a2…`，boot `5929394d-aa76-497d-ae12-4a9fee01c85d`）：

```
$ busybox sha256sum /dev/block/by-name/{boot_a,vendor_boot_a}
c83a8468…c9cae8  boot_a
778fb2a2…a712da  vendor_boot_a
DCM=4596          # hmos_dcm_service 正在跑
```

### 1.3.1 已刷镜像的 SA 65866 逐码回执（`/compat/bin/hmos_dcm_token_probe`）

```
code=1  ATTEST_IDS  rc=0 blob_len=808 cert_count=2
code=5  ATEST_IDS   rc=0 blob_len=808 cert_count=2
code=7  CERT_STATE  rc=0 len=4 state=1
code=10 CERT_STATE  rc=0 len=4 state=1
code=3  TOKEN_ISSUED request_id=… len=236 cb_rc=0
```

服务端日志（`/data/service/el1/public/device_cert_manager_server/hmos-dcm-service.log`）：

```
HMOS_DCM ATTEST_IDS local_chain len=808 caller_uid=20020149 ok=1
HMOS_DCM CERT_STATE local=1 caller_uid=20020149 ok=1
HMOS_DCM TOKEN_ISSUED caller_uid=0 request_id=0011f6406af8951a len=236 …
HMOS_DCM ANON_ATTEST local_chain dev=418 root=374 caller_uid=0 cb_rc=0
```

### 1.3.2 匿名证明（code 2）当场跑通

原来 `--code 2` 一直是 `rc=-1`，因为旧探针给 code 2 塞的是占位载荷（`32788, 0`），
服务端 `REJECT no callback object code=anon_attest`。补了真正的 code 2 驱动
（challenge + 回调 stub `dcmAnonymousCallback`）后，**当班镜像上直接通过**：

```
ANON_REQUEST challenge=32 write=1 unpad=0x…
ANON_CALLBACK request_id=0011f6e02cce1563 err=0 info= count=2 \
              cert0_len=418 head=3082019e cert1_len=374 head=30820172
ANON_REPLY errCode=0 requestId=0011f6e02cce1563
ANON_VERDICT=ok
```

`418` / `374` 就是本地 CA 的 device / root DER，头部 `30 82 01 9e` / `30 82 01 72`
是合法 SEQUENCE。即 **DCM 侧匿名证明链已经可用，不再需要改动**。

### 1.3.3 途中修掉的探针自身 bug（重要）

第一版 code 2 解析用了 `MessageParcel::ReadBuffer(len)`，读到的第二张证书长度是 `0`，
差点误判成服务端 bug。实测（`--parcel-selftest`，同一设备）：

| 写法 | 读回 |
| --- | --- |
| `ReadBuffer(len)` | `len0=418` → 下一字段 `24510464`（`0x01760000`，2 字节 padding 错位） |
| `ReadUnpadBuffer(len)` | `len0=418`、`len1=374`，`SELFTEST … = 对称` |

结论：`Parcel::WriteUnpadBuffer` 会补齐到 4 字节，`ReadBuffer` **不跳** padding，
`ReadUnpadBuffer` 才跳。所以读侧必须用 `ReadUnpadBuffer`。

同时用 IDA 在 donor 原件
`libdevice_cert_mgr_service.z.so: DcmAnonymousCallbackProxy::OnAnonymousAttestFetched`
（`0xa6bd0`）核对了写侧顺序，与我们的实现逐字段一致：

```
WriteInterfaceToken → WriteUint64(requestId) → WriteInt32(errCode) → WriteString(errInfo)
→ WriteUint32(certCount) → 每张: WriteUint32(len) + WriteUnpadBuffer(data, len)
```

### 1.3.4 剩下的唯一未验证环节

本地链路的 DCM 段已实测通过；**软 HUKS（`soft_huks_engine` 的 attest 分支与
`shim_attest_key` 的 64 字节 carrier）在 vendor_boot 里，只能靠刷 `66d82fe1` 验证**。
云端 TSMS 的裁决则无论本地怎么改都要等服务端回执。

免刷机复测入口（已上传到设备）：

```
busybox chroot /hmos-root /data/local/tmp/anon_probe --anon
busybox chroot /hmos-root /data/local/tmp/anon_probe --parcel-selftest
```

## 1.4 合并镜像刷新后仍失败的根因：HUKS HDI 库是旧版（2026-11-26 00:00–00:12）

用户刷新合并镜像后登录仍失败。COM9 现场取证（boot `1797b828…`，uptime < 700 s）：

```
[HUKS] DcmGenerateCertChain: DcmAnonymousAttestKey failed -10001
[DCM SDK] DcmCheckBlob[134]: blob is invalid params
[DCM SDK] DcmAnonymousAttestKey[88]: localAttestCert param is invalid
[AppGallery/OUCS] apply credential by tsms failed, code:631008
                  msg:get anon attestation failed: 12000012, HUKS operation failed.
```

关键判读：

1. `-10001` 出在 SDK 的 `DcmAnonymousAttestKey` **参数校验**（`size` 不在 1..0x2000 或 `data == NULL`），
   发生在 `SendDcmAnonymousAttestRequest` 之前，**没有 IPC**；
2. DCM 服务日志里 `grep -c ANON` 只有 2 条，且都是 `caller_uid=0`（我 23:41 用探针打的），
   即 HUKS 的匿名证明请求**从未到达 SA 65866**；
3. 说明 `certChain` 到 SDK 时已经退化成空 blob → 正是 `soft_huks_hdi.c` 里
   “64 字节 carrier” 补丁要解决的问题，而该补丁**不在**设备上的库里。

运行库比对（`/hmos-root/compat/lib64/`，三份副本同 hash）：

| 库 | 设备实际 | 当前源码重建 |
| --- | --- | --- |
| `libhuks_engine_core_standard.z.so` | `2ab58d7a…` 62 928 B | `2ab58d7a…` 62 928 B ✅ 一致 |
| `libhuks_hdi_service_1.1.z.so` | `abb74cf0…` 69 768 B | `0a50956f…` 69 984 B ❌ 旧版 |

`compat/huks/build_soft_huks.sh --cross` 重建结果可复现（两次构建 hash 完全相同），
且当前 `soft_huks_hdi.c` 明确含 carrier 分支；设备上的 69 768 B 版本比当前构建小 216 B，
不含该分支。也就是说：**刷进去的镜像里 HUKS HDI 是补丁之前的产物**，
引擎是新的（attest 返回成功），HDI 是旧的（不补 carrier），于是 HUKS 把空 blob 直接交给 DCM SDK。

### 1.4.1 修复与诊断增强

- `soft_huks_hdi.c` 的 `shim_attest_key` 增加两行日志，便于一次刷新就能定位：
  `HUKS_SHIM op=AttestKey in_data=… in_len=… synth=…` 与 `… engine_ret=… out_data=… out_len=…`；
- 自测同步到新契约：`engine.attest_defers_to_dcm` / `hdi.attest_defers_to_dcm`
  （旧断言 `attest_is_stub` 已过时，它与 `soft_attest_key` 返回 success 的现状冲突）；
- 现构建：`libhuks_hdi_service_1.1.z.so = 45466a67…`（70 088 B）；
- 重打镜像：`vendor_boot_h20_u4_dcm_soft_tee_912143fd.img`
  （`912143fd3ec536d38dd600ad209d3387b77c4f9f150658ccc25fc55b93e9961e`），
  基线与既有批次不变，仅替换 DCM 3 件 + HUKS 2 件。

### 1.4.2 刷新后的判读顺序

```
1) HUKS_SHIM op=AttestKey … synth=1 / engine_ret=0 out_data=1 out_len=64   ← HDI 已是新版
2) DCM 服务日志出现 ANON_ATTEST … caller_uid=<非 0>                        ← 请求真的到 SA 65866
3) HUKS 不再出现 DcmAnonymousAttestKey failed -10001
4) UCS 日志从 apply credential by tsms failed 变成 tsms response get error code
   —— 那串 tsms.* 才是云端裁决（见 1.2）
```

### 1.4.3 与《AppGallery 普通应用下载安装全流程》对照

用户提供的该文档（ark_disasm 级静态分析）给出两条与本节直接相关的结论：

1. §18.4：`sdk-facade` 的 TSMS appauth 由 HUKS 生成不可导出密钥 → `getAnonAttestation`
   取匿名设备证书链 → JWS（header `ED256`、`x5c` 放链、payload 带 `appId`/`packageName`/`pbk`）
   → POST `/tsms/v2/credentials` → **服务器验证证书链、JWS 签名、挑战值、安全等级、appId/包名**
   才下发 credential。这与 1.2 节逆向出的 SDK 行为逐项吻合，也说明本机自签链在服务端注定被拒。
2. §1/§18.2：`deviceToken` / `riskToken` / `TSMS` 属于设备信任链路，**不属于普通登录与普通下载的必选流程**。

因此“UCS 631008 → `signInWithCode` 630102”不能直接当作因果。现场复核显示当前全部失败都发生在
**silentLogin**（无已登录账号时为正常现象）：

```
[AccountManager]:preference dose not exist a user, try to get data by new auth code and refresh preference AG_START
[AuthManager]:signInWithCode error: 630102
[AccountManager]:silentLogin signInWithCode fail: 630102
[AccountManager]:doLoginOpt catch tryLoginOpt:"retryLogin fail"
```

`huawei_id_svc` 正常运行（HWID SA 在跑），排除账号服务缺失。要判定“登录异常”的真实根因，
必须抓一次**手动登录**的现场。

## 2. 逆向出的 wire contract（已按字节确认）

| 项 | 值 |
| --- | --- |
| 服务 descriptor | `ohos.security.devicecertmanager.service`（UTF-16，40 字符） |
| 请求 code | `3`（`SendDcmDeviceTokenRequest` 的 `transact` code） |
| 请求数据 | interface token + `WriteRemoteObject(callback)` |
| 同步 reply | `WriteInt32(errCode=0)` + `WriteUint64(requestId)`（`DcmReadAsyncRequestReply`） |
| 回调 descriptor | `ohos.security.devicecertmanager.dcmDeviceTokCallback`（UTF-16，52 字符） |
| 回调 code | `1`（`DEVICE_TOKEN_KEY_CALLBACK`） |
| 回调数据 | `WriteUint64(requestId)` + `WriteInt32(errCode)` + `WriteString(token)` |
| 回调 option | `TF_ASYNC`，8 字节 |

原厂 `DeviceCertManagerService` 构造函数里注册了 14 个 code。除 code 3 外，本实现统一在
reply 写 `int32 -10009`（原厂 `DcmAttestIdsCommon` 使用的 “not supported” 族码），不再写
其它字段；SDK 的通用 `SendRequest` 在 errCode 非 0 时不会读后续数据。code 与 handler 的
对应关系（按构造函数里 map 节点 key 与 handler 配对）：

| code | 原厂 handler | 本实现 |
| --- | --- | --- |
| 1 | `DcmIpcServerAttestIds` | 本地证书链 |
| 2 | `HandleAnonymousAttestRemoteRequest` | 返回 -10009 |
| 3 | `HandleDeviceTokenRemoteRequest` | 本地 token（本文主体） |
| 4 | `DcmIpcServerAttestSecInfo` | 返回 -10009 |
| 5 | `DcmIpcServerAttestIdsCompat` | 本地证书链 |
| 6 | `HandleCertUpdateRemoteRequest` | 返回 -10009 |
| 7 | `DcmIpcServerGetCertState` | 本地状态 |
| 8 | `HandleCertUpdateRemoteRequest`（无回调变体） | 返回 -10009 |
| 9 | `DcmIpcServerAttestIdsWithCertType` | 本地证书链 |
| 10 | `DcmIpcServerGetProvisionedInfoStatus` | 本地状态 |
| 11 | `HandleApplyAnonymousAttestRemoteRequest` | 返回 -10009 |
| 12 | `HandleLocalApplyAnonymousAttestRemoteRequest` | 返回 -10009 |
| 13 | `HandleAnonymousAttestRemoteRequestSe` | 返回 -10009 |
| 23 | `HandleApplyHctcAttestRemoteRequest` | 返回 -10009 |

服务端使用 donor `libipc_single.z.so` 的 `IPCObjectStub` 构造（descriptor 同上），
通过私有 vtable 替换 `OnRemoteRequest`（槽位 `200/8`，与 `native_screenlock_request`
相同的已上机验证模式）。SA 注册复用 donor `libsamgr_proxy.z.so` 的
`ISystemAbilityManager` 子对象 thunk（`_ZThn112_N4OHOS25SystemAbilityManagerProxy16AddSystemAbility...`，`sub x0, x0, #112` 后调 `AddSystemAbility(65866, stub, SAExtraProp)`）；
`SAExtraProp` 布局为 `{bool isDistributed; uint32_t dumpLevel; u16string capability;
u16string permission}`（56 字节，已与 `MarshalSAExtraProp` 对齐）。

## 3. token 派生

`compat/dcm/dcm_token.c`（无 OpenSSL 依赖，host 可自测）：

```
payload = {"v":1,"src":"hmos-soft-tee","sn":"<真实 SN>","model":"<真实型号>",
           "udid":"<可选>","req":"<requestId>","iat":<unix>}
body    = base64url(payload)
key     = SHA256("hmos-dcm-soft-tee|<sn>|<model>")
token   = "hmosdcm1." + body + "." + hex(HMAC-SHA256(key, body))
```

- SN/型号来自 stage 阶段 root 读取的 `/proc/bootconfig`（`androidboot.serialno`、
  `androidboot.hardware.sku`），写入 `/compat/etc/dcm-device.json` 供服务读取；
- 服务本身再回退 `/proc/bootconfig` / `/proc/cmdline`；
- 日志只打印 token 的 `SHA-256` 前 16 hex 与长度，不打印完整 token 或原始 SN；
- token 对同一设备稳定（`iat`/`req` 随时间变化），可用本地 key 复核 HMAC。

## 4. 组件

| 文件 | 说明 |
| --- | --- |
| `compat/dcm/dcm_token.c` / `.h` | SHA-256、HMAC-SHA256、base64url、token 组装、host 自测 |
| `compat/dcm/hmos_dcm_service.cpp` | SA 65866 clean-room 服务（注册 + code 3 + 回调） |
| `compat/dcm/hmos_dcm_token_probe.cpp` | 设备侧客户端探针（按原厂协议请求并接收 token） |
| `compat/dcm/dcm_local_ca.c` / `.h` | 本地 CA：P-256 根 + 设备证书，链 blob 序列化 |
| `devices/marble/runtime/dcm/init-dcm-marble.cfg` | init 服务 `hmos_dcm_service`（`param:marble-u4-dcm` + `post-fs-data` + `services:hmos_dcm` job 启动） |
| `tools/build/build_current.py` | `dcm-token` / `dcm-service` / `dcm-probe` 目标 |
| `tools/packaging/build_marble_h20_u4_dcm_soft_tee.py` | 基于 U3.2b `d5299691` 单变量打包 U4（同步移出 `sceneboard-optional-sa.list` 中的 `65866`） |
| `tests/test_marble_dcm_soft_tee.py` | token 向量、wire contract、init、打包器离线测试 |

服务名使用 `hmos_dcm_service`（不占用原厂 `dcm_service` 服务名）；SA 号仍是 `65866`。
原厂 `dcm_service`（`/system/bin/sa_main` + 原厂库）在缺席时由 SAMGR 按需拉起会超时失败；
此前 U2.1–U3.2b 镜像的 `/compat/etc/sceneboard-optional-sa.list` 仍含 `65866`，因此
`build_marble_h20_u4_dcm_soft_tee.py` 在 `stage-phase4-marble.sh` 中移除 `65866` 并在
`start-phase4-marble.sh` 设置 `marble.debug.start.u4_dcm=true` 拉起 `hmos_dcm_service`。

## 5. 构建与打包

```sh
export HMOS_SDK=$PWD/work/ohos-sdk/installed/native \
       HMOS_UPSTREAM=$PWD/work/upstream \
       HMOS_STAGE=$PWD/work/marble-stage \
       HMOS_DONOR_LINK=$PWD/work/donor-hdi-link
python3 tools/build/build_current.py --out work/marble-dcm-build dcm-service dcm-probe
python3 tools/packaging/build_marble_h20_u4_dcm_soft_tee.py --out /mnt/e/marble-tmp
```

打包器以 `vendor_boot_h20_u32b_heif_codec_d5299691.img` 为基线，校验 35 个既往批次
保护成员逐字节不变，只新增 3 个成员并 patch `hmos-init-debug.cfg`
（import `/compat/etc/init-dcm-marble.cfg`）、`stage-phase4-marble.sh` 与 `start-phase4-marble.sh`。
当前已验收镜像为 `/mnt/e/marble-tmp/vendor_boot_h20_u4_dcm_soft_tee_cbdd3cf9.img`
（`cbdd3cf9fa4105de526e85d21e2b69f41aedae5d93708d85c59cecb4bb51b713`）。

## 6. 上机验收

只刷 `vendor_boot_a`（`boot_a` 不动），刷后立即 `set_active a`：

```sh
# 控制脚本（preflight 会核对 boot_a、当前 vendor_boot、slot 和电量）
work/marble-dual-20261123/flash-h20-u4-control.sh preflight
work/marble-dual-20261123/flash-h20-u4-control.sh apply <当前 boot_id>
```

COM9 串口检查（只读）：

```sh
# 1) 服务是否注册了 SA 65866
/compat/bin/native_sa_check 65866
# 2) 服务日志
tail -c 2000 /data/service/el1/public/device_cert_manager_server/hmos-dcm-service.log
# 3) 按原厂协议请求 token（--dump-token 才输出 token 全文）
/compat/bin/hmos_dcm_token_probe
/compat/bin/hmos_dcm_token_probe --dump-token
# 验证本地证书状态（7/10）、本地证书链（1/5/9）及不支持分支（2/23 返回 -10009）
/compat/bin/hmos_dcm_token_probe --code 7
/compat/bin/hmos_dcm_token_probe --code 1
/compat/bin/hmos_dcm_token_probe --code 23
# 通过原厂 libdevice_cert_mgr_sdk.z.so 验证真实状态接口
/compat/bin/hmos_dcm_token_probe --sdk-state
```

COM9 实机验收输出（`NEW_BOOT=3f474e17-b0b6-4e26-ba28-73f2c58f8c79`，`vendor_boot_a=cbdd3cf9...`）：

```
NATIVE_SA_READY id=65866
PROBE_SA_65866=ready code=3
CALLBACK_REQUEST_ID=0011dd22b9c2b21d ERRCODE=0 TOKEN_LEN=236 TOKEN_DIGEST=058d8447e97d2a17
REQUEST rc=0
REPLY errCode=0 requestId=0011dd22b9c2b21d
TOKEN_CALLBACK=ok errCode=0 token_len=236
```

其余分支与 SDK 实测输出：

```
CERT_STATE_CODE code=7 rc=0 len=4 state=1
CERT_STATE_CODE code=10 rc=0 len=4 state=1
ATTEST_IDS_CODE code=1 rc=0 blob_len=808 cert_count=2
ATTEST_IDS_CODE code=5 rc=0 blob_len=808 cert_count=2
ATTEST_IDS_CODE code=9 rc=0 blob_len=808 cert_count=2
UNSUPPORTED_CODE code=2 rc=-10009 expected=-10009
UNSUPPORTED_CODE code=23 rc=-10009 expected=-10009
SDK_CERT_STATE rc=0 state=1
SDK_PROV_STATUS rc=0 status=1
```

服务日志实录（除主动探针外，开机已有真实系统调用方 `uid=7023`/`uid=1024` 成功访问）：

```
HMOS_DCM BOOT pid=4573 uid=3562 dcm-soft-tee
HMOS_DCM STUB_READY descriptor=ohos.security.devicecertmanager.service slot=25
HMOS_DCM ADD_SA id=65866 rc=0
HMOS_DCM READY sa=65866
HMOS_DCM DEVICE model=marble sn_len=8 udid_len=0
HMOS_DCM ATTEST_IDS local_chain len=808 caller_uid=7023 ok=1
HMOS_DCM CERT_STATE local=1 caller_uid=1024 ok=1
HMOS_DCM TOKEN_ISSUED caller_uid=0 request_id=0011dd22b9c2b21d len=236 digest=058d8447e97d2a17 cb_rc=0
```

随后在应用市场 / 主题商店触发一次需要设备令牌的操作，观察 `hiappevent`
`DeviceSecurityKit::getDeviceToken` 是否从 `-10002` 变为 `0`。本地 token 不会被
华为云端接受，商店服务端业务结果可能仍失败；这两层必须分开记录。

失败排查：

| 现象 | 处理 |
| --- | --- |
| `PROBE_SA_65866=missing` | 看服务日志 `ADD_SA` / `WAIT samgr`；确认 `param:marble-u4-dcm` job 执行、二进制在 `/compat/bin` |
| `ADD_SA rc=38` 或无 `ADD_SA` | 核对 `_ZThn112_` 子对象 thunk（`x0 - 112`）与 SAMGR 权限；查 `hilog` 的 SAMGR 记录 |
| `TOKEN_CALLBACK=timeout` | 回调 descriptor/code 或 `TF_ASYNC` 不匹配；核对第 2 节表格 |
| 服务崩溃 | `dmesg` / `hilog` 找 `dcm_service` 的 avc denial 或段错误；检查 `secon` 与 `/compat/bin` 执行权限 |

回退：`flash-h20-u4-control.sh restore <boot_id>` 恢复 U3.2b
`d5299691…`。

## 6.1 本地证书链与状态

`compat/dcm/dcm_local_ca.c` 在首次请求时于
`/data/service/el1/public/device_cert_manager_server/local-ca/`（`0700`，文件 `0600`）生成：

- `root.pem` / `root-key.pem`：P-256 自签根，`CN=marble local DCM root (NOT Huawei)`，
  `basicConstraints=critical,CA:TRUE`，10 年；
- `device.pem` / `device-key.pem`：P-256 设备证书，`CN=marble-local-<sn_sha256 前 16 hex>`，
  自定义扩展 `1.3.6.1.4.1.99999.1` 写 `local-dcm;model=<model>;sn_sha256=<hash>`，1 年，
  由根签发；设备证书过期或签发者不匹配时自动重签，根保留。

链序列化严格按 `DcmClientUnpackCertChainFromService` 的格式：
`uint32 certCount=2` + 设备证书 `uint32 len + DER + pad` + 根证书 `uint32 len + DER + pad`。
证书均为 DER X.509，顺序为设备证书在前、根在后。

- code 1/5/9：reply `int32 0 + uint32 len + blob`，本地链；
- code 7/10：reply `int32 0 + uint32 4 + int32 state`，`state=1` 表示本地链可用；
- code 2（匿名证明）：reply `int32 0 + uint64 requestId`，回调 `dcmAnonymousCallback` code 1 返回本地链（`certCount=2`，设备证书 + 根）；
- code 4（challenge 签名）与 6/8/11/12/13/23（证书申请/更新）仍返回 `-10009`。

## 6.2 当前状态（2026-11-25 22:40）

- 已合并：本地 CA/证书链（1/5/9）、本地状态（7/10）、本地 token（3）、
  匿名证明 code 2、软 HUKS 放行。
- 未合并：证书申请/更新（6/8/11/12/13/23）与 attest sec-info（4）仍为 `-10009`。
- 已刷入 `778fb2a2`（2026-11-25 22:50）：DCM code 2 未收到请求，HUKS 日志为
  `DcmAnonymousAttestKey[88]: localAttestCert param is invalid`（`DcmCheckBlob` -10001），
  说明 HUKS 传给 SDK 的 `certChain` data 为 NULL，SDK 在 IPC 前就拒绝。
- 已修复并打包新候选 `66d82fe1…`：
  ① `soft_huks_hdi.c` 的 `shim_attest_key` 在 `certChain` 为空时合成 64 字节 carrier，
  让 `DcmAnonymousAttestKey` 通过本地校验；
  ② DCM code 2 的回调延迟 200ms，等 `DcmGenerateCertChain` 注册 requestId 后再发送，
  否则 `HksDcmCallback` 的 `GetProxyWithoutLock` 找不到 proxy，链会被丢弃。
  未刷入，等待授权。

## 7. 未闭合项

1. 本地 token 不是华为签发；商店云端设备认证仍需官方设备凭证（不可由本仓库伪造）。
2. 应用市场 / 主题商店界面交互触发的 `DeviceSecurityKit::getDeviceToken` 与服务端业务结果仍需真机实操分别复核。
3. `getDeviceToken`、attest ids/cert state（1/5/9/7/10）与匿名证明（2）已有本地后端；
   证书申请/更新（6/8/11/12/13/23）与 attest sec-info（4）仍返回 -10009，需要原厂
   `dcm_service` + TEE + 云端。
4. 本地证书链只用于本地消费者；它不是华为签发，任何依赖华为信任根的验证方都不会接受。
   根私钥是软件密钥，与 TEE 保护的设备密钥不是同一安全等级。
5. （已闭合）`u:r:dcm_service:s0` 在 `/compat/bin/hmos_dcm_service`、`param:marble-u4-dcm` 与 `AddSystemAbility(65866)` 已于 `cbdd3cf9` COM9 实机启动并注册成功（`rc=0`）。
