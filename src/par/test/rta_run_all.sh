#!/bin/bash
# Orchestrator for the RTA-Part bench suite. Run from src/par/test/.
#
#   ./rta_run_all.sh
#
# 1. rta_bench.tcl       -- partition-only comparison (back-to-back static + RTA).
# 2. rta_pd_flow.tcl x2  -- full PD chain (partition -> place -> WNS) per mode.
# 3. rta_diff.sh         -- partition file diff (vertex reassignments).
# 4. summary             -- pulls BENCH lines out of the logs and prints a table.

set -e

HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

OPENROAD=${OPENROAD:-../../../build/bin/openroad}
if [ ! -x "$OPENROAD" ]; then
  echo "ERROR: $OPENROAD not found or not executable"
  echo "Set OPENROAD=/path/to/openroad if your build lives elsewhere."
  exit 1
fi

mkdir -p results
PART_LOG=results/rta_bench.log
STATIC_LOG=results/rta_pd_static.log
RTA_LOG=results/rta_pd_rta.log

echo "===== [1/3] Partition-only bench (back-to-back) ====="
"$OPENROAD" rta_bench.tcl 2>&1 | tee "$PART_LOG"

echo
echo "===== [2/3] PD-flow comparison ====="
echo "--- static mode ---"
RTA_MODE=static "$OPENROAD" rta_pd_flow.tcl 2>&1 | tee "$STATIC_LOG"
echo "--- rta mode ---"
RTA_MODE=rta    "$OPENROAD" rta_pd_flow.tcl 2>&1 | tee "$RTA_LOG"

echo
echo "===== [3/3] Partition file diff ====="
STATIC_PART=$(grep -E '^BENCH STATIC_FILE=' "$PART_LOG" | tail -1 | sed 's/.*=//')
RTA_PART=$(grep    -E '^BENCH RTA_FILE='    "$PART_LOG" | tail -1 | sed 's/.*=//')
./rta_diff.sh "$STATIC_PART" "$RTA_PART"

echo
echo "===== SUMMARY ====="
printf "%-18s  %-14s  %-14s\n" "metric" "static" "rta"
extract() {
  local key=$1 log=$2 mode_filter=$3
  if [ -n "$mode_filter" ]; then
    grep -E "BENCH MODE=$mode_filter $key=" "$log" | tail -1 | sed 's/.*=//'
  else
    grep -E "BENCH $key=" "$log" | tail -1 | sed 's/.*=//'
  fi
}
row() {
  local label=$1 key=$2 sval=$3 rval=$4
  printf "%-18s  %-14s  %-14s\n" "$label" "$sval" "$rval"
}
row "partition_wall_ms" "PARTITION_WALL_MS" \
    "$(extract STATIC_WALL_MS "$PART_LOG")" \
    "$(extract RTA_WALL_MS    "$PART_LOG")"
row "gpl_wall_ms"       "GPL_WALL_MS"       \
    "$(extract GPL_WALL_MS "$STATIC_LOG" static)" \
    "$(extract GPL_WALL_MS "$RTA_LOG"    rta)"
row "dpl_wall_ms"       "DPL_WALL_MS"       \
    "$(extract DPL_WALL_MS "$STATIC_LOG" static)" \
    "$(extract DPL_WALL_MS "$RTA_LOG"    rta)"
row "wns_post_gpl"      "WNS_POST_GPL"      \
    "$(extract WNS_POST_GPL "$STATIC_LOG" static)" \
    "$(extract WNS_POST_GPL "$RTA_LOG"    rta)"
row "tns_post_gpl"      "TNS_POST_GPL"      \
    "$(extract TNS_POST_GPL "$STATIC_LOG" static)" \
    "$(extract TNS_POST_GPL "$RTA_LOG"    rta)"
row "wns_post_dpl"      "WNS_POST_DPL"      \
    "$(extract WNS_POST_DPL "$STATIC_LOG" static)" \
    "$(extract WNS_POST_DPL "$RTA_LOG"    rta)"
row "tns_post_dpl"      "TNS_POST_DPL"      \
    "$(extract TNS_POST_DPL "$STATIC_LOG" static)" \
    "$(extract TNS_POST_DPL "$RTA_LOG"    rta)"
echo
echo "RTA log line check:"
grep -E 'PAR 41|RTA-Part: sequential' "$RTA_LOG" | head -3 || \
  echo "  (no PAR 41 line found in RTA log; flag may not have fired)"
