# McastPilot — 组播推流与管理平台

> 用 C++20 写的一体化推/拉流工具。核心目标：**把 VLC 那一堆点选操作，变成一次预设、一键启停**。
> 复用 FFmpeg 做"内容"（解封装/解码/编码），自研"发送"（TS 打包节奏 + 组播 UDP），从而完全掌控网卡绑定、码率节奏和实时统计。

---

## 1. 目标与非目标

### 目标

| 编号 | 目标 |
|---|---|
| G1 | 把本地文件 / 采集设备 / 网络流，以 MPEG-TS over UDP **组播**推出去 |
| G2 | 明确指定从哪块网卡发出（多网卡识别 + 源 IP 绑定） |
| G3 | 保存为预设，一键启停，支持多路并发（≤5 路，每路 ≤20 Mbps） |
| G4 | 服务端常驻 + 桌面 GUI + 可选 Web 端，跨机远程管理 |
| G5 | 实时统计：码率、发送包数、缓冲、丢包、异常告警 |
| G6 | 内置组播接收监测，测试时能直接判断"流到底通没通" |
| G7 | Windows 优先，网络层做抽象，后续可编 Linux |

### 非目标（当前阶段）

- 不做通用播放器（播放交给 VLC/mpv）
- 不做流媒体服务器（不做 RTMP 转发集群、不做 HLS 切片分发）
- 不做转码农场（默认 `copy`；采集场景才编码）
- 不做用户体系/多租户（单 token 认证足够）

---

## 2. 术语

| 词 | 含义 |
|---|---|
| 任务 / Task | 一条完整的"源 → 编码 → 封装 → 发送"链路 |
| 推流 / Push | 本机作为发送端，把 TS 打到组播组 |
| 拉流 / Pull | 加入组播组接收，用于监测、录制或二次转发 |
| 预设 / Preset | 持久化到磁盘的任务配置 |
| 节奏 / Pacing | 控制 UDP 发包速率，避免突发导致交换机丢包 |
| CBR | 恒定码率，muxer 用空包（null packet）填满 |

---

## 3. 总体架构

```
┌──────────────────────────────┐         ┌──────────────────────────────┐
│  mcastgui (Qt6 Widgets)      │         │  Browser (B 电脑 / 手机)      │
│  · 网卡列表 / 预设 / 状态面板  │         │  · 同一套 REST + WebSocket    │
└──────────────┬───────────────┘         └──────────────┬───────────────┘
               │  HTTP/JSON + WebSocket                │
               └───────────────┬───────────────────────┘
                               ▼
┌──────────────────────────────────────────────────────────────────────┐
│  mcastd  (服务端，无界面，可做 Windows 服务 / 前台 --console)          │
│                                                                      │
│  api/       REST + WebSocket + 静态页  + token 认证                    │
│  task/      任务模型、状态机、持久化、调度、日志                        │
│  media/     输入(文件/网络/设备) → 解封装 → [转码] → TS 封装 → 节奏     │
│  net/       网卡枚举、组播发送、组播接收、统计                          │
│  common/    字节环形缓冲、日志、配置、工具                             │
└──────────────────────────────────────────────────────────────────────┘
                               │
                               ▼  MPEG-TS / UDP
                     239.x.x.x:5000  ──────────►  VLC / 机顶盒 / 解码器
```

### 进程与产物

| 产物 | 说明 |
|---|---|
| `mcastd.exe` | 服务端（M2 已实现），唯一真正持有任务、HTTP/WS 和 FFmpeg 上下文的常驻进程 |
| `mcastctl.exe` | CLI 客户端（M1 已实现），也可独立当单次推流工具用 |
| `mcastgui.exe` | Qt6 桌面客户端（M3 已实现） |
| `web/` | 原生前端资源，由 `mcastd` 托管，可开关（M2 已实现） |

---

## 4. 数据通路（核心）

