# McastPilot 交接文档

> 更新时间：2026-11-18
> 代码状态：**M1 完成，可构建、可运行、已实测**
> 项目目录：`F:\091-Opencode\推流工具`
> 工具链目录：`F:\091-Opencode\toolchain\msys64`（项目外，可整体删除）

---

## 0. 一页速览

| 项 | 内容 |
|---|---|
| 做什么 | 用 C++ 写的组播/单播 MPEG-TS 推流工具，替代 VLC 那套繁琐的点选配置 |
| 架构 | C/S：`mcastd` 服务端 + Qt 桌面端 + 可选 Web 端（M2 起） |
| 现在到哪 | **M1 完成**：网卡枚举、组播/单播发送、FFmpeg 封装、CBR 节奏、实时统计、CLI |
| 产物 | `build\bin\mcastctl.exe`（唯一可执行文件） |
| 技术栈 | C++20 / CMake 4.4 / Ninja / GCC 16.2 / FFmpeg 9.0 / 便携 MSYS2 UCRT64 / Qt 6.12（已装未用） |
| 代码量 | 约 2100 行，`src` 下 5 个模块 |
| 实测 | 4.5 Mbps CBR，接收 4.22 Mbps，TS 同步错误 0，CC 连续性错误 0，FFmpeg 解码通过 |
| 未验证 | **跨机（第二台电脑）实播**——这是 M1 唯一没闭环的验收项 |
| 下一步 | M2：服务端 + REST/WS + 网页端 + 接收监测（见第 9 节） |

---

## 1. 需求边界（已和需求方确认）

| 问题 | 结论 |
|---|---|
| 平台 | Windows 优先，网络层做抽象，后续可编 Linux |
| 推流引擎 | FFmpeg，**进程内链接** `libavformat/libavcodec`，不调 `ffmpeg.exe` |
| 输入源 | 本地文件循环、采集卡、屏幕捕获、已有网络流转推（M4 补齐采集/屏幕） |
| 输出 | UDP 组播 MPEG-TS（核心）、UDP 单播、RTMP/RTSP/SRT、录制、多目的地（M4） |
| 客户端 | **Qt 桌面 GUI 为主**，可选 Web 端（可开关），给 B 电脑远程配置 |
| 部署 | 先同机，协议按跨机设计 |
| 组播能力 | 指定网卡/源 IP、TTL、DSCP、组播回环开关、CBR、自动重连、多路并发 |
| 转码 | 默认 `copy`，不转码 |
| 规模 | ≤5 路，每路 ≤20 Mbps |
| 文件选择 | 网页端只浏览服务端**白名单目录**，只读，不上传 |
| 工具链 | 便携 MSYS2 UCRT64，无管理员、不动系统、可一键删除 |

---

## 2. 架构

### 2.1 目标架构（完整形态）

```
┌──────────────────────────────┐         ┌──────────────────────────────┐
│  mcastgui (Qt6 Widgets)      │         │  Browser (B 电脑 / 手机)      │
│  · 网卡 / 预设 / 状态面板      │         │  · 同一套 REST + WebSocket    │
└──────────────┬───────────────┘         └──────────────┬───────────────┘
               │  HTTP/JSON + WebSocket                │
               └───────────────┬───────────────────────┘
                               ▼
┌──────────────────────────────────────────────────────────────────────┐
│  mcastd  (服务端，无界面，可做 Windows 服务 / 前台 --console)          │
│                                                                      │
│  api/       REST + WebSocket + 静态页 + token 认证                    │
│  task/      任务模型、状态机、持久化、调度、日志                        │
│  media/     输入(文件/网络/设备) → 解封装 → [转码] → TS 封装 → 节奏     │
│  net/       网卡枚举、组播发送、组播接收、统计                          │
│  common/    字节环形缓冲、日志、配置、工具                             │
└──────────────────────────────────────────────────────────────────────┘
                               │
                               ▼  MPEG-TS / UDP
                     239.x.x.x:5000  ──────────►  VLC / 机顶盒 / 解码器
```

### 2.2 当前实现（M1）

M1 没有服务端，`mcastctl` 把「任务」直接跑在自己进程里，方便先验证最难的发送链路：

