#!/usr/bin/env python3
import argparse
import csv
import json
import math
import os
import re
from pathlib import Path
from statistics import mean, pstdev


TIME_RE = re.compile(r"\[(\d+):(\d+)\]")
SEND_RE = re.compile(r"Prfl_frame_send@(\d+)")
RECV_RE = re.compile(r"Prfl_frame_recv@(\d+)")


def parse_time_ms(line: str):
    match = TIME_RE.search(line)
    if not match:
        return None
    return int(match.group(1)) * 1000 + int(match.group(2))


def parse_send_recv(send_path: Path, recv_path: Path):
    send_times = {}
    recv_times = {}

    if send_path.exists():
        for line in send_path.read_text(errors="ignore").splitlines():
            match = SEND_RE.search(line)
            if not match:
                continue
            ts = int(match.group(1))
            t_ms = parse_time_ms(line)
            if t_ms is None:
                continue
            if ts not in send_times or t_ms < send_times[ts]:
                send_times[ts] = t_ms

    if recv_path.exists():
        for line in recv_path.read_text(errors="ignore").splitlines():
            match = RECV_RE.search(line)
            if not match:
                continue
            ts = int(match.group(1))
            t_ms = parse_time_ms(line)
            if t_ms is None:
                continue
            if ts not in recv_times or t_ms < recv_times[ts]:
                recv_times[ts] = t_ms

    return send_times, recv_times


def quantile(sorted_vals, q):
    if not sorted_vals:
        return None
    if q <= 0:
        return sorted_vals[0]
    if q >= 1:
        return sorted_vals[-1]
    pos = q * (len(sorted_vals) - 1)
    lo = int(math.floor(pos))
    hi = int(math.ceil(pos))
    if lo == hi:
        return sorted_vals[lo]
    return sorted_vals[lo] + (sorted_vals[hi] - sorted_vals[lo]) * (pos - lo)


def compute_metrics(send_times, recv_times, stall_threshold_ms, latency_offset_ms):
    matched = []
    for ts, send_ms in send_times.items():
        recv_ms = recv_times.get(ts)
        if recv_ms is None:
            continue
        matched.append((ts, send_ms, recv_ms, recv_ms - send_ms - latency_offset_ms))

    matched.sort(key=lambda x: x[2])
    latencies = [m[3] for m in matched]
    recv_timeline = [m[2] for m in matched]

    stall_gaps = []
    stall_duration_ms = 0
    stall_count = 0
    total_duration_ms = 0
    if len(recv_timeline) >= 2:
        total_duration_ms = recv_timeline[-1] - recv_timeline[0]
        for prev, cur in zip(recv_timeline, recv_timeline[1:]):
            gap = cur - prev
            if gap > stall_threshold_ms:
                stall_count += 1
                stall_duration_ms += gap
                stall_gaps.append(gap)

    latency_sorted = sorted(latencies)
    latency_p50 = quantile(latency_sorted, 0.50)
    latency_p95 = quantile(latency_sorted, 0.95)
    latency_p99 = quantile(latency_sorted, 0.99)
    latency_mean = mean(latencies) if latencies else None

    stall_rate = 0
    if total_duration_ms > 0:
        stall_rate = stall_duration_ms / total_duration_ms

    return {
        "frames_sent": len(send_times),
        "frames_recv": len(recv_times),
        "frames_matched": len(matched),
        "latency_p50_ms": latency_p50,
        "latency_p95_ms": latency_p95,
        "latency_p99_ms": latency_p99,
        "latency_mean_ms": latency_mean,
        "stall_count": stall_count,
        "stall_duration_ms": stall_duration_ms,
        "stall_rate": stall_rate,
        "total_duration_ms": total_duration_ms,
    }, matched, stall_gaps


def safe_mean(values):
    values = [v for v in values if v is not None]
    if not values:
        return None
    return mean(values)


def safe_pstdev(values):
    values = [v for v in values if v is not None]
    if len(values) < 2:
        return 0.0
    return pstdev(values)


def maybe_plot(out_dir: Path, series, title, xlabel, filename):
    try:
        import matplotlib.pyplot as plt
    except Exception:
        return False

    plt.figure(figsize=(8, 5))
    has_series = False
    for label, values in series.items():
        if not values:
            continue
        x = sorted(values)
        y = [(i + 1) / len(x) for i in range(len(x))]
        plt.plot(x, y, label=label)
        has_series = True
    plt.title(title)
    plt.xlabel(xlabel)
    plt.ylabel("CDF")
    plt.grid(True, alpha=0.3)
    if has_series:
        plt.legend()
    plt.tight_layout()
    plt.savefig(out_dir / filename)
    plt.close()
    return True


def plot_bar(out_dir: Path, metrics_by_label):
    try:
        import matplotlib.pyplot as plt
    except Exception:
        return False

    labels = list(metrics_by_label.keys())
    metrics = [
        ("stall_count_mean", "stall_count"),
        ("stall_duration_ms_mean", "stall_duration_ms"),
        ("stall_rate_mean", "stall_rate"),
        ("latency_p50_ms_mean", "latency_p50_ms"),
        ("latency_p95_ms_mean", "latency_p95_ms"),
    ]
    fig, axes = plt.subplots(1, len(metrics), figsize=(16, 4))
    for ax, (metric_key, title) in zip(axes, metrics):
        vals = [metrics_by_label[label].get(metric_key) for label in labels]
        vals = [v if v is not None else 0 for v in vals]
        ax.bar(labels, vals)
        ax.set_title(title)
        ax.tick_params(axis="x", rotation=20)
    plt.tight_layout()
    plt.savefig(out_dir / "compare_metrics.png")
    plt.close()
    return True


