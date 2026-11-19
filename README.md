# McastPilot

[English](README.en.md) | 简体中文

组播推流控制台 —— 把 VLC 里点来点去的配置，变成**一次预设、一键启停**。

> GitHub: <https://github.com/jiexu-bit/McastPilot>
> 完整设计见 [DESIGN.md](DESIGN.md)；接手/交接请看 [HANDOVER.md](HANDOVER.md)（现状、代码地图、踩坑记录、下一步）。

- **C++20**，复用 **FFmpeg** 做解封装/编码，自研 TS 组播发送层
- **C/S 架构**：`mcastd` 常驻服务端 + 原生 Web 控制台 + Qt6 桌面客户端
- **多网卡**识别与强制绑定，**组播/单播**推流，TTL / DSCP / CBR / 实时统计
- 任务持久化、断流自动重连、内置 UDP/MPEG-TS 接收监测

## 状态

| 里程碑 | 内容 | 状态 |
|---|---|---|
| M1 | 网卡枚举 + 组播/单播发送 + FFmpeg 封装 + CLI | 完成，本机回环已验证 |
| M2 | `mcastd` + REST/WS + Web 控制台 + 接收监测 | 完成，本机联调已验证；跨机 B 电脑访问待测 |
| M3 | Qt6 桌面客户端 | 完成，本机启动/连接/任务展示/启停/VLC 已验证 |
| M4 | 采集/转码/录制/多目的地 | 进行中：多目的地 + TS 录制 + H.264/AAC 转码已完成；采集/屏幕待做 |

### M1 实测（Windows 11 / WLAN）

| 项目 | 结果 |
|---|---|
| 输入 | 720p25 H.264 + AAC，10s 循环 |
| 输出 | MPEG-TS CBR 4.5 Mbps，`pkt_size=1316` |
| 发送速率 | 4.47–4.51 Mbps |
| 接收端丢包 | 0（组播回环） |
| TS 同步错误 / CC 错误 | 0 / 0 |
| 跨循环点 | 无 `dts` 回退、无 PCR 异常 |

### M2 实测（本机）

| 项目 | 结果 |
|---|---|
| 服务端 | `mcastd` 单进程，HTTP + WebSocket 正常 |
| 任务 | 新建、启动、停止、删除、编辑、服务重启后保留 |
| 并发 | 2 路同文件、各 4.5 Mbps CBR：均 4.47 Mbps，0 丢弃/错误 |
| 统计 | 1Hz 推送码率、缓冲、包数、丢包、发送错误 |
| 日志 | 每个任务独立日志环形缓冲，WebSocket 实时追加 |
| 断流重连 | 文件 EOF 后按退避自动重启，恢复 running |
| 接收监测 | 单播/组播均 4.47–4.48 Mbps，TS/CC 错误 0 |
| 安全 | 认证开启时无 token 401；路径穿越、Windows junction 外跳均 403 |
| 重启统计 | 停止再启动后发送计数归零，不累计旧数据 |

### M3 实测（本机）

| 项目 | 结果 |
|---|---|
| 桌面端 | `mcastgui.exe` 启动、连接 `mcastd`、显示任务/状态卡片 |
| 任务操作 | 停止/启动按钮驱动服务端任务，状态 1Hz 刷新 |
| VLC | 桌面端“VLC”按钮启动本机 VLC 播放当前任务 |
| 高码率 | 4K HEVC 25 Mbps 持续发送，buffer 2–4%，0 丢包/错误 |
| 打包 | windeployqt + 运行时 DLL 复制，双击 exe 可运行 |

### M4 实测（多目的地 / 录制 / 转码）

| 项目 | 结果 |
|---|---|
| 多目的地 | 同一源同时推 `127.0.0.1:5300` 和 `:5301`，两路均收到，0 TS/CC 错误 |
| 统计 | `destinations=2`，总发送约 50 Mbps（每路 25 Mbps） |
| 录制 | 自动写 `%LOCALAPPDATA%\Mcast\recordings\<id>_<时间戳>.ts` |
| 录制校验 | ffprobe 识别 HEVC 3440x1440 + AAC，文件可正常解析 |
| 转码 | 720p H.264 → H.264 2 Mbps/AAC 128k，CBR 3 Mbps，TS/CC 错误 0 |
| 跨编码 | HEVC 720p → H.264/AAC，ffprobe 输出 H.264 |
| 转码循环 | 8s 文件连续跑 12s 跨循环，TS/CC 错误 0 |
| copy 回归 | copy 模式 4.5 Mbps 正常，0 丢弃/错误 |

