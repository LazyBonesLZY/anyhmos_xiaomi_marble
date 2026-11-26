# Marble 离线 Web 渲染夹具

[正向页](fixture.html)加载后显示 `JS READY` 与红绿／蓝黄四象限，真实点击一次后显示 `JS TAP 1` 与绿蓝／黄红。 [禁用脚本页](no-js.html)仅改变 CSP：保留静态文字、空画布和禁用按钮。两页及脚本均为合成素材，与 donor 固件或用户数据无关。

## 本机验证

```sh
sha256sum -c SHA256SUMS
node verify.mjs
```

[验证器](verify.mjs)用本机 mock 检查状态、像素、点击和同源标记；不启动浏览器或监听，不代表设备 WebView 通过。

## 重新接机后的分层验收

1. 固定当次镜像、boot ID 和窗口身份。用原浏览器的地址栏打开测试 URL；只在已有正常嵌入入口的应用里做独立 WebView 验收。跳转到外部浏览器只记浏览器。
2. [生成器](prepare.py)仅在新建的主机输出目录生成四个脚本，本身不执行它们。`--boot` 填当次实测 boot ID；每次换 `--run`：

   ```sh
   python3 prepare.py --boot BOOT_ID --run h20a1 --out /mnt/e/marble-tmp/web-h20a1
   ```

   在获得实测窗口后，按 [COM9 工具说明](../../../docs/MARBLE_HANDOFF.md#L98-L109)串行提交 `push-p.sh`、`push-n.sh`。每个脚本须有读回 SHA 和对应 `CMD END exit=0 signal=0`。再运行 `serve.sh`：仅目标机 `127.0.0.1:18719`，限时 40 秒，服务前重新校验两页。先确认 BusyBox applet、端口及绑定支持；保持环回，不启用全接口或改变应用网络策略。测试后 `cleanup.sh` 仅清理本次 RAM 目录中的两个文件。
3. 正向访问 `http://127.0.0.1:18719/h20a1p.html?run=h20a1p`，截图 `JS READY`，点一次 `Tap once` 后验证 `JS TAP 1` 和色块重排。负控使用 `http://127.0.0.1:18719/h20a1n.html?run=h20a1n`，必须有文档 GET 200，但保持 `STATIC / JS NOT RUN`，无 JS 标记请求。`/mark-h20a1p-ready.gif` 和 `/mark-h20a1p-tap1.gif` 是预期 **404**：记录请求抵达即可，页面不依赖其响应。COM9 服务窗口内保持串行；用外部摄录或在窗口后采已有画面。
4. 对齐 GET、真实点击、画面、宿主焦点及新 renderer 的 PPid／UID、实际 engine/render 映射或首个错误。无 GET 先查交付；有 GET 无脚本效果再查 CSP／renderer；脚本有效而无色块再查 Canvas 和图形提交。仅有 `nwebspawn` 或库映射不算页面验收。[固定 MP4 播放、拖动进度和音画同步](../marble-media/README.md)另验。
