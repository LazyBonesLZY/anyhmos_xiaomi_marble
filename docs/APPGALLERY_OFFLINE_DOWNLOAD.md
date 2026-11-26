# AppGallery 全链路逆向 → 脱机 HAP 下载（进行中）

目标：不经过 AppGallery 应用本身，用主机侧脚本拿到商店数据（详情 + 下载地址）并直接下载
HAP（示例包：微信 `com.tencent.wechat`），最后按 `sha256`／`fileSize` 校验。
本文只记录**读接口**的逆向与实测，不涉及安装、不写入设备。

## 1. 素材与产物

| 产物 | 路径 |
| --- | --- |
| AppGallery 包内 ArkTS 字节码（23 个模块） | `work/appgallery-re/abc/*.abc` |
| GRS 路由表（应用级 + SDK 级） | `work/appgallery-re/cfg/grs_app_global_route_config.json` 等 |
| 主机侧客户端 | `tools/appgallery/ag_web.py` |

从 `entry.hap: resources/rawfile/grs_config/router/grs_app_global_route_config.json`
拿到 `AppGalleryForHarmony / com.huawei.hmsapp.appgallery` 的 DR1（CN）地址：

```
STORE       = https://store-drcn.hispace.dbankcloud.com
AHEADCONN   = https://appdl-drcn.dbankcdn.com          # 下载 CDN
CDNBACKUPDOMAIN1/2/3 = https://appdl-{1,2,3}-drcn.dbankcdn.com
SEARCH      = https://aisearch-drcn.hispace.dbankcloud.cn
REPORT      = https://metrics1-drcn.dt.dbankcloud.cn:443
```

字节码里的客户端常量：`clientId = 108377959`、`encodingCode = 0200`、
`storeUrlSuffix = /hwmarket/harmony/client`（另有 `/hwmarket3/harmony/client` 等区域变体）。

## 2. 两条可走的路

### 2.1 Web 端 API（已跑通，`web-drcn.hispace.dbankcloud.com/edge`）

依赖一个「接口码」把守：

```
GET  /edge/webedge/getInterfaceCode     -> 一段 JWT（432 字符）
后续请求头： interface-code: <jwt>_<epoch_ms>
```

不带该头 → `403 {"rtnCode":1002,"rtnDesc":"InterfaceCode Verification failed."}`。
带上即通过（实测 `rtnCode 0`）。已确认存在的端点：

| 端点 | 方法 | 实测结果 |
| --- | --- | --- |
| `/webedge/getInterfaceCode` | GET | 200，返回 JWT |
| `/webAppLink/getAppDownloadUrl` | GET `appid`,`pkgName` | 200 `{"rtnCode":0,"url":...}`，但**只给 AppGallery 自己的 APK**（白名单） |
| `/appinfo/query` | POST `pkgNames`/`appIds` | 200 `{"rtnCode":-1,"rtnDesc":"queryOpenAppInfos is error"}` |
| `/index/gettabdetail` | POST | 400 需要 body 参数 `clientTabDetailReq` |
| `/harmony/client-page-detail` | POST | 400 `buildPage.pageDetailReq.pageSize/pageNum must not be null`；扁平 body → 200 `rtnCode 2` |
| `/substance/detail` | GET | 502 |

`POST /webAppLink/getAppDownloadUrl`（表单或 JSON）固定 502，说明该端点只收 GET。

### 2.2 客户端 API（`store-drcn.hispace.dbankcloud.com/hwmarket/harmony/client`）

`GET` 该路径返回 `405 {"rtnCode":1,"rtnDesc":"Only POST and the requests in the WhiteList are supported!"}`
→ 方法名在 **body** 里而不是路径里。`POST` 目前一律 502（无 body），说明还需要完整的
请求信封（字节码里可见 `deviceId`/`clientVersion`/`appVersion`/`deviceModel`/`locale`/
`serviceToken`/`X-Authorization`/`X-Request-Source` 等字段，以及 `encodingCode=0200` 的编码选择）。

字节码里可用的方法名（节选）：

```
client.getPageDetail        client.fetchHarmonyFiles      client.queryClientHarmonyAppInfo
client.getCloudChallenge    client.getNegotiationKey      client.user.harmonySyncAppSwitch
client.harmonyos.agdSecurityVerify   client.intelcontrol.applist  client.installListBatchReport
```

下载相关的 bean 字段已经确认：`downloadUrl`、`sha256`、`fileSize`、`moduleId`、`moduleType`、
`compressInfo`、`encType`（`download-install-default` / `app-view-4-developer` 模块）。


## 2.3 实测突破（2026-11-26）

带 `interface-code` 后三个端点全部可用：

| 调用 | 结果 |
| --- | --- |
| `POST /edge/webedge/appinfo {"pkgName":"com.tencent.wechat"}` | **200，返回微信完整档案**：`appId=C5765880207854244859`、`id=d3a0edf847ca47fd992f14b789ceaf57`、`name=微信`、`version=8.0.23.37`、`size=517353204`、`sha256=7b09e56e4da31463b1806fd9e34404ca22788019b6fb7461cb40a4d3f1e54a0d`、`ctype=17`、`packingType=4`、`detailId=app\|C5765880207854244859`；`downUrl` 为空 |
| `GET /edge/uowap/index?method=internal.getTabDetail&serviceType=20&reqPageNum=1&uri=app%7CC5765880207854244859&maxResults=25&locale=zh_CN&countryCode=CN` | 200，返回 30KB 布局 JSON（`layoutData[].dataList[].list[]`） |
| 同上 `uri=pkgName\|com.tencent.wechat` / 无 `uri` | 200 但 `layoutData` 为空 |

布局 JSON 的每个 app 元素都带这些字段（推荐卡里 `downurl` 为 null）：

```
package / appid / name / appVersionName / versionCode / size / fullSize
sha256 / md5 / ctype / realCtype / sizeDesc / kindName / tagName / downurl
```

即：**`downurl` 就在详情卡片里**，只要命中「详情」卡片（web 端组件名
`detailheadcard` / `detailhiddencard`，见 bundle 里的 `case "detailheadcard"`）。
当前返回的是 `horizonhomecard`（相关推荐），说明 `uri=app|…` 命中的是「相关推荐」页而不是详情页，
下一步要换 `uri`/`tabId` 或改用 `clientTabDetailReq` 里的详情请求字段。


## 2.4 第二轮：CDN 命名规律 + POST 502 的真正原因（2026-11-26）

### 2.4.1 CDN 对象名 = appinfo 的 `id`

拿 appmarket 验证：`POST /edge/webedge/appinfo {"pkgName":"com.huawei.appmarket"}` 返回
`id=7dfbe78d41bd438bbf402848eff97548`，而商店给出的官方下载地址是

```
/dl/appdl/application/apk/7d/7dfbe78d41bd438bbf402848eff97548/com.huawei.appmarket.2611110925.apk
```

即路径 = `/dl/appdl/application/apk/<id 前两位>/<id>/<pkg>.<token>.apk`，
`id` 与 appinfo 完全一致；该地址 302 到 `appdl-1-drcn.dbankcdn.com` 后 `206` 可下载。
剩下唯一未知是尾部的 `<token>`（appmarket 是 `2611110925`，与 `versionCode=160801300` 无关）。

