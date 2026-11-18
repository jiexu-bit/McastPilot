# McastPilot 交接文档

> 更新时间：2026-11-18
> 代码状态：**M1、M2、M3 完成；M4 进行中（多目的地 + 录制 + H.264/AAC 转码已完成）**
> 项目目录：`F:\091-Opencode\推流工具`
> 工具链目录：`F:\091-Opencode\toolchain\msys64`（项目外，可整体删除）

---

## 0. 一页速览

| 项 | 内容 |
|---|---|
| 做什么 | C++ 写的组播/单播 MPEG-TS 推流工具，替代 VLC 那套繁琐点选配置 |
| 架构 | C/S：`mcastd` 服务端 + 原生 Web 控制台 + Qt6 桌面客户端 |
| 现在到哪 | **M1/M2/M3 完成，M4 进行中**：发送内核、CLI、服务端、任务管理、REST/WS、网页端、Qt 桌面端、接收监测、多目的地、TS 录制、H.264/AAC 转码 |
| 产物 | `build\bin\mcastctl.exe`、`build\bin\mcastd.exe`、`build\bin\mcastgui.exe`、`build\bin\web\` |
| 技术栈 | C++20 / CMake / Ninja / GCC / FFmpeg / cpp-httplib / nlohmann-json / Qt6 / 便携 MSYS2 |
| 代码量 | `src` 约 7k 行，新增 `task/api/server/gui/web` |
| 未验证 | **跨机**：B 电脑浏览器/GUI 管理 + VLC 播放仍是唯一未闭环验收项 |
| 下一步 | M4 剩余：采集卡/屏幕捕获；协议扩展见 DESIGN 第 15 节 |

---

## 1. 需求边界（已确认）

| 问题 | 结论 |
|---|---|
| 平台 | Windows 优先，网络层抽象，后续可编 Linux |
| 推流引擎 | FFmpeg，进程内链接，不调 `ffmpeg.exe` |
| 输入源 | 本地文件循环、网络流转推（M4 补采集/屏幕） |
| 输出 | UDP 组播（核心）、UDP 单播；RTMP/RTSP/SRT、录制、多目的地属 M4 |
| 客户端 | **Qt 桌面 GUI 为主**（M3 已完成），Web 为可选远程端（M2 已完成） |
| 部署 | 先同机，协议按跨机设计 |
| 组播能力 | 指定网卡/源 IP、TTL、DSCP、回环、CBR、自动重连、多路并发 |
| 转码 | 默认 `copy` |
| 规模 | ≤5 路，每路 ≤20 Mbps |
| 文件选择 | 网页只浏览服务端白名单目录，只读 |
| 工具链 | 便携 MSYS2 UCRT64，无管理员、可一键删除 |

---

## 2. 架构

### 2.1 组成

```
┌──────────────────────────────┐         ┌──────────────────────────────┐
│  mcastgui (Qt6)               │         │  Browser (本机 / B 电脑)      │
└──────────────┬───────────────┘         └──────────────┬───────────────┘
               │  HTTP/JSON + WebSocket                │
               └───────────────┬───────────────────────┘
                               ▼
┌──────────────────────────────────────────────────────────────────────┐
│  mcastd                                                              │
│  api/      REST + WS + 静态页 + token 鉴权                            │
│  task/     任务模型、状态机、持久化、1Hz 采样、自动重连、任务日志       │
│  media/    FFmpeg 输入 → mpegts CBR → 节奏                            │
│  net/      网卡、组播/单播发送、UDP/TS 接收监测、统计                  │
│  common/   环形缓冲、日志、配置、滚动文件日志                          │
└──────────────────────────────────────────────────────────────────────┘
                               │  MPEG-TS / UDP
                     239.x.x.x:5000  ──────────►  VLC / 机顶盒 / 解码器
```

### 2.2 进程与线程

| 进程 | 职责 |
|---|---|
| `mcastctl` | CLI 单次推流，直接持有 Pipeline |
| `mcastd` | 常驻服务端，持有任务表、HTTP/WS、多个 Pipeline |

`mcastd` 线程：

| 线程 | 职责 |
|---|---|
| 主线程 | 参数、配置、信号、启动/停止服务 |
| HTTP/WS 线程池 | cpp-httplib 默认线程池，处理 REST 和 WebSocket |
| TaskManager worker | 1Hz：采样统计、检测断流、自动重连、广播状态 |
| 每个任务 2 线程 | reader（解封装/封装）、sender（令牌桶/sendto） |
| UdpReceiver | 可选，接收监测线程 |

### 2.3 数据流

```
文件/URL
   │ av_read_frame
   ▼