```
 ┌─────────┐   AVPacket   ┌──────────────┐  TS bytes  ┌───────────┐  UDP  ┌──────────┐
 │ 输入     │ ───────────► │ mpegts muxer │ ─────────► │ ByteRing  │ ────► │ 组播发送  │
 │ demux    │  (copy)      │  CBR/空包填充 │  write_cb  │ 4MB 环形  │       │ socket   │
 └─────────┘              └──────────────┘            └───────────┘       └──────────┘
      ▲                         ▲                          ▲                    │
      │ 实时节流(-re 语义)        │ muxrate                    │ 令牌桶节奏          │ 统计
      └─────────────────────────┴────────────────────────────┴────────────────────┘
```

关键点：

1. **输入侧按 DTS 实时节流**（等价 `ffmpeg -re`），否则文件会被瞬间读完，muxer 一次性吐出几百 MB
2. **muxer 设置 `muxrate`**，由它插入空包补齐到恒定码率，PCR 稳定，接收端缓冲不抖
3. **发送侧再用令牌桶平滑**，把 muxer 的批量输出摊平成小包，避免微突发（micro-burst）
4. **自定义 `AVIOContext`** 接管 muxer 输出，不落盘、不走管道，直接写入环形缓冲
5. 一路源可挂**多个组播目的地**，共享同一次封装（M4）

### 为什么不用 `ffmpeg -f mpegts udp://...`

| 问题 | ffmpeg CLI | 本项目 |
|---|---|---|
| 网卡绑定 | 只能 `localaddr=` | 源 IP + 接口索引 + `SO_BINDTODEVICE` 三重 |
| 发送节奏 | 无，靠 `-re` 粗控 | 令牌桶 + 可配突发窗口 |
| 状态 | 无 | 实时计数、缓冲水位、错误分类 |
| 多目的地 | 一个输出一个进程 | 一次封装，多处发送 |
| 任务管理 | 无 | 预设、状态机、自动重启 |
| 进程模型 | 一路一进程 | 单进程多任务，线程隔离 |

---

## 5. 组播发送设计

### 5.1 网卡枚举

- Windows：`GetAdaptersAddresses()`，取 `FriendlyName`、`AdapterName`(GUID)、`IfIndex`、`IfType`、`OperStatus`、`TransmitLinkSpeed`、`PhysicalAddress`、IPv4/IPv6 地址与前缀、网关
- Linux：`getifaddrs()` + `/sys/class/net/<if>/`（`operstate`、`speed`、`address`）
- 标记虚拟网卡（TAP / VMware / Hyper-V / VPN / Loopback），UI 默认折叠
- **源 IP 必须来自被选中的网卡**，否则直接报错，不做"猜"

### 5.2 Socket 选项

| 选项 | 值 | 目的 |
|---|---|---|
| `IP_MULTICAST_IF` | 选定网卡的 IPv4 | 决定出口网卡（仅组播） |
| `bind()` | 源 IP : 0 | 双保险，固定源地址 |
| `IP_MULTICAST_TTL` | 默认 16，可配 1-255 | 跨网段范围 |
| `IP_MULTICAST_LOOP` | 默认关 | 避免本机回环干扰 |
| `IP_TOS` | `DSCP << 2` | 交换机 QoS 优先转发 |
| `SO_SNDBUF` | 4 MB（失败则退到系统上限） | 吸收调度抖动 |
| `SO_REUSEADDR` | 开 | 允许快速重启/多实例 |
| `SIO_UDP_CONNRESET`(Win) | 关 | 避免 ICMP 端口不可达导致 `sendto` 永久失败 |
| 非阻塞 | 开 | 发送失败立即计数，不卡住流水线 |

### 5.3 节奏控制（M1 实现）

