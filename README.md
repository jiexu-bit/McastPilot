# McastPilot

组播推流控制台 —— 把 VLC 里点来点去的配置，变成**一次预设、一键启停**。

> GitHub: <https://github.com/jiexu-bit/McastPilot>
> 设计细节见 [DESIGN.md](DESIGN.md)；**接手/交接请看 [HANDOVER.md](HANDOVER.md)**（现状、代码地图、踩坑记录、M2 计划）。

- **C++20**，复用 **FFmpeg** 做解封装/编码，自研 TS 组播发送层
- **C/S 架构**：`mcastd` 常驻服务端 + Qt 桌面客户端 + 可选 Web 端
- **多网卡**识别与强制绑定，**组播**推流，TTL / DSCP / CBR / 实时统计

> 设计细节见 [DESIGN.md](DESIGN.md)；**接手/交接请看 [HANDOVER.md](HANDOVER.md)**（现状、代码地图、踩坑记录、M2 计划）。

## 状态

| 里程碑 | 内容 | 状态 |
|---|---|---|
| M1 | 网卡枚举 + 组播/单播发送 + FFmpeg 封装 + CLI | ✅ 已完成 |
| M2 | 服务端 + REST/WS + 网页端 | 待开始 |
| M3 | Qt6 桌面客户端 | 待开始 |
| M4 | 采集/转码/录制/接收监测 | 待开始 |

### M1 实测（本机 Windows 11 / WLAN 2.4Gbps）

| 项目 | 结果 |
|---|---|
| 输入 | 720p25 H.264 + AAC，10s 循环 |
| 输出 | MPEG-TS CBR 4.5 Mbps，`pkt_size=1316` |
| 发送速率 | 4.47–4.51 Mbps（稳定） |
| 缓冲水位 | 1%–14% |
| 接收端丢包 | 0（组播回环 5ms 突发窗口） |
| TS 同步错误 | 0 |
| 解码 | FFmpeg 解码零错误（首帧从 GOP 中间开始除外） |
| 循环点 | 跨循环无 `dts` 回退、无 PCR 异常 |

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

产物：`build\bin\mcastctl.exe`

> 直接双击 exe 会缺 FFmpeg/Qt 的 DLL，请用 `scripts\mcastctl.ps1` 转发参数（它只在当前进程里加 PATH），
> 或者把 `toolchain\msys64\ucrt64\bin` 加到你自己的 PATH。

## 快速使用

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

# 推单播（同一套封装与节奏，只是目的地不是组播地址）
powershell -File scripts\mcastctl.ps1 send `
    --input D:\Videos\demo.ts --loop `
    --group 192.168.1.50 --port 5000 --muxrate 8000000
```

接收端验证：

```powershell
# 官方工具
ffplay "udp://@239.1.1.1:5000?overrun_nonfatal=1&fifo_size=5000000"

# 仓库自带的验证脚本：码率 / 包数 / TS 同步错误 / CC 连续性错误
python tests\udp_receiver.py --group 239.1.1.1 --port 5000 --iface 192.168.3.15 --seconds 10
```

> 同机回环测试时，发送端要加 `--loopback`；跨机不需要。

## 已知限制（M1）

- **跨机播放尚未实测**（本机单播/组播回环 + 解码已通过）
- Windows 本机回环的组播发送突发窗口不能太大：过大时 Windows/Wi‑Fi 会丢包。
  当前默认 5 ms 突发窗口，实测 4.5 Mbps 回环零丢包；跨机建议实测后再调 `--muxrate`。
- FFmpeg 对 B 帧流会打印 `dts < pcr, TS is invalid`，输出本身可正常解码，已在日志层过滤。
- 输入为 `copy` 模式，不做转码（M4 再加）。
- 断流自动重连、接收监测、Web/桌面客户端属于 M2–M4。

## 目录

```
src/common   日志、字节环形缓冲
src/net      网卡枚举、组播发送/接收
src/media    FFmpeg 输入、TS 封装、发送节奏
src/cli      mcastctl
```
