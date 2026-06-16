#!/bin/bash
# Multi-arm sweep runner. Calls gcd_asap7_bench.tcl with arm x seed
# combinations, emits a CSV. Usage:
#   ./rta_bench_sweep.sh <driver.tcl> <openroad_bin> <out.csv> \
#       "arm1 arm2..." "seed1 seed2..."
# Example:
#   ./rta_bench_sweep.sh gcd_asap7_bench.tcl ../build/bin/openroad \
#       results/bench.csv "a b c d" "1 2 3 4 5"
#
# Arms a/b/c are deterministic (no seed effect since no partition); we
# still run them once per nominal seed to keep the table rectangular.

set -e
DRIVER=$1
OPENROAD=$2
OUT=$3
ARMS=$4
SEEDS=$5

if [ -z "$DRIVER" ] || [ -z "$OPENROAD" ] || [ -z "$OUT" ] || [ -z "$ARMS" ] || [ -z "$SEEDS" ]; then
  echo "usage: $0 <driver.tcl> <openroad_bin> <out.csv> \"arms\" \"seeds\""
  exit 1
fi

HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"
mkdir -p results
echo "arm,seed,wns_post_gpl,tns_post_gpl,ff_count,partition_wall_ms,fence_wall_ms,gpl_wall_ms,total_wall_ms" > "$OUT"

for arm in $ARMS; do
  # arms a/b/c are deterministic -- only seed=1 matters; skip duplicate runs
  if [ "$arm" = "a" ] || [ "$arm" = "b" ] || [ "$arm" = "c" ]; then
    iter_seeds="1"
  else
    iter_seeds="$SEEDS"
  fi
  for seed in $iter_seeds; do
    log="results/bench_${arm}_s${seed}.log"
    echo "--- arm=$arm seed=$seed ---" >&2
    BENCH_ARM="$arm" BENCH_SEED="$seed" "$OPENROAD" "$DRIVER" > "$log" 2>&1 || {
      echo "WARN: openroad exit nonzero arm=$arm seed=$seed; see $log" >&2
    }
    wns=$(grep -oE "WNS_POST_GPL=[-0-9.e+]+" "$log" | tail -1 | sed 's/.*=//')
    tns=$(grep -oE "TNS_POST_GPL=[-0-9.e+]+" "$log" | tail -1 | sed 's/.*=//')
    ff=$(grep -oE "FF_COUNT=[0-9]+" "$log" | tail -1 | sed 's/.*=//')
    pwall=$(grep -oE "PARTITION_WALL_MS=[0-9]+" "$log" | tail -1 | sed 's/.*=//')
    fwall=$(grep -oE "FENCE_WALL_MS=[0-9]+" "$log" | tail -1 | sed 's/.*=//')
    gwall=$(grep -oE "GPL_WALL_MS=[0-9]+" "$log" | tail -1 | sed 's/.*=//')
    twall=$(grep -oE "TOTAL_WALL_MS=[0-9]+" "$log" | tail -1 | sed 's/.*=//')
    echo "$arm,$seed,${wns:-NA},${tns:-NA},${ff:-NA},${pwall:-0},${fwall:-0},${gwall:-NA},${twall:-NA}" >> "$OUT"
    echo "  arm=$arm seed=$seed wns=$wns tns=$tns ff=$ff gpl=$gwall ms" >&2
  done
done

echo "CSV: $OUT" >&2
echo
column -t -s, "$OUT" 2>/dev/null || cat "$OUT"