- 令牌桶：`rate = muxrate / 8` 字节/秒，突发窗口 **5 ms**
- 令牌按**实测流逝时间**补充（不按"请求 sleep 的时长"），每次醒来批量发送
- 每次发送 `pkt_size`（默认 1316 = 188×7）字节，必须为 188 的倍数
- Windows 启动时 `timeBeginPeriod(1)`，退出时恢复；否则 1ms 的 sleep 会变成 15.6ms
- 环形缓冲满时丢弃最旧数据，**按 188 字节粒度对齐**，并计入 `buffer_drop_bytes`
- 发送前校验 TS 同步字节 `0x47`，失步时按 `+0/+188/+376` 重新对齐
- 断流时 PAT/PMT 由 muxer 周期重发（`pat_period=0.2`）

### 5.4 统计口径

| 指标 | 说明 |
|---|---|
| `bitrate_mbps` | 1 秒滑动窗口（按实际发出的字节） |
| `muxed_bytes` | muxer 产出字节（可与发送量对比，判断瓶颈在封装还是发送） |
| `packets_sent` / `output_bytes` | 累计发送 |
| `send_errors` / `packets_dropped` | `sendto` 硬错误 / `EWOULDBLOCK` 丢弃 |
| `buffer_drop_bytes` | 环形缓冲溢出丢弃的字节（按 188 对齐） |
| `buffer_fill` | 当前水位 / 容量 |
| `muxrate_bps` | 当前 CBR 目标码率 |
| `elapsed_sec` | 运行时长 |

---

## 6. C/S 协议

- 传输：HTTP/1.1 + JSON；实时事件走 WebSocket
- 默认端口 `8787`，默认只监听 `127.0.0.1`
- 认证默认关闭，可在设置或命令行开启；开启后 `Authorization: Bearer <token>`，WebSocket 用 `?token=`
- token 可自定义；启用认证且为空时自动生成，写入配置文件，控制台打印
- `--listen 0.0.0.0` 且认证关闭时启动日志会警告

### 6.1 REST

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/v1/health` | 版本、运行时长、任务数 |
| GET | `/api/v1/nics` | 网卡列表 |
| GET | `/api/v1/media/roots` | 媒体库白名单根目录 |
| GET | `/api/v1/media/list?path=` | 浏览白名单内目录 |
| GET | `/api/v1/tasks` | 任务列表 |
| POST | `/api/v1/tasks` | 新建任务 |
| GET | `/api/v1/tasks/{id}` | 任务详情 |
| PATCH | `/api/v1/tasks/{id}` | 修改（仅停止态） |
| DELETE | `/api/v1/tasks/{id}` | 删除 |
| POST | `/api/v1/tasks/{id}/start|stop` | 启动 / 停止 |
| POST | `/api/v1/tasks/{id}/open-vlc` | 服务器本机打开 VLC（仅 loopback/本机 IP） |
| GET | `/api/v1/tasks/{id}/stats` | 实时统计 |
| GET | `/api/v1/tasks/{id}/logs?tail=200` | 最近日志 |
| POST | `/api/v1/probe` | 探测输入（编码/分辨率/码率） |
| GET/PATCH | `/api/v1/settings` | 全局设置（含 `web.enabled`） |
| GET | `/api/v1/monitor` | 当前接收监测状态 |
| POST | `/api/v1/monitor/start` | 开始接收监测（组播加入 / 单播监听） |
| POST | `/api/v1/monitor/stop` | 停止接收监测 |
| WS | `/api/v1/events` | 状态/统计/日志推送 |

### 6.2 任务模型

```json
{
  "id": "a3f19c02",
  "name": "CCTV-1 测试流",
  "state": "running",
  "source": {
    "type": "file",
    "uri": "D:\\Videos\\demo.ts",
    "loop": true
  },
  "output": {
    "protocol": "udp-multicast",
    "group": "239.1.1.1",
    "port": 5000,
    "nic": "192.168.1.10",
    "ttl": 16,
    "dscp": 34,
    "loopback": false,
    "pkt_size": 1316,
    "extra": [
      { "group": "239.1.1.2", "port": 5000 }
    ]
  },
  "record": true,
  "mux": {
    "format": "mpegts",
    "muxrate": 8000000,
    "cbr": true
  },
  "encode": { "mode": "copy" },
  "restart": { "auto": true, "max_retries": 10, "backoff_ms": 1000 }
}
```

### 6.3 状态机

```
        ┌────────┐  start   ┌──────────┐  ok   ┌─────────┐
        │  idle  │ ───────► │ starting │ ────► │ running │
        └────────┘          └──────────┘       └────┬────┘
             ▲                    │ err             │ stop / error
             │                    ▼                 ▼
             │              ┌──────────┐      ┌──────────┐
             └──────────────│  error   │◄─────│ stopping │
                            └──────────┘      └──────────┘