## 构建

工具链是**便携**的，装在项目外的独立目录，删掉即卸载，不动系统环境：

```powershell
# 1) 安装便携 MSYS2 + UCRT64 工具链（gcc / cmake / ninja / gdb / ffmpeg / qt6）
powershell -ExecutionPolicy Bypass -File scripts\setup-toolchain.ps1

# 2) 编译
powershell -ExecutionPolicy Bypass -File scripts\build.ps1

# 3) 卸载工具链（可选）
powershell -ExecutionPolicy Bypass -File scripts\clean-toolchain.ps1
```

产物：

```
build\bin\mcastctl.exe   命令行推流工具
build\bin\mcastd.exe     常驻服务端
build\bin\mcastgui.exe   Qt6 桌面客户端（已带 Qt 运行库）
build\bin\web\           网页端静态资源（构建时自动复制）
```

> `mcastgui.exe` 已经通过 windeployqt 和运行时 DLL 复制做到双击可运行。
> `mcastctl` / `mcastd` 依赖 FFmpeg DLL，请用 `scripts\mcastctl.ps1` / `scripts\mcastd.ps1` 包装运行，
> 或把 `toolchain\msys64\ucrt64\bin` 加到你自己的 PATH。

> Windows 上 360 等安全软件可能把自编译的 `mcastd.exe` 误报为 `HEUR/QVM...` 并删除/锁死。
> 请把 `F:\091-Opencode\推流工具\build` 加入信任区/白名单，而不是关闭杀毒软件。

依赖：`third_party/httplib.h`（cpp-httplib 0.66.0）、`third_party/nlohmann/json.hpp`（nlohmann/json 3.12.0），均为单头文件。

## 快速使用

### 1. 网卡、探测与 CLI 推流

```powershell
# 列出网卡（含 IP / 速率 / 虚拟网卡标记）
powershell -File scripts\mcastctl.ps1 nics

# 探测输入源的编码信息
powershell -File scripts\mcastctl.ps1 probe --input D:\Videos\demo.ts

# 推组播：从指定网卡 192.168.1.10 推到 239.1.1.1:5000
powershell -File scripts\mcastctl.ps1 send `
    --input D:\Videos\demo.ts --loop `
    --group 239.1.1.1 --port 5000 `
    --nic 192.168.1.10 --ttl 16 --muxrate 8000000

# 推单播
powershell -File scripts\mcastctl.ps1 send `
    --input D:\Videos\demo.ts --loop `
    --group 192.168.1.50 --port 5000 --muxrate 8000000
```

### 2. 启动服务端与网页控制台

```powershell
powershell -File scripts\mcastd.ps1 `
    --listen 127.0.0.1 --port 8787 `
    --media-root D:\Videos
