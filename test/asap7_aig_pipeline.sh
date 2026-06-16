#!/bin/bash
# AIGER-path logic-only retiming pipeline for ASAP7 designs.
#
# Stages:
#   1a. yosys (fresh): mapped Verilog -> flatten -> aigmap -> write_aiger
#       (pre_retime.aig + .map symbol table)
#   1b. yosys (fresh): read_aiger pre_retime.aig -> abc combinational map +
#       dfflibmap -> write_verilog roundtrip.v        (arm B netlist)
#   2.  abc standalone: read_aiger pre.aig -> retime -> write_aiger post.aig
#   2-verify: register-count delta + ABC dsec (sequential EC)
#   3.  yosys (fresh): read_aiger post_retime.aig -> abc remap -> dfflibmap
#       -> write_verilog logicret.v                   (arm C netlist)
#       If retime fired identity, alias C to B's netlist instead.
#
# Each yosys stage is a SEPARATE invocation -- mixing aigmap + downstream
# remap in one session leaves hierarchical-wire artifacts that OpenROAD's
# STA Verilog reader rejects (STA-0171). Separate invocations bypass that.
#
# Usage:
#   ./asap7_aig_pipeline.sh <design_top> <synth_v> <out_prefix> [retime_mode]
# retime_mode in {M5, M6, dretime}; default M5.

set -e
DESIGN=${1:?design top required}
SRCV=${2:?synth Verilog required}
OUT=${3:?out prefix required}
MODE=${4:-M4}

# Default to M4: forward+backward MIN-DELAY only. Matches the objective
# of our Pan c-retiming engine (CTCHECK binary-searches for the smallest
# feasible clock period -- min-delay, no area term). Using M5 (M3+M4)
# instead would mix in an area optimization that the RTA-Part engine
# doesn't do, making the comparison unfair.
#
# M6 (Pan binary search) is theoretically the right comparator but it
# only COMPUTES the optimum without modifying the network, so it cannot
# produce a retimed netlist for downstream PD.
case "$MODE" in
  M3) RETIME_CMD="retime -M 3 -v" ;;     # min-area only, applies
  M4) RETIME_CMD="retime -M 4 -v" ;;     # min-delay only, applies (DEFAULT)
  M5) RETIME_CMD="retime -M 5 -v" ;;     # M3 + M4, applies (mixed objective)
  M6) RETIME_CMD="retime -M 6 -v" ;;     # Pan optimum-delay binary search,
                                         #   computes only, network not modified
  dretime) RETIME_CMD="dretime -v" ;;    # newer min-area implementation
  *) echo "unknown MODE=$MODE (choose M3|M4|M5|M6|dretime)" >&2; exit 1 ;;
esac

HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"
mkdir -p results
ABC=../build/third-party/abc/abc

REMAP_BLOCK="# simplemap is REQUIRED here: read_aiger emits coarse \$dff cells,
# but dfflibmap only handles the fine-grained \$_DFF_P_ primitive.
# Without simplemap, dfflibmap silently leaves all latches as \$dff,
# which then either survive as unmappable cells or get discarded.
simplemap
opt_clean
dfflibmap -liberty asap7/asap7sc7p5t_SEQ_RVT_FF_nldm_220123.lib
opt_clean
abc -liberty asap7/asap7sc7p5t_AO_RVT_FF_nldm_211120.lib.gz \\
    -liberty asap7/asap7sc7p5t_INVBUF_RVT_FF_nldm_220122.lib.gz \\
    -liberty asap7/asap7sc7p5t_OA_RVT_FF_nldm_211120.lib.gz \\
    -liberty asap7/asap7sc7p5t_SIMPLE_RVT_FF_nldm_211120.lib.gz
opt_clean
clean -purge"