```

- `running` 下源断开 → 自动重连（指数退避），超过重试次数进 `error`
- 状态变更全部落盘并广播到 WS

### 6.4 接收监测

`mcastd` 内置一个 `UdpReceiver`，用于测试时直接判断“流到底通没通”：

- 组播：绑定端口后 `IP_ADD_MEMBERSHIP`，可指定接收网卡
- 单播：绑定指定本地 IP 或 `0.0.0.0`
- 统计：接收包数、字节、平均码率、运行时长、TS 同步错误、CC 连续性错误
- 同一时间只跑一路；`duration_sec=0` 表示一直运行
- 网页端“监测”弹窗 1Hz 轮询；也可以直接用 `tests/udp_receiver.py`

### 6.5 任务日志与优雅停机

- `LOG_*` 写入 stderr，同时通过 sink 写滚动文件、任务日志环形缓冲和 WebSocket
- Pipeline reader/sender 线程设置 thread-local 任务 id，FFmpeg 日志也按任务归类
- 任务日志使用独立锁，避免“停止任务时持有 manager 锁 join 线程、线程又写日志”的死锁
- 停机顺序：停 HTTP → 停 TaskManager worker → 停所有 Pipeline → 清 sink → 关 WSA

---

## 7. 媒体库与安全

- 配置 `media.roots` 白名单，例如 `["D:\\Videos", "E:\\Stream"]`
- 浏览接口做 **realpath 规范化 + 前缀校验**，挡住 `..\`、符号链接和 Windows junction
  - Windows 使用 `GetFinalPathNameByHandleW` 解析 reparse point（`weakly_canonical` 对 junction 不可靠）
- 本地输入只允许白名单内路径；网络输入只允许 `rtsp/rtmp/http/https/udp/rtp/srt/tcp/mms`，拒绝 `file://`
- 只读：不提供删除/改名/上传
- 网页端开关 `web.enabled` 可动态切换；监听地址默认 `127.0.0.1`
- 单 token + 常量时间比较；跨机需显式 `--listen 0.0.0.0`，只在可信局域网使用
- 所有日志、路径按 UTF-8 处理，避免中文路径乱码

---

## 8. 目录结构

```
推流工具/
├─ CMakeLists.txt
├─ README.md
├─ DESIGN.md
├─ HANDOVER.md              交接文档（现状 / 坑 / 下一步）
├─ scripts/
│  ├─ setup-toolchain.ps1   便携 MSYS2 + 依赖安装
│  ├─ build.ps1             cmake configure + build
│  ├─ mcastctl.ps1          CLI 运行包装器
│  ├─ mcastd.ps1            服务端运行包装器
│  └─ clean-toolchain.ps1   一键卸载工具链
├─ src/
│  ├─ common/               日志、环形缓冲、配置、滚动文件日志
│  ├─ net/                  网卡枚举、UDP 发送、UDP/TS 接收、平台层
│  ├─ media/                FFmpeg 输入、TS 封装、节奏、probe
│  ├─ task/                 任务模型、TaskManager、持久化、自动重连
│  ├─ api/                  HTTP/WS 服务、鉴权、静态页
│  ├─ server/               mcastd 入口
│  ├─ cli/                  mcastctl
│  └─ gui/                  mcastgui（Qt6 Widgets）
├─ web/                     原生 HTML/CSS/JS 控制台
├─ third_party/             httplib.h、nlohmann/json.hpp
└─ tests/                   udp_receiver.py 等验证脚本
```