AVPacket ──(copy)──► mpegts muxer（muxrate=CBR，补空包）
                          │ 32KB AVIO
                          ▼
                     ByteRing（4MB，188 对齐丢包）
                          │ 1316B/包
                          ▼
                    令牌桶（5ms 突发）
                          │
                          ▼
                  UDP socket（网卡绑定 / TTL / DSCP）
                          │
                          ▼
                   239.x.x.x:5000
```

控制面：Web/API → TaskManager → Pipeline；统计与日志反向经 WS 推送。

---

## 3. 关键实现

### 3.1 网卡枚举与强制绑定

- Windows `GetAdaptersAddresses(AF_UNSPEC)`：友好名、GUID、IfIndex、速率、MAC、IPv4/IPv6、网关
- 过滤 169.254/16、IPv6 link-local；标记 virtual
- `IP_MULTICAST_IF` + `bind(源IP:0)` 双保险；Linux 额外 `SO_BINDTODEVICE`
- `--nic` 支持 IP、名称/描述子串、1 基序号；没有 IPv4 直接报错

代码：`src/net/nic.cpp`、`src/net/multicast_sender.cpp`

### 3.2 时间戳归一化（M1 踩坑最多）

问题：B 帧源首包 `dts > pts`，直接减 `dts` 会让 PTS 变负，muxer 持续报 `dts < pcr`。

最终方案：

1. 可 seek 输入先预读最多 600 包，取所有流 `min(dts, pts)` 换算到 AV_TIME_BASE
2. 回到文件头，`out_shift = -min_ts`，保证最早时间戳为 0
3. 循环播放 `loop_offset = last_norm + 1`，所有流加同一偏移，A/V 不漂
4. 实时节流用 `norm_dts - loop_offset`，每轮从 0 重新计时

不可 seek 的直播流跳过预读，退回 `ic->start_time`。

代码：`Pipeline::ReaderLoop()`

### 3.3 发送节奏（Windows 定时器坑）

问题：Windows 默认定时器 15.6 ms，`sleep_for(2ms)` 实际睡 15 ms；按“请求时长”记账会严重偏慢。

方案：

- 令牌按**实测流逝时间**补充
- 每次醒来尽量把 budget 用满（批量发）
- `timeBeginPeriod(1)`，退出 `timeEndPeriod(1)`
- 突发窗口 **5 ms**（20 ms 在 Windows/Wi-Fi 回环实测丢 50%）

代码：`Pipeline::SenderLoop()`、`net::InitSockets()`

### 3.4 环形缓冲与 TS 对齐

- `ByteRing(capacity, 188)`：丢包按 188 粒度，保持 TS 边界
- 发送前检查 `buf[0] == 0x47`，失步时在 `+0/+188/+376` 找同步，找不到整块丢
- 计数 `buffer_drop_bytes` 和 non-TS chunks

代码：`src/common/byte_ring.cpp`、`FindTsSyncOffset()`

### 3.5 CBR

- `muxrate` 交给 mpegts muxer 补空包
- 未指定时取输入码率 ×1.15（下限 1 Mbps）
- `muxrate` 必须 ≥ 实际码率，否则 ring 持续丢包
- `pkt_size` 必须是 188 的倍数

### 3.6 FFmpeg 日志过滤

`dts < pcr, TS is invalid` 对 B 帧 copy 是良性但会刷屏。自定义 `av_log` 回调只过滤这一条，其余 warning/error 走统一日志 sink。

### 3.7 单播 / 组播统一

`MulticastSender::Open()` 根据目的地址判断：224/4 走组播路径（`IP_MULTICAST_IF`/TTL/LOOP），否则走单播路径（`IP_TTL`，OS 路由）。所以 `--group` 既可填 `239.x.x.x`，也可填 `192.168.x.x` / `127.0.0.1`。

### 3.8 配置、数据目录与媒体库

- 默认数据目录 `%LOCALAPPDATA%\Mcast`，可用 `--data-dir` 覆盖
- `config.json`：`listen`、`port`、`auth.enabled`、`token`、`media.roots`、`web.enabled`、`vlc_path`、日志参数
- **认证默认关闭**，局域网测试不需要 token；可在网页“设置”或 `--auth/--no-auth` 开关
- 启用认证时 token 可自定义（`--token` 或设置页）；为空则自动生成，写入配置并在控制台打印
- `vlc_path` 可选；“用 VLC 打开”按钮优先用它，否则自动检测常见安装路径
- 路径统一 UTF-8：Windows 用 `path.u8string()` / `fs::path(std::u8string)`，避免中文路径乱码
- 根目录和任务输入统一绝对化；`RealPathUtf8` 用 `GetFinalPathNameByHandleW` 解析 junction/symlink
- 网页端只允许白名单根目录下的媒体文件；网络输入允许 `rtsp/rtmp/http/https/udp/rtp/srt/tcp/mms`，拒绝 `file://` 绕过

