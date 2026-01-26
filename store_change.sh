#!/usr/bin/env bash
set -euo pipefail

if [ $# -ne 1 ]; then
  echo "Usage: bash store_change.sh <time>"
  exit 1
fi

time_tag="$1"
branch="agent/case_1_fix_${time_tag}"

# Create archive branch, commit current changes, then return to base.
git checkout -b "$branch"
git add -A
git commit -m "archive changes ${time_tag}"
git checkout agent_base_0
