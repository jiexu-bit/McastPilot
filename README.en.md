# McastPilot

[简体中文](README.md) | English

Multicast streaming console — turn VLC's click-heavy setup into **one preset, one-click start/stop**.

> GitHub: <https://github.com/jiexu-bit/McastPilot>
> Full design: [DESIGN.md](DESIGN.md). Handover notes (Chinese): [HANDOVER.md](HANDOVER.md).

- **C++20**, FFmpeg for demux/decode/encode, custom MPEG-TS multicast sender
- **C/S architecture**: `mcastd` daemon + native web console + Qt6 desktop client
- **Multi-NIC** detection and forced binding, **multicast/unicast**, TTL / DSCP / CBR / live stats
- Persistent tasks, automatic reconnect, built-in UDP/MPEG-TS receive monitor

## Status

| Milestone | Scope | Status |
|---|---|---|
| M1 | NIC enumeration + multicast/unicast send + FFmpeg muxing + CLI | Done, local loopback verified |
| M2 | `mcastd` + REST/WS + web console + receive monitor | Done locally; cross-machine check pending |
| M3 | Qt6 desktop client | Done, launch/connect/tasks/start-stop/VLC verified locally |
| M4 | Capture / transcode / record / multi-destination | In progress: multi-destination + TS recording + H.264/AAC transcode done; capture/screen pending |

### Verified locally on Windows 11 / WLAN

| Area | Result |
|---|---|
| 720p25 H.264 + AAC, 10 s loop | MPEG-TS CBR 4.5 Mbps, `pkt_size=1316` |
| Send / receive | 4.47–4.51 Mbps sent, 0 packet loss on loopback |
| TS / CC errors | 0 / 0 (multicast and unicast) |
| Loop point | No DTS rollback, no PCR anomaly |
| Concurrent tasks | 2 × 4.5 Mbps CBR, 0 drops/errors |
| Auto reconnect | Recovers to `running` after file EOF |
| Receive monitor | 4.47–4.48 Mbps, 0 TS/CC errors |
| High bitrate | 4K HEVC 25 Mbps sustained, 2–4% buffer, 0 drops |
| Multi-destination | Two unicast destinations received in parallel, 0 TS/CC errors |
| Recording | `<data_dir>/recordings/<taskid>_<timestamp>.ts`, ffprobe sees HEVC + AAC |
| Transcode | 720p H.264 → H.264/AAC 2 Mbps video + 128 kbps audio, CBR 3 Mbps |
| Cross-codec | HEVC 720p → H.264/AAC, ffprobe reports H.264 |
| Transcode loop | 8 s source running 12 s across loop points, 0 TS/CC errors |
| Copy regression | Copy mode 4.5 Mbps, 0 drops/errors |

## Build

The toolchain is **portable** and lives outside the project. Deleting its directory uninstalls it:

```powershell
# 1) Install portable MSYS2 + UCRT64 (gcc / cmake / ninja / gdb / ffmpeg / qt6)
powershell -ExecutionPolicy Bypass -File scripts\setup-toolchain.ps1

# 2) Build
powershell -ExecutionPolicy Bypass -File scripts\build.ps1

# 3) Remove the toolchain (optional)
powershell -ExecutionPolicy Bypass -File scripts\clean-toolchain.ps1
```

Outputs:

```
build\bin\mcastctl.exe   CLI sender
build\bin\mcastd.exe     daemon
build\bin\mcastgui.exe   Qt6 desktop client (Qt runtime bundled)
build\bin\web\           web assets (copied during build)
```

> `mcastgui.exe` can be launched by double-click; windeployqt plus the needed runtime DLLs are copied next to it.
> `mcastctl` / `mcastd` need FFmpeg DLLs. Use `scripts\mcastctl.ps1` / `scripts\mcastd.ps1`,
> or add `toolchain\msys64\ucrt64\bin` to your PATH.

> On Windows, 360 Total Security and similar tools may flag self-built `mcastd.exe` as `HEUR/QVM...`
> and delete or lock it. Add the project `build` directory to the trust/whitelist — do not disable
> the antivirus globally.

Dependencies: `third_party/httplib.h` (cpp-httplib 0.66.0) and
`third_party/nlohmann/json.hpp` (nlohmann/json 3.12.0), both single-header.

## Quick start

### 1. NICs, probe, CLI streaming

