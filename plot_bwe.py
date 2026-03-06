import argparse
import re

import plotly.graph_objects as go
import plotly.io as pio
from plotly.subplots import make_subplots


def parse_mah_log(path):
    init_ts = None
    base_ts = None
    samples = []
    rtp_time_ms = {}
    enqueue_re = re.compile(r"^(\d+)\s+\+\s+\d+\s+(\d+)$")
    with open(path, "r", encoding="utf-8", errors="ignore") as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            if line.startswith("#"):
                if "init timestamp:" in line:
                    init_ts = int(line.split("init timestamp:")[1].strip())
                elif "base timestamp:" in line:
                    base_ts = int(line.split("base timestamp:")[1].strip())
                continue
            if not line[0].isdigit():
                continue
            enqueue_match = enqueue_re.match(line)
            if enqueue_match:
                time_ms = int(enqueue_match.group(1))
                rtp_ts = int(enqueue_match.group(2))
                if rtp_ts != 0 and rtp_ts not in rtp_time_ms:
                    rtp_time_ms[rtp_ts] = time_ms
                continue
            parts = line.split()
            if len(parts) < 3:
                continue
            if parts[1] != "#":
                continue
            time_ms = int(parts[0])
            size_bytes = int(parts[2])
            samples.append((time_ms, size_bytes))
    if not samples:
        raise ValueError("mah.log 没有找到 # 记录")
    if base_ts is None:
        base_ts = samples[0][0]
    normalized = [(t - base_ts, b) for t, b in samples]
    normalized_rtp = {rtp: (t - base_ts) for rtp, t in rtp_time_ms.items()}
    return normalized, normalized_rtp, init_ts, base_ts


