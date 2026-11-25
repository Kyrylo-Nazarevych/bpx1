#!/bin/bash
# Mirror this project folder (on the Windows side) into the Linux file system, where all tools run.
#
#   From Windows:  wsl.exe -d Ubuntu --cd "<this project folder>" -- bash tools/wsl_sync.sh
#
# The Windows folder is the source of truth. The mirror is ~/work/bpx1. Generated data (benchmark
# builds, traces, simulation and LibreLane runs) lives outside git: ~/work/bpx1/out, ~/work/bpx1/runs.
set -e
SRC="$(cd "$(dirname "$0")/.." && pwd)"
DST="$HOME/work/bpx1"
mkdir -p "$DST"
rsync -a --delete \
  --exclude '/out/' --exclude '/runs/' --exclude '/tt' --exclude '/tt_submission/' \
  --exclude 'sim_build/' --exclude '__pycache__/' --exclude 'obj_dir/' --exclude '/build/' \
  --exclude '/test/gate_level_netlist.v' --exclude '/test/results.xml' --exclude '*.fst' \
  --exclude '/src/user_config.json' --exclude '/src/config_merged.json' \
  "$SRC"/ "$DST"/
# Tiny Tapeout support tools: shared checkout (branch main, as the CI uses), never committed here.
if [ ! -e "$DST/tt" ] && [ -d "$HOME/work/tt-branch-predictor/tt" ]; then
  ln -s "$HOME/work/tt-branch-predictor/tt" "$DST/tt"
fi
mkdir -p "$DST/out"
echo "synced -> $DST ($(find "$DST" -path "$DST/out" -prune -o -path "$DST/runs" -prune -o -path "$DST/.git" -prune -o -type f -print | wc -l) files)"
