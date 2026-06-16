# RTA-Part PD-flow comparison driver on aes_asap7.
#
# Same env-driven 2-way comparison as gcd_asap7_rta.tcl, but on a much
# larger design (~15k cells on a 68.7x68.7 um die at ASAP7). The bigger
# placement freedom is where partition-driven fence regions are more
# likely to be helpful rather than constraining.
#
# Env var RTA_MODE selects:
#   baseline -> no triton_part_design; pure canonical PD flow.
#   rta      -> triton_part_design -retiming_aware_flag true (default Pan
#               c-retiming + real STA delays) -> rta_apply_partition.
#
# Both modes report WNS/TNS at the same stage (post-GPL, before any
# timing repair) so the comparison is apples-to-apples.
#
# Run from test/:
#   RTA_MODE=baseline ../build/bin/openroad aes_asap7_rta.tcl 2>&1 | tee results/rta_aes_baseline.log
#   RTA_MODE=rta      ../build/bin/openroad aes_asap7_rta.tcl 2>&1 | tee results/rta_aes_rta.log

source "helpers.tcl"
source "flow_helpers.tcl"
source "asap7/asap7.vars"

set design "aes"
set top_module "aes_cipher_top"
set synth_verilog "aes_asap7.v"
set sdc_file "aes_asap7.sdc"
set die_area {0.0 0.0 68.746 68.746}
set core_area {2.052 2.160 66.744 66.690}

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
# aes_asap7 is ~15k cells; 4 vertical strips give each partition ~3.75k
# cells over ~17 um of die width -- plenty of placement freedom inside each
# strip, and 4 is a typical multi-die / floorplan-quadrant count.
set num_parts 4
set part_file [make_result_file aes_${mode}.part]
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

# --- Pre-placement: tapcell, macro placement if any, PDN -----------------
if { [have_macros] } {
  lassign $macro_place_halo halo_x halo_y
  set_macro_base_halo $halo_x $halo_y
  set report_dir [make_result_file ${design}_${platform}_rtlmp_${mode}]
  rtl_macro_placer -report_directory $report_dir
}
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

# --- Report post-GPL timing (the fair comparison stage) ------------------
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

# --- Persist DEF for downstream inspection --------------------------------
set def_file [make_result_file aes_${mode}.def]
write_def $def_file
puts "BENCH MODE=$mode DEF_FILE=$def_file"

set bench_end [clock milliseconds]
puts "BENCH MODE=$mode TOTAL_WALL_MS=[expr {$bench_end - $bench_start}]"

exit 0