实测：把 `<token>` 换成 `versionCode`、去掉、或换成 `.hap`（`hap/`、`app/` 目录）全部
`403 AccessDenied`，说明对象名必须逐字匹配，`<token>` 只能从详情/文件接口取。

### 2.4.2 POST 502 的真正原因：缺 `apsid/sid/ts/ver`

从 bundle 里挖到 web 客户端的公共参数构造函数：

```js
function d(e){ var a=Date.now();
  p = (e.watchType===undefined ? 26 : e.watchType);
  var t={apsid:l, sid:a, ts:a, serviceType:p, ver:"1.1", clientCapability: <getter>};
  Object.assign(e,t); return e; }
function _(e,a){ return axios.post(e,a) }        // 全部是 POST
```

补上 `apsid/sid/ts/ver` 后，`POST /edge/uowap/index` 由 **502 变为 200**
（`rtnCode 0`）。这是本轮最重要的解锁点。

### 2.4.3 方法名清单与 `sign` 机制

bundle 里出现的全部 `method:` 字面量：

```
internal.getTabDetail   internal.getTemplate   internal.getNewHotSearchList   internal.completeSearchWord
client.getTabDetail     client.getLayoutDetail  client.startDownloadRep  client.downloadResultRep
client.installResultRep client.diffUpgrade2     client.completeSearchWord
```

`client.*` 必须在参数里带 `sign`，而 `sign` 来自"front"引导接口
（`frontInterface → POST /uowap/index`，返回 `{sign, tabInfo[]}` 并缓存为 `FRONT_DATA`）。
详情页调用形态（bundle 原文）：

```js
{clientPackage, deviceId, deviceIdType, method:"client.getTabDetail",
 sign:<front sign>, uri:<front tabId>, watchType, deviceSpecParams}
```

也就是说：**没有 front 的 sign，就拿不到 `client.getTabDetail` 的详情卡片**，
而 `internal.getTabDetail` 只给 `horizonhomecard`（相关推荐，`downurl` 全为 null）。

### 2.4.4 静态语料

web 端 25 个 chunk 已全量下载到 `/tmp/agweb/`（可复现：manifest 里的映射表
`js/<id>.<hash>.js`）。详情卡片渲染分支已定位：`case "detailheadcard"` /
`"detailhiddencard"` / `"detailappinfocard"`，字段名 `downurl/sha256/md5/size/ctype/appid`
与接口返回一致。


## 2.5 第三轮：HarmonyOS 详情页 API 被完整复原（2026-11-26）

### 2.5.1 `/harmony/page-detail` —— 详情页全量数据

从 chunk 10/11/12 里逆出调用形态：

```js
POST /edge/harmony/page-detail
header: interface-code: <jwt>_<ms>, Net-Msg-Id: <uuid>
body: {
  pageNum: 1, pageSize: 25,
  pageId: "<prefix>|<key>",            // prefix 见 env.js
  businessParam: { animation: 0 },
  clientPackage: "com.huawei.hmsapp.appgallery",
  accountZone: "CN"
}
```

`env.js` 里的 pageId 前缀（已全部拿到）：

```
harmonyPageDetailPrefix       = webAgAppDetail
harmonyPageDetailPkgPrefix    = webAgPackage
harmonySubstanceDetailPrefix  = webAgSubstanceDetail
harmonyPageOrderPrefix        = webAgAppOrder
harmonyPrivacyIntroPrefix     = webAgPrivacyIntro
harmonyPrivacyDataCollectPrefix = webAgAppPrivacyDataCollect
harmonyClientPackage          = com.huawei.hmsapp.appgallery
harmonyAccountZone            = CN
```

实测：`pageId = webAgAppDetail|C5765880207854244859` → **200，46 519 字节，rtnCode 0**，
6 个 layoutData 区块；核心的 `pages[0].data.card.data[0].list[0].refs_app` 就是完整应用档案：

```
appId C5765880207854244859   packageName com.tencent.wechat   name 微信
version 8.0.23.37            versionCode 8394533              sha256 7b09e56e…4d3e54a0d
id d3a0edf847ca47fd992f14b789ceaf57   ctype 17   minSdkLevel {hmosApiVersion:60002, ohosApiVersion:22}
adaptInfo {btnDisable:0, nonAdaptType:0}   distributionControlCode 10000000010
```

注意：`pageId` 用 **appId**（`C…`）才有内容；用 `pkgName` 会返回 `rtnCode 102
"app is filtered by appInfo"` + 空 `layoutData`（脚本已自动把 pkgName 解析成 appId）。

### 2.5.2 结论：web 层拿不到文件地址

- 全部 6 个 layoutData 区块、以及 `webAgPackage|com.tencent.wechat`（46 513 字节）
  里都没有 `downurl / packageUrl / hap / moduleName / fileSize`；
- `refs_app.detailId` 里带的是**页面埋点**参数（`aglocation`、`layoutBuilderTraceId`），不是下载令牌；
- 页面通过 `flayout://…appdetailhiddencard?…&sign=…` 交给客户端渲染，下载按钮由客户端接管。

### 2.5.3 客户端协议的签名/加密证据

`@hw-ae-foundation+network-framework-default` 的字节码里可见：

```
methodCodeMap   bodyParam   bodyLength   encrypt   signature   signAlg
appIdentifier   X-Authorization   deviceId   serviceType   clientVersion
```

即原生客户端请求体是**带方法码 + 签名/加密**的封装（`encodingCode = 0200`），
这也是 `store-drcn/hwmarket/harmony/client` 对我们裸 POST 一律 502 的原因：
网关在校验签名层就挡掉了，进不到业务逻辑。


## 2.6 第四轮：原生客户端协议被打开，且下载接口直指 TSMS（2026-11-26）

### 2.6.1 信封终于对上了

`store-drcn/hwmarket/harmony/client` 的正确姿势（实测）：

```
POST /hwmarket/harmony/client?apsid=<s>&sid=<s>&ts=<s>&ver=1.1&serviceType=20
                              &encodingCode=0200&clientId=108377959&method=client.<Name>
Content-Type: application/json;charset=UTF-8
body: { ...扁平字段... }          # 不是嵌套信封
```

三个关键点（都是试出来的）：

1. `method` 必须放在 **query string**；放 body 里会被 WAF 直接 502；
2. `Content-Type` 若是 `application/x-www-form-urlencoded` 而 body 是 JSON，服务端回
   `{"rtnCode":1,"rtnDesc":"Failed to get path mapping !"}`（说明 method 没被识别）；
3. body 里出现 `pageDetailReq` 之类的**多余包装**会让校验退回
   `getPageDetail.pageDetailReq.pageSize: must not be null`——正确做法是**扁平**字段。

### 2.6.2 方法映射表（实测 MAPPED / 未映射）

| method | 结果 |
| --- | --- |
| `client.getPageDetail` | MAPPED，需 `pageSize`+`pageNum`；扁平 body 进到下一步 `miss leagueAppId` |
| `client.queryClientHarmonyAppInfo` | MAPPED，需 `bundleName`+`queryScene`，全场景 0..6 均 `rtnCode 2 FAIL` |
| **`client.fetchHarmonyFiles`** | MAPPED，**`{"rtnCode":630102,"rtnDesc":"TSMS verify parameters blank."}`** |
| `client.getAppInfo` / `queryAppInfo` / `getTabDetail` / `getLayoutDetail` / `startDownload` 等 | 未映射（`Failed to get path mapping !`） |

