# Marble 固定媒体验收样本

[生成器](generate.py)在主机生成三个 **6.000 秒**的合成样本：`tones.wav` 是 PCM s16le，`tones.m4a` 是 AAC-LC，两者均为 48 kHz 单声道；`av_seek.mp4` 是 320×240 / 30 fps 的 H.264 Baseline、I420（BT.601 窄范围），音轨与 M4A 相同。样本只包含色块和纯音，无需账号、外网、字体或原厂素材。

| 内容区间 | MP4 画面 | 三个文件的声音 | 精确定位样本 |
| --- | --- | --- | --- |
| 0–2 秒 | 红底、一枚白块 | 400 Hz | 1.200 秒，第 36 帧 |
| 2–4 秒 | 绿底、两枚白块 | 600 Hz | 3.200 秒，第 96 帧 |
| 4–6 秒 | 蓝底、三枚白块 | 800 Hz | 4.200 秒，第 126 帧 |

帧编号从 0 开始。画面顶部八格从左到右是帧号的二进制表示（白 = 1），底部白块每帧移动。色相之外还可用帧号和白块区分静止旧帧、继续播放和拖动后重新出帧；色相相同不代表帧号相同。

## 主机生成与校验

从仓库根执行，需要本机 Python 3 和带 `libx264`、`aac` 编码器的 FFmpeg / ffprobe：

```sh
batch=$(mktemp -d work/marble-media-check.XXXXXX)
python3 -B tests/fixtures/marble-media/generate.py --out "$batch/samples"
python3 -B tests/fixtures/marble-media/generate.py --verify "$batch/samples"
PYTHONDONTWRITEBYTECODE=1 python3 -B -m unittest tests.test_marble_media_fixture
```

生成模式只写新目录；生成报告记录文件 SHA-256、编码器版本、全帧/PCM 哈希和三个 seek 落点。校验模式先核对该次生成的长度与 SHA，再实际解码，验证 180 帧的时间戳、色度、帧号与移动标记，核对三个声音频率及关键帧位置（每秒一个）。三处 seek 在主机分别重新打开文件验证，不代表同一播放器完成了暂停、拖动和恢复。AAC 解码器可能输出编码填充样点，报告将解码样点数和呈现时长分开。不同编码器版本可能生成不同字节；每次实测固定一组报告与文件，不混用不同生成批次。

## 原 AVPlayer 探针：只验时间线

用本地 OHOS SDK [交叉构建](../../../compat/codec/build-native-avplayer-probe.sh)，不会启动目标程序：

```sh
build=$(mktemp -d work/marble-avplayer-check.XXXXXX)
AVPLAYER_PROBE_OUT="$build" sh compat/codec/build-native-avplayer-probe.sh
```

接机后只在目标机的 OHOS 运行环境中使用同一组已核验的样本，分别运行 `timeout -k 2 30 ./native_avplayer_probe /path/to/tones.wav` 和 M4A 对照。外层超时也覆盖 SDK 调用或释放挂起。探针要求 `SEEKDONE` 和 `GetCurrentTime` 都落在 4200 / 1200 ms 的各 ±150 ms 内，随后还须取得恢复推进、自然结束、同步释放，才打印 `AVPLAYER_PROBE_PASS evidence=timeline_only`。这条控制链和原 App 的界面、扬声器出声、真实 PCM 分开验；无需变更权限或打开网络源。

## 接机后的独立验收

1. **交付对照**：固定 boot ID、镜像、文件大小和读回 SHA。使用装机中原有的本地文件入口；若测试 Web 视频，先单独通过 [Web DOM/Canvas 正负对照](../marble-webview/README.md)，再核实际 HTTP 状态、媒体 MIME、所需字节范围和播放器收到的媒体长度。传输或导航失败单独记录。
2. **原视频播放器**：用同一个 MP4 播放实例记录实际时间推进与两个不同帧号。先暂停，拖到约 4.2 秒并观察蓝底，向后拖到约 1.2 秒并观察红底；分别记录所选目标与播放器报告的实际着陆。若只能停在前一个关键帧，记录该模式的真实落点，并和精确 seek 分开统计。恢复后帧号和音调应随时间推进，到 6 秒自然完成；首帧、暂停后静止以及拖动后变化分别取证。浏览器、应用内 WebView 和系统原播放器独立记账。
3. **原音乐入口和音频对照**：先以 WAV 验播放、暂停、前后拖动及原扬声器输出，再以同内容 M4A 定位 AAC 或容器差异。UI 结果分开保留；时间轴走动本身不证明扬声器出声。若测音画同步，使用同一时基的外部摄音或真实 PCM 与画面采样，注明精度。硬件解码须另有实际组件、映射和驱动调用证据。

这三个样本是媒体业务验收对照，不指定解码器产出的 native buffer 格式。即使 MP4 显示通过，也要看真实 `externalFormat` 才知道覆盖了 NV12 或 NV21，不能从容器中的 I420 推定底层分配格式。