```
mcastctl.exe
  ├ 解析参数 / 枚举网卡
  ├ Pipeline
  │   ├ 读线程：avformat 解封装 → mpegts 封装（CBR）→ 写环形缓冲
  │   └ 发线程：令牌桶节奏 → UDP sendto（组播/单播）
  └ 主线程：每秒打印统计
```

M2 会把 `Pipeline` 原样搬进 `mcastd`，外面套任务管理和 HTTP API，发送层不用改。

### 2.3 模块职责

| 模块 | 文件 | 职责 |
|---|---|---|
| common | `log.h/cpp` | 线程安全日志，级别/颜色/时间戳 |
| common | `byte_ring.h/cpp` | 字节环形缓冲，按 188 字节粒度丢包 |
| net | `platform.h/cpp` | Winsock 初始化、`timeBeginPeriod(1)`、错误文本 |
| net | `nic.h/cpp` | 网卡枚举（Windows `GetAdaptersAddresses` / Linux `getifaddrs`）、按 IP/名称/序号查找 |
| net | `multicast_sender.h/cpp` | UDP 发送 socket：网卡绑定、TTL、DSCP、环回、`SIO_UDP_CONNRESET`、统计 |
| media | `pipeline.h/cpp` | FFmpeg 输入 → mpegts CBR → 自定义 AVIO → 环形缓冲；时间戳归一化；实时节流 |
| cli | `main.cpp` | `nics` / `probe` / `send` 三个子命令 |

### 2.4 数据流（M1 核心）

```
文件/URL
   │ av_read_frame
   ▼
AVPacket ──(copy，不转码)──► mpegts muxer（muxrate=CBR，补空包）
                                   │ write callback（32KB AVIO）
                                   ▼
                            ByteRing（默认 4MB，丢包按 188 对齐）
                                   │
                                   ▼
                        令牌桶（5ms 突发窗口）
                                   │ 1316B/包（188×7）
                                   ▼
                     UDP socket（IP_MULTICAST_IF + bind + TTL + DSCP）
                                   │
                                   ▼
                          239.x.x.x:5000
```

### 2.5 线程模型

| 线程 | 职责 | 关键点 |
|---|---|---|
| 主线程 | 参数解析、启动、每秒 `Snapshot()` 打印 | 不碰 FFmpeg |
| reader 线程 | `av_read_frame` → 时间戳处理 → 实时 sleep → `av_interleaved_write_frame` | 写回调也在该线程执行 |
| sender 线程 | 从 ring 读 → 令牌桶 → `sendto` | 绝不阻塞 reader |
| FFmpeg 内部 | 解封装/封装 | 通过 `AVIOContext` 回调与 ring 解耦 |

ring 是「单生产者 / 单消费者 + mutex + condvar」，不是无锁队列；当前规模（≤5 路、≤20 Mbps）完全够用，后续要极致性能再换。

---

## 3. 关键实现（交接重点）

### 3.1 网卡枚举与强制绑定

- Windows 用 `GetAdaptersAddresses(AF_UNSPEC)` 一次拿全：友好名、GUID、IfIndex、OperStatus、链路速率、MAC、IPv4/IPv6、网关
- 过滤 169.254/16、IPv6 link-local；标记 virtual（TAP/VMware/Hyper-V/VPN/蓝牙）
- `IP_MULTICAST_IF`（源 IP）+ `bind(源IP:0)` 双保险；Linux 额外 `SO_BINDTODEVICE`
- `--nic` 支持三种写法：IP、名称/描述子串、1 基序号
- 选中的网卡没有 IPv4 直接报错，不猜

代码：`src/net/nic.cpp`、`src/net/multicast_sender.cpp`

### 3.2 时间戳归一化（踩坑最多的地方）

问题：源文件第一个视频包 `dts > pts`（B 帧），直接减 `dts` 会让 `pts` 变负，muxer 持续报 `dts < pcr, TS is invalid`。

最终方案：

1. 对**可 seek** 的输入，先预读最多 600 个包，取所有流 `min(dts, pts)`（换算到 AV_TIME_BASE）
2. 回退到文件头，`out_shift = -min_ts`（换算到输出 time_base），保证最早时间戳为 0
3. 循环播放时用全局 `loop_offset` 递增：`loop_offset = last_norm + 1`，所有流加同一个偏移，**A/V 同步不漂**
4. 实时节流用 `(norm_dts - loop_offset)`，每轮循环从 0 重新计时，循环点不卡顿

