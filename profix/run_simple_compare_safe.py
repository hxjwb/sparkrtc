#!/usr/bin/env python3
import argparse
import shutil
import subprocess
import sys
import time
from pathlib import Path


def run(cmd, cwd=None, check=True):
    result = subprocess.run(cmd, cwd=cwd, check=False)
    if check and result.returncode != 0:
        raise RuntimeError(f"Command failed: {' '.join(cmd)}")
    return result.returncode


def run_series(label, repo_root, out_dir, runs, sleep_seconds):
    label_dir = out_dir / label
    if label_dir.exists():
        shutil.rmtree(label_dir)
    label_dir.mkdir(parents=True, exist_ok=True)

    for i in range(1, runs + 1):
        run_dir = label_dir / f"run_{i:02d}"
        run_dir.mkdir(parents=True, exist_ok=True)
        log_path = run_dir / "run.log"
        with log_path.open("w") as log_file:
            subprocess.run(["bash", "run.sh"], cwd=repo_root / "profix",
                           stdout=log_file, stderr=subprocess.STDOUT, check=False)
        time.sleep(sleep_seconds)
        send_path = repo_root / "profix" / "logs" / "send_0"
        recv_path = repo_root / "profix" / "logs" / "recv_0"
        if send_path.exists():
            shutil.copy2(send_path, run_dir / "send_0")
        if recv_path.exists():
            shutil.copy2(recv_path, run_dir / "recv_0")


def ensure_build_dir(repo_root):
    out_t = repo_root / "out" / "t"
    build_ninja = out_t / "build.ninja"
    if not build_ninja.exists():
        raise RuntimeError("Missing out/t/build.ninja. Run `gn gen out/t` first.")
    return out_t


def main():
    parser = argparse.ArgumentParser(
        description="A/B compare using existing baseline logs.")
    parser.add_argument("--runs", type=int, default=2)
    parser.add_argument("--sleep", type=int, default=1)
    parser.add_argument("--out", required=True, help="output directory")
    parser.add_argument("--latency-offset", type=int, default=1000)
    parser.add_argument("--build", action="store_true",
                        help="run ninja -C out/t before each series")
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parents[1]
    out_dir = Path(args.out).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    out_t = ensure_build_dir(repo_root)
    print(f"Running current version ({args.runs} runs)")
    if args.build:
        run(["ninja", "-C", str(out_t)], check=False)
    run_series("current", repo_root, out_dir, args.runs, args.sleep)

    baseline_dir = repo_root / "profix" / "cases" / "case_2" / "baseline_logs"
    if not baseline_dir.exists():
        raise RuntimeError(f"Missing baseline logs at: {baseline_dir}")

    analyze = repo_root / "profix" / "analyze_prfl_frames.py"
    run([
        sys.executable,
        str(analyze),
        "--input", f"baseline={baseline_dir}",
        "--input", f"current={out_dir / 'current'}",
        "--out", str(out_dir),
        "--latency-offset", str(args.latency_offset),
    ])

    print(f"OUT_DIR={out_dir}")


if __name__ == "__main__":
    main()