---

## 9. 构建

工具链装在独立目录，删掉即卸载，不改系统环境：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\setup-toolchain.ps1
powershell -ExecutionPolicy Bypass -File scripts\build.ps1
```

默认工具链位置：`F:\091-Opencode\toolchain\msys64`（UCRT64）。

---

## 10. 里程碑

| 阶段 | 内容 | 状态 | 验收标准 |
|---|---|---|---|
| **M1** | 网卡枚举 + 组播/单播发送 + FFmpeg 封装 + CLI | 完成（本机回环；跨机待测） | `mcastctl send ...` 后另一台机器 VLC 稳定播放，码率/丢包可见 |
| **M2** | 服务端 + REST/WS + 任务持久化 + 网页端 + 接收监测 | 完成（本机联调；跨机待测） | 浏览器新建任务、启停、实时曲线；服务重启任务保留；两路并发；路径穿越拒绝；断流恢复 |
| **M3** | Qt6 桌面客户端 | 完成（本机验证核心链路） | 网卡下拉、任务列表、一键启停、状态卡片、日志/监测、VLC |
| **M4** | 采集/屏幕/转码/录制/多目的地 | 进行中：多目的地 + TS 录制已完成 | 采集卡推流、同时推两个组播、录制存档 |
| **M5** | 更多输出协议：RTMP/SRT/RTSP/HLS | 待开始 | 同一任务可切换协议；RTMP/SRT 推流可被服务端稳定接收 |

---

## 11. 已定技术决策

| 项 | 决策 | 理由 |
|---|---|---|
| 平台 | Windows 优先，网络层抽象 | 当前主力环境；不影响后续 Linux |
| 推流引擎 | FFmpeg，进程内链接 `libavformat/libavcodec` | 可控性最强，能接管 TS 输出 |
| 发送层 | 自研 UDP 组播 | 网卡绑定、节奏、统计、多目的地 |
| 默认编码 | `copy`（不转码） | CPU 近零，画质无损 |
| 客户端 | Qt6 桌面为主 + 可选 Web | 桌面体验 + 远程配置 |
| 部署 | 先同机，协议按跨机设计 | 降低初期复杂度 |
| 组播能力 | 网卡/TTL/DSCP/CBR/自动重连/多路 | 覆盖 IPTV 与测试场景 |
| 工具链 | 便携 MSYS2 (UCRT64) | 无管理员、可一键删除 |

---

## 12. 风险与对策

| 风险 | 影响 | 对策 |
|---|---|---|
| Windows 多网卡路由不确定 | 包从错误网卡出去 | `IP_MULTICAST_IF` + `bind` 双绑定；启动时打印实际出口并可选自检 |
| 交换机 IGMP snooping 未配 | 收不到流 | 文档给出 IGMP 配置检查清单；内置接收监测辅助定位 |
| 微突发导致丢包 | 花屏 | 令牌桶 + `SO_SNDBUF` + 可调 `pkt_size` |
| 源时间戳跳变 | 接收端花屏/PCR 抖动 | 时间戳偏移归一化 + 断流重建 PAT/PMT |
| FFmpeg 版本 ABI 差异 | 链接失败 | 统一用 MSYS2 的 `mingw-w64-ucrt-x86_64-ffmpeg`，与编译器同源 |
| 中文路径 | 打不开文件/日志乱码 | 全链路 UTF-8，FFmpeg 输入用 `avformat_open_input` 的 UTF-8 路径（Windows 下 FFmpeg 内部转换） |

---

## 13. M1 实现记录（与初版设计的偏差）

实现过程中有几处按实测数据调整了设计，详细复现步骤见 [HANDOVER.md](HANDOVER.md) 第 7 节：

| 项 | 初版设计 | 实际实现 | 原因 |
|---|---|---|---|
| 输出 | 仅组播 | 组播 + 单播 | 单播是需求之一，且便于排除组播环境问题 |
| 突发窗口 | 20 ms | **5 ms** | 20ms 一次吐 8 个包，Windows/Wi‑Fi 回环丢 50% |
| 令牌补充 | 按请求 sleep 时长 | **按实测流逝时间** | Windows 定时器 15.6ms，按请求时长记账会严重偏慢 |
| 环形缓冲丢包 | 按字节 | **按 188 字节粒度** | 按字节丢会破坏 TS 对齐 |
| 时间戳 | 按首包 dts 归一 | **预读 min(dts,pts) + 循环全局偏移** | B 帧首包 dts > pts，会导致负 PTS / `dts < pcr` |
| 日志 | 直接输出 | **过滤良性 `dts < pcr`** | B 帧 copy 流会刷屏 |
| 发送校验 | 无 | **0x47 同步校验 + 重同步** | 任何原因失步时能自愈 |

M1 验收状态：

- 本机单播 / 组播回环：4.2 Mbps、0 TS 同步错误、0 CC 错误、FFmpeg 解码通过
- 跨机（第二台电脑）VLC 播放：**尚未实测**
- 断流重连：M2 已实现

---

## 14. M2 实现记录（与初版设计的偏差）

| 项 | 初版设计 | 实际实现 | 原因 |
|---|---|---|---|
| Web 默认开关 | 默认 `false` | 默认 `true`，可 `--no-web` | 当时 M3 尚未开始，默认开才能直接用浏览器验收 |
| Web 技术 | 原始 HTML/CSS/JS | 保持原生，无 npm | 交接成本最低 |
| 接收监测 | 计划内 | 单实例 `UdpReceiver`，REST + 网页弹窗 | 满足“流通没通”的测试需求，多路留后续 |
| 任务日志 | `/logs` API | 每任务 500 行环形缓冲 + WS 实时推送 | 比轮询更直观 |
| 静态页 | cpp-httplib mount | 自写静态路由 + `fs::path` | 项目路径含中文，mount 的窄字符串在 Windows 不可靠 |
| 路径安全 | realpath 前缀 | 额外用 `GetFinalPathNameByHandleW` | `weakly_canonical` 不解析 NTFS junction |
| 输入 URL | 未限制 | 只允许 rtsp/rtmp/http/udp/rtp/srt/tcp/mms | 阻止 `file://` 绕过媒体白名单 |
| 任务状态持久化 | 状态落盘 | 只持久化任务配置，重启回 idle | 断流/运行的瞬态状态不适合直接恢复 |
| 设置更新 | PATCH | 先复制校验、保存成功再替换内存配置 | 避免部分字段生效、部分失败 |
| 认证 | 单 token 必选 | **默认关闭**，设置/命令行可开启并自定义 | 局域网测试免登录；暴露到局域网时再开启 |
| VLC 播放 | 未设计 | 任务行“VLC”按钮，服务器本机 `ShellExecuteW` | 方便同机验收；远程浏览器禁止启动服务器程序 |
| 输出协议 | 预留字段 | `output.protocol` 由地址推导为 UDP 组播/单播 | 协议扩展见第 15 节 |

