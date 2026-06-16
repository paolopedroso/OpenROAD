# rta_apply_partition.tcl
#
# Convert a TritonPart .part file into ODB regions+groups so downstream
# placement (gpl::global_placement, dpl::detailed_placement) respects the
# partition decisions. Without this, the partition file is informational
# only -- it does NOT influence placement and WNS/TNS/area cannot differ
# between RTA and non-RTA runs of the same canonical flow.
#
# Strategy: split the die into N equal-width vertical strips (one per
# partition_id) and create one dbRegion + dbGroup per strip. Each
# instance from the .part file is assigned to its partition's group; gpl
# / dpl honor the region's bounding box during placement.
#
# Usage from a flow TCL (after triton_part_design produces $part_file,
# before global_placement):
#   rta_apply_partition $part_file $num_parts
#
# Bterms (primary I/Os) are NOT assigned to regions -- they're handled
# separately by place_pins, which already constrains them to the die
# boundary's perimeter.

proc rta_apply_partition { part_file num_parts } {
  if { ![file exists $part_file] } {
    puts "rta_apply_partition: ERROR: partition file not found: $part_file"
    return 0
  }
  set block [::ord::get_db_block]
  if { $block == "NULL" } {
    puts "rta_apply_partition: ERROR: no design block loaded"
    return 0
  }
  set die [$block getDieArea]
  set lx [$die xMin]
  set ly [$die yMin]
  set ux [$die xMax]
  set uy [$die yMax]
  set width [expr {$ux - $lx}]
  if { $num_parts <= 0 } {
    puts "rta_apply_partition: ERROR: num_parts must be > 0 (got $num_parts)"
    return 0
  }
  set strip_w [expr {$width / $num_parts}]
  puts "rta_apply_partition: die [list $lx $ly $ux $uy] DBU,\
        $num_parts strips of width $strip_w DBU each"

  # Parse the partition file. Each line: "<inst_or_bterm_name> <block_id>"
  # (block_id is the LAST whitespace-separated token; names can contain
  # internal whitespace via brackets like req_msg[0]).
  set fd [open $part_file r]
  array set inst_to_part {}
  while {[gets $fd line] >= 0} {
    set toks [regexp -all -inline {\S+} $line]
    if {[llength $toks] < 2} continue
    set name [join [lrange $toks 0 end-1] " "]
    set bid  [lindex $toks end]
    set inst_to_part($name) $bid
  }
  close $fd
  set parsed [array size inst_to_part]
  puts "rta_apply_partition: parsed $parsed entries from $part_file"

  # Create one dbRegion + dbGroup per partition_id, each spanning a
  # vertical strip of the die.
  array set part_to_group {}
  for {set i 0} {$i < $num_parts} {incr i} {
    set rname "rta_region_$i"
    set region [odb::dbRegion_create $block $rname]
    if { $region == "NULL" } {
      # Region with this name already exists (e.g. re-run in same DB);
      # find and reuse it. NOTE: existing boxes / group bindings remain.
      set region [$block findRegion $rname]
      if { $region == "NULL" } {
        puts "rta_apply_partition: WARN: could not create or find $rname"
        continue
      }
    } else {
      set rlx [expr {$lx + $i * $strip_w}]
      set rux [expr {$lx + ($i + 1) * $strip_w}]
      odb::dbBox_create $region $rlx $ly $rux $uy
    }
    set gname "rta_group_$i"
    set group [odb::dbGroup_create $region $gname]
    if { $group == "NULL" } {
      set group [$block findGroup $gname]
    }
    set part_to_group($i) $group
  }

  # Assign each instance to its partition's group. Bterms are silently
  # skipped (handled by place_pins separately).
  set assigned 0
  set bterm_skipped 0
  set unmatched 0
  set no_group 0
  foreach name [array names inst_to_part] {
    set inst [$block findInst $name]
    if { $inst == "NULL" } {
      # Probably a bterm (primary I/O) -- check.
      set bterm [$block findBTerm $name]
      if { $bterm == "NULL" } {
        incr unmatched
      } else {
        incr bterm_skipped
      }
      continue
    }
    set bid $inst_to_part($name)
    if { ![info exists part_to_group($bid)] } {
      incr no_group
      continue
    }
    $part_to_group($bid) addInst $inst
    incr assigned
  }
  puts "rta_apply_partition: $assigned insts assigned to regions,\
        $bterm_skipped bterms skipped,\
        $unmatched unmatched,\
        $no_group with unknown partition_id"
  return $assigned
}
