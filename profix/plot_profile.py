#!/usr/bin/env python3
import argparse
import re
from collections import defaultdict

import matplotlib.pyplot as plt
from matplotlib.collections import LineCollection


FRAME_RE = re.compile(r"PRFL_FRAME\s+(.*)$")
PKT_RE = re.compile(r"PRFL_PKT\s+(.*)$")
REPORT_RE = re.compile(r"=== Profiling Report ===")
STALL_RE = re.compile(r"PRFL_STALL\s+rtp_ts=(\d+)")


def parse_kv(blob):
    out = {}
    for part in blob.strip().split():
        if "=" not in part:
            continue
        key, value = part.split("=", 1)
        try:
            out[key] = int(value)
        except ValueError:
            out[key] = value
    return out


def load_profiles(log_path):
    segments = []
    frames = {}
    packets = defaultdict(list)
    stall_rtp_ts = None
    in_report = False
    with open(log_path, "r", encoding="utf-8", errors="ignore") as f:
        for line in f:
            m = STALL_RE.search(line)
            if m:
                # Start a new segment for the next report to avoid a later
                # stall overwriting the marker for the current report.
                if frames or packets:
                    segments.append((frames, packets, stall_rtp_ts))
                    frames = {}
                    packets = defaultdict(list)
                stall_rtp_ts = int(m.group(1))
                in_report = False
                continue
            if REPORT_RE.search(line):
                if frames or packets:
                    segments.append((frames, packets, stall_rtp_ts))
                    frames = {}
                    packets = defaultdict(list)
                    stall_rtp_ts = None
                in_report = True
                continue
            m = FRAME_RE.search(line)
            if m:
                in_report = True
                data = parse_kv(m.group(1))
                rtp_ts = data.get("rtp_ts")
                if rtp_ts is not None:
                    frames[rtp_ts] = data
                continue
            m = PKT_RE.search(line)
            if m and in_report:
                data = parse_kv(m.group(1))
                rtp_ts = data.get("rtp_ts")
                if rtp_ts is not None:
                    packets[rtp_ts].append(data)
    if frames or packets:
        segments.append((frames, packets, stall_rtp_ts))
    return segments


def collect_time_origin(frames, packets):
    times = []
    for f in frames.values():
        for key in ("enc_start_ms", "enc_end_ms", "first_send_ms", "last_recv_ms"):
            v = f.get(key, -1)
            if isinstance(v, int) and v >= 0:
                times.append(v)
    for plist in packets.values():
        for p in plist:
            for key in ("send_ms", "recv_ms"):
                v = p.get(key, -1)
                if isinstance(v, int) and v >= 0:
                    times.append(v)
    return min(times) if times else 0


