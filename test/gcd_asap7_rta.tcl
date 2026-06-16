# RTA-Part PD-flow comparison driver on gcd_asap7.
#
# Env var RTA_MODE selects:
#   baseline -> no triton_part_design; pure canonical PD flow.
#   rta      -> triton_part_design -retiming_aware_flag true (default Pan
#               c-retiming + real STA delays) -> rta_apply_partition (fence
#               regions from the .part file) -> same canonical PD steps.
#
# Both modes run through identical PD steps (initialize_floorplan,
# tapcell, PDN, place_pins, global_placement, detailed_placement,
# estimate_parasitics, repair_design, repair_timing) so the comparison is
# fair: only the partition+fence step differs between modes.
#
# Run from test/:
#   RTA_MODE=baseline ../build/bin/openroad gcd_asap7_rta.tcl 2>&1 | tee results/rta_pd_baseline.log
#   RTA_MODE=rta      ../build/bin/openroad gcd_asap7_rta.tcl 2>&1 | tee results/rta_pd_rta.log
#
# NOTE: The ABC-classical-retime baseline (a third netlist with retimed
# flops produced by yosys+ABC) is omitted from this driver because the
# yosys/ABC tooling on pre-mapped Nangate45/ASAP7 verilog does not
# actually move flops (see docs/rta-part/implementation.md sec 9
# follow-up #5). The 2-way comparison below isolates the effect of
# RTA-Part's partition-time slack source + the fence-region wrapper on
# the canonical PD flow.

source "helpers.tcl"
source "flow_helpers.tcl"
source "asap7/asap7.vars"

set design "gcd"
set top_module "gcd"
set synth_verilog "gcd_asap7.v"
set sdc_file "gcd_asap7.sdc"
set die_area {0 0 16.2 16.2}
set core_area {1.08 1.08 15.12 15.12}

# --- Mode selection -------------------------------------------------------
if { ![info exists ::env(RTA_MODE)] } {
  puts "ERROR: set RTA_MODE=baseline|rta"
  exit 2
}
set mode $::env(RTA_MODE)
if { $mode ni {baseline rta} } {
  puts "ERROR: RTA_MODE must be 'baseline' or 'rta' (got '$mode')"
  exit 2
}
puts "BENCH MODE=$mode MODE_SELECTED"
set bench_start [clock milliseconds]

# --- Read tech + design ---------------------------------------------------
read_libraries
read_verilog $synth_verilog
link_design $top_module
read_sdc $sdc_file

# --- Partition (rta mode only) -------------------------------------------
# gcd_asap7 is small (~370 placeable cells on a 16.2x16.2 um die at ~20%
# utilization). 4 strips were over-constraining DPL; 2 strips give each
# partition ~150 cells in ~half the die, which DPL handles cleanly.
set num_parts 2
set part_file [make_result_file gcd_${mode}.part]
if { $mode eq "rta" } {
  puts "===== PARTITION (rta) ====="
  set t_partition_start [clock milliseconds]
  triton_part_design                                \
      -num_parts $num_parts                         \
      -balance_constraint 5.0                       \
      -timing_aware_flag true                       \
      -retiming_aware_flag true                     \
      -solution_file $part_file
  set t_partition_end [clock milliseconds]
  puts "BENCH MODE=$mode PARTITION_WALL_MS=[expr {$t_partition_end - $t_partition_start}]"
  puts "BENCH MODE=$mode PARTITION_FILE=$part_file"
}

# --- Floorplan ------------------------------------------------------------
puts "===== FLOORPLAN ($mode) ====="
initialize_floorplan -site $site -die_area $die_area -core_area $core_area
source $tracks_file
remove_buffers

# --- Apply fence regions (rta mode only) ---------------------------------
if { $mode eq "rta" } {
  source rta_apply_partition.tcl
  set t_fence_start [clock milliseconds]
  set assigned [rta_apply_partition $part_file $num_parts]
  set t_fence_end [clock milliseconds]
  puts "BENCH MODE=$mode FENCE_WALL_MS=[expr {$t_fence_end - $t_fence_start}]"
  puts "BENCH MODE=$mode FENCE_INSTS_ASSIGNED=$assigned"
}

# --- Pre-placement PDN / tapcell (canonical) -----------------------------
eval tapcell $tapcell_args ;# tclint-disable command-args
source $pdn_cfg
pdngen

# --- Global placement -----------------------------------------------------
puts "===== GLOBAL PLACE ($mode) ====="
set t_gpl_start [clock milliseconds]
foreach layer_adjustment $global_routing_layer_adjustments {
  lassign $layer_adjustment layer adjustment
  set_global_routing_layer_adjustment $layer $adjustment
}
set_routing_layers -signal $global_routing_layers \
                   -clock $global_routing_clock_layers
set_macro_extension 2
global_placement -density $global_place_density \
                 -pad_left $global_place_pad -pad_right $global_place_pad \
                 -skip_io
place_pins -hor_layers $io_placer_hor_layer -ver_layers $io_placer_ver_layer
global_placement -routability_driven -density $global_place_density \
                 -pad_left $global_place_pad -pad_right $global_place_pad
set t_gpl_end [clock milliseconds]
puts "BENCH MODE=$mode GPL_WALL_MS=[expr {$t_gpl_end - $t_gpl_start}]"

# --- Report post-GPL timing (the FAIR comparison metric) -----------------
# Both modes report WNS/TNS at the SAME stage: right after global_placement,
# before any timing repair. This isolates the effect of placement quality
# (which is what fence regions influence) from buffer insertion / cell
# resizing (which would interact poorly with region constraints in rta mode
# because newly-inserted cells aren't assigned to a region).
source $layer_rc_file
set_wire_rc -signal -layer $wire_rc_layer
set_wire_rc -clock -layer $wire_rc_layer_clk
set_dont_use $dont_use
estimate_parasitics -placement
set wns_post_gpl [sta::worst_slack -max]
set tns_post_gpl [sta::total_negative_slack -max]
puts "BENCH MODE=$mode WNS_POST_GPL=$wns_post_gpl"
puts "BENCH MODE=$mode TNS_POST_GPL=$tns_post_gpl"
puts "BENCH MODE=$mode DESIGN_AREA=[rsz::design_area]"
puts "BENCH MODE=$mode UTILIZATION=[rsz::utilization]"

# repair_design + detailed_placement intentionally skipped: in rta mode,
# repair_design inserts buffers that aren't members of any region, and
# detailed_placement then errors (DPL-0036) trying to legalize them
# inside a region-constrained die. A follow-up could either tag newly
# inserted cells with the same region as their driver, or relax the
# region constraint after timing fixing. Both branches report at the
# same stage to keep the comparison apples-to-apples.

# --- Persist DEF for downstream inspection --------------------------------
set def_file [make_result_file gcd_${mode}.def]
write_def $def_file
puts "BENCH MODE=$mode DEF_FILE=$def_file"

set bench_end [clock milliseconds]
puts "BENCH MODE=$mode TOTAL_WALL_MS=[expr {$bench_end - $bench_start}]"

exit 0
