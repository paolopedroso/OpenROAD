# gcd_asap7 investigation driver: ablation ladder A/B/C/D plus
# partition-without-fence sanity check + Task 5 softer-fence variant.
# Env vars:
#   RTA_INV_MODE  = baseline | cutsize | static | rta | rta_nofence | rta_soft
#   RTA_INV_SEED  = integer (passed to triton_part_design -seed; ignored for baseline)
#
# All non-baseline modes share the same fence geometry generator
# (rta_apply_partition.tcl); the only thing that changes is which slack
# signal (if any) drives the partition and whether fences are strict.
#
# baseline    -> A; no partition, no fences
# cutsize     -> B; partition with -timing_aware_flag false; fences applied
# static      -> C; partition with -timing_aware_flag true, RTA off; fences applied
# rta         -> D; partition with -retiming_aware_flag true (Pan default); fences applied
# rta_nofence -> sanity; same as D but fences NOT applied (unused partition file)
# rta_soft    -> Task 5; same as D but with overlap_factor=1.5 (soft fences)

source "helpers.tcl"
source "flow_helpers.tcl"
source "asap7/asap7.vars"

set design "gcd"
set top_module "gcd"
set synth_verilog "gcd_asap7.v"
set sdc_file "gcd_asap7.sdc"
set die_area {0 0 16.2 16.2}
set core_area {1.08 1.08 15.12 15.12}

if { ![info exists ::env(RTA_INV_MODE)] } {
  puts "ERROR: set RTA_INV_MODE=baseline|cutsize|static|rta|rta_nofence"
  exit 2
}
set mode $::env(RTA_INV_MODE)
set valid_modes {baseline cutsize static rta rta_nofence rta_soft}
if { $mode ni $valid_modes } {
  puts "ERROR: RTA_INV_MODE must be one of: $valid_modes (got '$mode')"
  exit 2
}
set seed 1
if { [info exists ::env(RTA_INV_SEED)] } {
  set seed $::env(RTA_INV_SEED)
}
puts "BENCH MODE=$mode SEED=$seed"
set bench_start [clock milliseconds]

# --- Read tech + design ---------------------------------------------------
read_libraries
read_verilog $synth_verilog
link_design $top_module
read_sdc $sdc_file

# --- Partition (every non-baseline mode) ---------------------------------
set num_parts 2
set part_file [make_result_file gcd_${mode}_s${seed}.part]
if { $mode ne "baseline" } {
  set timing_aware "true"
  set rta_flag "false"
  if { $mode eq "cutsize" } {
    set timing_aware "false"
  }
  if { $mode in {rta rta_nofence rta_soft} } {
    set rta_flag "true"
  }
  set t_part_start [clock milliseconds]
  triton_part_design                                \
      -num_parts $num_parts                         \
      -balance_constraint 5.0                       \
      -seed $seed                                   \
      -timing_aware_flag $timing_aware              \
      -retiming_aware_flag $rta_flag                \
      -solution_file $part_file
  set t_part_end [clock milliseconds]
  puts "BENCH MODE=$mode SEED=$seed PARTITION_WALL_MS=[expr {$t_part_end - $t_part_start}]"
  puts "BENCH MODE=$mode SEED=$seed PARTITION_FILE=$part_file"
}

# --- Floorplan ------------------------------------------------------------
initialize_floorplan -site $site -die_area $die_area -core_area $core_area
source $tracks_file
remove_buffers

# --- Apply fences (B, C, D, rta_soft — NOT rta_nofence sanity) -----------
if { $mode ni {baseline rta_nofence} } {
  source rta_apply_partition.tcl
  set overlap 1.0
  if { $mode eq "rta_soft" } { set overlap 1.5 }
  set t_f_start [clock milliseconds]
  set assigned [rta_apply_partition $part_file $num_parts $overlap]
  set t_f_end [clock milliseconds]
  puts "BENCH MODE=$mode SEED=$seed FENCE_WALL_MS=[expr {$t_f_end - $t_f_start}]"
  puts "BENCH MODE=$mode SEED=$seed FENCE_INSTS_ASSIGNED=$assigned"
}

# --- Pre-placement: tapcell, PDN -----------------------------------------
eval tapcell $tapcell_args ;# tclint-disable command-args
source $pdn_cfg
pdngen

# --- Global placement -----------------------------------------------------
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
puts "BENCH MODE=$mode SEED=$seed GPL_WALL_MS=[expr {$t_gpl_end - $t_gpl_start}]"

# --- Post-GPL timing ------------------------------------------------------
source $layer_rc_file
set_wire_rc -signal -layer $wire_rc_layer
set_wire_rc -clock -layer $wire_rc_layer_clk
set_dont_use $dont_use
estimate_parasitics -placement
set wns_post_gpl [sta::worst_slack -max]
set tns_post_gpl [sta::total_negative_slack -max]
puts "BENCH MODE=$mode SEED=$seed WNS_POST_GPL=$wns_post_gpl"
puts "BENCH MODE=$mode SEED=$seed TNS_POST_GPL=$tns_post_gpl"
puts "BENCH MODE=$mode SEED=$seed DESIGN_AREA=[rsz::design_area]"
puts "BENCH MODE=$mode SEED=$seed UTILIZATION=[rsz::utilization]"

set bench_end [clock milliseconds]
puts "BENCH MODE=$mode SEED=$seed TOTAL_WALL_MS=[expr {$bench_end - $bench_start}]"
exit 0
