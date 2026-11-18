#!/usr/bin/env python3
"""Minimal UDP / MPEG-TS receiver used to validate mcastctl output.

Examples
--------
Multicast (join the group on a specific NIC):
    python tests/udp_receiver.py --group 239.1.1.1 --port 5000 --iface 192.168.1.10 --seconds 10

Unicast:
    python tests/udp_receiver.py --group 127.0.0.1 --port 5000 --seconds 10

Save the received transport stream for ffprobe/ffmpeg:
    python tests/udp_receiver.py --group 239.1.1.1 --port 5000 --iface 192.168.1.10 --out cap.ts
"""

from __future__ import annotations

import argparse
import socket
import struct
import sys
import time

TS_PACKET_SIZE = 188
TS_SYNC_BYTE = 0x47


def is_multicast(address: str) -> bool:
    try:
        first = int(address.split(".")[0])
    except (ValueError, IndexError):
        return False
    return 224 <= first <= 239


def parse_ts(payload: bytes, cc_state: dict[int, int]) -> tuple[int, int]:
    """Return (sync_errors, cc_errors) for one datagram."""
    sync_errors = 0
    cc_errors = 0

    if len(payload) % TS_PACKET_SIZE != 0:
        sync_errors += 1

    for offset in range(0, len(payload) - TS_PACKET_SIZE + 1, TS_PACKET_SIZE):
        if payload[offset] != TS_SYNC_BYTE:
            sync_errors += 1
            continue
        pid = ((payload[offset + 1] & 0x1F) << 8) | payload[offset + 2]
        if pid == 0x1FFF:  # null packet
            continue
        afc = (payload[offset + 3] >> 4) & 0x03
        if afc in (1, 3):
            counter = payload[offset + 3] & 0x0F
            previous = cc_state.get(pid)
            if previous is not None and counter != ((previous + 1) & 0x0F):
                cc_errors += 1
            cc_state[pid] = counter
    return sync_errors, cc_errors


def main() -> int:
    parser = argparse.ArgumentParser(description="UDP/MPEG-TS receiver for mcastctl validation")
    parser.add_argument("--group", default="239.255.1.1", help="destination address")
    parser.add_argument("--port", type=int, default=5000)
    parser.add_argument("--iface", default="", help="local IPv4 to join/send on (multicast)")
    parser.add_argument("--seconds", type=float, default=10.0)
    parser.add_argument("--out", default="", help="write the received TS to this file")
    parser.add_argument("--rcvbuf", type=int, default=8 * 1024 * 1024)
    args = parser.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, args.rcvbuf)
    sock.bind(("" if is_multicast(args.group) else args.iface or "0.0.0.0", args.port))

    if is_multicast(args.group):
        iface = args.iface
        if not iface:
            print("multicast requires --iface <local-ip>", file=sys.stderr)
            return 2
        mreq = struct.pack("4s4s", socket.inet_aton(args.group), socket.inet_aton(iface))
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)

    sock.settimeout(1.0)
    out = open(args.out, "wb") if args.out else None

    packets = 0
    octets = 0
    sync_errors = 0
    cc_errors = 0
    cc_state: dict[int, int] = {}
    started = time.time()
    last_report = started

    print(
        f"listening on {args.group}:{args.port}"
        + (f" via {args.iface}" if is_multicast(args.group) else " (unicast)")
        + f" for {args.seconds:.0f}s"
    )

    try:
        while time.time() - started < args.seconds:
            try:
                payload, _ = sock.recvfrom(65535)
            except socket.timeout:
                continue
            packets += 1
            octets += len(payload)
            if out:
                out.write(payload)
            s_err, c_err = parse_ts(payload, cc_state)
            sync_errors += s_err
            cc_errors += c_err

            now = time.time()
            if now - last_report >= 1.0:
                last_report = now
                elapsed = now - started
                print(
                    f"[{elapsed:5.1f}s] {octets * 8 / elapsed / 1e6:6.2f} Mbps  "
                    f"pkts={packets}  sync_err={sync_errors}  cc_err={cc_errors}",
                    flush=True,
                )
    finally:
        if out:
            out.close()

    elapsed = max(time.time() - started, 1e-9)
    print(
        f"\nsummary: {packets} datagrams, {octets} bytes, "
        f"{octets * 8 / elapsed / 1e6:.2f} Mbps, "
        f"sync_err={sync_errors}, cc_err={cc_errors}"
    )
    if packets == 0:
        print(
            "no data: check --iface, multicast TTL/IGMP, Windows firewall, "
            "and that the sender used --loopback for same-host tests",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
