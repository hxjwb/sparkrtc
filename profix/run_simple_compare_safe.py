#!/usr/bin/env python3
import argparse
import os
import shutil
import subprocess
import sys
import time
from datetime import datetime
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


def main():
    parser = argparse.ArgumentParser(
        description="Safe A/B compare without git stash (uses worktree).")
    parser.add_argument("--runs", type=int, default=2)
    parser.add_argument("--sleep", type=int, default=5)
    parser.add_argument("--out", required=True, help="output directory")
    parser.add_argument("--latency-offset", type=int, default=1000)
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parents[1]
    out_dir = Path(args.out).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    baseline_worktree = Path(
        f"/tmp/sparkrtc_baseline_{datetime.now().strftime('%Y%m%d_%H%M%S')}"
    )

    try:
        print(f"Running current version ({args.runs} runs)")
        run_series("current", repo_root, out_dir, args.runs, args.sleep)

        print("Creating baseline worktree (clean tree)")
        run(["git", "-C", str(repo_root), "worktree", "add",
             "--detach", str(baseline_worktree), "HEAD"])

        print("Building baseline")
        run(["ninja", "-C", str(baseline_worktree / "out" / "t")], check=False)

        print(f"Running baseline version ({args.runs} runs)")
        run_series("baseline", baseline_worktree, out_dir, args.runs, args.sleep)
    finally:
        if baseline_worktree.exists():
            run(["git", "-C", str(repo_root), "worktree", "remove", "-f",
                 str(baseline_worktree)], check=False)

    analyze = repo_root / "profix" / "analyze_prfl_frames.py"
    run([
        sys.executable,
        str(analyze),
        "--input", f"baseline={out_dir / 'baseline'}",
        "--input", f"current={out_dir / 'current'}",
        "--out", str(out_dir),
        "--latency-offset", str(args.latency_offset),
    ])

    print(f"OUT_DIR={out_dir}")


if __name__ == "__main__":
    main()