`630102` 就是应用商店登录时那个错误码——**文件下载接口和登录共用同一道 TSMS 门禁**。

### 2.6.3 结论：HAP 直链必须过 TSMS

- web 层（page-detail / card-list）只给元数据，无文件地址（三轮验证）；
- `getAppDownloadUrl` 有 appId 白名单，只放 AppGallery 自己（Android 包 `com.tencent.mm`
  和鸿蒙包 `com.tencent.wechat` 都被拒）；
- 客户端 `fetchHarmonyFiles` 明确要求 **TSMS 校验参数**，即需要 UCS 拿到的 credential
  （签名/加密的请求体），而 credential 需要华为签发的匿名证明链。

### 2.6.4 Bug 记录：`Fetch-→ 206`

`client.fetchHarmonyFiles` 返回的 HTTP 状态是 **206** 而不是 200/4xx，业务码在 body 里
（`rtnCode 630102`）。脚本里判断成功要看 `rtnCode`，不能只看 HTTP 状态。


## 2.7 第五轮：签名机制定位 —— 整条客户端 API 都被 UCS 凭证锁死（2026-11-26）

### 2.7.1 请求签名 = HMAC-SHA256 + UCS credential

`@hw-ae-foundation+network-framework-default` 字符串表里挖到签名相关的一整组符号：

```
GUCS_SIGNATURE_ALGORITHM_HMAC_SHA256   HMAC_SHA256   signAlg
credential   credentialId   expireTime   srcData   base64
X-Authorization -> serviceToken        ucsTimeCost / grsTimeCost
```

即客户端先用 UCS 拿到的 credential（内含 HMAC 密钥）对 `srcData` 做 HMAC-SHA256 签名，
再把 `credentialId / signAlg / sign` 随请求发出。服务端的
`"TSMS verify parameters blank."`（630102）就是这个签名参数组为空。

### 2.7.2 不是“漏字段”，是硬门禁（穷举验证）

对 `client.fetchHarmonyFiles` 做了系统性试探，全部维持 630102：

| 试探维度 | 取值 | 结果 |
| --- | --- | --- |
| `encodingCode` | `0000/0100/0200/0201/0300` | 全部 630102 |
| body 字段名 | `sign/signature/tsmsToken/credential/credentialId/token/authCode/nonce/ek/iv` | 无变化 |
| query 参数名 | 同上一组 + `X-Authorization/authorization/tsmsParam/signInfo/bizTicket` | 无变化 |
| header 名 | `X-TSMS-Token/tsms-token/sign/X-Signature/credential` | 无变化 |
| 模拟器标记 | `isSimulator/pcEmulator/emulator/isEmulator/deviceType`（query 与 body） | 无变化 |

### 2.7.3 新增的映射方法

```
client.getCardList      MAPPED  -> {"rtnCode":2,"rtnDesc":"get card list miss leagueAppId!"}
client.batchAppDetail   MAPPED  -> {"rtnCode":1}（无描述，疑同样缺凭证上下文）
```

`leagueAppId` 也做过穷举：扁平/嵌套(`clientContext`/`context`/`header`/`clientInfo`)、
字符串/数字、7 种拼写、值取 `0/1/1000/C27162/5765880207854244859` —— 全部 `miss leagueAppId`。
说明它不是普通入参，而是由凭证上下文注入的字段。

### 2.7.4 结论

- web 面（`/edge/*`）→ 元数据，无文件地址；
- 客户端面（`/hwmarket/harmony/client`）→ 全部下载/卡片接口都要 UCS 凭证签名；
- 二者之间没有第三条匿名通道（`getAppDownloadUrl` 只放 AppGallery 自己）。

因此“脱机下载 HAP”要么拿到一枚**华为签发的 UCS credential**，
要么从一台**真实华为设备**上取一次直链（CDN 对象本身不绑设备，签名在 URL 上）。


## 2.8 第六轮：TSMS 端点实测探针（2026-11-26）

### 2.8.1 CDN 令牌是硬要求（对照实验）

用已知可下载的 appmarket 做对照，逐一替换路径成分：

| 路径 | 结果 |
| --- | --- |
| `apk/7d/<id>/com.huawei.appmarket.apk` | 403 |
| `apk/7d/<id>/<id>.apk` | 403 |
| `apk/7d/<id>/com.huawei.appmarket.160801300.apk`（versionCode） | 403 |
| `apk/7d/<id>/com.huawei.appmarket.2611110925.apk`（服务端给的 token） | **206** |

结论：CDN 对象名必须逐字匹配，`<token>` 无法从 appinfo 推导，只能由接口下发。

### 2.8.2 新增工具：`tools/appgallery/tsms_probe.py`

按 `libcloud_app_identity_auth_api.z.so` 的契约自建请求：

- 自签 P-256 根 + 叶证书（`/tmp/tsms-probe/{root,leaf,key}.pem`）；
- JWS：header `{alg:"ED256", cty:"anon_cert", x5c:[leaf,root], kid}`，
  payload `{alg:6, kekAlg:2, dkAlgLen:1, akskVersion:2, pbk, len:32, packageName, serviceName, appId}`，
  签名为 ECDSA-P256-SHA256 的 `R||S`（base64url）；
- POST `https://tsms-drcn.platform.dbankcloud.com/tsms/v2/credentials`。

### 2.8.3 服务端错误码地图（实测）

| 发送形态 | 服务端回执 | 含义 |
| --- | --- | --- |
| `{"request": "<jws 原文>"}` | `tsms.1033` illegal fieldName input4 … 乱码 | 服务端把 `request` **base64 解码**后再解析 JSON，解出二进制即报错 |
| `{"request": "<base64(json)>"}` | `tsms.1009` Invalid jwt format | base64 解出的 JSON 被接受，但里面没有合法 JWT |
| 整包 base64 的 JSON 当 body | `tsms.1005` JSON parse error … 显示 base64 文本 | body 本身必须是明文 JSON |
| `{"request": "<base64({"jws":…}/{"header","payload","signature"})>"}` | `tsms.1009` | 内层 JSON 结构仍不对 |
| 空 body / GET / 其他 Content-Type | `tsms.1000` / 405 | — |

即：**`request` = base64(某段 JSON)，该 JSON 里带一个 JWT**；JWT 所在字段/结构还没定死。
已排除的字段名：`jws / jwt / token / signature / request / credential / data / assertion`，
已排除的结构：`{header,payload,signature}`、`{jwt:{…}}`、`{jws:{…}}`、`{jwt,header}`。

### 2.8.4 下一步

用 IDA 在 `UcsClient::ApplyCredential`（`sub_BEE0`）里继续跟 `v317` 之前的三段 base64
（`v354/v344/v336`）如何被包进内层 JSON——那里就是 TSMS 真正的入参结构。


## 2.9 第七轮：TSMS 判决定论 —— 自签链必被拒（2026-11-26）

### 2.9.1 `request` 就是 JWS 原文

用一枚普通 `ES256` token 试出服务端的第一道校验：

```
{"request":"<ES256 token>"} -> tsms.1009 "not support jwt alg ES256"
```

