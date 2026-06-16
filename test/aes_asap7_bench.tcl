# Multi-arm PD harness for aes_asap7 (mirror of gcd_asap7_bench.tcl).
# aes is ~15k cells on a 68.7x68.7 um die at ~38% utilization -- a more
# realistic stress test for the partition-driven fence approach than the
# tiny gcd. Same env-controlled arm + seed semantics.
#
# Env vars:
#   BENCH_ARM  = a | b | c | d | e
#     a baseline           original mapped netlist, no retime, no RTA
#     b roundtrip_control  AIGER round-trip Verilog, no retime, no RTA
#     c logic_retime       ABC-retimed Verilog (or arm B aliased when
#                          retime fires identity -- see header in the
#                          .v file produced by asap7_aig_pipeline.sh)
#     d rta_part           original netlist + triton_part_design RTA +
#                          fences (NOT a netlist retiming -- partition-
#                          and-place guidance only)
#     e phys_retime        STUB / TODO: apply r* to the netlist (next
#                          phase). This driver exits non-zero on this arm.
#   BENCH_SEED = integer (default 1); only matters for arm d
#
# Run from test/:
#   BENCH_ARM=a BENCH_SEED=1 ../build/bin/openroad aes_asap7_bench.tcl \
#       2>&1 | tee results/bench_aes_a_s1.log

source "helpers.tcl"
source "flow_helpers.tcl"
source "asap7/asap7.vars"

set design "aes"
set top_module "aes_cipher_top"
set sdc_file "aes_asap7.sdc"
set die_area {0.0 0.0 68.746 68.746}
set core_area {2.052 2.160 66.744 66.690}

if { ![info exists ::env(BENCH_ARM)] } {
  puts "ERROR: set BENCH_ARM=a|b|c|d|e"
  exit 2
}
set arm $::env(BENCH_ARM)
set seed 1
if { [info exists ::env(BENCH_SEED)] } {
  set seed $::env(BENCH_SEED)
}

set arm_label "?"
set arm_synth_v "?"
set use_rta 0
switch -- $arm {
  a {
    set arm_label "baseline"
    set arm_synth_v "aes_asap7.v"
  }
  b {
    set arm_label "roundtrip_control"
    set arm_synth_v "results/aes_roundtrip.v"
  }
  c {
    set arm_label "logic_retime"
    set arm_synth_v "results/aes_logicret.v"
  }
  d {
    set arm_label "rta_part"
    set arm_synth_v "aes_asap7.v"
    set use_rta 1
  }
  e {
    puts "ARM=e (phys_retime) is a STUB. Apply-r* netlist mutation"
    puts "is pending the next phase. Skipped."
    exit 3
  }
  default {
    puts "ERROR: BENCH_ARM must be a|b|c|d|e (got '$arm')"
    exit 2
  }
}
puts "BENCH ARM=$arm LABEL=$arm_label SEED=$seed SYNTH_V=$arm_synth_v"
set t_bench_start [clock milliseconds]

if { ![file exists $arm_synth_v] } {
  puts "ERROR: synth verilog not found: $arm_synth_v"
  puts "       (run ./asap7_aig_pipeline.sh aes_cipher_top aes_asap7.v results/aes M6 for b/c)"
  exit 4
}

# --- Read tech + design ---------------------------------------------------
read_libraries
read_verilog $arm_synth_v
link_design $top_module
read_sdc $sdc_file

# --- Partition + fences (arm d only) -------------------------------------
# aes is large enough that 4 strips give plenty of placement room
# (~3.75k cells per strip on a 17 um wide strip).
set num_parts 4
set part_file [make_result_file aes_bench_${arm}_s${seed}.part]
if { $use_rta } {
  set t_part_start [clock milliseconds]
  triton_part_design                                \
      -num_parts $num_parts                         \
      -balance_constraint 5.0                       \
      -seed $seed                                   \
      -timing_aware_flag true                       \
      -retiming_aware_flag true                     \
      -retiming_algorithm pan                       \
      -solution_file $part_file
  set t_part_end [clock milliseconds]
  puts "BENCH ARM=$arm SEED=$seed PARTITION_WALL_MS=[expr {$t_part_end - $t_part_start}]"
}

# --- Floorplan ------------------------------------------------------------
initialize_floorplan -site $site -die_area $die_area -core_area $core_area
source $tracks_file
remove_buffers

# --- Fences (arm d only) -------------------------------------------------
if { $use_rta } {
  source rta_apply_partition.tcl
  set t_f_start [clock milliseconds]
  set assigned [rta_apply_partition $part_file $num_parts]
  set t_f_end [clock milliseconds]
  puts "BENCH ARM=$arm SEED=$seed FENCE_WALL_MS=[expr {$t_f_end - $t_f_start}]"
  puts "BENCH ARM=$arm SEED=$seed FENCE_INSTS_ASSIGNED=$assigned"
}

# --- Macro placement (aes may have macros) + tapcell + PDN ---------------
if { [have_macros] } {
  lassign $macro_place_halo halo_x halo_y
  set_macro_base_halo $halo_x $halo_y
  set report_dir [make_result_file aes_${arm}_s${seed}_rtlmp]
  rtl_macro_placer -report_directory $report_dir
}
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
puts "BENCH ARM=$arm SEED=$seed GPL_WALL_MS=[expr {$t_gpl_end - $t_gpl_start}]"

# --- Post-GPL timing (fair common stage) ---------------------------------
source $layer_rc_file
set_wire_rc -signal -layer $wire_rc_layer
set_wire_rc -clock -layer $wire_rc_layer_clk
set_dont_use $dont_use
estimate_parasitics -placement
set wns [sta::worst_slack -max]
set tns [sta::total_negative_slack -max]
puts "BENCH ARM=$arm SEED=$seed WNS_POST_GPL=$wns"
puts "BENCH ARM=$arm SEED=$seed TNS_POST_GPL=$tns"
puts "BENCH ARM=$arm SEED=$seed DESIGN_AREA=[rsz::design_area]"
puts "BENCH ARM=$arm SEED=$seed UTILIZATION=[rsz::utilization]"

# --- FF count (informational; should change if/when arm E lands) --------
set block [::ord::get_db_block]
set ff_count 0
foreach inst [$block getInsts] {
  set master_name [[$inst getMaster] getName]
  if { [string match "*DFF*" $master_name] || [string match "*SDFF*" $master_name] } {
    incr ff_count
  }
}
puts "BENCH ARM=$arm SEED=$seed FF_COUNT=$ff_count"

set t_bench_end [clock milliseconds]
puts "BENCH ARM=$arm SEED=$seed TOTAL_WALL_MS=[expr {$t_bench_end - $t_bench_start}]"
exit 0