不可 seek 的直播流跳过预读，退回 `ic->start_time`。

代码：`Pipeline::ReaderLoop()`（`src/media/pipeline.cpp`）

### 3.3 发送节奏（Windows 定时器坑）

问题：Windows 默认定时器精度 **15.6ms**，`sleep_for(2ms)` 实际睡 15ms；令牌桶按"请求时长"记账，4.5 Mbps 只发出 0.68 Mbps。

最终方案：

- 用**实测流逝时间**补充令牌：`budget += elapsed * bytes_per_sec`
- 每次醒来尽量把 `budget` 用满（批量发），不为每个包单独 sleep
- `timeBeginPeriod(1)` 提高精度（`net::InitSockets`），退出时 `timeEndPeriod(1)`
- 突发窗口 **5ms**（不是设计初稿的 20ms）：20ms 会让 Windows/Wi‑Fi 回环一次吐 8 个包，实测丢 50%

代码：`Pipeline::SenderLoop()`、`net::InitSockets()`

### 3.4 环形缓冲与 TS 对齐

问题：ring 溢出时按字节丢，后续 TS 包全部错位（抓包 26% 的 datagram 不以 `0x47` 开头）。

方案：

- `ByteRing(capacity, 188)`：丢包量向下取整到 188 的倍数
- 发送前检查 `buf[0] == 0x47`；不对就找 `0x47` 在 `+0/+188/+376` 同时成立的偏移；找不到整块丢弃并计数

代码：`src/common/byte_ring.cpp`、`FindTsSyncOffset()`、`Pipeline::SenderLoop()`

### 3.5 CBR

- `av_dict_set_int(&opts, "muxrate", muxrate)`，mpegts muxer 用空包补齐
- `muxrate` 未指定时取 `输入码率 × 1.15`（下限 1 Mbps）
- 注意：`muxrate` 必须 ≥ 实际码率，否则 muxer 会追不上，ring 持续丢包
- `pkt_size` 必须是 188 的倍数（已加校验）

### 3.6 FFmpeg 日志过滤

`dts < pcr, TS is invalid` 对 B 帧 copy 流是良性的，但 ~40 行/秒会刷爆日志。
`InstallFfmpegLogFilter()` 用自定义 `av_log_set_callback` **只过滤这一条**，其余 warning/error 照常输出。

代码：`src/media/pipeline.cpp` 顶部

### 3.7 单播 / 组播统一

`MulticastSender::Open()` 根据目的地址自动判断：
- 224.0.0.0/4 → 组播路径：`IP_MULTICAST_IF` / TTL / LOOP
- 其它 → 单播路径：只设 `IP_TTL`，走 OS 路由（`--nic` 仍会 bind 源地址）

所以 `--group` 既能填 `239.x.x.x`，也能填 `192.168.x.x` 或 `127.0.0.1`。

---

## 4. 代码地图

| 文件 | 行数* | 角色 | 先看哪里 |
|---|---|---|---|
| `src/cli/main.cpp` | ~450 | CLI 入口、参数解析、统计打印 | `CmdNics` / `CmdProbe` / `CmdSend` / `main` |
| `src/media/pipeline.cpp` | ~560 | 核心：FFmpeg 封装 + 时间戳 + 节奏 | `Start` / `ReaderLoop` / `SenderLoop` / `WriteCallback` |
| `src/net/multicast_sender.cpp` | ~240 | UDP socket 与发送 | `Open` / `Send` |
| `src/net/nic.cpp` | ~270 | 网卡枚举 | `EnumerateNics` / `FindNic` |
| `src/common/byte_ring.cpp` | ~100 | 环形缓冲 | `Write` / `Read` |
| `src/common/log.cpp` | ~120 | 日志 | `LogWrite` |
| `src/net/platform.cpp` | ~75 | Winsock / 定时器精度 | `InitSockets` |

调用链：