即 `request` 收的是 **JWT 紧凑原文**（不是 base64、不是 JSON 串）。

### 2.9.2 `x5c` 必须标准 base64，`kid` 是 base64(JSON)

- `x5c` 段若用 base64url → 服务端解不出 DER；改成 **标准 base64** 后才会走到链校验；
- header 缺 `x5c` → `tsms.1009 "missed x5c in header"`；
- `kid` 本身也是 **base64 编码的 JSON**（我们塞 base64url(随机 16 字节) 会被当成 JSON 解，报
  `tsms.1033 illegal fieldName ... character \u0004`）。

### 2.9.3 决定性回执

`tools/appgallery/tsms_probe.py --reuse --minimal`（自签 P-256 根 + 叶，空 payload）：

```
HTTP 400
{"errorCode":"tsms.1032","errorMessage":"trustCa list not match: cn=probe local dcm root"}
```

服务端把我们自签根的 CN 原样回显 —— **信任锚白名单里没有它**。

也就是说：

- 本地自签证书链（包括 marble 上 `dcm_local_ca.c` 生成的那套）**在服务端必然被拒**，
  与签名算法、payload 字段、编码方式无关；
- AppGallery 登录（UCS credential）与 HAP 下载（`client.fetchHarmonyFiles` 的 TSMS 参数）
  **共用这道校验**，因此两条链路都被同一把锁锁住；
- 这同时给前几轮 DCM 软 TEE 项目里悬着的问题下了定论：
  “本地 API 能过、云端不可能过”。

### 2.9.4 复现

```sh
python3 tools/appgallery/tsms_probe.py               # 新生成一套自签链
python3 tools/appgallery/tsms_probe.py --reuse --minimal   # 只看信任锚判定
```


## 2.10 第八轮：下载闭环打通（缺的只剩 URL）

### 2.10.1 新工具 `tools/appgallery/dl.py`

```
python3 tools/appgallery/dl.py info  com.tencent.wechat     # 商店元数据（size/sha256/appId/id）
python3 tools/appgallery/dl.py plan  com.tencent.wechat     # 元数据 + 待填 URL 的用法提示
python3 tools/appgallery/dl.py fetch --url <CDN URL> --out <file>         --sha256 <期望值> --size <字节数>                   # 多段并发下载 + 续传 + sha256 校验
```

实测把 AppGallery 自己的安装包完整跑通（60 164 563 字节，4 段并发 ~30 MB/s）：

```
downloading 60164563 bytes -> /tmp/appmarket.apk with 4 ranges
100.0%
size   60164563
sha256 7ee626839bc97b288fcf662322c0bb38fcb4c89e04a8d2e0174ad18728b26d68
expect 7ee626839bc97b288fcf662322c0bb38fcb4c89e04a8d2e0174ad18728b26d68
VERIFY ok
```

即：**URL 一到，微信包（517 353 204 字节 / `7b09e56e…`）的下载与校验是现成的**。

### 2.10.2 方法面再扩 17 个：全部同门禁

新增扫描（此前未测）：

```
client.fetchHarmonyFilesByBundleNames              TSMS 630102
client.unifiedDownloadServiceSecurityVerification  TSMS 630102
client.harmony.hopCheck / installFreeAuth / getApkProfile   TSMS 630102
client.getCloudChallenge / getNegotiationKey       TSMS 630102
client.harmonyos.agdSecurityVerify / agdBatchInstallVerify  TSMS 630102
client.getHostAppPluginList                        TSMS 630102
client.getAppOperInfos       需 req.ver + req.installApps
client.startTestDistribution / agreeTestDistribution / queryTestTaskQualification / stopTestTask
                             1000 "sessionId is empty"
client.harmonyos.agdSecurityVerifyNoUcsAuth  需 reqParams.mediaAppSigns（唯一 NoUcsAuth 分支）
client.getDisposalInfo / getHarmonyUpdateNotifyInfo / getCardData / crowdtest.join  参数缺失
```

结论不变：**下载与安全验证家族统统一把 TSMS 锁**。

### 2.10.3 匿名 `/harmony/*` 端点也试过了

web 网关上的 HarmonyOS 页面接口（无需凭证）表现：

| 端点 | 结果 |
| --- | --- |
| `/harmony/client-page-detail`（扁平 pageId） | 200 但 body 为空 |
| `/harmony/client-page-detail`（嵌套 pageDetailReq） | `rtnCode 2 The interface parameter is incorrect or missing` |
| `/harmony/client-card-list`（正确 dataId） | 同样 `rtnCode 2` |
| `/harmony/client-app-upgrade`（bundleVersionList） | `rtnCode 1`，list 为空 |
| `/web-ag/agd-link-generate`（detailType=5） | `rtnCode 220002 media no found for partner`（需注册合作方） |

没有第四条匿名通道。


## 2.11 第九轮：旁路全部试完 + 信任锚伪造实验（2026-11-26）

### 2.11.1 其他入口全灭

| 试探 | 结果 |
| --- | --- |
| 备用客户端路径 `/hwmarket/api`、`/hwmarket/client`（`method=internal.getTabDetail`） | **403 Tomcat 页**（另一套网关，拒绝本机） |
| `/hwmarket3/harmony/client`、`aisearch-drcn…/hwmarket/harmony/client` | 404 |
| CDN 别名 `dl/appdl/latest/<pkg>`、`/application/<pkg>`、`/app/<pkg>`、`.app` 包名 | 全部 403 |
| 微信官网 `weixin.qq.com` | 只发 Android APK，**没有 HarmonyOS 包**，AppGallery 是唯一来源 |

### 2.11.2 信任锚是“比对证书本体”，不是比对 DN

把自签根的 DN 伪造成与华为一致（`C=CN, O=Huawei, OU=Huawei CBG, CN=Huawei CBG Root CA`）：

```
tsms.1032  trustCa list not match:
           cn=huawei cbg root ca,ou=huawei cbg,o=huawei,c=cn
```

DN 完全相同仍被判“不匹配”，说明服务端比对的是**证书本体/公钥/指纹**，
不是名字。伪造 DN 这条路也彻底封死。

### 2.11.3 结论（可写进报告的定论）

1. 元数据：可脱机取（已完成）；
2. 下载器：可脱机跑（已实测 60 MB / sha256 一致）；
3. **直链：必须持有华为签发的 UCS 凭证**，凭证的信任锚是华为 CA，本地无法伪造，
   服务端逐证书比对；
4. 因此“完全脱机的 AppGallery HAP 下载”在本机条件下不可达，
   除非从一台真实华为设备上取到一次 `appdl` 直链（CDN 对象不绑设备）。


## 2.12 原包里的信任锚：找到了，但只有公钥（2026-11-26）

### 2.12.1 锚点在哪个文件

```
system/system/etc/security/trusted_attest_root_ca.cer     (2720 B, PEM 捆绑包)
```

里面正好两张根：

| subject | 算法 | DER | sha256 |
| --- | --- | --- | --- |
| `CN=Huawei CBG Root CA,OU=Huawei CBG,O=Huawei,C=CN` | RSA-4096, sha256WithRSA | 1384 B | `3aa81ad7b08f0158…` |
| `CN=Huawei CBG Root CA G2,OU=Huawei CBG,O=Huawei,C=CN` | EC P-384 | 542 B | `df21a3c09f795457…` |