代码：`src/common/config.*`、`src/api/http_server.cpp`

### 3.9 任务模型与 TaskManager

任务 JSON 采用设计里的嵌套结构：

```json
{
  "id": "e6c73443",
  "name": "demo loop",
  "source": { "type": "file", "uri": "D:\\Videos\\demo.ts", "loop": true },
  "output": { "protocol": "udp-multicast", "group": "239.1.1.1", "port": 5000,
              "nic": "192.168.1.10", "ttl": 16, "dscp": 0, "loopback": false,
              "pkt_size": 1316 },
  "mux": { "format": "mpegts", "muxrate": 8000000, "cbr": true },
  "encode": { "mode": "copy" },
  "restart": { "auto": true, "max_retries": 10, "backoff_ms": 1000 },
  "realtime": true,
  "speed": 1.0,
  "buffer_kb": 4096
}
```

状态机：

```
idle → starting → running → stopping → idle
                     └── error（自动重连：1s/2s/4s…上限 10 次，最长 30s）
```

- 启动时解析 `nic` 为源 IP，构造 Pipeline
- Tick 每 1 秒 `Snapshot()`，更新缓存并广播
- Pipeline 自行停止后：自动重连或进 error；成功运行 60s 后重试计数清零
- 停止/编辑/删除规则：只有 idle/error 可编辑删除；running/starting 必须先停止
- `tasks.json` 原子写入；服务重启后任务保留，状态回 idle（不自动拉起）
- 每个任务独立日志环形缓冲（500 行），sink 通过 Pipeline 线程上下文路由

代码：`src/task/task.*`、`src/task/task_manager.*`

### 3.10 REST / WebSocket / 安全