def main():
    parser = argparse.ArgumentParser(description="Analyze Prfl frame send/recv.")
    parser.add_argument(
        "--input",
        action="append",
        default=[],
        help="label=path to runs directory (contains run_XX/send_0, recv_0)",
    )
    parser.add_argument("--out", required=True, help="output directory")
    parser.add_argument("--stall-threshold", type=int, default=100)
    parser.add_argument("--latency-offset", type=int, default=0)
    args = parser.parse_args()

    inputs = []
    for item in args.input:
        if "=" not in item:
            raise ValueError("input must be label=path")
        label, path = item.split("=", 1)
        inputs.append((label, Path(path)))

    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    per_run_rows = []
    all_latencies = {}
    all_stall_gaps = {}
    metrics_by_label = {}

    for label, runs_dir in inputs:
        all_latencies[label] = []
        all_stall_gaps[label] = []
        run_metrics = []

        for run_dir in sorted(runs_dir.glob("run_*")):
            send_path = run_dir / "send_0"
            recv_path = run_dir / "recv_0"
            send_times, recv_times = parse_send_recv(send_path, recv_path)
            metrics, matched, stall_gaps = compute_metrics(
                send_times,
                recv_times,
                args.stall_threshold,
                args.latency_offset,
            )
            metrics["label"] = label
            metrics["run_id"] = run_dir.name
            per_run_rows.append(metrics)
            run_metrics.append(metrics)
            all_latencies[label].extend([m[3] for m in matched])
            all_stall_gaps[label].extend(stall_gaps)

            per_frame_path = out_dir / f"latency_{label}_{run_dir.name}.csv"
            with per_frame_path.open("w", newline="") as f:
                writer = csv.writer(f)
                writer.writerow(["rtp_ts", "send_ms", "recv_ms", "latency_ms"])
                for ts, send_ms, recv_ms, latency in matched:
                    writer.writerow([ts, send_ms, recv_ms, latency])

        metrics_by_label[label] = {
            "runs": len(run_metrics),
            "frames_matched_mean": safe_mean([m["frames_matched"] for m in run_metrics]),
            "latency_p50_ms_mean": safe_mean([m["latency_p50_ms"] for m in run_metrics]),
            "latency_p95_ms_mean": safe_mean([m["latency_p95_ms"] for m in run_metrics]),
            "latency_p99_ms_mean": safe_mean([m["latency_p99_ms"] for m in run_metrics]),
            "latency_mean_ms_mean": safe_mean([m["latency_mean_ms"] for m in run_metrics]),
            "stall_count_mean": safe_mean([m["stall_count"] for m in run_metrics]),
            "stall_duration_ms_mean": safe_mean([m["stall_duration_ms"] for m in run_metrics]),
            "stall_rate_mean": safe_mean([m["stall_rate"] for m in run_metrics]),
            "total_duration_ms_mean": safe_mean([m["total_duration_ms"] for m in run_metrics]),
            "latency_p50_ms_std": safe_pstdev([m["latency_p50_ms"] for m in run_metrics]),
            "latency_p95_ms_std": safe_pstdev([m["latency_p95_ms"] for m in run_metrics]),
            "stall_count_std": safe_pstdev([m["stall_count"] for m in run_metrics]),
            "stall_duration_ms_std": safe_pstdev([m["stall_duration_ms"] for m in run_metrics]),
            "stall_rate_std": safe_pstdev([m["stall_rate"] for m in run_metrics]),
        }

    if per_run_rows:
        with (out_dir / "per_run_metrics.csv").open("w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=sorted(per_run_rows[0].keys()))
            writer.writeheader()
            writer.writerows(per_run_rows)

    with (out_dir / "summary.json").open("w") as f:
        json.dump(metrics_by_label, f, indent=2)

    with (out_dir / "summary.csv").open("w", newline="") as f:
        labels = list(metrics_by_label.keys())
        if labels:
            fieldnames = ["label"] + sorted(
                k for k in metrics_by_label[labels[0]].keys() if k != "label"
            )
            writer = csv.DictWriter(f, fieldnames=fieldnames)
            writer.writeheader()
            for label in labels:
                row = {"label": label}
                row.update(metrics_by_label[label])
                writer.writerow(row)

    plotted = False
    plotted |= maybe_plot(
        out_dir,
        all_latencies,
        "Per-frame latency CDF",
        "Latency (ms)",
        "cdf_latency.png",
    )
    plotted |= maybe_plot(
        out_dir,
        all_stall_gaps,
        f"Stall gap CDF (> {args.stall_threshold} ms)",
        "Gap (ms)",
        "cdf_stall_gap.png",
    )
    plotted |= plot_bar(out_dir, metrics_by_label)

    if not plotted:
        print("matplotlib not available; skipped plots")


if __name__ == "__main__":
    main()
