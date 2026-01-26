# Simple Compare Workflow

This workflow runs a small A/B test between the current working tree and a
baseline (clean tree) without switching branches. It uses `git stash` to
temporarily hide local changes and ensures the same execution path is used
for both versions.

## What It Measures
- Per-frame latency using `Prfl_frame_send@<rtp_ts>` (send) and
  `Prfl_frame_recv@<rtp_ts>` (recv).
- Stall gaps: any recv gap > 100ms is treated as a stall.
- Aggregated metrics (stall count, stall duration, stall rate, latency CDF).

## Run

```bash
bash profix/run_simple_compare.sh
```

Optional parameters:

```bash
bash profix/run_simple_compare.sh --runs 2 --sleep 5 --out profix/analysis/simple_compare
```

## Outputs
- `profix/analysis/simple_compare/summary.csv`
- `profix/analysis/simple_compare/summary.json`
- `profix/analysis/simple_compare/per_run_metrics.csv`
- `profix/analysis/simple_compare/cdf_latency.png`
- `profix/analysis/simple_compare/cdf_stall_gap.png`
- `profix/analysis/simple_compare/compare_metrics.png`
- Raw logs for each run under `profix/analysis/simple_compare/{baseline|current}/run_XX/`

## Notes
- The script runs **current** first, then stashes changes to run **baseline**
  on a clean tree, and finally restores your changes.
- If `profix/run.sh` exits non-zero due to file copy warnings, the script
  still captures `send_0` and `recv_0` for analysis.
