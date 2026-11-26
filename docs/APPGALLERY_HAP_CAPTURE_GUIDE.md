# 抓一次真机 HAP 下载请求：操作手册

目标：从任何一台**能正常下载 AppGallery 应用**的华为/鸿蒙设备上，取到一次
`.app`/`.hap` 的下载请求（URL + 授权头），或者直接把下载好的包拷出来。
拿到之后，本仓库的工具可以在任意主机上脱机完成下载与校验。

本手册只描述读取与导出；不涉及修改设备、不影响账号。

---

## 方案 A：直接从设备文件系统取包（最省事）

AppGallery 下载完成的包会落在设备的数据分区，先找文件再拷出来：

```sh
hdc shell "find /data -name '*.hap' -o -name '*.app' 2>/dev/null | head -20"
hdc file recv /data/.../xxxx.app ./wechat.app     # 按上一步的路径替换
```

常见位置：

```
/data/app/el1/bundle/public/<bundle>/          已安装后的模块
/data/service/el1/public/...                  服务侧缓存
/data/storage/el2/base/...                    应用沙箱内的下载缓存
```

拷出来后校验（本机执行）：

```sh
python3 tools/appgallery/dl.py info com.tencent.wechat        # 期望值
sha256sum ./wechat.app                                        # 与上面的 sha256 比对
```

> 注意：安装后 `/data/app` 里是解包过的模块，`sha256` 与商店里的整包值不同；
> 想要整包请抓“下载完成后、安装前”的缓存文件。

---

## 方案 B：抓下载请求（URL + 授权头）

AppGallery 走 hquic/HTTP3 且可能对 CDN 域名做证书固定，抓包要按下面做：

1. 设备开热点，PC 接入；或设备与 PC 同网段。
2. PC 起代理（mitmproxy 推荐）：

   ```sh
   mitmproxy --mode transparent --listen-port 8080
   # 或普通 HTTP 代理模式
   mitmproxy -p 8080
   ```

3. 设备侧：
   - 若可 root：把 mitmproxy 的 CA 放到系统信任库（`/system/etc/security/cacerts/`），
     设全局代理；
   - 若不能 root：只能抓明文/未固定域名的流量；AppGallery 对 CDN 大概率固定证书，
     此时改用 `hdc shell` 侧的 `tcpdump`（若镜像带）或方案 A。

4. 在 AppGallery 里**下载目标应用**（例如微信），在代理里找这些特征：

   ```
   host : appdl-drcn.dbankcdn.com / appdl-1-drcn.dbankcdn.com
   path : /dl/appdl/application/<CN>/<yyyymmdd+序号>/<epoch>-<uuid>.app   ← HarmonyOS 包
          /dl/appdl/application/apk/<id2>/<id>/<pkg>.<token>.apk          ← Android 包
   ```

5. 把这一条请求导出成 JSON（本仓库工具可直接吃）：

   ```json
   {
     "url": "https://appdl-1-drcn.dbankcdn.com/dl/appdl/application/CN/2025.../....app?maple=0&trackId=0&distOpEntity=HWSW",
     "headers": {
       "User-Agent": "...",
       "X-Authorization": "...",
       "Cookie": "...",
       "sign": "...",
       "任何以 X- 开头或看起来像签名的字段": "..."
     }
   }
   ```

   **除了 URL，重点是所有请求头**。目前实测：`.app` 对象的 ACL 与
   `Referer/Origin/Cookie/X-Authorization` 的“存在”无关，只认具体值，
   所以需要真机上那一刻的完整头部。

---

## 拿到之后本机怎么用

```sh
# 直接下载 + 校验（会自动按包名取商店里的 sha256/size）
python3 tools/appgallery/dl.py grab \
  --package com.tencent.wechat \
  --url '<粘贴 URL>'

# 或者用抓包文件（自动读取 url 与 headers）
python3 tools/appgallery/dl.py fetch --from-capture capture.json --out /tmp/wechat.app
```

成功判据：

```
size   <与商店元数据一致>
sha256 <与商店元数据一致>
VERIFY ok
```

微信的期望值（2026-11-26 抓取）：

```
size   517353204
sha256 7b09e56e4da31463b1806fd9e34404ca22788019b6fb74661cb40a4d3f1e54a0d
```

---

## 什么算“抓到有用的一条”

- URL 里出现 `.app` 或 `.hap` 结尾 → 就是 HarmonyOS 包，正是我们要的；
- URL 是 `.apk` → 那是 Android 包，说明该应用在商店里同时有安卓版本；
- 只要 URL 能在**另一台机器**上用 `dl.py fetch` 跑出 `VERIFY ok`，就说明签名/授权可复用，
  后续所有下载都能脱机完成。