同一对根还出现在：

```
system/system/etc/memospace_service/conf/ca.pem
system/system/lib64/platformsdk/libcertificate_framework_core.z.so   （内嵌）
system/system/lib64/libcollaborationfwk_service_trans.z.so
system/system/lib64/libshare_service.z.so
各应用包 resources/rawfile/{ucs,grs_config/config}/cbg_root.pem     （几十份）
```

### 2.12.2 有没有私钥？没有

全量扫描 ROM（82 526 个文件，含所有 hap/hsp）：

- 真实 PEM 私钥（`BEGIN * PRIVATE KEY` 且带 64 字节以上 base64 体）：**0 个**
  （命中的 19 处全是 mbedtls/openhitls 等库里的 **PEM 解析器字符串**）；
- `.p12 / .pfx / .bks / .jks / .keystore` 密钥库：**0 个**；
- 这些根只是**公钥证书**，用来在校验侧做链验证，签发侧的私钥在华为 HSM 里。

### 2.12.3 新工具 `tools/appgallery/attest_roots.py`

```
python3 tools/appgallery/attest_roots.py                 # 列出固件里的信任锚
python3 tools/appgallery/attest_roots.py --verify chain.pem   # 用锚点验证一条链
```

自检：对本地自签链判定 `anchor: NOT one of the shipped attestation roots`
——与 TSMS 的 `tsms.1032` 结论一致。

用途：一旦拿到真机的证明链/凭证，可以用它离线确认“是否真的挂在华为锚上”，
不用每次都去问服务器。


## 2.13 TEE / modem / 全部固件部件的密钥普查（2026-11-26）

### 2.13.1 普查范围（不是只看 fs/）

| 范围 | 规模 | 结果 |
| --- | --- | --- |
| 解包后的文件系统 `fs/` | 82 526 个文件 | PEM 证书 943 段 → 175 张唯一证书，其中 19 张 Huawei 主题；**私钥 0**、密钥库 0 |
| 原始镜像（system/vendor/sys_prod/chip_prod/boot/ramdisk/eng_*） | 7.3 GB | PKCS#8/PKCS#1 私钥特征、PEM 私钥：**0** |
| **OTA 里未解包的部件**（50 个，含 TEE 与 modem） | 523 MiB | **0 私钥、0 PEM 证书** |

其中从 `update.bin` 里单独抠出来的关键部件：

```
teeos.img 4.6 MB   trustfirmware.img 0.4 MB   thee.img 2 MB   tzsp.img 0.5 MB
hhee.img 0.3 MB    seplat.img 0.75 MB         hisee_img 0.13 MB   dacc.img 74 KB
modem_fw（123 MB，解包时归入 50 部件统计）  modemnvm_update 16 MB  modemnvm_cust 16 MB
```

TEE 镜像里没有任何密钥材料——密钥在 RPMB/安全存储里，不在镜像的代码段中。

### 2.13.2 更关键：固件里根本没有 TSMS 的信任锚

把固件里 19 张 Huawei 主题证书**逐一**当作链根发给 TSMS：

```
leaf + <候选根>  ->  tsms.1032 trustCa list not match: <候选根 DN>
```

包括 `Huawei CBG Root CA`、`Huawei CBG Root CA G2`、`Huawei CBG ECC Equipment Root CA 3`、
`Huawei CBG ECC IoT Equipment Root CA 1`、`Huawei BPIT Root CA`、`HUAWEI TLS SM2 Root CA S240`、
`HUAWEI C2PA ECC384 Root CA E346`、`Huawei CBG Application Integration CA` 等 —— **全部不匹配**。

唯一回执不同的是 `Huawei CBG Application Integration CA`：`tsms.1005 invalid param: max cert should be 2048`
（它的 base64 长度 2052 超过服务端 2048 上限，属尺寸检查，不是信任）。

即：**TSMS 的信任锚既不在固件里，也不是设备侧那两张 CBG 根**。设备侧
`trusted_attest_root_ca.cer` 只用于本机校验 DCM 链（这也解释了为什么本地链能过、云端过不去）。

### 2.13.3 结论

- 本地签发（自签、伪造 DN、把真实华为根接在链末）三种变通都试过，服务端一律拒绝；
- 固件（含 TEE/modem）里没有私钥、没有密钥库、没有 TSMS 锚点；
- 所以“本地签发密钥变通”这条路已经穷尽，凭证只能由真实华为设备向 TSMS 申请得到。


## 2.14 第十轮：一条命令的成品链路 + CDN token 语义收窄

### 2.14.1 `dl.py grab`（一条命令：URL → 下载 → 校验 → 报告）

```sh
python3 tools/appgallery/dl.py grab   --package com.tencent.wechat   --url '<appdl 直链>'   --out /tmp/wechat.hap
```

它会：按 `--package` 自动从商店取 `sha256/size` → 多段并发下载 → 校验 → 打印 JSON 报告。
实测（用 AppGallery 自己的包当靶）：

```
target : 华为应用市场(com.huawei.appmarket) 16.8.1.300
store  : 60164563 bytes  sha256 7ee6268…
100.0%
VERIFY ok
REPORT {"url": "...", "file": "/tmp/grab_test.apk", "bytes": 60164563, "result": "ok"}
```

微信一拿到 URL，把 `--package` 换成 `com.tencent.wechat` 即是同一条命令。

### 2.14.2 CDN token 不是随便猜的

- **token 是每包独立的**：把 appmarket 的 token `2611110925` 拼到微信的 id/包名上、或把微信的
  `versionCode` 拼到 appmarket 上，全部 `403`；
- **前 6 位是上传日期**：appmarket 的 `261111` 与 appinfo 里
  `releaseDate=1794383368000`（2026-11-11）**日期完全一致**；微信是 `261113`（2026-11-13）；
- 后 4 位（appmarket 为 `0925`）不等于 `releaseDate` 的时分（07:49 UTC / 15:49 CST），
  也不出现在 appinfo、详情页 JSON 的任何字段里 —— 应是构建/上传时刻，无法从公开元数据推出；
- CDN 桶本身不可列举：`appdl-drcn.dbankcdn.com/dl/appdl/...` 一律 S3 风格 `AccessDenied`。

### 2.14.3 状态

```
元数据 ✅   下载器 ✅   一条命令闭环 ✅   直链 ❌（需要真机凭证或外部给出的 URL）
```


## 2.15 第十一轮：匿名“发码”端点 + token 日期规律（2026-11-26）

### 2.15.1 新端点：`appgallery.cloud.huawei.com/appdl/<appId>`

从 `env.js` 的 `HWClientDownloadUrl: 'https://appgallery.cloud.huawei.com/appdl/C27162'` 挖出来，
实测它是个**无需登录的分发跳转**：

```
GET https://appgallery.cloud.huawei.com/appdl/C27162
302 location: https://store-drcn.hispace.dbankcloud.com/dl/appdl/application/apk/
              7d/7dfbe78d41bd438bbf402848eff97548/com.huawei.appmarket.2611110925.apk
              ?maple=0&trackId=0&distOpEntity=HWSW
```

即：**它把 token 一起发下来了**。这是目前唯一一个“我们自己发请求就能拿到直链”的入口。

### 2.15.2 但只对华为自家应用放行