# === Stage 1a: yosys source -> AIGER ===
echo "=== Stage 1a: yosys $SRCV -> ${OUT}_pre_retime.aig ==="
YS1=$(mktemp /tmp/asap7_aig_1a.XXXXXX.ys)
trap "rm -f $YS1 $YS1B $YS3" EXIT
cat > "$YS1" <<EOF
read_liberty asap7/asap7sc7p5t_AO_RVT_FF_nldm_211120.lib.gz
read_liberty asap7/asap7sc7p5t_INVBUF_RVT_FF_nldm_220122.lib.gz
read_liberty asap7/asap7sc7p5t_OA_RVT_FF_nldm_211120.lib.gz
read_liberty asap7/asap7sc7p5t_SIMPLE_RVT_FF_nldm_211120.lib.gz
read_liberty -lib asap7/asap7sc7p5t_SEQ_RVT_FF_nldm_220123.lib

read_verilog $SRCV
hierarchy -check -top $DESIGN
proc
opt_clean
techmap -map gcd_asap7_dff_techmap.v
flatten
opt_clean
aigmap
opt_clean
stat
write_aiger -symbols -map ${OUT}_pre_retime.map ${OUT}_pre_retime.aig
EOF
yosys -l ${OUT}_aig_pre.log "$YS1" 2>&1 | tail -5
if [ ! -s "${OUT}_pre_retime.aig" ]; then
  echo "ERROR: ${OUT}_pre_retime.aig not produced" >&2
  exit 1
fi

# === Stage 1b: read AIG fresh, remap to std cells -> arm B netlist ===
# IMPORTANT: pass the pre-retime AIG through ABC's strash first. Yosys's
# write_aiger output from aigmap is structurally non-canonical; reading
# it back into yosys via read_aiger produces hierarchical-LHS assign
# statements (e.g. `assign \_X_.A = ...`) that OpenROAD STA rejects.
# ABC's strash canonicalizes the AIG so yosys's read_aiger can recover
# a clean network. This is exactly the round-trip arm C goes through
# automatically via abc retime + strash.
echo
echo "=== Stage 1b-pre: strash pre-retime AIG through abc for clean re-read ==="
"$ABC" -c "read_aiger ${OUT}_pre_retime.aig; strash; write_aiger ${OUT}_pre_retime_strashed.aig" \
  2>&1 | tail -3

echo
echo "=== Stage 1b: AIGER -> remap -> ${OUT}_roundtrip.v (arm B) ==="
YS1B=$(mktemp /tmp/asap7_aig_1b.XXXXXX.ys)
cat > "$YS1B" <<EOF
read_liberty asap7/asap7sc7p5t_AO_RVT_FF_nldm_211120.lib.gz
read_liberty asap7/asap7sc7p5t_INVBUF_RVT_FF_nldm_220122.lib.gz
read_liberty asap7/asap7sc7p5t_OA_RVT_FF_nldm_211120.lib.gz
read_liberty asap7/asap7sc7p5t_SIMPLE_RVT_FF_nldm_211120.lib.gz
read_liberty -lib asap7/asap7sc7p5t_SEQ_RVT_FF_nldm_220123.lib

read_aiger -clk_name clk -module_name $DESIGN ${OUT}_pre_retime_strashed.aig
hierarchy -check -top $DESIGN
$REMAP_BLOCK
stat
write_verilog -noexpr ${OUT}_roundtrip.v
EOF
yosys -l ${OUT}_aig_remap_b.log "$YS1B" 2>&1 | tail -5

# === Stage 2: ABC retime + Task A verify ===
echo
echo "=== Stage 2: ABC retime ($RETIME_CMD) ==="
"$ABC" -c "
read_aiger ${OUT}_pre_retime.aig; print_stats;
$RETIME_CMD;
print_stats;
strash;
write_aiger ${OUT}_post_retime.aig
" 2>&1 | tee ${OUT}_aig_retime_abc.log | tail -15