def plot_profile(frames,
                 packets,
                 out_path,
                 relative=True,
                 title=None,
                 max_pkts_per_frame=0,
                 show_packets=True,
                 stall_rtp_ts=None):
    if not frames:
        raise RuntimeError("No PRFL_FRAME entries found in log.")

    ordered = sorted(frames.items(), key=lambda x: x[0])
    t0 = collect_time_origin(frames, packets) if relative else 0

    fig, ax = plt.subplots(figsize=(14, 8))
    y_ticks = []
    y_labels = []
    encode_segments = []
    decode_segments = []
    first_send_x = []
    first_send_y = []
    send_x = []
    send_y = []
    recv_x = []
    recv_y = []

    stall_y = None
    stall_x = None
    for idx, (rtp_ts, f) in enumerate(ordered):
        y = idx
        y_ticks.append(y)
        y_labels.append(str(rtp_ts))
        enc_start = f.get("enc_start_ms", -1)
        enc_end = f.get("enc_end_ms", -1)
        if stall_rtp_ts is not None and rtp_ts == stall_rtp_ts:
            stall_y = y
            if enc_start >= 0:
                stall_x = enc_start - t0
        if enc_start >= 0 and enc_end >= 0:
            encode_segments.append([(enc_start - t0, y), (enc_end - t0, y)])

        first_send = f.get("first_send_ms", -1)
        if first_send >= 0:
            first_send_x.append(first_send - t0)
            first_send_y.append(y)
            if stall_y == y and stall_x is None:
                stall_x = first_send - t0

        last_recv = f.get("last_recv_ms", -1)
        last_recv_to_decode = f.get("last_recv_to_decode_ms", -1)
        decode_time = f.get("decode_time_ms", -1)
        if last_recv >= 0 and last_recv_to_decode >= 0 and decode_time >= 0:
            decode_start = last_recv + last_recv_to_decode
            decode_end = decode_start + decode_time
            decode_segments.append([(decode_start - t0, y), (decode_end - t0, y)])

        if show_packets:
            plist = packets.get(rtp_ts, [])
            if max_pkts_per_frame and len(plist) > max_pkts_per_frame:
                step = max(1, len(plist) // max_pkts_per_frame)
                plist = plist[::step][:max_pkts_per_frame]
            for p in plist:
                send_ms = p.get("send_ms", -1)
                recv_ms = p.get("recv_ms", -1)
                if send_ms >= 0:
                    send_x.append(send_ms - t0)
                    send_y.append(y)
                if recv_ms >= 0:
                    recv_x.append(recv_ms - t0)
                    recv_y.append(y)

    if encode_segments:
        ax.add_collection(LineCollection(
            encode_segments, colors="tab:blue", linewidths=4, label="encode"))
    if decode_segments:
        ax.add_collection(LineCollection(
            decode_segments, colors="tab:orange", linewidths=4, label="decode"))
    if first_send_x:
        ax.scatter(first_send_x, first_send_y, color="tab:cyan", s=16,
                   label="first_send")
    if send_x:
        ax.scatter(send_x, send_y, color="tab:green", s=8, label="send_pkt")
    if recv_x:
        ax.scatter(recv_x, recv_y, color="tab:red", s=8, label="recv_pkt")
    if stall_y is not None:
        mark_x = stall_x if stall_x is not None else 0
        ax.scatter([mark_x], [stall_y], color="black", marker="*", s=160,
                   label="stall_frame")

    ax.set_yticks(y_ticks)
    ax.set_yticklabels(y_labels)
    ax.set_xlabel("time (ms, relative)" if relative else "time (ms)")
    ax.set_ylabel("rtp_timestamp")
    ax.set_title(title or "Stall Profiling Timeline")
    ax.grid(True, axis="x", linestyle="--", alpha=0.4)
    ax.legend(loc="upper right", ncol=3)
    fig.tight_layout()
    fig.savefig(out_path, dpi=150)


def main():
    parser = argparse.ArgumentParser(
        description="Plot stall profiling timeline from PRFL_* logs.")
    parser.add_argument("--log", default="logs/send_0",
                        help="Path to send log file (default: logs/send_0)")
    parser.add_argument("--out", default="logs/profile_timeline.png",
                        help="Output image path (single report or base name)")
    parser.add_argument("--absolute", action="store_true",
                        help="Use absolute time axis instead of relative")
    parser.add_argument("--title", default=None, help="Plot title")
    parser.add_argument("--split", action="store_true",
                        help="Generate one plot per profiling report")
    parser.add_argument("--max-pkts-per-frame", type=int, default=0,
                        help="Limit packet points per frame for speed (0 = no limit)")
    parser.add_argument("--no-pkts", action="store_true",
                        help="Skip packet points to speed up plotting")
    args = parser.parse_args()

    segments = load_profiles(args.log)
    if not segments:
        raise RuntimeError("No profiling reports found in log.")

    if args.split and len(segments) > 1:
        base, ext = args.out.rsplit(".", 1) if "." in args.out else (args.out, "png")
        for idx, (frames, packets, stall_rtp_ts) in enumerate(segments, start=1):
            out_path = f"{base}_{idx}.{ext}"
            title = args.title or f"Stall Profiling Timeline #{idx}"
            plot_profile(frames, packets, out_path,
                         relative=not args.absolute, title=title,
                         max_pkts_per_frame=args.max_pkts_per_frame,
                         show_packets=not args.no_pkts,
                         stall_rtp_ts=stall_rtp_ts)
    else:
        frames, packets, stall_rtp_ts = segments[-1]
        plot_profile(frames, packets, args.out, relative=not args.absolute,
                     title=args.title,
                     max_pkts_per_frame=args.max_pkts_per_frame,
                     show_packets=not args.no_pkts,
                     stall_rtp_ts=stall_rtp_ts)


if __name__ == "__main__":
    main()