批量实测（`appinfo` 取 appId → 打端点 → 看 302 目标）：

| 包 | appId | 结果 |
| --- | --- | --- |
| com.huawei.himovie | C10393298 | ✅ CDN（token `2611160856`） |
| com.huawei.music | C101444301 | ✅（`2611111123`） |
| com.huawei.hwid | C10132067 | ✅（`2611190942`） |
| com.huawei.appmarket | C27162 | ✅（`2611110925`） |
| com.huawei.smarthome | C10406921 | ✅（`2611191941`） |
| com.huawei.gamebox | C10059090 | ✅（`2611121201`） |
| com.huawei.search | C100619727 | ✅（`2411221446`） |
| **com.tencent.wechat** | C5765880207854244859 | ❌ 302 → `https://appgallery.cloud.huawei.com`（首页） |
| com.tencent.mm / com.tencent.qqlive / com.taobao.taobao / com.ss.hm.ugc.aweme | — | ❌ 同上 |

换 UA（HarmonyOS/ArkWeb）、加 `cno=portal`、`clientId=108377959`、`maple/trackId/distOpEntity`
全参数、加 Referer —— 第三方包依旧跳首页。白名单在服务端，与请求形态无关。

### 2.15.3 token 规律：日期段已破解，时间段没有

7 组（releaseDate, token）对照：

| 包 | release（UTC） | token | 日期一致 | 时间差 |
| --- | --- | --- | --- | --- |
| himovie | 2026-11-17 00:56 | 2611**16**0856 | ✗(差一天) | +8h CST 对齐 |
| music | 2026-11-11 06:14 | 261111**1123** | ✅ | +5h09 |
| hwid | 2026-11-19 09:04 | 261119**0942** | ✅ | +0h38 |
| appmarket | 2026-11-11 07:49 | 261111**0925** | ✅ | +1h36 |
| smarthome | 2026-11-19 12:44 | 261119**1941** | ✅ | +6h57 |
| gamebox | 2026-11-12 07:15 | 261112**1201** | ✅ | +4h46 |
| search | 2024-11-22 08:24 | 241122**1446** | ✅ | +6h22 |

结论：token = `yyMMdd` + `HHmm`(构建时刻)，**日期段与 releaseDate 同日**（himovie 是 CST 跨日），
**时间段与 releaseDate 无固定偏移**（38 分钟 ~ 8 小时不等），公开元数据推不出来。
微信的日期段是 `261113`，时间段仍未知（10^4 空间，不做暴力尝试）。

### 2.15.4 工具

```sh
python3 tools/appgallery/dl.py mint com.huawei.appmarket   # MINT ok，打印直链
python3 tools/appgallery/dl.py mint com.tencent.wechat     # MINT restricted（服务端白名单）
```


## 2.16 第十二轮：第一方 HarmonyOS 包能拿到 URL，但对象是 ACL 保护的（2026-11-26）

### 2.16.1 mint 端点对第一方 HarmonyOS 应用生效

扫描 28 个华为包，命中两个 `packingType` 为 4/5（HarmonyOS 包）的第一方应用：

```
com.huawei.hmos.notepad  appId=C5765880207853993265  备忘录 15.0.9.751
   size=8 644 898  sha256=98a0b5deef1cadea0f8d134dce46747029ccba101715de1f510899e8621df3ec
   location=https://appdl-drcn.dbankcdn.com/dl/appdl/application/CN/2025060406/
            1749017010704-4933d694-198c-4b67-bc5e-5b2ab6a29b2c.app?maple=0&trackId=0&distOpEntity=HWSW

com.huawei.notepad       appId=C101195705               备忘录 13.0.9.310
   location=.../application/CN/2022101103/1665459191039-9eff9dce-...-e8a491835aeb.app?...
```

**HAP/`.app` 包的 CDN 路径格式**（与 APK 完全不同）：

```
/dl/appdl/application/<国家>/<yyyymmdd + 2 位序号>/<构建 epoch ms>-<uuid>.<app|hap>
```

### 2.16.2 但包对象拿不下来：AccessDenied

对同一个 URL 做了 6 种组合（直连 appdl-1/2/3、appdlssl；带/不带 `maple/trackId/distOpEntity`；
加 `Referer`/`Origin`；经 `store-drcn` 302 跳转）——**全部 403 AccessDenied**。

同一时刻的 APK 对照对象（`/dl/appdl/application/apk/…`）**206 正常返回**。

即：

| 对象类型 | 公开可读 |
| --- | --- |
| Android APK（`/application/apk/<id2>/<id>/<pkg>.<token>.apk`） | ✅ 206 |
| HarmonyOS 包（`/application/<CN>/<日期>/<epoch>-<uuid>.app`） | ❌ 403 AccessDenied（需要额外授权） |

### 2.16.3 结论

HAP/`.app` 下载被**两道门**同时挡住：

1. **mint 门**：第三方 appId（微信/抖音/淘宝…）不返回包地址，只跳首页；
2. **对象 ACL 门**：即便拿到第一方的包 URL，CDN 也不给公开读取。

这解释了为什么“用第一方 appId 绕过”也不行——URL 拿到了，字节拿不到。


## 2.17 第十三轮：ACL 矩阵收口 + 下载器加固（2026-11-26）

### 2.17.1 `.app` 对象的 ACL 试到尽头

对 2 个第一方 HarmonyOS 包（`hmos.notepad`、`notepad`）的 CDN 对象做了完整矩阵：

| 变量 | 取值 | 结果 |
| --- | --- | --- |
| 方法 | GET（full）/ GET+Range / HEAD | 403 AccessDenied |
| 主机 | appdl-1 / appdl-2 / appdl-3 / appdlssl | 403 |
| 查询 | 无参数 / `maple=0` / `distOpEntity` / 三者齐 / 追加 `&sign=1` | 403 |
| 请求头 | `X-Authorization` / `Authorization` / `sign` / `X-Sign` / `Cookie` / `Accept-Encoding: identity` / `Referer` / `Origin` | 403 |
| APK 对照（同样时刻） | `/dl/appdl/application/apk/…` | **206 ok** |

结论：**HAP/`.app` 对象的 ACL 与请求形态无关**，只认客户端授权（或地域/会话），
APK 对象才是公开的。

### 2.17.2 客户端下载走的是什么

`download-install` 的字节码里可见下载器初始化：

```
addQuicHint appdl-drcn.dbankcdn.com / appdl-drcn.dbankcdn.cn
            appdl-1-drcn.dbankcdn.com / appdl-1-drcn.dbankcdn.cn
hwhttp_hquic_disable_tls_0rtt / hwhttp_disable_cert_max_validity_check
init with hquic / init without hquic
```

即 AppGallery 用 **hquic（HTTP/3）** 下载，并对上述 CDN 主机做 QUIC hint。
传输层是 QUIC，不改变 ACL —— 但说明“用 curl 拿不到”并不是协议问题。

### 2.17.3 下载器加固

- 新增 `--header "Name: value"`（可重复）：一旦拿到带授权头/签名的 URL，可以直接喂给工具；
- HEAD 失败时自动降级为 `Range: bytes=0-0` 探测；
- 拿不到对象时输出明确结论而不是抛栈：

```
HEAD failed: HTTP 403
GET  failed: HTTP 403 <?xml ... AccessDenied ...>
REFUSED — this object needs authorization the client did not supply
```

