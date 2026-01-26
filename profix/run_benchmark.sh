#!/usr/bin/env bash
set -u

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BASELINE_DIR=""
RUNS=10
SLEEP_SECONDS=5
OUT_DIR="$ROOT_DIR/profix/analysis"

usage() {
  echo "Usage: $0 --baseline-dir /path/to/baseline [--runs N] [--sleep S] [--out /path]"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --baseline-dir)
      BASELINE_DIR="$2"
      shift 2
      ;;
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

if [[ -z "$BASELINE_DIR" ]]; then
  usage
  exit 1
fi

mkdir -p "$OUT_DIR/runs"

sync_baseline_profix() {
  echo "Syncing profix scripts into baseline."
  mkdir -p "$BASELINE_DIR/profix"
  cp -a "$ROOT_DIR/profix/." "$BASELINE_DIR/profix/"
  if [[ -f "$ROOT_DIR/.gclient" ]]; then
    cp -a "$ROOT_DIR/.gclient" "$BASELINE_DIR/.gclient"
  fi
  if [[ -f "$ROOT_DIR/.gclient_entries" ]]; then
    cp -a "$ROOT_DIR/.gclient_entries" "$BASELINE_DIR/.gclient_entries"
  fi
}

ensure_build() {
  local repo_root="$1"
  if [[ ! -f "$repo_root/out/t/args.gn" ]]; then
    echo "Generating build directory in $repo_root/out/t"
    mkdir -p "$repo_root/out/t"
    if [[ -f "$ROOT_DIR/out/t/args.gn" ]]; then
      cp -a "$ROOT_DIR/out/t/args.gn" "$repo_root/out/t/args.gn"
    fi
    (cd "$repo_root" && gn gen out/t)
  fi
  if [[ ! -f "$repo_root/out/t/build.ninja" ]]; then
    echo "ERROR: build.ninja not found in $repo_root/out/t"
    exit 1
  fi
  echo "Building $repo_root/out/t"
  (cd "$repo_root" && ninja -C out/t)
}

run_series() {
  local label="$1"
  local repo_root="$2"
  local i run_dir

  echo "Running $label series from $repo_root"
  rm -rf "$OUT_DIR/runs/$label"
  for i in $(seq 1 "$RUNS"); do
    run_dir="$OUT_DIR/runs/$label/run_$(printf '%02d' "$i")"
    mkdir -p "$run_dir"

    echo "[$label] Run $i/$RUNS"
    (cd "$repo_root/profix" && bash run.sh >"$run_dir/run.log" 2>&1) || true
    sleep "$SLEEP_SECONDS"

    if [[ -f "$repo_root/profix/logs/send_0" ]]; then
      cp "$repo_root/profix/logs/send_0" "$run_dir/send_0"
    fi
    if [[ -f "$repo_root/profix/logs/recv_0" ]]; then
      cp "$repo_root/profix/logs/recv_0" "$run_dir/recv_0"
    fi
  done
}

sync_baseline_profix
ensure_build "$BASELINE_DIR"
ensure_build "$ROOT_DIR"
run_series "baseline" "$BASELINE_DIR"
run_series "current" "$ROOT_DIR"

python3 "$ROOT_DIR/profix/analyze_prfl_frames.py" \
  --input "baseline=$OUT_DIR/runs/baseline" \
  --input "current=$OUT_DIR/runs/current" \
  --out "$OUT_DIR"
