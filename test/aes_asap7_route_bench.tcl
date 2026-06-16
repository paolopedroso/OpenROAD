# aes_asap7_route_bench.tcl
#
# End-to-end PD harness for the RTA-Part Phase 0+1 validation. Extends the
# post-GPL bench past the DPL-0036 wall to post-route, with the
# drop-fences-before-repair_design fix. Both arms start from the SAME
# properly-synthesized netlist (the arm-B AIG-roundtrip + abc remap output)
# so the comparison is uncontaminated by synthesis-quality differences.
#
# Env vars:
#   BENCH_ARM  = baseline | rta
#     baseline  same canonical PD chain, no triton_part_design, no fences
#     rta       canonical PD chain + triton_part_design + fences during GPL,
#               fences DROPPED before repair_design (Task 0.3 fix)
#   BENCH_SEED = integer (default 1); only matters for the rta arm
#   BENCH_STOP_AT = (optional) gpl|repair|dpl|cts|repair2|grt|drt (default drt)
#                   Lets a long-running detailed-route be skipped if needed.
#
# Run from test/:
#   BENCH_ARM=baseline ../build/bin/openroad -no_init -no_splash \
#       aes_asap7_route_bench.tcl > results/route_aes_baseline.log 2>&1
#   BENCH_ARM=rta BENCH_SEED=1 ../build/bin/openroad -no_init -no_splash \
#       aes_asap7_route_bench.tcl > results/route_aes_rta_s1.log 2>&1

source "helpers.tcl"
source "flow_helpers.tcl"
source "asap7/asap7.vars"

set design "aes"
set top_module "aes_cipher_top"
set sdc_file "aes_asap7.sdc"
set die_area {0.0 0.0 68.746 68.746}
set core_area {2.052 2.160 66.744 66.690}

# Both arms read THE SAME properly-synthesized netlist (arm-B output:
# AIG roundtrip + abc -liberty asap7 combinational remap, NO retime).
# Using the raw yosys netlist (aes_asap7.v) would contaminate the
# comparison -- arm B alone is +1021 ps WNS vs that baseline.
set arm_synth_v "results/aes_roundtrip.v"

if { ![info exists ::env(BENCH_ARM)] } {
  puts "ERROR: set BENCH_ARM=baseline|rta"
  exit 2
}
set arm $::env(BENCH_ARM)
set seed 1
if { [info exists ::env(BENCH_SEED)] } {
  set seed $::env(BENCH_SEED)
}
set stop_at "drt"
if { [info exists ::env(BENCH_STOP_AT)] } {
  set stop_at $::env(BENCH_STOP_AT)
}

set use_rta 0
switch -- $arm {
  baseline { set use_rta 0 }
  rta      { set use_rta 1 }
  default {
    puts "ERROR: BENCH_ARM must be baseline|rta (got '$arm')"
    exit 2
  }
}
puts "BENCH ARM=$arm SEED=$seed SYNTH_V=$arm_synth_v STOP_AT=$stop_at"
set t_bench_start [clock milliseconds]

if { ![file exists $arm_synth_v] } {
  puts "ERROR: synth verilog not found: $arm_synth_v"
  puts "       (regenerate with ./asap7_aig_pipeline.sh aes_cipher_top aes_asap7.v results/aes M4)"
  exit 4
}

# --- Helper: emit one metrics line for a stage ---------------------------
proc bench_metrics { stage arm seed } {
  set wns [sta::worst_slack -max]
  set tns [sta::total_negative_slack -max]
  set wns_min [sta::worst_slack -min]
  set area [rsz::design_area]
  set util [rsz::utilization]
  set block [::ord::get_db_block]
  set ff_count 0
  set inst_count 0
  foreach inst [$block getInsts] {
    incr inst_count
    set master_name [[$inst getMaster] getName]
    if { [string match "*DFF*" $master_name] || [string match "*SDFF*" $master_name] } {
      incr ff_count
    }
  }
  puts "BENCH ARM=$arm SEED=$seed STAGE=$stage WNS_MAX=$wns TNS_MAX=$tns WNS_MIN=$wns_min AREA=$area UTIL=$util INST_COUNT=$inst_count FF_COUNT=$ff_count"
}

# --- Read tech + design ---------------------------------------------------
read_libraries
read_verilog $arm_synth_v
link_design $top_module
read_sdc $sdc_file

# --- Partition (rta arm only) --------------------------------------------
set num_parts 4
set part_file [make_result_file aes_route_${arm}_s${seed}.part]
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

# --- Apply fences (rta arm only) -----------------------------------------
if { $use_rta } {
  source rta_apply_partition.tcl
  set t_f_start [clock milliseconds]
  set assigned [rta_apply_partition $part_file $num_parts]
  set t_f_end [clock milliseconds]
  puts "BENCH ARM=$arm SEED=$seed FENCE_WALL_MS=[expr {$t_f_end - $t_f_start}]"
  puts "BENCH ARM=$arm SEED=$seed FENCE_INSTS_ASSIGNED=$assigned"
}

# --- Macros + tapcell + PDN ----------------------------------------------
if { [have_macros] } {
  lassign $macro_place_halo halo_x halo_y
  set_macro_base_halo $halo_x $halo_y
  set report_dir [make_result_file aes_${arm}_s${seed}_route_rtlmp]
  rtl_macro_placer -report_directory $report_dir
}
eval tapcell $tapcell_args ;# tclint-disable command-args
source $pdn_cfg
pdngen

# --- Global placement (honors fences if rta arm) -------------------------
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

# --- Post-GPL timing report (same as before, for trajectory comparison) --
source $layer_rc_file
set_wire_rc -signal -layer $wire_rc_layer
set_wire_rc -clock -layer $wire_rc_layer_clk
set_dont_use $dont_use
estimate_parasitics -placement
bench_metrics "post_gpl" $arm $seed