回归：公开 APK 对象仍然 `VERIFY ok`（60 164 563 B / `7ee6268…`）。


## 2.18 第十四轮：抓包交接通道 + 操作手册（2026-11-26）

### 2.18.1 再排除两个变量

- `.cn` 域名的 CDN（`appdl-drcn.dbankcdn.cn`、`appdl-1-drcn.dbankcdn.cn`）同样 403；
- 本机 curl 8.18 **不带 HTTP3**（无 quic 支持），但 ACL 与传输层无关：
  以 HTTP/1.1 的 APK 对象 206、`.app` 对象 403，差异只在对象 ACL。

### 2.18.2 下载器新增“抓包回放”入口

```sh
python3 tools/appgallery/dl.py fetch --from-capture capture.json --out /tmp/wechat.app
```

`capture.json` 形如：

```json
{"url": "https://appdl-1-drcn.dbankcdn.com/dl/appdl/application/CN/....app?...",
 "headers": {"X-Authorization": "...", "Cookie": "...", "sign": "..."}}
```

解析、套用请求头、下载、校验一条龙；被拒时输出
`REFUSED — this object needs authorization the client did not supply`。

### 2.18.3 新文档：真机抓一次请求的操作手册

见 [APPGALLERY_HAP_CAPTURE_GUIDE.md](APPGALLERY_HAP_CAPTURE_GUIDE.md)：

- 方案 A：`hdc shell find /data -name '*.app' -o -name '*.hap'` → `hdc file recv` 直接取包；
- 方案 B：mitmproxy/Charles 抓 `appdl-*.dbankcdn.com` 的 `.app` 请求，导出 URL + 全部头部；
- 给出微信的期望 `size/sha256` 作为成功判据；
- 说明“URL 能在另一台机器跑出 VERIFY ok”即代表签名可复用。


## 2.19 第十五轮：无真机条件下的穷尽结论（2026-11-26）

### 2.19.1 又排掉的变量

| 试探 | 结果 |
| --- | --- |
| mint 得到的会话 Cookie（HWWAFSESID/HWWAFSESTIME） | 带上仍然 403 |
| `-L` 跟随 mint 302 直接取对象 | 403 |
| 区域 CDN（appdl-dra/dre/drru）同路径 | 403（同一 ACL） |
| 微信其它包名 `com.tencent.wechat.pc` / `.mm` / `wework` | mint 全部跳首页，无包地址 |
| 微信官网 `weixin.qq.com` | 只挂 `appgallery.huawei.com/app/detail?id=com.tencent.wechat(.pc)`，**不提供 HAP** |

### 2.19.2 结论

在没有“真机 / 真机抓包 / 已签名直链”的前提下，微信 HAP 无法获取。
这不是请求形态问题，而是**服务端两道授权**：

1. 第三方 appId 不进入 mint 白名单；
2. 即使第一方 HarmonyOS 包，`.app` 对象对匿名请求一律 `AccessDenied`。

APK 对象是唯一公开可读的产物（已实测完整下载 + sha256 校验通过）。

### 2.19.3 仍然可行的三条路（都需要外部资源）

1. **HarmonyOS 模拟器**（DevEco Studio + 模拟器镜像 + 华为账号）：
   模拟器里的 AppGallery 可以下载应用，之后用 `hdc file recv` 把 `.app` 拉出来；
2. **任意一台能正常下载的华为设备**：同上，或抓一次 `.app` 请求的 URL + 头部；
3. **一条现成的带签名直链**。

三者任一到位，本仓库的 `dl.py grab/fetch --from-capture` 都能直接完成下载与校验。


## 2.20 第十六轮：GitHub 同名工作对照 —— 结论完全一致（2026-11-26）

### 2.20.1 找到的项目

`github.com/rickytan/hap`（Go CLI + 设备探针 HAP），描述就是
“Experimental CLI for searching and downloading HarmonyOS packages from Huawei AppGallery”。
它的 README 与 `device-probe/README.md` 把同一堵墙从另一侧验证了一遍：

| 该项目的结论 | 与本仓库实测对照 |
| --- | --- |
| 桌面客户端打 `client.fetchHarmonyFiles` → `x-error-code=630102 / TSMS verify parameters blank.` | 完全一致（2.6 节） |
| TSMS 签名 = HMAC-SHA256(API method + 毫秒时间戳)，密钥来自 HUKS 匿名证明 | 与 2.7 节挖到的 `GUCS_SIGNATURE_ALGORITHM_HMAC_SHA256` 一致 |
| web 详情接口可用，但**原生 HarmonyOS 包的 URL 是设备/会话相关且被隐去** | 与 2.5/2.16 节一致 |
| 真机探针（VYG-AL30 OpenHarmony 7.0.0.105；SGT-AL50 HarmonyOS 6.1.0.135 + AppGallery 7.9.1.302）**成功拿到 TSMS 凭证、成功导入 wrapped SK/DK、成功算出 32 字节 HMAC**，但 Store 仍回 `HTTP 206 rtnCode=634001 TSMS identify verify failed.` | 新增：凭证与**调用方的包名/签名身份绑定**，换个 app 就不认 |
| 连接 `DownInstallServiceExtensionAbility` → `16000004 Cannot start an invisible component` | 新增：HAP 安装分发器对普通三方应用不可见 |
| AppGallery 7.9.1.302 的 HAP-install dispatcher 对探针请求判 `Untrusted Source`（调用方 UID 门禁） | 新增：即便连得上也有 UID 门 |

### 2.20.2 用它给出的设备画像再打一次

把该项目 `harmonyBody()` 的完整设备画像（`serviceType=80`、`code=0200`、
`harmonyInfo.hmosApiLevel=60101`、`clientVersionCode=1470701300`、`deviceRiskScore=10`、
`singleHarmonyClient=1`、query 带 `msgver/nonce`）原样搬到本机，对**第一方** HarmonyOS 包
（`com.huawei.hmos.notepad`）和微信各打一次：

```
notepad-first-party  /hwmarket/harmony/client  [206] {"rtnDesc":"TSMS verify parameters blank.","rtnCode":630102}
wechat               /hwmarket/harmony/client  [206] {"rtnDesc":"TSMS verify parameters blank.","rtnCode":630102}
两种包 /hwmarket/api/clientApi                 [200] {"rtnCode":1,"rtnDesc":"Failed to get path mapping !"}
```

设备画像再完整也过不去——与该项目“普通型号/能力/GPU 字段不足以过门”的结论一致。

### 2.20.3 定论

HAP 下载被三重门挡住，且已被**独立实现**从另一侧确认：

1. **凭证门**：未签名/非设备身份 → `630102 TSMS verify parameters blank`；
2. **身份绑定门**：真机真凭证，但凭证绑定到申请它的包名+签名 → `634001 TSMS identify verify failed`；
3. **UID 门**：即便能连分发服务，调用方 UID 不是 AppGallery → `Untrusted Source`。

因此“脱机实现 HAP 下载”在现有公开手段下不可达；唯一能拿到包的是**AppGallery 本体在真机/模拟器上自己下载**。


## 2.21 第十七轮：`appdl` 的真身 + 数据库普查（2026-11-26）

### 2.21.1 `appdl/<appId>` 是“客户端自下载”服务，不是通用分发