```
main
 └ CmdSend
    ├ net::EnumerateNics / FindNic
    ├ Pipeline::Start
    │   ├ MulticastSender::Open
    │   ├ thread SenderLoop
    │   └ thread ReaderLoop
    │        ├ avformat_open_input / find_stream_info
    │        ├ avformat_alloc_output_context2("mpegts")
    │        ├ avio_alloc_context(WriteCallback)
    │        ├ avformat_write_header
    │        └ loop: av_read_frame → shift → pace → av_interleaved_write_frame
    └ loop: Snapshot() 1Hz
```

> `build/` 里是测试素材和日志，已 gitignore，不属于源码。

---

## 5. 构建与运行

### 5.1 环境（当前已就绪）

| 项 | 值 |
|---|---|
| OS | Windows 11 专业版 |
| CPU / RAM | Ryzen 7 9700X / 31 GB |
| 网卡 | WLAN Realtek 8922AE（`192.168.3.15`，2.4Gbps，已连接）；Realtek 2.5GbE（未连接）；TAP-Windows（虚拟） |
| 工具链 | `F:\091-Opencode\toolchain\msys64`（便携 MSYS2） |
| 版本 | GCC 16.2.0 / CMake 4.4.4 / Ninja 1.13.2 / FFmpeg 9.0.2 / Qt 6.12.0 / GDB 18.1 |

工具链**不写注册表、不改系统 PATH**；`scripts\build.ps1` 只在当前进程里注入 PATH。
卸载：`powershell -File scripts\clean-toolchain.ps1`（删目录，之后需重装才能编译）。

### 5.2 构建

```powershell
cd F:\091-Opencode\推流工具
powershell -ExecutionPolicy Bypass -File scripts\build.ps1
```

产物：`build\bin\mcastctl.exe`。

> 直接双击 exe 会缺 FFmpeg DLL。请用 `scripts\mcastctl.ps1` 包装运行，或自行把
> `F:\091-Opencode\toolchain\msys64\ucrt64\bin` 加进 PATH。

### 5.3 运行

```powershell
# 网卡列表
powershell -File scripts\mcastctl.ps1 nics

# 输入探测
powershell -File scripts\mcastctl.ps1 probe --input D:\Videos\demo.ts

# 推组播
powershell -File scripts\mcastctl.ps1 send `
    --input D:\Videos\demo.ts --loop `
    --group 239.1.1.1 --port 5000 `
    --nic 192.168.3.15 --ttl 16 --muxrate 8000000

# 推单播
powershell -File scripts\mcastctl.ps1 send `
    --input D:\Videos\demo.ts --loop `
    --group 192.168.3.50 --port 5000 --muxrate 8000000
```

参数表：

| 参数 | 默认 | 说明 |
|---|---|---|
| `--input` | 必填 | 文件路径或 URL（rtsp/rtmp/udp） |
| `--group` | 必填 | 目的地址，组播或单播 |
| `--port` | 必填 | 1-65535 |
| `--nic` | 空 | IP / 名称 / 序号；空=系统默认路由 |
| `--ttl` | 16 | 组播 TTL，1-255 |
| `--dscp` | 0 | QoS，0-63 |
| `--pkt-size` | 1316 | 必须是 188 的倍数 |
| `--muxrate` | 0 | bps，0=按输入码率×1.15 自动 |
| `--buffer` | 4096 | 环形缓冲 KB |
| `--loop` | 关 | 文件循环 |
| `--no-realtime` | 关 | 不限速（压测用） |
| `--speed` | 1.0 | 倍速 |
| `--duration` | 0 | 秒，0=一直跑 |
| `--loopback` | 关 | 组播回环（同机接收必须开） |
| `--sndbuf` | 4MB | socket 发送缓冲 |
| `--verbose` | 关 | Debug 日志 |

### 5.4 生成测试素材

```powershell
$env:PATH="F:\091-Opencode\toolchain\msys64\ucrt64\bin;$env:PATH"
ffmpeg -y -f lavfi -i "testsrc2=size=1280x720:rate=25" `
       -f lavfi -i "sine=frequency=800:sample_rate=48000" -t 10 `
       -c:v libx264 -preset veryfast -b:v 3000k -g 25 -keyint_min 25 -sc_threshold 0 `
       -c:a aac -b:a 128k -f mpegts build\demo10.ts
```

