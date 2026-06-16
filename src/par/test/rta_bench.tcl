# RTA-Part bench: runs triton_part_design twice on gcd -- once with the
# static OpenSTA slack baseline, once with -retiming_aware_flag true -- and
# prints wall-clock for each plus paths to the two partition files for
# follow-on diffing via rta_diff.sh.
#
# Run from this directory:
#   ../../../build/bin/openroad rta_bench.tcl 2>&1 | tee results/rta_bench.log
source helpers.tcl
source flow_helpers.tcl

read_liberty Nangate45/Nangate45_typ.lib
read_lef Nangate45/Nangate45.lef
read_verilog gcd.v
link_design gcd
read_sdc gcd_nangate45.sdc

set static_part [make_result_file gcd_static.part]
set rta_part    [make_result_file gcd_rta.part]

puts "===== STATIC BASELINE ====="
set t0 [clock milliseconds]
triton_part_design -solution_file $static_part
set t1 [clock milliseconds]
puts "BENCH STATIC_WALL_MS=[expr {$t1 - $t0}]"
puts "BENCH STATIC_FILE=$static_part"

# Each triton_part_design call constructs a fresh TritonPart instance
# internally, so no per-call reset is needed.
puts "===== RTA RUN ====="
set t0 [clock milliseconds]
triton_part_design -solution_file $rta_part -retiming_aware_flag true
set t1 [clock milliseconds]
puts "BENCH RTA_WALL_MS=[expr {$t1 - $t0}]"
puts "BENCH RTA_FILE=$rta_part"

exit 0