web 端 25 个 chunk 里一共只有 3 个 appdl 链接，全是自家客户端：

```
https://appgallery.cloud.huawei.com/appdl/C27162                      (AppGallery 手机版)
https://appgallery.cloud.huawei.com/appdl/C27162?cno=portal
https://appgallery.cloud.huawei.com/appdl/C499170478846388928?cno=portal  (AppGallery PC 版)
```

这就解释了白名单：它本质是“请下载我们的客户端”，所以只给华为自家 appId 发地址。
不是“按 appId 分发任意应用”的接口，也就不存在参数绕过。

### 2.21.2 私有数据库普查：固件里没有

| 范围 | 方法 | 结果 |
| --- | --- | --- |
| 解包文件系统 49 534 个文件 | 读文件头 16 字节比对 `SQLite format 3\0` | **0 个** |
| `vendor.img` 原始镜像 | mmap 全文搜 magic | 1 处命中，解出来 page_size=154 无效 → **假阳性** |
| `sys_prod / chip_prod / cust / eng_system` | 同上 | 0 |
| AppGallery 的 23 个 hap/hsp | 列举 `*.db/*.sqlite/*.rdb/*.db3` | 0 |

AppGallery 的下载任务库、`app_table`、`appCertIdInfos` 等（字节码里出现的表名）都在
**/data 分区**，OTA 包里不含——没有真机就拿不到。

### 2.21.3 搜索类接口也验证了一遍

```
internal.getTabDetail uri=search|微信      [200] layoutData 为空
internal.search keyword=微信               [502] 未映射
client.completeSearchWord keyword=微信     [502] 未映射
internal.completeSearchWord keyword=微信   [200] 只有 6 KB 关键词联想，无地址
```

搜索面不返回任何 `downurl`（返回的卡片 `downurl` 全为 null）。

### 2.21.4 结论

“私有数据库”这条路在无真机条件下同样走不通；`appdl` 也不是通用分发口。
公开面能拿到的极限仍然是：**元数据 + 公开 APK 对象**。


## 2.22 第十九轮：回到真机（marble）核实 + NoUcsAuth 面（2026-11-26）

### 2.22.1 现场又换了一版镜像，但 HUKS 还是旧的

```
BOOT        = 4c5b5d82-d6eb-49e6-a52b-a254d89a590c   (uptime 68s，刚重启)
vendor_boot = 34d38ad4888de68320297ee1e50f3489043e29cb07b93bec7e1daa512bf24832
/hmos-root/compat/lib64/libhuks_hdi_service_1.1.z.so = abb74cf0…（69 768 B，旧版）
/hmos-root/compat/lib64/libhuks_engine_core_standard.z.so = 2ab58d7a…（一致）
```

也就是说：本轮/上一轮的合并镜像**仍然没有带上 carrier 修好的 HUKS HDI**
（新版应为 `45466a67…`，70 088 B）。这解释了为什么 `DcmAnonymousAttestKey` 还是 `-10001`。

修法（任选其一）：

```sh
# A. 用已经打好的候选（内含 45466a67）
#    /mnt/e/marble-tmp/vendor_boot_h20_u4_dcm_soft_tee_912143fd.img

# B. 在你自己的打包流程里，确保打包前先重建 HUKS 产物
sh compat/huks/build_soft_huks.sh --cross --out work/marble-huks-build
sha256sum work/marble-huks-build/arm64/libhuks_hdi_service_1.1.z.so   # 期望 45466a67…
```

注意：即使补上这一条，云端（TSMS）依旧会拒——它只解决 `-10001` 这个本地确定性 bug。

### 2.22.2 `NoUcsAuth` 面只有一条，且不是下载

字节码里全库只有 `client.harmonyos.agdSecurityVerifyNoUcsAuth` 带 NoUcsAuth，
实测参数要求：

```
reqParams.mediaAppSigns / reqParams.mediaPkgName / reqParams.callType
```

它是“媒体应用签名安全校验”，不返回任何下载信息；不存在“免凭证下载”的对应方法。

### 2.22.3 设备侧 UI 触发仍然不可行

`aa start store://appgallery.huawei.com/app/detail?id=com.tencent.wechat` 依旧返回
“内存不足 / 拉起 Ability 过多” 的提示（marble 内存压力大），
没法用 UI 自动化去点下载。


## 2.23 第二十轮：private 端点的 POST 形态 + interface-code 不授权 CDN（2026-11-26）

### 2.23.1 `webAppLink/getAppDownloadUrl` 的正确 POST 形态

bundle 里它是 **private** 调用：`getServicePrivate.post("/webAppLink/getAppDownloadUrl", {appId, packageName})`。
之前的 502 是因为我们把 `apsid/sid/ts/ver/serviceType` 一起塞进去了。改成**最小 JSON + interface-code**：

```http
POST /edge/webAppLink/getAppDownloadUrl
interface-code: <jwt>_<epoch_ms>
Content-Type: application/json;charset=UTF-8
{"appId":"C5765880207854244859","packageName":"com.tencent.wechat"}
```

| 包 | rtnCode | 结果 |
| --- | --- | --- |
| com.huawei.appmarket | 0 | 正常返回 `.apk` 地址 |
| com.huawei.hmos.notepad | 0 | 正常返回 `.app` 地址 |
| com.tencent.wechat | 1 | `valid the app failed Non-White List appid:{} pkgName:{}` |

顺带纠正：那句错误里的 `{}` 是**字面占位符**（永远不会被填值），
不是“服务端没收到参数”，所以换 `appid/appId/packageName/pkgName` 各种拼法都没有用——
白名单就是按 appId 判定。

### 2.23.2 `interface-code` 不能给 CDN 对象授权

拿 `.app` 对象做了 4 组对照（裸请求 / 带 interface-code / 再带 Net-Msg-Id / 再带 Referer）：
**全部 403 AccessDenied**。web 网关的接口码与 CDN 的对象 ACL 是两套东西。

## 3. 现状与下一步

已经能确定：主机侧可无账号访问 web 网关、可拿到接口码、可复现端点错误码。

还差两块拼图：

1. ~~WeChat 的 appId~~ 已解决：`webedge/appinfo` 直接按 `pkgName` 解析
   （`com.tencent.wechat → C5765880207854244859`）。
2. ~~POST 502~~ 已解决：补 `apsid/sid/ts/ver`（见 2.4.2）。
2. **详情/文件请求的 body 结构**：`clientTabDetailReq`、`buildPage.pageDetailReq` 的字段表，
   以及 `store-drcn` 客户端信封的必填项。需要 `ark_disasm` 或对 `.abc` 做结构化解析。

拿到后即可：详情 → `downloadUrl` + `sha256` + `fileSize` → 直连 `appdl-*` CDN 下载 → 校验。

## 4. 用法

```sh
python3 tools/appgallery/ag_web.py interface-code
python3 tools/appgallery/ag_web.py download-url --appid C27162 --pkg com.tencent.wechat
python3 tools/appgallery/ag_web.py appinfo --pkg com.tencent.wechat
python3 tools/appgallery/ag_web.py page-detail --pkg com.tencent.wechat
python3 tools/appgallery/ag_web.py raw --path /index/gettabdetail --method POST --json '{"clientTabDetailReq":"..."}'
```