> 关键帧间隔要小（`-g 25`），否则从 GOP 中间开始抓包会解码失败，看起来像"流坏了"。

### 5.5 本机验证（已通过）

单播（最干净，不受 Wi‑Fi 组播影响）：

```powershell
$exe = "F:\091-Opencode\推流工具\build\bin\mcastctl.exe"
$p = Start-Process $exe -ArgumentList @("send","--input","build\demo10.ts","--loop",
     "--group","127.0.0.1","--port","5005","--muxrate","4500000","--duration","7") `
     -RedirectStandardOutput build\v.out -RedirectStandardError build\v.err -WindowStyle Hidden -PassThru
Start-Sleep -Seconds 1
python tests\udp_receiver.py --group 127.0.0.1 --port 5005 --seconds 5
Get-Process mcastctl | Stop-Process -Force
```

组播（同机回环必须加 `--loopback`）：

```powershell
$p = Start-Process $exe -ArgumentList @("send","--input","build\demo10.ts","--loop",
     "--group","239.255.1.1","--port","5010","--nic","192.168.3.15","--ttl","1",
     "--muxrate","4500000","--loopback","--duration","9") `
     -RedirectStandardOutput build\m.out -RedirectStandardError build\m.err -WindowStyle Hidden -PassThru
Start-Sleep -Seconds 1
python tests\udp_receiver.py --group 239.255.1.1 --port 5010 --iface 192.168.3.15 --seconds 6
```

`tests/udp_receiver.py` 会输出：实时 Mbps、包数、**TS 同步错误**、**CC 连续性错误**。

### 5.6 跨机验证（待办）

A 电脑推：

```powershell
powershell -File scripts\mcastctl.ps1 send --input D:\Videos\demo.ts --loop `
    --group 239.1.1.1 --port 5000 --nic <A 的有线 IP> --ttl 16 --muxrate 8000000