```

启动后浏览器打开 <http://127.0.0.1:8787/> 即可。默认 **不启用 token**，局域网测试直接进；需要鉴权时在“设置”里打开，并填写自定义 token（留空会自动生成）。

网页端支持：

- 新建 / 编辑 / 启停 / 删除任务
- 网卡下拉、媒体库只读浏览、输入探测
- 每任务实时码率 / 缓冲 / 丢包 / 发送错误
- 任务日志实时查看
- 多目的地：一个任务最多 4 个额外 `group:port`
- TS 录制：按任务开关，写入服务端数据目录
- 转码：可选 H.264 + AAC（libx264，视频/音频码率、preset、GOP 可配）
- 接收监测（组播加入 / 单播监听、TS/CC 统计）
- 任务运行中可点“VLC”按钮，在**服务器本机桌面**直接打开 VLC 播放该流（仅本机访问时可用）

默认数据目录 `%LOCALAPPDATA%\Mcast\`：

```
config.json      监听地址、端口、auth.enabled、token、media.roots、web.enabled、vlc_path
tasks.json       任务预设（服务重启后保留，状态回到 idle）
logs/mcastd.log  滚动日志
recordings/      勾选录制后生成的 TS 文件（<taskid>_<时间戳>.ts）
```

常用参数：

| 参数 | 默认 | 说明 |
|---|---|---|
| `--listen` | `127.0.0.1` | 监听地址；跨机访问改成 `0.0.0.0` |
| `--port` | `8787` | HTTP 端口 |
| `--data-dir` | `%LOCALAPPDATA%\Mcast` | 配置/任务/日志目录 |
| `--media-root` | `%USERPROFILE%\Videos` | 媒体库白名单根目录，可重复 |
| `--web` / `--no-web` | 开 | 是否托管网页端 |
| `--auth` / `--no-auth` | 关 | 是否要求访问 token |
| `--token` | 空 | 自定义 token；认证开启且为空时自动生成 |
| `--vlc-path` | 自动检测 | “用 VLC 打开”按钮使用的 vlc.exe 路径 |
| `--log-level` | `info` | trace/debug/info/warn/error/off |

### 3. Qt6 桌面客户端

直接双击 `build\bin\mcastgui.exe`，或：

```powershell
powershell -File scripts\mcastgui.ps1
```

- 默认连接 `http://127.0.0.1:8787`
- 认证关闭时 Token 留空即可；开启认证时填 token 后点“连接”
- 支持新建/编辑/启停/删除、网卡下拉、服务器媒体库、任务日志、接收监测
- 运行中的任务可直接点“VLC”在**启动 GUI 的这台电脑**上播放

桌面端通过 REST 每秒轮询服务端；服务端/网页端变化会自动反映到列表。

### 4. 接收验证

网页端点“监测”，填组播地址/端口和接收网卡即可看到码率、包数、TS 同步错误、CC 错误。监测结束或到时后会自动释放 UDP 端口，不影响 VLC 等接收端继续播放。

VLC 接收组播时必须带 `@`：

```text
udp://@239.1.1.1:5000
```

不要写成 `udp://239.1.1.1:5000`，后者不会加入组播组。

也可以直接用仓库自带脚本：

```powershell
# 组播
python tests\udp_receiver.py --group 239.1.1.1 --port 5000 --iface 192.168.3.15 --seconds 10

# 单播
python tests\udp_receiver.py --group 127.0.0.1 --port 5000 --seconds 10

# 官方播放器
ffplay "udp://@239.1.1.1:5000?overrun_nonfatal=1&fifo_size=5000000"
```

> 同机测试组播时，发送任务要勾选“组播回环”（CLI 加 `--loopback`）。

## API 摘要

所有 `/api/v1/*`（除 `health`）默认开放。开启认证后需要 `Authorization: Bearer <token>`；WebSocket 用 `?token=`。认证在网页“设置”里开关，token 可自定义；也可用 `--auth` / `--no-auth`、`--token` 启动参数。

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/v1/health` | 版本、运行时长、任务数 |
| GET | `/api/v1/nics` | 网卡列表 |
| GET | `/api/v1/media/roots` | 媒体库根目录 |
| GET | `/api/v1/media/list?path=` | 只读浏览白名单目录 |
| GET/POST | `/api/v1/tasks` | 任务列表 / 新建 |
| GET/PATCH/DELETE | `/api/v1/tasks/{id}` | 详情 / 修改 / 删除 |
| POST | `/api/v1/tasks/{id}/start|stop` | 启动 / 停止 |
| POST | `/api/v1/tasks/{id}/open-vlc` | 在服务器本机用 VLC 播放（仅本机访问） |
| GET | `/api/v1/tasks/{id}/stats` | 实时统计 |
| GET | `/api/v1/tasks/{id}/logs?tail=200` | 最近日志 |
| POST | `/api/v1/probe` | 探测输入 |
| GET/PATCH | `/api/v1/settings` | 全局设置 |
| GET | `/api/v1/monitor` | 当前接收监测状态 |
| POST | `/api/v1/monitor/start|stop` | 开始 / 停止接收监测 |
| WS | `/api/v1/events` | 任务/统计/日志推送 |

## 后续协议规划

当前输出只有 **UDP（组播/单播）MPEG-TS**。任务 JSON 里的 `output.protocol` 目前由目的地址推导为 `udp-multicast` / `udp-unicast`，网页端还没有协议下拉框。

工具链里的 FFmpeg 已确认支持 `rtmp`、`srt`、`rtsp`、`hls` 等协议和 muxer。后续会按下面的路线扩展：

| 阶段 | 协议 | 实现方式 |
|---|---|---|
| 现在 | UDP 组播/单播 | 自研 socket + 令牌桶 + CBR TS |
| 下一步 | RTMP / RTMPS | FFmpeg `flv` muxer + `rtmp://` 输出 |
| 下一步 | SRT / RTP | FFmpeg `mpegts` + `srt://` / `rtp_mpegts` |
| 之后 | RTSP | FFmpeg `rtsp` muxer |
| 之后 | HLS / 录制文件 | FFmpeg `hls` / `mpegts` 文件输出 |