```powershell
# List adapters (IP / speed / virtual flag)
powershell -File scripts\mcastctl.ps1 nics

# Probe an input
powershell -File scripts\mcastctl.ps1 probe --input D:\Videos\demo.ts

# Multicast from a specific adapter
powershell -File scripts\mcastctl.ps1 send `
    --input D:\Videos\demo.ts --loop `
    --group 239.1.1.1 --port 5000 `
    --nic 192.168.1.10 --ttl 16 --muxrate 8000000

# Unicast
powershell -File scripts\mcastctl.ps1 send `
    --input D:\Videos\demo.ts --loop `
    --group 192.168.1.50 --port 5000 --muxrate 8000000
```

### 2. Daemon and web console

```powershell
powershell -File scripts\mcastd.ps1 `
    --listen 127.0.0.1 --port 8787 `
    --media-root D:\Videos
```

Open <http://127.0.0.1:8787/>. Authentication is **off by default**; enable it in Settings
and set a custom token if needed (an empty token is generated automatically).

The web UI supports:

- Create / edit / start / stop / delete tasks
- Adapter dropdown, read-only server media library, input probe
- Per-task live bitrate / buffer / drops / send errors
- Live task logs
- Up to 4 extra `group:port` destinations per task
- Optional TS recording into the server data directory
- Optional H.264 + AAC transcode (libx264; video/audio bitrate, preset, GOP)
- Receive monitor (multicast join / unicast listen, TS/CC stats)
- "VLC" button for a running task, launching VLC on the **server desktop** (local access only)

Default data directory `%LOCALAPPDATA%\Mcast\`:

```
config.json       listen, port, auth.enabled, token, media.roots, web.enabled, vlc_path
tasks.json        task presets (kept across restarts; state returns to idle)
logs/mcastd.log   rolling log
recordings/       TS files when recording is enabled (<taskid>_<timestamp>.ts)
```

Common options:

| Option | Default | Description |
|---|---|---|
| `--listen` | `127.0.0.1` | Listen address; use `0.0.0.0` for remote access |
| `--port` | `8787` | HTTP port |
| `--data-dir` | `%LOCALAPPDATA%\Mcast` | Config / tasks / logs directory |
| `--media-root` | `%USERPROFILE%\Videos` | Media-library allow-list root, repeatable |
| `--web` / `--no-web` | on | Serve the web UI |
| `--auth` / `--no-auth` | off | Require an access token |
| `--token` | empty | Custom token; generated when auth is on and token is empty |
| `--vlc-path` | auto-detect | VLC path used by the "Open in VLC" button |
| `--log-level` | `info` | trace/debug/info/warn/error/off |

### 3. Qt6 desktop client

Double-click `build\bin\mcastgui.exe`, or:

```powershell
powershell -File scripts\mcastgui.ps1
```

- Defaults to `http://127.0.0.1:8787`
- If the local `mcastd` is not running, the GUI starts it automatically and listens on `0.0.0.0`;
  another computer can then open `http://<A-LAN-IP>:8787/`
- Leave Token empty when auth is off; enter it when auth is on
- Create/edit/start/stop/delete tasks, adapter dropdown, server media browser, logs, receive monitor
- "VLC" button launches VLC on the machine running the GUI
- Uses 1 Hz REST polling (WebSocket is not wired into the GUI yet)

### 4. Receive verification

Use the web "Monitor" dialog, or VLC. For multicast, the URL **must include `@`**:

```text
udp://@239.1.1.1:5000
```

`udp://239.1.1.1:5000` (without `@`) does not join the multicast group.

```powershell
# Multicast
python tests\udp_receiver.py --group 239.1.1.1 --port 5000 --iface 192.168.3.15 --seconds 10

# Unicast
python tests\udp_receiver.py --group 127.0.0.1 --port 5000 --seconds 10

# ffplay
ffplay "udp://@239.1.1.1:5000?overrun_nonfatal=1&fifo_size=5000000"
```

> For same-host multicast tests, enable "multicast loopback" on the task (CLI: `--loopback`).

## API summary

All `/api/v1/*` endpoints except `health` are open by default. When auth is enabled, send
`Authorization: Bearer <token>`; WebSocket uses `?token=`. Auth can be toggled in Settings,
or via `--auth` / `--no-auth` and `--token`.