```

B 电脑收（任选其一）：

```powershell
python tests\udp_receiver.py --group 239.1.1.1 --port 5000 --iface <B 的 IP> --seconds 60
# 或
ffplay "udp://@239.1.1.1:5000?overrun_nonfatal=1&fifo_size=5000000"
```

> B 电脑跑 Python 脚本需要 Python 3.9+（只用标准库）。
> 组播不通时按顺序查：`--nic` 是否选对、TTL、Windows 防火墙、交换机 IGMP snooping、B 是否加入同一组。

---

## 6. 实测数据（M1）

环境：本机 Windows 11，WLAN 2.4Gbps，同机回环。
素材：720p25 H.264 + AAC，10s，源码率 3.4 Mbps。
配置：CBR 4.5 Mbps，`pkt_size=1316`，ring 4 MB。

| 指标 | 单播 | 组播 |
|---|---|---|
| 发送速率 | 4.47–4.51 Mbps | 4.46–4.49 Mbps |
| 接收速率 | 4.24 Mbps | 4.22 Mbps |
| 接收包数（5–6s） | 2015 | 2408 |
| TS 同步错误 | 0 | 0 |
| CC 连续性错误 | 0 | 0 |
| 缓冲水位 | 1%–15% | 1%–15% |
| 发送丢包 | 0 KB | 0 KB |
| 跨循环点 | 无 `dts` 回退 / 无 PCR 异常 | 同左 |
| FFmpeg 解码 | 通过（首帧从 GOP 中间开始除外） | 同左 |

> 接收速率略低于发送速率是**统计窗口错位**（接收端晚启动 0.8–1s），不是持续丢包；
> CC 错误为 0 可以证明 PID 上没有丢包。

---

## 7. 踩坑记录（强烈建议读）

| # | 现象 | 根因 | 修复 | 位置 |
|---|---|---|---|---|
| 1 | 配 4.5 Mbps 只发出 0.68 Mbps | Windows 定时器 15.6ms，按"请求 sleep 时长"记令牌 | 按实测流逝时间补令牌 + `timeBeginPeriod(1)` + 批量发送 | `SenderLoop` / `InitSockets` |
| 2 | 26% 的 UDP 包不以 `0x47` 开头 | ring 溢出按字节丢，破坏 188 对齐 | 按 188 粒度丢 + 发送前重同步 | `ByteRing` / `FindTsSyncOffset` |
| 3 | `dts < pcr` 刷屏、PTS 为负 | B 帧首包 `dts > pts`，直接减 dts | 预读取 `min(dts,pts)` 做统一偏移 | `ReaderLoop` |
| 4 | 循环播放时接收端花屏/卡顿 | 循环后时间戳回退 | `loop_offset` 全局递增，所有流加同一偏移 | `ReaderLoop` |
| 5 | 同机组播回环丢 50% | 20ms 突发 = 一次 8 个包，Windows/Wi‑Fi 队列吃不消 | 突发窗口降到 5ms | `SenderLoop` |
| 6 | 日志被 `dts < pcr` 刷爆 | mpegts muxer 对 B 帧 copy 的良性告警 | 自定义 `av_log` 回调只过滤这一条 | `FfmpegLogCallback` |
| 7 | 刚编译完启动失败/文件被占用 | 杀软扫描新 exe，或旧进程未退出 | 先 `Stop-Process mcastctl`，等 1s 再启动 | 构建/测试脚本 |
| 8 | 抓包解码报 `non-existing PPS` | 素材 GOP=250 帧（10s 一个关键帧），抓包从 GOP 中间开始 | 测试素材用 `-g 25` | 测试素材命令 |
| 9 | 中文路径乱码 | Windows 控制台默认编码 | `SetConsoleOutputCP(CP_UTF8)`，全链路 UTF-8 | `main` |
| 10 | 直接双击 exe 缺 DLL | exe 依赖 `libav*.dll` | 用 `scripts\mcastctl.ps1` 注入 PATH | 包装脚本 |

---

## 8. 已知限制与技术债

| 优先级 | 项 | 说明 |
|---|---|---|
| **P0** | 跨机未实测 | 本机回环 + 解码已过，但"另一台电脑 VLC 稳定播放"还没跑 |
| P1 | 无断流重连 | 网络源读失败直接退出；`restart` 配置还没实现（M2） |
| P1 | 无服务端/持久化 | 预设、任务列表、开机自启都还没有（M2） |
| P1 | 无接收监测 | 目前靠 Python 脚本，正式工具没有（M2） |
| P1 | 无多目的地 | 一路源只能推一个地址（M4） |
| P2 | 无转码/采集/屏幕 | 输入必须是 FFmpeg 能直接 copy 的源（M4） |
| P2 | 仅 IPv4 | 组播/单播都是 AF_INET |
| P2 | 仅 Windows 实测 | Linux 分支代码在，但没编译验证 |
| P3 | 无单元测试 / CI | `tests/` 只有手工验证脚本 |
| P3 | 未 git init | 目录还不是 git 仓库，`.gitignore` 已准备好 |
| P3 | `muxrate` 自动估算 | 取 `ic->bit_rate × 1.15`；TS 源码率不准时建议显式指定 |
| P3 | DSCP 可能被驱动忽略 | Windows 上部分网卡不认 `IP_TOS`，需要交换机侧验证 |

---

## 9. 下一步：M2 实施方案

### 9.1 目标

把 M1 的 `Pipeline` 搬进常驻服务 `mcastd`，加任务管理、REST/WS、网页端和接收监测。
**发送层（net/media）不改**，只加外壳。

### 9.2 新增依赖（都放 `third_party/`，单头文件，无构建链）

| 依赖 | 用途 | 备注 |
|---|---|---|
| `cpp-httplib` | HTTP + WebSocket + 静态文件 | 单头文件，MIT；不启用 OpenSSL（局域网 HTTP） |
| `nlohmann/json` | JSON 序列化 | 单头文件，MIT |
| 前端 | 原生 HTML/CSS/JS + 一个小的 canvas 折线图 | 不引入 npm/Vue，避免构建链 |

### 9.3 新增文件

```
src/server/main.cpp          mcastd 入口：--listen/--port/--data-dir/--console
src/common/config.h/.cpp     config.json、token、媒体库白名单
src/task/task.h/.cpp         Task 模型、JSON 读写、参数校验
src/task/task_manager.h/.cpp 任务表、启停、状态机、持久化、自动重连
src/api/http_server.h/.cpp   httplib 封装、鉴权、路由、WS 广播
src/net/udp_receiver.h/.cpp  组播接收 + TS/CC 统计（监测用）
web/index.html / app.js / style.css
```

数据目录默认 `%LOCALAPPDATA%\Mcast\`：

```
config.json     监听地址、端口、token、media.roots、web.enabled
tasks.json      任务预设与运行状态
logs/mcastd.log 滚动日志
```

### 9.4 API（与 DESIGN 第 6 节一致）

- `GET  /api/v1/health`
- `GET  /api/v1/nics`
- `GET  /api/v1/media/roots` / `GET /api/v1/media/list?path=`
- `GET/POST /api/v1/tasks`
- `GET/PATCH/DELETE /api/v1/tasks/{id}`
- `POST /api/v1/tasks/{id}/start|stop`
- `GET  /api/v1/tasks/{id}/stats`
- `GET  /api/v1/tasks/{id}/logs?tail=200`
- `POST /api/v1/probe`
- `GET/PATCH /api/v1/settings`
- `WS   /api/v1/events`（state / stats 1Hz / log）

鉴权：`Authorization: Bearer <token>`；WS 用 `?token=`。token 首次启动生成并打印。

### 9.5 任务状态机

```
idle → starting → running → stopping → idle
                    └── error（自动重连退避：1s/2s/4s… 上限 10 次）