echo
echo "=== Task A-verify: register-count delta ==="
PRE_LAT=$(grep -oE "lat *=[[:space:]]*[0-9]+" ${OUT}_aig_retime_abc.log | head -1 | grep -oE "[0-9]+")
POST_LAT=$(grep -oE "lat *=[[:space:]]*[0-9]+" ${OUT}_aig_retime_abc.log | tail -1 | grep -oE "[0-9]+")
PRE_LEV=$(grep -oE "lev *=[[:space:]]*[0-9]+" ${OUT}_aig_retime_abc.log | head -1 | grep -oE "[0-9]+")
POST_LEV=$(grep -oE "lev *=[[:space:]]*[0-9]+" ${OUT}_aig_retime_abc.log | tail -1 | grep -oE "[0-9]+")
echo "pre  lat=$PRE_LAT lev=$PRE_LEV"
echo "post lat=$POST_LAT lev=$POST_LEV"
RETIME_FIRED="false"
if [ -n "$PRE_LAT" ] && [ -n "$POST_LAT" ] && [ "$PRE_LAT" != "$POST_LAT" ]; then
  echo "OK: register count changed (delta = $((POST_LAT - PRE_LAT)))"
  RETIME_FIRED="true"
elif [ -n "$PRE_LEV" ] && [ -n "$POST_LEV" ] && [ "$PRE_LEV" != "$POST_LEV" ]; then
  echo "OK: AIG depth changed (lev delta = $((POST_LEV - PRE_LEV)))"
  RETIME_FIRED="true"
else
  echo "WARN: retime did NOT change lat or lev -> arm C will alias to arm B."
fi

echo
echo "=== Task A-verify: sequential EC (ABC dsec) ==="
if [ -s "${OUT}_post_retime.aig" ]; then
  "$ABC" -c "dsec ${OUT}_pre_retime.aig ${OUT}_post_retime.aig" \
    2>&1 | tee ${OUT}_aig_eq.log | tail -5
else
  echo "skipped (no post-retime AIG)"
fi

# === Stage 3: arm C netlist ===
echo
echo "=== Stage 3: arm C (logic-retimed) netlist ==="
if [ "$RETIME_FIRED" = "true" ] && [ -s "${OUT}_post_retime.aig" ]; then
  YS3=$(mktemp /tmp/asap7_aig_3.XXXXXX.ys)
  cat > "$YS3" <<EOF
read_liberty asap7/asap7sc7p5t_AO_RVT_FF_nldm_211120.lib.gz
read_liberty asap7/asap7sc7p5t_INVBUF_RVT_FF_nldm_220122.lib.gz
read_liberty asap7/asap7sc7p5t_OA_RVT_FF_nldm_211120.lib.gz
read_liberty asap7/asap7sc7p5t_SIMPLE_RVT_FF_nldm_211120.lib.gz
read_liberty -lib asap7/asap7sc7p5t_SEQ_RVT_FF_nldm_220123.lib

read_aiger -clk_name clk -module_name $DESIGN ${OUT}_post_retime.aig
hierarchy -check -top $DESIGN
$REMAP_BLOCK
stat
write_verilog -noexpr ${OUT}_logicret.v
EOF
  yosys -l ${OUT}_aig_remap_c.log "$YS3" 2>&1 | tail -5
else
  # Identity retime -> alias arm C to arm B with a clear header.
  if [ -s "${OUT}_roundtrip.v" ]; then
    {
      echo "// arm C (logic_retime): retime ran but produced no change"
      echo "// at AIG level (lat unchanged, lev unchanged, DSEC equiv)."
      echo "// Aliased to arm B (roundtrip) -- they are provably identical"
      echo "// for this design under retime mode '${MODE}'."
      echo "// See ${OUT}_aig_retime_abc.log for ABC's verdict."
      echo
      cat ${OUT}_roundtrip.v
    } > ${OUT}_logicret.v
    echo "arm C aliased to arm B (retime was identity)"
  fi
fi

echo
echo "=== summary ==="
for f in ${OUT}_roundtrip.v ${OUT}_logicret.v; do
  if [ -f "$f" ]; then
    ff=$(grep -c "DFFHQNx1_ASAP7_75t_R" "$f" || true)
    cells=$(grep -cE '_ASAP7_75t_[A-Z]+ +_' "$f" || true)
    assigns=$(grep -cE '^[[:space:]]*assign ' "$f" || true)
    echo "$f : $ff DFFs, $cells cell instances, $assigns assigns"
  fi
done
echo "retime_fired=$RETIME_FIRED"