if { $stop_at eq "gpl" } { exit 0 }

# --- Task 0.3: drop fence regions before repair_design -------------------
# After GPL has clustered cells per partition, the constraint has done its
# job. Repair_design will insert buffers that belong to no region; DPL-0036
# crashes when it can't place them. Destroy the groups+regions so repair/
# DPL/CTS/route run unconstrained. The clustering survives in the GPL
# coordinates -- destroying ODB groups only drops the constraint, never
# moves the instances themselves.
if { $use_rta } {
  set t_drop_start [clock milliseconds]
  set block [::ord::get_db_block]
  # Snapshot positions of first ~5 inst per group as a displacement canary.
  array set canary_pos {}
  set canary_count 0
  foreach inst [$block getInsts] {
    if { $canary_count >= 20 } break
    set bbox [$inst getBBox]
    set canary_pos([$inst getName]) [list [$bbox xMin] [$bbox yMin]]
    incr canary_count
  }
  set dropped_groups 0
  foreach group [$block getGroups] {
    odb::dbGroup_destroy $group
    incr dropped_groups
  }
  set dropped_regions 0
  foreach region [$block getRegions] {
    odb::dbRegion_destroy $region
    incr dropped_regions
  }
  set moved 0
  foreach name [array names canary_pos] {
    set inst [$block findInst $name]
    if { $inst == "NULL" } continue
    set bbox [$inst getBBox]
    lassign $canary_pos($name) px py
    if { [$bbox xMin] != $px || [$bbox yMin] != $py } { incr moved }
  }
  set t_drop_end [clock milliseconds]
  puts "BENCH ARM=$arm SEED=$seed FENCE_DROP_MS=[expr {$t_drop_end - $t_drop_start}] dropped_groups=$dropped_groups dropped_regions=$dropped_regions canary_moved=$moved/$canary_count"
}

# --- Repair design (max slew/cap/fanout) ---------------------------------
set t_rd_start [clock milliseconds]
repair_design -slew_margin $slew_margin -cap_margin $cap_margin
repair_tie_fanout -separation $tie_separation $tielo_port
repair_tie_fanout -separation $tie_separation $tiehi_port
set_placement_padding -global -left $detail_place_pad -right $detail_place_pad
detailed_placement
set t_rd_end [clock milliseconds]
puts "BENCH ARM=$arm SEED=$seed REPAIR_DESIGN_WALL_MS=[expr {$t_rd_end - $t_rd_start}]"
bench_metrics "post_repair" $arm $seed

if { $stop_at eq "repair" || $stop_at eq "dpl" } { exit 0 }

# --- CTS ------------------------------------------------------------------
set t_cts_start [clock milliseconds]
repair_clock_inverters
clock_tree_synthesis -root_buf $cts_buffer -buf_list $cts_buffer \
                     -sink_clustering_enable \
                     -sink_clustering_max_diameter $cts_cluster_diameter
repair_clock_nets
detailed_placement
set t_cts_end [clock milliseconds]
puts "BENCH ARM=$arm SEED=$seed CTS_WALL_MS=[expr {$t_cts_end - $t_cts_start}]"
bench_metrics "post_cts" $arm $seed

if { $stop_at eq "cts" } { exit 0 }

# --- Setup/hold repair_timing (propagated clocks) ------------------------
set t_rt_start [clock milliseconds]
set_propagated_clock [all_clocks]
estimate_parasitics -placement
repair_timing -skip_gate_cloning
set t_rt_end [clock milliseconds]
puts "BENCH ARM=$arm SEED=$seed REPAIR_TIMING_WALL_MS=[expr {$t_rt_end - $t_rt_start}]"
detailed_placement
bench_metrics "post_repair_timing" $arm $seed

if { $stop_at eq "repair2" } { exit 0 }

# --- Global route ---------------------------------------------------------
set t_grt_start [clock milliseconds]
pin_access
set route_guide [make_result_file aes_${arm}_s${seed}_route.guide]
global_route -guide_file $route_guide \
             -congestion_iterations 100 -verbose
set t_grt_end [clock milliseconds]
puts "BENCH ARM=$arm SEED=$seed GRT_WALL_MS=[expr {$t_grt_end - $t_grt_start}]"
estimate_parasitics -global_routing
bench_metrics "post_grt" $arm $seed

if { $stop_at eq "grt" } { exit 0 }

# --- Detailed route ------------------------------------------------------
set t_drt_start [clock milliseconds]
detailed_route -output_drc [make_result_file aes_${arm}_s${seed}_route_drc.rpt] \
               -output_maze [make_result_file aes_${arm}_s${seed}_maze.log] \
               -no_pin_access \
               -verbose 0
set t_drt_end [clock milliseconds]
set drv_count [detailed_route_num_drvs]
puts "BENCH ARM=$arm SEED=$seed DRT_WALL_MS=[expr {$t_drt_end - $t_drt_start}] DRV=$drv_count"
bench_metrics "post_drt" $arm $seed

# --- Wirelength (post-DRT) -----------------------------------------------
set block [::ord::get_db_block]
set total_wl 0
foreach net [$block getNets] {
  set wire [$net getWire]
  if { $wire != "NULL" } {
    set total_wl [expr {$total_wl + [$wire getLength]}]
  }
}
puts "BENCH ARM=$arm SEED=$seed WIRELENGTH_DBU=$total_wl"

set t_bench_end [clock milliseconds]
puts "BENCH ARM=$arm SEED=$seed TOTAL_WALL_MS=[expr {$t_bench_end - $t_bench_start}]"
exit 0