| Method | Path | Description |
|---|---|---|
| GET | `/api/v1/health` | Version, uptime, task counts |
| GET | `/api/v1/nics` | Network adapters |
| GET | `/api/v1/media/roots` | Media-library roots |
| GET | `/api/v1/media/list?path=` | Read-only directory listing |
| GET/POST | `/api/v1/tasks` | List / create tasks |
| GET/PATCH/DELETE | `/api/v1/tasks/{id}` | Detail / update / delete |
| POST | `/api/v1/tasks/{id}/start|stop` | Start / stop |
| POST | `/api/v1/tasks/{id}/open-vlc` | Open VLC on the server machine (local only) |
| GET | `/api/v1/tasks/{id}/stats` | Live stats |
| GET | `/api/v1/tasks/{id}/logs?tail=200` | Recent logs |
| POST | `/api/v1/probe` | Probe an input |
| GET/PATCH | `/api/v1/settings` | Global settings |
| GET | `/api/v1/monitor` | Current receive-monitor state |
| POST | `/api/v1/monitor/start|stop` | Start / stop receive monitor |
| WS | `/api/v1/events` | Task / stats / log events |

## Protocol roadmap

Current output is **UDP (multicast/unicast) MPEG-TS** only. `output.protocol` in the task JSON
is derived from the destination address as `udp-multicast` / `udp-unicast`; the web UI does not
expose a protocol selector yet.

The FFmpeg toolchain already supports `rtmp`, `srt`, `rtsp`, `hls`, etc. Planned order:

| Stage | Protocol | Implementation |
|---|---|---|
| Now | UDP multicast/unicast | Custom socket, token bucket, CBR TS |
| Next | RTMP / RTMPS | FFmpeg `flv` muxer + `rtmp://` output |
| Next | SRT / RTP | FFmpeg `mpegts` + `srt://` / `rtp_mpegts` |
| Later | RTSP | FFmpeg `rtsp` muxer |
| Later | HLS / file output | FFmpeg `hls` / `mpegts` file muxers |

This requires extracting the UDP sender into an `OutputSink`:

```text
Pipeline (demux/mux)
   └── OutputSink
        ├── UdpTsSink      current multicast/unicast, CBR, pacing, stats
        ├── FfmpegUrlSink  rtmp/srt/rtsp/http URL
        └── FileSink       recording
```

The task model will gain protocol selection and target URL/server/key fields; the web UI will
gain a protocol dropdown and protocol-specific fields.

## Known limitations

- **Cross-machine acceptance is still pending**: local loopback, browser/API, GUI and VLC have
  passed, but a full "B machine browser + VLC playback" run is still to be recorded.
- Windows loopback multicast needs a small send burst window; current default is 5 ms.
- Capture cards and screen capture are not implemented yet (M4 remaining).
- Transcoding is software libx264 only; 4K high-frame-rate inputs may not sustain real time.
  Hardware encoding and resolution scaling are not implemented.
- Recording writes a raw CBR MPEG-TS stream; no slicing, retention policy, or rotation yet.
- Qt desktop client uses 1 Hz REST polling; it does not subscribe to WebSocket events.
- Desktop server URL/token are stored in a local QSettings INI file in plain text.
- No installer or code signing for the desktop client.
- Output is UDP MPEG-TS only; RTMP/SRT/RTSP/HLS are planned as described above.
- "Open in VLC" works only when the web UI is opened on the server machine; a remote browser
  cannot launch a local VLC instance on another computer.
- Authentication is off by default. If you expose `--listen 0.0.0.0` on a LAN, enable a token.
  The GUI auto-start launches `mcastd` on `0.0.0.0`, so enable auth outside a trusted test network.
- Plain HTTP over LAN; no HTTPS or user accounts. Use only on a trusted network.
- Task state is not resumed after a daemon restart: `tasks.json` keeps the tasks, but they return
  to `idle`.
- Receive monitor handles one stream at a time.
- IPv4 only. Linux code paths exist but have not been compiled/tested in this environment.
- FFmpeg logs `dts < pcr, TS is invalid` for B-frame copy streams; it is benign and filtered.

## Layout

```
src/common   logging, ring buffer, config, rolling file log
src/net      NIC enumeration, multicast sender, UDP/TS receiver, platform layer
src/media    FFmpeg input, TS muxing, pacing, probe, transcoder
src/task     task model, TaskManager, persistence, 1 Hz sampling, reconnect
src/api      REST + WebSocket + token auth + static files
src/server   mcastd entry point
src/cli      mcastctl
src/gui      Qt6 desktop client (REST client)
web/index.html / app.js / style.css
third_party  httplib.h, nlohmann/json.hpp
tests        udp_receiver.py
```
