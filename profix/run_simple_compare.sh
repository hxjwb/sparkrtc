#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
RUNS=2
SLEEP_SECONDS=5
OUT_DIR="$ROOT_DIR/profix/analysis/simple_compare"

usage() {
  echo "Usage: $0 [--runs N] [--sleep S] [--out /path]"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --runs)
      RUNS="$2"
      shift 2
      ;;
    --sleep)
      SLEEP_SECONDS="$2"
      shift 2
      ;;
    --out)
      OUT_DIR="$2"
      shift 2
      ;;
    *)
      usage
      exit 1
      ;;
  esac
done

mkdir -p "$OUT_DIR/current" "$OUT_DIR/baseline"

run_series() {
  local label="$1"
  local repo_root="$2"
  local i run_dir

  rm -rf "$OUT_DIR/$label"
  mkdir -p "$OUT_DIR/$label"

  for i in $(seq 1 "$RUNS"); do
    run_dir="$OUT_DIR/$label/run_$(printf '%02d' "$i")"
    mkdir -p "$run_dir"
    (cd "$repo_root/profix" && bash run.sh >"$run_dir/run.log" 2>&1) || true
    sleep "$SLEEP_SECONDS"
    cp "$repo_root/profix/logs/send_0" "$run_dir/send_0" 2>/dev/null || true
    cp "$repo_root/profix/logs/recv_0" "$run_dir/recv_0" 2>/dev/null || true
  done
}

echo "Running current version ($RUNS runs)"
run_series "current" "$ROOT_DIR"

echo "Stashing changes for baseline"
git -C "$ROOT_DIR" stash push -u -m "tmp-simple-compare"

echo "Building baseline (clean tree)"
ninja -C "$ROOT_DIR/out/t"

echo "Running baseline version ($RUNS runs)"
run_series "baseline" "$ROOT_DIR"

echo "Restoring current changes"
git -C "$ROOT_DIR" stash pop
ninja -C "$ROOT_DIR/out/t"

python3 "$ROOT_DIR/profix/analyze_prfl_frames.py" \
  --input "baseline=$OUT_DIR/baseline" \
  --input "current=$OUT_DIR/current" \
  --out "$OUT_DIR" \
  --latency-offset 1000