M2 验收状态：

- 本机单任务 / 双任务并发：4.47 Mbps，0 丢弃/错误
- 新建、启停、编辑、删除、服务重启任务保留：通过
- 单播 / 组播接收监测：0 TS 同步错误、0 CC 错误
- 断流自动重连、路径穿越/junction/`file://` 拒绝：通过
- 跨机 B 电脑浏览器 + VLC：**尚未实测**

---

## 15. 协议扩展规划

当前 `Pipeline` 只面向 UDP 组播/单播：解封装 → mpegts CBR → ByteRing → 令牌桶 → `sendto`。任务 JSON 里的 `output.protocol` 目前由目的地址推导。

要让同一套任务管理支持 RTMP/SRT/RTSP/HLS，需要把发送层抽象为 `OutputSink`：

```
Pipeline（解封装/封装）
   └── OutputSink
        ├── UdpTsSink      组播/单播、CBR、5ms 节奏、发送统计
        ├── FfmpegUrlSink  rtmp/rtmps、srt、rtsp、http
        └── FileSink       mpegts/hls 录制
```

任务模型：

```json
{
  "output": {
    "protocol": "rtmp",
    "url": "rtmp://live.example.com/app/key",
    "ttl": 16,
    "dscp": 0
  }
}
```

实现顺序：