- 认证默认关闭；开启后 `Authorization: Bearer <token>`，WS 用 `?token=`；`/health` 始终匿名
- token 比较使用常量时间；设置页可开关认证、填写自定义 token
- 如果 `--listen 0.0.0.0` 且认证关闭，启动日志会明确警告局域网内任何人可控制
- 静态页由 mcastd 自己读文件（支持中文路径），不使用 httplib 的 mount
- `web.enabled` 可动态关闭；非 API 请求直接 404
- 媒体列表做 realpath 前缀校验，挡住 `..\`、symlink、Windows junction
- 请求体上限 1 MB；API 错误统一 `{"error": "..."}`
- WS 事件：`snapshot`、`task`、`deleted`、`log`、`pong`
- 更新回调在 TaskManager 锁外调用；WS 连接注册表与发送各有锁

代码：`src/api/http_server.*`、`web/*`

### 3.11 接收监测

- `UdpReceiver`：组播 `IP_ADD_MEMBERSHIP` / 单播 bind，500 ms 接收超时
- 统计：包数、字节、平均码率、运行时长、TS 同步错误、CC 连续性错误
- 单实例，同一时间只跑一路；`duration_sec=0` 一直运行
- API：`GET /api/v1/monitor`、`POST /api/v1/monitor/start|stop`
- 网页端“监测”弹窗 1Hz 轮询显示

代码：`src/net/udp_receiver.*`

### 3.12 日志与优雅停机
- `LOG_*` 写 stderr；可选 sink 同时写滚动文件、任务日志、WS
- Pipeline reader/sender 线程设置 `thread_local` 任务上下文，FFmpeg 日志也走同一 sink
- 文件日志按大小滚动，默认 10 MB × 3
- 停机顺序：`api.Stop()` → `TaskManager::StopWorker()` → `TaskManager::StopAll()` → 清 sink → 析构 → `ShutdownSockets()`
- 任务日志用独立 `log_mutex_`，避免 Stop 持有 manager 锁时与 pipeline 线程日志互锁

### 3.13 用 VLC 打开与协议扩展

- `POST /api/v1/tasks/{id}/open-vlc`：任务运行中时，用 `ShellExecuteW` 在**服务器本机**启动 VLC，参数为 `udp://@<group>:<port>`
- 只允许来自 loopback 或服务器本机 IP 的请求；从 B 电脑调用返回 403
- 单播任务的目标必须是本机地址，否则 VLC 在服务器上收不到
- VLC 路径优先取 `vlc_path`，否则自动检测 `Program Files` / `Program Files (x86)`
- 当前输出只有 UDP；`output.protocol` 目前由地址推导。新增 RTMP/SRT/RTSP/HLS 时需要把 UDP 发送层抽象成 `OutputSink`，详见 `DESIGN.md` 的协议扩展规划

### 3.14 Qt6 桌面客户端

- `src/gui/api_client.*`：`QNetworkAccessManager` 封装 GET/POST/PATCH/DELETE，token 可选，15s 超时
- `main_window.*`：连接栏、状态卡片、任务表、工具栏；1Hz 定时器轮询 `/api/v1/tasks`，每 5 秒刷新 health
- `task_dialog.*`：新建/编辑任务，网卡下拉、服务器媒体库、本地文件、探测；字段与 Web 端一致
- `dialogs.*`：日志、接收监测、服务器媒体库
- 认证关闭时 Token 留空即可；认证开启时填 token 后连接
- “VLC”按钮用 `QProcess::startDetached` 在**运行 GUI 的电脑**上打开 VLC，参数 `udp://@<group>:<port>`
- 服务器地址/token 保存在 `QSettings`（`McastPilot/mcastgui`，INI，明文）
- Windows 构建后用 `windeployqt` 部署 Qt 运行库，并额外复制 freetype 等非 Qt DLL，双击 `mcastgui.exe` 可运行
- 未接 WebSocket，因此桌面端是 1Hz 轮询；Web 端仍是实时推送

### 3.15 多目的地与录制（M4 第一批）

- `TaskSpec.extra_outputs`：最多 4 个额外 `group:port`，与主目的地共享网卡/TTL/DSCP/回环
- `Pipeline` 持有 `vector<unique_ptr<MulticastSender>>`：一次读 ring，同一个 1316B 包发送到所有目的地；令牌桶只按读取字节扣一次
- 统计聚合：`output_bytes/packets/errors` 为所有目的地之和，`destinations` 字段表示路数；总码率因此是发送总带宽
- 每个目的地独立 socket/统计，一个失败不影响其它
- 录制：`TaskSpec.record=true` 时，TaskManager 在 `<data_dir>/recordings/` 生成 `<id>_<毫秒时间戳>.ts`，Pipeline 在 muxer write callback 里同步写文件
- 录制内容是 CBR MPEG-TS 原始流（含空包），不做切片/转码/清理；`stats.recorded_bytes` 上报
- Web 表单：额外目的地每行一个 `group:port`；Qt 表单同样有额外目的地和录制开关
- 增加/删除目的地不需要改发送节奏；只影响 socket 数量与总带宽

### 3.16 H.264/AAC 转码

- 任务字段 `encode.mode`：`copy`（默认）或 `transcode`
- `transcode` 参数：`video_bitrate`、`audio_bitrate`、`preset`、`gop`
- `src/media/transcoder.*`：每个音视频流建立 解码器 → swscale/swr → libx264/AAC 编码器，输出到 mpegts
- 视频统一转 YUV420P H.264，`preset` + `tune=zerolatency`，默认无 B 帧；音频统一 AAC
- Pipeline 在 transcode 分支用微秒级时间偏移归一化输入时间戳，循环时递增 offset，保证跨循环 PTS 单调
- CBR：`muxrate=0` 时按 `video_bitrate + 音频码率` 估算；输出仍补空包
- copy 模式代码路径保持不变，转码是显式开关
- 新增链接 `libswscale`；要求 FFmpeg 带 libx264/AAC 编码器
- 当前限制：软件 4K 高帧率可能跟不上实时；无硬件编码、无分辨率缩放

---

## 4. 代码地图

| 文件 | 角色 |
|---|---|
| `src/cli/main.cpp` | mcastctl：nics / probe / send |
| `src/server/main.cpp` | mcastd：参数、配置、日志、信号、装配 |
| `src/media/pipeline.cpp` | 核心：FFmpeg 封装、时间戳、节奏、发送 |
| `src/media/probe.*` | 输入探测（CLI + API 复用） |
| `src/net/multicast_sender.cpp` | UDP 发送 socket、网卡绑定、统计 |
| `src/net/udp_receiver.*` | UDP/TS 接收监测 |
| `src/net/nic.cpp` | 网卡枚举与查找 |
| `src/common/byte_ring.cpp` | 环形缓冲 |
| `src/common/log.*` | 日志、sink、线程上下文 |
| `src/common/config.*` | config.json、token、路径、安全校验 |
| `src/common/file_log.*` | 滚动文件日志 |
| `src/task/task.*` | TaskSpec、JSON、校验 |
| `src/task/task_manager.*` | 状态机、持久化、采样、重连、日志 |
| `src/api/http_server.*` | REST、WS、鉴权、静态页、监测 |
| `src/gui/*` | Qt6 桌面客户端：API、主窗口、任务表单、日志/监测/媒体库 |
| `web/` | 原生 HTML/CSS/JS 控制台 |
| `third_party/` | httplib.h、nlohmann/json.hpp |
| `tests/udp_receiver.py` | 独立接收验证脚本 |

调用链：

```
mcastd main
 ├ LoadConfig / FileLogger / TaskManager.Load
 ├ ApiServer.Start → httplib listen
 ├ 主线程等待 SIGINT/SIGBREAK
 └ TaskManager worker (1Hz)
      ├ Pipeline.Snapshot
      ├ 断流检测 / 自动重连
      └ update callback → ApiServer.BroadcastTask → WS
```

---

## 5. 构建与运行

### 5.1 环境

| 项 | 值 |
|---|---|
| OS | Windows 11 |
| 工具链 | `F:\091-Opencode\toolchain\msys64`（便携 MSYS2 UCRT64） |
| 版本 | GCC 16.2 / CMake 4.4 / Ninja / FFmpeg 9.0 / Qt 6.12 |

### 5.2 构建

```powershell
cd F:\091-Opencode\推流工具
powershell -ExecutionPolicy Bypass -File scripts\build.ps1
```

产物：

```
build\bin\mcastctl.exe
build\bin\mcastd.exe
build\bin\mcastgui.exe
build\bin\web\
```

`mcastgui.exe` 已部署 Qt 运行库，可直接双击；`mcastctl` / `mcastd` 用包装脚本运行。

### 5.3 运行

```powershell
# CLI
powershell -File scripts\mcastctl.ps1 nics
powershell -File scripts\mcastctl.ps1 probe --input D:\Videos\demo.ts

# 服务端
powershell -File scripts\mcastd.ps1 --listen 127.0.0.1 --port 8787 --media-root D:\Videos
```

浏览器打开 `http://127.0.0.1:8787/`，默认无需 token；若启用认证则输入 token。

桌面客户端：

```powershell
build\bin\mcastgui.exe
# 或
powershell -File scripts\mcastgui.ps1
```

默认连接 `http://127.0.0.1:8787`，认证关闭时 Token 留空。

数据目录：

```
%LOCALAPPDATA%\Mcast\
  config.json
  tasks.json
  logs\mcastd.log
```

### 5.4 跨机

A 电脑：

```powershell
powershell -File scripts\mcastd.ps1 --listen 0.0.0.0 --port 8787 --media-root D:\Videos
```

B 电脑：浏览器打开 `http://<A 的 IP>:8787/`；如果启用了认证，输入 token。

组播接收端：

```powershell
ffplay "udp://@239.1.1.1:5000?overrun_nonfatal=1&fifo_size=5000000"
python tests\udp_receiver.py --group 239.1.1.1 --port 5000 --iface <B 的 IP> --seconds 60
```

组播不通按顺序查：`--nic`、TTL、Windows 防火墙、交换机 IGMP snooping、B 是否加入同一组。

---

## 6. 实测数据

### 6.1 M1（本机回环）

| 指标 | 单播 | 组播 |
|---|---|---|
| 发送速率 | 4.47–4.51 Mbps | 4.46–4.49 Mbps |
| 接收速率 | 4.24 Mbps | 4.22 Mbps |
| TS 同步错误 / CC 错误 | 0 / 0 | 0 / 0 |
| 缓冲水位 | 1%–15% | 1%–15% |
| 跨循环点 | 无 dts 回退 / 无 PCR 异常 | 同左 |

### 6.2 M2（本机）

| 项目 | 结果 |
|---|---|
| 单任务 | 4.47 Mbps，0 丢弃，0 错误 |
| 双任务并发 | 各 4.47 Mbps，0 丢弃，0 错误 |
| 单播监测 | 4.46 Mbps，0 sync，0 CC |
| 组播监测 | 4.48 Mbps，0 sync，0 CC |
| 自动重连 | 文件 EOF 后约 1.5s 恢复 running |
| 停止再启动 | 发送计数归零，不累计 |
| 服务重启 | tasks.json 保留任务，状态回 idle |
| 安全 | 认证开启时无 token 401；越界路径/junction 403；`file://` 400 |
| 优雅停机 | SIGBREAK 后 exit code 0 |

### 6.3 M3（本机）

| 项目 | 结果 |
|---|---|
| GUI 启动 | `mcastgui.exe` 双击启动，窗口正常 |
| 连接 | 自动连接 `127.0.0.1:8787`，认证关闭无需 token |
| 任务展示 | 任务/状态/源/目的/码率/缓冲/发送统计正常 |
| 一键启停 | GUI“停止/启动”驱动服务端任务，1Hz 刷新 |
| 状态卡片 | 任务总数/运行中/总码率/认证状态正常 |
| VLC | GUI“VLC”按钮启动本机 VLC 并播放 |
| 高码率 | 4K HEVC 25 Mbps：24.9 Mbps、buffer 2–4%、0 丢包/错误 |
| 依赖部署 | windeployqt + freetype 等 DLL 复制，无 PATH 也能启动 |

### 6.4 M4 第一批（本机）

| 项目 | 结果 |
|---|---|
| 双目的地 | 同一源推 `127.0.0.1:5300` + `:5301`，两路均收到，TS/CC 错误 0 |
| 并发统计 | `destinations=2`，总发送约 50 Mbps（每路 25 Mbps） |
| 低码率回归 | 4.5 Mbps 双目的地正常，ring 不溢出 |
| 录制 | `recordings/<id>_<时间戳>.ts` 生成 |
| 录制校验 | ffprobe 识别 HEVC 3440x1440 + AAC |
| 多目的地失败隔离 | 单个 socket 失败只累计该路错误，其它路继续 |
| 转码 | 720p H.264 → H.264 2 Mbps/AAC 128k，CBR 3 Mbps，TS/CC 错误 0 |
| 跨编码 | HEVC 720p → H.264/AAC，ffprobe 输出 H.264 |
| 转码循环 | 8s 文件连续 12s 跨循环，TS/CC 错误 0 |
| copy 回归 | copy 模式 4.5 Mbps，0 丢弃/错误 |

---

## 7. 踩坑记录

| # | 现象 | 根因 | 修复 |
|---|---|---|---|
| 1 | 配 4.5 Mbps 只发出 0.68 Mbps | Windows 定时器 15.6ms，按请求 sleep 记账 | 实测流逝时间补令牌 + `timeBeginPeriod(1)` + 批量发 |
| 2 | 26% UDP 包不以 `0x47` 开头 | ring 溢出按字节丢 | 按 188 粒度丢 + 发送前重同步 |
| 3 | `dts < pcr` 刷屏、PTS 负 | B 帧首包 dts > pts | 预读 min(dts,pts) 统一偏移 |
| 4 | 循环播放花屏 | 时间戳回退 | `loop_offset` 全局递增 |
| 5 | 组播回环丢 50% | 20ms 突发一次 8 包 | 突发窗口降到 5ms |
| 6 | 日志刷爆 | B 帧 copy 的良性告警 | 自定义 av_log 只过滤这一条 |
| 7 | 抓包解码 `non-existing PPS` | 素材 GOP=250 | 测试素材用 `-g 25` |
| 8 | 中文路径乱码 | Windows 控制台/文件路径编码 | `SetConsoleOutputCP`、全链路 UTF-8、`u8string` |
| 9 | exe 缺 DLL | 依赖 libav*.dll | `scripts\mcastctl.ps1` / `mcastd.ps1` |
| 10 | 杀软/旧进程占用 exe | 构建/启动失败 | 先停进程再构建 |
| 11 | 服务重启 `tasks.json` ACCESS_DENIED | 上一个进程/杀软短暂占用，`MoveFileExW` 失败 | 30ms × 10 重试 + remove/rename 回退；无变更不重写 |
| 12 | 停止任务时卡死 | Stop 持 manager 锁 join 线程，线程日志又拿同一把锁 | 任务日志改用独立 `log_mutex_` |
| 13 | 网页表格每秒整表重建，按钮点击丢失 | 每次 stats 推送重建 DOM | 按 id 原地更新行，按钮元素复用 |
| 14 | Windows junction 绕过媒体白名单 | `weakly_canonical` 不解析 junction | `GetFinalPathNameByHandleW` 做 realpath 复检 |
| 15 | `file://` 可绕过媒体根 | 任意 scheme 都当网络 URL | 只允许 rtsp/rtmp/http/udp/rtp/srt/tcp/mms |
| 16 | 浏览器连着 WS 时停机等待 | `ws.read` 默认无超时 | `ws.set_read_timeout(1)`，停机 0.56s 退出 |
| 17 | 监测结束后 VLC 收不到流 | 监测时长到期只停线程，socket 仍占用 UDP 端口 | `Loop` 退出时 `CloseSocket` 释放端口 |
| 18 | 高码率下 `buffer_fill` 超过 100% | 溢出丢弃量向下取整，腾出的空间不够，`size_` 越过容量 | 丢弃量向上取整并限制在 `size_` 内 |
| 19 | 25 Mbps 推流 4 秒后掉到 8 Mbps | `sleep_for(2ms)` 被调度成十几 ms，5ms 突发窗口跟不上 | `PreciseSleep` 使用 Windows 高精度可等待定时器，按 token 缺口精确等待 |
| 20 | `mcastgui.exe` 双击报缺 `libfreetype-6.dll` | windeployqt 未复制 Qt 的非 Qt 运行时依赖 | 构建后额外复制 freetype/stdc++/icu 等 DLL 到 `build\bin` |
| 21 | `mcastd.exe` 编译后消失 / Permission denied | 360 杀毒误报 `HEUR/QVM202.0.7E5D.Malware.Gen`，隔离并锁文件 | 把项目 `build` 目录加信任区/白名单，从恢复区“恢复并信任” |
| 22 | 转码代码在 FFmpeg 9 下编译失败 | `AVCodec::sample_fmts` 已移除、`AVFrame::pkt_duration` 改名 | AAC 固定 `FLTP`，帧时长用 `duration` |

---

## 8. 已知限制与技术债

| 优先级 | 项 | 说明 |
|---|---|---|
| **P0** | 跨机未实测 | M2 本机浏览器/接口通过，B 电脑访问 + VLC 播放还没跑 |
| P1 | 无 HTTPS / 用户体系 | 认证默认关闭；开启后单 token + 局域网 HTTP，只用于可信网络 |
| P1 | VLC 按钮仅本机 | 从 B 电脑网页调用会被拒绝；B 电脑需自己打开 VLC |
| P1 | 输出仅 UDP | RTMP/SRT/RTSP/HLS 尚未接入，见协议扩展规划 |
| P1 | 任务不自动恢复 | 服务重启后任务保留但回到 idle |
| P1 | 无 Windows 服务安装 | 目前前台 `--console` 运行 |
| P1 | Qt 端轮询 | 桌面端 1Hz REST 轮询，未接 WebSocket |
| P2 | Qt 端 token 明文 | 保存在本机 QSettings INI，未接 Windows 凭据管理器 |
| P2 | 无安装包 | GUI 是 build 产物，未做安装器/签名 |
| P2 | 接收监测单路 | 同一时间只跑一个 UdpReceiver |
| P2 | 多目的地共享网卡 | 额外目的地沿用主目的地的 NIC/TTL/DSCP/回环 |
| P2 | 录制无切割/清理 | 原始 CBR TS，文件只增不减 |
| P2 | 转码为纯软件 | libx264 软件编码；4K 高帧率可能跟不上实时，无硬件编码/缩放 |
| P2 | 无转码/采集/屏幕 | 输入必须能 copy（M4） |
| P2 | 仅 IPv4 | 组播/单播均 AF_INET |
| P2 | 仅 Windows 实测 | Linux 分支在，未编译验证 |
| P3 | 无单元测试/CI | tests 仍是手工脚本 |
| P3 | Web UI 表宽固定 | 窄屏需横向滚动 |
| P3 | DSCP 可能被驱动忽略 | 需交换机侧验证 |

---

## 9. 下一步

### 9.1 跨机验收（P0）

A 电脑 `--listen 0.0.0.0`，B 电脑：

- 浏览器访问 `http://<A>:8787/`，新建/启停任务
- Qt 客户端填 A 的地址，验证任务表、启停、VLC
- 用 VLC / `tests/udp_receiver.py` 验证组播接收

### 9.2 M4：采集/屏幕捕获

- 多目的地 + TS 录制 + H.264/AAC 转码已完成
- 剩余：采集卡、屏幕捕获输入
- 复用现有转码管线（采集设备通常输出未压缩/MJPEG，必须编码）
- 录制切片/清理、多目的地独立网卡

### 9.3 协议扩展（M5）

RTMP/RTMPS → SRT/RTP → RTSP → HLS/录制。前提是先把 UDP 发送层抽成 `OutputSink`，详见 `DESIGN.md` 第 15 节。

### 9.4 工程化

- Windows 服务安装、HTTPS/反向代理、多路监测、任务自动恢复
- Qt 客户端接 WebSocket（替代 1Hz 轮询）、token 存 Windows 凭据
- 单元测试/CI、安装包与签名

---

## 10. 待决策

1. **网页端认证**：默认关闭；需要时在设置里开启并自定义 token。`--listen 0.0.0.0` 且关闭认证时只在可信局域网使用。
2. **网页端默认监听**：当前默认 `127.0.0.1`；跨机需显式 `--listen 0.0.0.0`。
3. **VLC 按钮**：当前只允许服务器本机调用，避免远程浏览器启动服务器程序。
4. **HTTPS**：局域网是否接受 HTTP；若需要，优先用反向代理。
4. **任务自动恢复**：服务重启是否自动拉起上次 running 的任务。
5. **并发上限**：当前按 5 路设计，代码未硬限制。

---

## 11. 交接 Checklist

环境与构建：

- [ ] `F:\091-Opencode\toolchain\msys64\ucrt64\bin\g++.exe` 可执行
- [ ] `powershell -File scripts\build.ps1` 成功
- [ ] `powershell -File scripts\mcastctl.ps1 nics` 能列出网卡
- [ ] `powershell -File scripts\mcastd.ps1` 能启动；默认无需 token
- [ ] 需要时在设置里启用认证并填写自定义 token
- [ ] 浏览器登录网页端，新建任务并启动
- [ ] 双击 `build\bin\mcastgui.exe`，连接服务端并看到任务
- [ ] GUI 停止/启动、VLC 按钮正常
- [ ] 单播/组播监测，`sync_err=0 / cc_err=0`
- [ ] `git status` 清楚当前改动是否已提交

项目状态：

- [x] M1/M2/M3 + M4 第一批代码已提交
- [ ] 跨机验收完成
- [ ] `build/` 是测试产物，随时可删
- [ ] 工具链在项目外，换机器需重跑 `setup-toolchain.ps1`

---

## 12. 版本管理

| 项 | 值 |
|---|---|
| 远程 | `git@github.com:jiexu-bit/McastPilot.git` |
| 分支 | `main` |
| 已有 tag | `v0.1.0-m1`、`v0.3.0-m3` |
| 当前版本 | `0.3.0-m3`（M4 第一批已包含） |
| 许可证 | MIT |

约定：

- 仓库只放源码/文档/脚本/单头依赖；`build/`、`*.log`、`.vscode/` 忽略
- `.gitattributes`：代码 LF，`.ps1` 检出 CRLF
- 不把 token/密码写进仓库

---

## 13. 相关文档

- [DESIGN.md](DESIGN.md)：完整设计（目标、协议、任务模型、风险）
- [README.md](README.md)：快速开始、API 摘要、已知限制
- [tests/udp_receiver.py](tests/udp_receiver.py)：独立接收验证脚本