```

### 9.6 媒体库安全

- `std::filesystem::weakly_canonical` 规范化后做前缀校验
- 只允许白名单根目录下的 `.ts/.mp4/.mkv/.mov/.m4v/.flv/.mp3/.aac`
- 只读，不提供删除/改名/上传
- 路径必须是 UTF-8，返回 JSON 时转义

### 9.7 实现顺序（建议）

1. **config + 日志落盘 + 数据目录**（半天）
2. **Task / TaskManager**：先把 `Pipeline` 包起来，能 start/stop/取 stats（1 天）
3. **HTTP + token + nics/tasks/stats API**（1–2 天）
4. **Web UI**：网卡下拉、媒体库浏览、任务表单、启停、实时表格（2 天）
5. **WS 推送 + 任务持久化 + 自动重连**（1–2 天）
6. **接收监测**（`udp_receiver` + 网页入口）（1 天）
7. **联调 + 跨机验收**（1 天）

### 9.8 M2 验收

- [ ] B 电脑浏览器打开 `http://<A>:8787`，输入 token
- [ ] 新建任务：选 A 上的文件、选网卡、填组播地址、设码率
- [ ] 一键启动，1 秒内状态 running，码率/丢包实时刷新
- [ ] 停止 / 重启任务正常，服务重启后任务列表还在
- [ ] 同时跑 2 路不互相影响
- [ ] 路径穿越（`..\..\Windows`）被拒绝
- [ ] 断流后自动重连并恢复 running

---

## 10. 待决策（需要需求方拍板）

1. **网页端默认监听**
   - `127.0.0.1`：安全，但 B 电脑要手动改 `0.0.0.0`
   - `0.0.0.0`：装好即远程可访问，但必须强制 token + 防火墙提示
   - 建议：默认 `127.0.0.1`，桌面端提供"允许远程访问"开关
2. **并发上限**
   - 先按 5 路做，代码结构留到 20 路（每路 2 线程，20 路 = 40 线程，仍可控）
3. **Web 前端**
   - 建议原生 JS（无 npm、无构建步骤），交接成本最低
4. **GUI 框架**
   - 建议 Qt Widgets（不是 QML），和工具链里已有的 Qt 6.12 匹配

---

## 11. 交接 Checklist

环境与构建：

- [ ] `F:\091-Opencode\toolchain\msys64` 存在，`ucrt64\bin\g++.exe` 可执行
- [ ] `powershell -File scripts\build.ps1` 成功
- [ ] `powershell -File scripts\mcastctl.ps1 nics` 能列出网卡
- [ ] 跑一次 5.5 节的单播回环，`sync_err=0 / cc_err=0`
- [ ] 知道 exe 不能直接双击（缺 DLL），要用包装脚本
- [ ] `ssh -T git@github.com` 返回 `Hi jiexu-bit!`
- [ ] `git push` 成功，`origin/main` 与本地 `main` 一致

