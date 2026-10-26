# uxplay-mini

[English](README.md) | [简体中文](README_CN.md)

从 UxPlay 源码裁剪出的独立 AirPlay → RTP 接收程序，也可作为其他应用的 sidecar。
项目独立构建，不依赖父工程。保留局域网发现、配对、
FairPlay 握手、音视频解密、音频重传和 NTP 同步；移除 GStreamer/GLib、
渲染器、播放窗口、HLS、反向 HTTP/Event、录制、X11 和 D-Bus。

视频不解码、不转码，直接从 Annex B 封装成 H.264/H.265 RTP。
默认仅启用 H.264；通过 `--h265` 显式允许 H.265/HEVC。
AAC-ELD/AAC-LC/ALAC 统一通过 FFmpeg 解码成 L16 PCM RTP，所有平台共用实现。
只链接 libavcodec、libavutil，不使用 libavformat、libavdevice、libavfilter、
libswresample、libswscale 或 FFmpeg 命令行程序。

## 依赖

| 依赖 | 用途 |
| --- | --- |
| OpenSSL libcrypto ≥ 1.1.1 | 配对、密钥交换、AES 解密；不链接 libssl |
| libplist ≥ 2.3 | AirPlay 二进制 plist |
| llhttp | 从 vcpkg 的 CMake target 引用；项目内不附带 llhttp 源码 |
| libavcodec ≥ 59、libavutil ≥ 57 | AAC 和 ALAC 解码，以及样本格式转换 |
| PlayFair 源码 | UxPlay 随附的 FairPlay 协议实现 |

macOS 默认使用系统 Bonjour；Windows、Linux、FreeBSD 使用随源码保留的
最小 mDNS responder。也可以在 macOS 使用内置后端。Unix 使用系统 pthreads，
Windows 使用 Win32 线程和同步原语，不需要额外的 pthread 库。

当前只在 macOS arm64 验证构建和自动化测试。Windows、Linux、FreeBSD
保留了平台实现，尚未在对应系统构建或实机验证。

## 构建

`CMakePresets.json` 提供参考构建配置。将 `VCPKG_ROOT` 设置为 vcpkg 安装目录后，
在项目根目录执行：

```sh
cmake --preset arm64-macos-release
cmake --build build/binary/arm64-macos-release
ctest --test-dir build/binary/arm64-macos-release --output-on-failure
```

Debug 使用 `arm64-macos-debug`。Windows、Linux 可以使用参考文件中的对应
桌面 presets。移动平台和 WebAssembly presets 只是保留参考配置，不代表受支持。

不使用 preset 时：

```sh
cmake -S . -B build/local -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build/local
```

FreeBSD 也可以用以上方式配置，并根据已安装依赖指定 `VCPKG_TARGET_TRIPLET`。
在 FreeBSD 构建前，请确认使用的 FFmpeg 端口支持该平台，并准备可用的
libavcodec/libavutil。
这里不会自动修改或安装该平台的依赖。

配置选项：

| 选项 | 默认值 | 用途 |
| --- | --- | --- |
| `UXPLAY_MINI_WITH_AUDIO` | `ON` | 设为 `OFF` 可完全去掉 FFmpeg 链接，生成纯视频版 |
| `UXPLAY_MINI_INTERNAL_MDNS` | `OFF` | macOS 改用内置 mDNS；其他平台始终使用内置后端 |
| `UXPLAY_MINI_SANITIZERS` | `OFF` | 为项目源码启用 ASan 和 UBSan |
| `BUILD_TESTING` | `ON` | 构建单元和集成测试；Python 3 仅用于集成测试 |

## 使用

```sh
./build/binary/arm64-macos-release/uxplay-mini \
  -n "UxPlay Mini" --host 127.0.0.1 \
  --video-port 5000 --audio-port 5002 --sdp build/uxplay-mini.sdp
```

在同一局域网的 iPhone 上打开“屏幕镜像”，选择 `UxPlay Mini`。
程序不创建播放窗口，数据通过 UDP 发送到目标地址；Ctrl-C 或 SIGTERM 退出。
接收方应先监听对应端口。全部参数见 `--help`。

### 固定 AirPlay 密码

添加 `--password`，要求 iPhone 连接时输入指定密码：

```sh
./build/binary/arm64-macos-release/uxplay-mini --name "Private receiver" --password "example-pass"
```

在 iPhone 提示时输入相同密码。只要启动时传入相同参数，多次连接和进程重启后密码都不变。
省略该参数则关闭密码认证。密码长度为 1–64 字节，不能包含控制字符；空密码会被拒绝。
不提供随机验证码模式。mini 日志不会输出密码，但命令行参数可能出现在操作系统的进程列表中。