1. 抽 `OutputSink` 接口，把现有 UDP 逻辑原样搬进 `UdpTsSink`，保证 M1/M2 行为不变
2. 增加 `FfmpegUrlSink`：用 `avformat_alloc_output_context2` + `avio_open2` 打开 RTMP/SRT/RTSP
3. Task 表单增加协议下拉、URL/服务器/流密钥字段；UDP 仍显示网卡/组播地址
4. 统计与重连：UDP 用现有统计；FFmpeg 输出用写失败/EOF 触发 TaskManager 重连
5. 多目的地：一个封装结果 fan-out 到多个 `OutputSink`（M4）

工具链 FFmpeg 9.0 已确认包含：`rtmp/rtmps`、`srt`、`rtsp`、`udp`、`tcp`、`http` 协议，以及 `flv`、`mpegts`、`rtsp`、`hls` muxer。

---

## 16. Qt6 桌面客户端（M3）

- Qt Widgets + Qt Network，CMake `find_package(Qt6 COMPONENTS Widgets Network)`
- 独立进程，通过 REST 访问 `mcastd`，不直接持有 Pipeline
- 主界面：连接栏、任务/运行/总码率/认证状态卡片、任务表、工具栏
- 任务表单：名称、输入、循环/实时、网卡下拉、组播地址/端口、TTL/DSCP、CBR、包大小、缓冲、倍速、回环、自动重连
- 服务器媒体库浏览、本地文件选择、输入探测
- 日志、接收监测弹窗；运行中任务可在 GUI 所在电脑上打开 VLC
- 1Hz REST 轮询（未接 WebSocket）；服务器地址/token 存 QSettings（明文）
- Windows 用 windeployqt + 额外运行时 DLL 复制，双击可运行；无安装包/签名

---

## 17. M4 第一批：多目的地与录制

### 17.1 多目的地

- 任务 JSON 的 `output.extra` 最多 4 项，每项 `{group, port}`
- 与主目的地共享网卡、TTL、DSCP、组播回环、包大小
- Pipeline 读一次 ring 后向所有 `MulticastSender` 发送同一份 UDP payload；令牌桶只按读取字节扣一次
- 每个目的地独立 socket 和统计；单路失败只影响该路
- 统计中 `output_bytes/packets/errors` 为所有目的地之和，`destinations` 为路数，`bitrate_mbps` 是发送总带宽

### 17.2 TS 录制

- 任务字段 `record: true`
- TaskManager 在 `<data_dir>/recordings/` 生成 `<taskid>_<毫秒时间戳>.ts`
- Pipeline 在 muxer 的 AVIO write callback 中同步写文件，内容为 CBR MPEG-TS 原始流（含空包）
- `/tasks/{id}/stats` 增加 `recorded_bytes`
- 暂不做切片、滚动清理、转码；停止任务即关闭文件

### 17.3 后续

- 采集卡 / 屏幕捕获输入
- 转码（H.264/AAC）与 RTMP 兼容
- 多目的地独立网卡、录制切片/清理
