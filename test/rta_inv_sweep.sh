#!/bin/bash
# Sweep modes x seeds and emit a CSV. Usage:
#   ./rta_inv_sweep.sh <driver.tcl> <openroad_bin> <out.csv> "mode1 mode2..." "seed1 seed2..."
# Example:
#   ./rta_inv_sweep.sh gcd_asap7_inv.tcl ../build/bin/openroad \
#       results/gcd_sweep.csv "baseline cutsize static rta rta_nofence" "1 2 3 4 5"

set -e
DRIVER=$1
OPENROAD=$2
OUT=$3
MODES=$4
SEEDS=$5

if [ -z "$DRIVER" ] || [ -z "$OPENROAD" ] || [ -z "$OUT" ] || [ -z "$MODES" ] || [ -z "$SEEDS" ]; then
  echo "usage: $0 <driver.tcl> <openroad_bin> <out.csv> \"modes\" \"seeds\""
  exit 1
fi

HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"
mkdir -p results
echo "mode,seed,wns_post_gpl,tns_post_gpl,partition_wall_ms,fence_wall_ms,gpl_wall_ms,total_wall_ms" > "$OUT"

for mode in $MODES; do
  # baseline ignores seed; only run once
  if [ "$mode" = "baseline" ]; then
    seed_iter="1"
  else
    seed_iter="$SEEDS"
  fi
  for seed in $seed_iter; do
    log="results/inv_${mode}_s${seed}.log"
    echo "--- mode=$mode seed=$seed ---" >&2
    RTA_INV_MODE="$mode" RTA_INV_SEED="$seed" "$OPENROAD" "$DRIVER" > "$log" 2>&1 || {
      echo "WARN: openroad exit nonzero for mode=$mode seed=$seed; see $log" >&2
    }
    wns=$(grep -oE "WNS_POST_GPL=[-0-9.e+]+" "$log" | tail -1 | sed 's/.*=//')
    tns=$(grep -oE "TNS_POST_GPL=[-0-9.e+]+" "$log" | tail -1 | sed 's/.*=//')
    pwall=$(grep -oE "PARTITION_WALL_MS=[0-9]+" "$log" | tail -1 | sed 's/.*=//')
    fwall=$(grep -oE "FENCE_WALL_MS=[0-9]+" "$log" | tail -1 | sed 's/.*=//')
    gwall=$(grep -oE "GPL_WALL_MS=[0-9]+" "$log" | tail -1 | sed 's/.*=//')
    twall=$(grep -oE "TOTAL_WALL_MS=[0-9]+" "$log" | tail -1 | sed 's/.*=//')
    echo "$mode,$seed,${wns:-NA},${tns:-NA},${pwall:-0},${fwall:-0},${gwall:-NA},${twall:-NA}" >> "$OUT"
    echo "  -> wns=$wns tns=$tns gpl=$gwall ms total=$twall ms" >&2
  done
done

echo "CSV written to $OUT" >&2
echo
echo "=== summary ==="
column -t -s, "$OUT" 2>/dev/null || cat "$OUT"