def bin_bandwidth(samples, bin_ms):
    buckets = {}
    for t_ms, size_bytes in samples:
        bucket = int(t_ms // bin_ms)
        buckets[bucket] = buckets.get(bucket, 0) + size_bytes
    times = []
    kbps = []
    for bucket in sorted(buckets):
        start_ms = bucket * bin_ms
        bytes_sum = buckets[bucket]
        rate_kbps = bytes_sum * 8.0 / (bin_ms / 1000.0) / 1000.0
        times.append(start_ms)
        kbps.append(rate_kbps)
    return times, kbps


def parse_send_log(path):
    time_re = re.compile(r"^\[(\d+):(\d+)\]")
    est_re = re.compile(r"estimate_bps=(\d+)")
    samples = []
    with open(path, "r", encoding="utf-8", errors="ignore") as handle:
        for line in handle:
            time_match = time_re.search(line)
            est_match = est_re.search(line)
            if not time_match or not est_match:
                continue
            sec = int(time_match.group(1))
            ms = int(time_match.group(2))
            time_ms = sec * 1000 + ms
            estimate_bps = int(est_match.group(1))
            samples.append((time_ms, estimate_bps))
    if not samples:
        raise ValueError("send_0 没有找到 estimate_bps 记录")
    return samples


def parse_send_rtp_map(path):
    time_re = re.compile(r"^\[(\d+):(\d+)\]")
    prfl_re = re.compile(r"Prfl_pkt_send@(\d+)")
    rtp_time_ms = {}
    with open(path, "r", encoding="utf-8", errors="ignore") as handle:
        for line in handle:
            time_match = time_re.search(line)
            prfl_match = prfl_re.search(line)
            if not time_match or not prfl_match:
                continue
            sec = int(time_match.group(1))
            ms = int(time_match.group(2))
            time_ms = sec * 1000 + ms
            rtp_ts = int(prfl_match.group(1))
            if rtp_ts != 0 and rtp_ts not in rtp_time_ms:
                rtp_time_ms[rtp_ts] = time_ms
    return rtp_time_ms


def median(values):
    values = sorted(values)
    n = len(values)
    if n == 0:
        return None
    mid = n // 2
    if n % 2 == 1:
        return values[mid]
    return (values[mid - 1] + values[mid]) / 2.0


def rtp_sync_offset_ms(mah_rtp_map, send_rtp_map, min_matches=100):
    common = sorted(set(mah_rtp_map).intersection(send_rtp_map))
    if len(common) < min_matches:
        return 0, len(common), None, None
    diffs = [mah_rtp_map[rtp] - send_rtp_map[rtp] for rtp in common]
    diffs_sorted = sorted(diffs)
    return int(round(median(diffs_sorted))), len(common), diffs_sorted[0], diffs_sorted[-1]


def _bucketize_average(samples, bin_ms):
    buckets = {}
    counts = {}
    for t_ms, value in samples:
        bucket = int(t_ms // bin_ms)
        buckets[bucket] = buckets.get(bucket, 0.0) + float(value)
        counts[bucket] = counts.get(bucket, 0) + 1
    return {k: buckets[k] / counts[k] for k in buckets}


def _pearson_corr(xs, ys):
    n = 0
    sum_x = 0.0
    sum_y = 0.0
    sum_x2 = 0.0
    sum_y2 = 0.0
    sum_xy = 0.0
    for x, y in zip(xs, ys):
        if x is None or y is None:
            continue
        x = float(x)
        y = float(y)
        n += 1
        sum_x += x
        sum_y += y
        sum_x2 += x * x
        sum_y2 += y * y
        sum_xy += x * y
    if n < 3:
        return None
    denom_x = (n * sum_x2 - sum_x * sum_x) ** 0.5
    denom_y = (n * sum_y2 - sum_y * sum_y) ** 0.5
    if denom_x == 0.0 or denom_y == 0.0:
        return None
    return (n * sum_xy - sum_x * sum_y) / (denom_x * denom_y)


def auto_sync_offset_ms(mah_bandwidth, send_est_kbps, bin_ms, max_lag_s):
    if not mah_bandwidth or not send_est_kbps:
        return 0

    mah_map = _bucketize_average(mah_bandwidth, bin_ms)
    send_map = _bucketize_average([(t, v / 1000.0) for t, v in send_est_kbps],
                                  bin_ms)

    mah_min = min(mah_map)
    mah_max = max(mah_map)
    send_min = min(send_map)
    send_max = max(send_map)

    best_lag = 0
    best_corr = None

    max_lag_bins = int((max_lag_s * 1000) // bin_ms)
    for lag_bins in range(-max_lag_bins, max_lag_bins + 1):
        start = max(mah_min, send_min + lag_bins)
        end = min(mah_max, send_max + lag_bins)
        if end - start < 3:
            continue
        xs = []
        ys = []
        for b in range(start, end + 1):
            xs.append(mah_map.get(b))
            ys.append(send_map.get(b - lag_bins))
        corr = _pearson_corr(xs, ys)
        if corr is None:
            continue
        if best_corr is None or corr > best_corr:
            best_corr = corr
            best_lag = lag_bins

    return best_lag * bin_ms


def parse_p_log(path):
    frame_re = re.compile(r"^RTP TS:\s*(\d+).*\bE2E\s+(-?\d+(?:\.\d+)?)\b")
    frames = []
    with open(path, "r", encoding="utf-8", errors="ignore") as handle:
        for line in handle:
            line = line.strip()
            match = frame_re.match(line)
            if not match:
                continue
            rtp_ts = int(match.group(1))
            e2e_ms = float(match.group(2))
            frames.append((rtp_ts, e2e_ms))
    return frames


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--mah-log", default="logs/mah.log")
    parser.add_argument("--send-log", default="logs/send_0")
    parser.add_argument("--p-log", default="logs/p.log")
    parser.add_argument("--out", default="logs/bwe_plot.html")
    parser.add_argument("--bin-ms", type=int, default=100)
    parser.add_argument("--mah-offset-ms", type=int, default=0)
    parser.add_argument("--send-offset-ms", type=int, default=0)
    parser.add_argument("--no-align-zero", action="store_true")
    parser.add_argument("--no-rtp-sync", action="store_true")
    parser.add_argument("--auto-sync", action="store_true")
    parser.add_argument("--max-lag-s", type=int, default=30)
    args = parser.parse_args()

    mah_samples, mah_rtp_map, _, _ = parse_mah_log(args.mah_log)
    send_samples = parse_send_log(args.send_log)
    send_rtp_map = parse_send_rtp_map(args.send_log)

    mah_samples = [(t + args.mah_offset_ms, v) for t, v in mah_samples]
    send_samples = [(t + args.send_offset_ms, v) for t, v in send_samples]

    mah_rtp_adj = {k: v + args.mah_offset_ms for k, v in mah_rtp_map.items()}
    send_rtp_adj = {k: v + args.send_offset_ms for k, v in send_rtp_map.items()}

    rtp_synced = False
    rtp_anchor_ms = None
    if not args.no_rtp_sync:
        lag_ms, match_count, min_diff, max_diff = rtp_sync_offset_ms(mah_rtp_adj, send_rtp_adj)
        if match_count >= 100:
            rtp_synced = True
            if lag_ms != 0:
                send_samples = [(t + lag_ms, v) for t, v in send_samples]
                send_rtp_adj = {k: v + lag_ms for k, v in send_rtp_adj.items()}
            common = set(mah_rtp_adj).intersection(send_rtp_adj)
            anchor_rtp = min(common, key=lambda r: send_rtp_adj[r])
            rtp_anchor_ms = mah_rtp_adj[anchor_rtp]
            print("rtp-sync lag_ms:", lag_ms, "matches:", match_count, "diff_range:", (min_diff, max_diff), "anchor_rtp:", anchor_rtp)

    mah_times, mah_kbps = bin_bandwidth(mah_samples, args.bin_ms)
    if args.auto_sync:
        lag_ms = auto_sync_offset_ms(list(zip(mah_times, mah_kbps)),
                                     send_samples,
                                     args.bin_ms,
                                     args.max_lag_s)
        if lag_ms != 0:
            send_samples = [(t + lag_ms, v) for t, v in send_samples]
            print("auto-sync lag_ms:", lag_ms)

    start_ms = 0
    if not args.no_align_zero:
        if rtp_synced and rtp_anchor_ms is not None:
            start_ms = rtp_anchor_ms
        else:
            start_ms = min(min(t for t, _ in mah_samples), min(t for t, _ in send_samples))
        if start_ms != 0:
            mah_samples = [(t - start_ms, v) for t, v in mah_samples]
            send_samples = [(t - start_ms, v) for t, v in send_samples]
            mah_times, mah_kbps = bin_bandwidth(mah_samples, args.bin_ms)

    send_times = [t for t, _ in send_samples]
    send_kbps = [bps / 1000.0 for _, bps in send_samples]

    p_frames = parse_p_log(args.p_log)
    p_times = []
    p_delays = []
    for rtp_ts, e2e_ms in p_frames:
        t_abs = mah_rtp_adj.get(rtp_ts)
        if t_abs is None:
            t_abs = send_rtp_adj.get(rtp_ts)
        if t_abs is None:
            continue
        if not args.no_align_zero:
            t_plot = t_abs - start_ms
        else:
            t_plot = t_abs
        p_times.append(t_plot)
        p_delays.append(e2e_ms)

    fig = make_subplots(rows=2, cols=1, shared_xaxes=True, vertical_spacing=0.08)
    fig.add_trace(go.Scatter(x=[t / 1000.0 for t in mah_times], y=mah_kbps,
                             mode="lines", name="Mahimahi带宽(kbps)"),
                  row=1, col=1)
    fig.add_trace(go.Scatter(x=[t / 1000.0 for t in send_times], y=send_kbps,
                             mode="lines", name="估计带宽(kbps)"),
                  row=1, col=1)
    fig.add_trace(go.Scatter(x=[t / 1000.0 for t in p_times], y=p_delays,
                             mode="markers", name="每帧延迟(ms)"),
                  row=2, col=1)
    fig.update_layout(
        title="Mahimahi带宽与WebRTC估计带宽 / 每帧延迟",
        legend_title="曲线",
        template="plotly_white",
    )
    fig.update_yaxes(title_text="带宽 (kbps)", row=1, col=1)
    fig.update_yaxes(title_text="延迟 (ms)", row=2, col=1)
    fig.update_xaxes(title_text="时间 (s)", row=2, col=1)
    pio.write_html(fig, args.out, include_plotlyjs=True, auto_open=False)
    print("输出:", args.out)
    print("Mahimahi点数:", len(mah_times), "估计点数:", len(send_times), "帧点数:", len(p_times))


if __name__ == "__main__":
    main()