架构上需要把现在紧耦合的 UDP 发送层抽成 `OutputSink`：

```text
Pipeline(解封装/封装)
   └── OutputSink
        ├── UdpTsSink      现有组播/单播、CBR、节奏、统计
        ├── FfmpegUrlSink  rtmp/srt/rtsp/http URL
        └── FileSink       录制/落盘
```

任务模型会增加协议选择、目标 URL/服务器/流密钥等字段，网页端增加协议下拉和对应表单；UDP 之外的协议由 FFmpeg 负责传输，断流重连仍由 TaskManager 统一管理。

## 已知限制

- **跨机（第二台电脑）未实测**：M1 本机回环、M2 本机浏览器/接口均已通过，但“B 电脑浏览器管理 + VLC 稳定播放”还没跑。
- Windows 本机回环的组播发送突发窗口不能太大：当前默认 5 ms；跨机建议实测后再调 `--muxrate`。
- 发送端默认 `copy`，也可选 H.264/AAC 软件转码；采集卡、屏幕捕获尚未实现。
- Qt 桌面端目前用 REST 1Hz 轮询，没有接 WebSocket；实时性足够，但不如 Web 端推送即时。
- 桌面端的服务器地址和 token 保存在本机 `QSettings` INI 中（明文），没有接 Windows 凭据管理器。
- 桌面端是源码构建产物，没有做安装包/签名；换机器需要带 `build\bin` 下的 Qt 运行库或重新构建。
- 输出目前只有 UDP 组播/单播 MPEG-TS；RTMP/SRT/RTSP/HLS 属于后续协议扩展，见上文路线。
- “用 VLC 打开”只能在服务器本机访问网页时使用；从 B 电脑点击会弹提示并给出可复制的 VLC 地址，不会静默失败。
- 认证默认关闭；一旦 `--listen 0.0.0.0` 暴露到局域网，建议在设置里启用 token，避免任何人都能控制推流。
- 局域网 HTTP，没有 HTTPS / 用户体系；跨机使用请只放在可信网络。
- 任务状态不跨服务重启自动恢复：`tasks.json` 会保留任务，但重启后统一回到 `idle`。
- 录制写的是 CBR MPEG-TS 原始流，不做切片、滚动清理或转码；文件路径固定为数据目录 `recordings/`。
- 转码是软件 libx264 编码，4K 高帧率源可能跟不上实时；暂不支持硬件编码、分辨率缩放。
- 采集卡、屏幕捕获尚未实现，属于 M4 后续。
- 接收监测同一时间只跑一路；多路同时监测留给后续版本。
- 仅 IPv4；Linux 分支代码在，但未编译验证。
- FFmpeg 对 B 帧流会打印 `dts < pcr, TS is invalid`，输出本身可正常解码，已在日志层过滤。

## 目录

```
src/common   日志、环形缓冲、配置、滚动文件日志
src/net      网卡枚举、组播发送、UDP/TS 接收监测、平台层
src/media    FFmpeg 输入、TS 封装、发送节奏、probe
src/task     任务模型、TaskManager、持久化、1Hz 采样、自动重连
src/api      REST + WebSocket + token 鉴权 + 静态页
src/server   mcastd 入口
src/cli      mcastctl
src/gui      Qt6 桌面客户端（REST 客户端）
web/index.html / app.js / style.css
third_party  httplib.h、nlohmann/json.hpp
tests        udp_receiver.py
```