项目状态：

- [ ] 目录**还不是 git 仓库**；`.gitignore` 已写好，建议先 `git init` + 首次提交
- [ ] `build/` 里是测试素材和日志，可随时删
- [ ] 根目录 `Typora_Hook_Log.txt` **与本项目无关**，未处理
- [ ] 工具链在项目外，若换机器需重跑 `setup-toolchain.ps1`（约 2GB）
- [ ] 已知限制清单（第 8 节）已读

下一步：

- [ ] 确认第 10 节两个决策
- [ ] 按 9.7 的顺序开 M2
- [ ] M2 第一个可演示目标：浏览器里新建一个任务并启动

---

## 12. 版本管理与 GitHub

### 12.1 仓库

| 项 | 值 |
|---|---|
| 远程 | `git@github.com:jiexu-bit/McastPilot.git`（SSH） |
| 网页 | <https://github.com/jiexu-bit/McastPilot> |
| 分支 | `main` |
| 首个版本 | `v0.1.0-m1` |
| 提交身份 | `jiexu-bit <yangjiexu@live.com>` |
| 许可证 | MIT（`LICENSE`，GitHub 建仓时生成） |

### 12.2 本机 SSH 配置

```
C:\Users\Yangjiexu\.ssh\id_ed25519_mcastpilot      私钥（不要外传）
C:\Users\Yangjiexu\.ssh\id_ed25519_mcastpilot.pub  公钥（已加到 GitHub）
C:\Users\Yangjiexu\.ssh\config                     github.com 指定使用该 key
```

`config` 内容：

```text
Host github.com
    HostName github.com
    User git
    IdentityFile ~/.ssh/id_ed25519_mcastpilot
    IdentitiesOnly yes
    StrictHostKeyChecking accept-new
```

验证：

```powershell
ssh -T git@github.com
# Hi jiexu-bit! You've successfully authenticated, but GitHub does not provide shell access.
```

> 返回码是 `1`，这是正常的：GitHub 认证成功但不提供 shell。

公钥已添加到 <https://github.com/settings/keys>，标题 `McastPilot dev (Windows)`，
指纹 `SHA256:wOZLGmRTw6RNDHnVRFLbUK21wRN74PQFOqJfl6MHM1Q`。

### 12.3 换一台机器怎么接

1. 生成**新的** key（不要复制私钥）：

   ```powershell
   ssh-keygen -t ed25519 -f $env:USERPROFILE\.ssh\id_ed25519_mcastpilot -N '""' -C "jiexu-bit@McastPilot"
   ```

2. 把 `id_ed25519_mcastpilot.pub` 的内容加到 <https://github.com/settings/ssh/new>
3. 按 12.2 写 `~/.ssh/config`
4. `ssh -T git@github.com` 看到 `Hi jiexu-bit!` 即成功
5. 克隆并构建：

   ```powershell
   git clone git@github.com:jiexu-bit/McastPilot.git
   cd McastPilot
   powershell -File scripts\setup-toolchain.ps1
   powershell -File scripts\build.ps1
   ```

### 12.4 日常命令

```powershell
cd F:\091-Opencode\推流工具

git status
git diff
git add -A
git commit -m "feat: ..."
git push

# 发版本
git tag -a v0.2.0-m2 -m "M2: 服务端 + Web"
git push --tags
```

### 12.5 约定

- 仓库只放源码/文档/脚本；`build/`、`*.log`、`.vscode/`、`*.user` 已忽略
- `.gitattributes`：代码统一 LF，`.ps1` 检出为 CRLF；仓库本地 `core.autocrlf=false`，避免行尾抖动
- **不要把 token/密码写进仓库或聊天**；GitHub 认证只走 SSH
- 构建产物（`mcastctl.exe`、DLL 副本）不进仓库
- 需要 Issue / Release / PR 管理时再装 `gh`：`winget install --id GitHub.cli`，然后 `gh auth login`

---

## 13. 相关文档

- [DESIGN.md](DESIGN.md)：完整设计（目标、协议、任务模型、风险）
- [README.md](README.md)：快速开始
- [tests/udp_receiver.py](tests/udp_receiver.py)：验证用接收器（组播/单播、TS/CC 统计）