此功能认证传入的 AirPlay 会话，不改变输出的 RTP/RTCP 或音视频格式。桌面应用需要显式
传入该参数才能启用密码认证；当前桌面启动器尚未传入。

多字符选项统一使用双横线，参数值以空格分隔。只提供 `-h`（帮助）、`-n`（名称）、
`-d`（调试）三个单字母短选项。不保留原版 UxPlay 的命令行兼容别名；
未知选项、缺少参数值和超出范围的数值均报告错误，不会静默忽略。

默认不向发送端宣告 HEVC 能力，并拒绝 H.265 视频。需要 H.265 时，在启动参数中
添加 `--h265`；程序会设置对应的 AirPlay 能力位，并在 SDP 中加入
H265/90000 PT 98。启用后仍允许 H.264，最终编码由发送端选择，不会强制转码。
接入只支持 H.264 的客户端时，不要添加此开关。

`--no-audio` 仅关闭音频解码和音频 RTP/RTCP 输出；仍接受音频协议协商及收包，
收到的数据直接丢弃。必须保留 AirPlay 音频能力声明，否则部分 iPhone 会一直转圈，
约一分钟后主动断开。`UXPLAY_MINI_WITH_AUDIO=OFF` 构建也遵循此行为，且完全不链接 FFmpeg。

| 媒体 | RTP 默认端口 | PT | 编码与时钟 |
| --- | --- | --- | --- |
| H.264 视频 | 5000 | 96 | H264/90000，RFC 6184 单 NAL / FU-A |
| H.265 视频（需 `--h265`） | 5000 | 98 | H265/90000，RFC 7798 单 NAL / FU |
| 音频 | 5002 | 97 | L16/44100/2，有符号 16 位大端、双声道交错 |

默认 RTP 最大 UDP 数据报为 1200 字节，通过 `--mtu` 调整。视频访问单元的
最后一个包设置 marker；L16 分包保持完整双声道采样帧，时间戳按样本数递增。
音频保留客户端的原始 RTP 采样时钟，不使用 UDP 到达时刻生成采样时间戳。
NTP 尚不可用时，仅建立一次本地时间锚点，再按采样数推进，避免到达抖动变成静音缺口。
RTCP 发送到对应 RTP 端口 + 1，包含 Sender Report 和共同的 CNAME，
用于将两个媒体时钟映射到同一个 NTP 时间轴。不实现 RTCP 反馈或下游丢包重传。

`--device-id` 可指定稳定且唯一的接收器 MAC 形式标识。多个实例应分别设置
名称、device-id 和输出端口。`--keyfile` 可以保存随机配对密钥；不指定时沿用
上游基于 device-id 派生密钥的行为。默认新客户端可以接管连接，`--hold` 禁止接管。

## 接入 RTP 客户端

客户端通过 `--host` / `--video-port` / `--audio-port` 指定输出地址和端口，
按照上表中的 payload type 和编码处理收到的 RTP 数据。不需要 GStreamer
运行环境。项目只提供接收和转发程序，不包含播放器或特定应用的集成代码。

Electron 音频端需要监听音频 RTP，解包 L16 后按大端读取 int16 并转换为
浮点 PCM，再交给 AudioWorklet。RTP 本身不能直接交给 HTML audio 或 WebCodecs；
无需依赖 Electron 对 AAC-ELD/ALAC 的原生解码支持。播放器需实现适量缓冲和时钟同步。

## 验证范围

自动化测试覆盖 H.264/H.265 分片重组、marker、序列号回绕、RTP/RTCP 时间映射、
命令行帮助、参数命名、旧别名拒绝、缺值和取值范围，
L16 字节序、音频无 NTP/时钟抖动/采样时间戳回绕、AAC-ELD/AAC-LC 初始化、ALAC 实际解码、UDP 收发、RTSP 增量解析、
二进制 plist、配对 setup 接口、HLS 拒绝、默认/显式启用 H.265 的能力位与 SDP，
以及进程退出。macOS 还通过系统独立的 `dns-sd` 客户端检查 AirPlay 服务的端口、
TXT 广播记录及 HEVC 能力位。
另有显式 `--no-audio` 回归测试，检查 `/info` 和 DNS-SD TXT 中保留音频能力，
但 SDP 与日志不再宣告下游音频输出。

测试不等同于完整 iPhone 实机验收：AirPlay 的完整握手、真实镜像媒体、
长时间音画同步及其他桌面平台仍需实机验证。

源码来源、精简记录和许可证见 [`UPSTREAM.md`](UPSTREAM.md) 与 [`LICENSE`](LICENSE)。
整体项目为 GPL-3.0-or-later，原文件中的各项许可证声明均保留。
