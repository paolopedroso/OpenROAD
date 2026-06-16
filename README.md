# RTA-Part — Retiming-Aware Partitioning for OpenROAD

A retiming-aware extension to OpenROAD's TritonPart hypergraph partitioner.
Implements Leiserson–Saxe and Pan c-retiming algorithms as a standalone
engine, sources real per-cell and per-net delays from OpenSTA, and feeds
the resulting per-net sequential slacks to TritonPart as edge weights for
timing-driven K-way partitioning.

> This fork's root README documents the RTA-Part work on the `rta-par`
> branch. The upstream OpenROAD README is preserved at
> [OPENROAD-DOC.md](OPENROAD-DOC.md).

## What this is

A new opt-in flag on `triton_part_design`:

```tcl
triton_part_design \
    -num_parts 4 \
    -balance_constraint 5.0 \
    -seed 1 \
    -timing_aware_flag true \
    -retiming_aware_flag true   ;# new: enable RTA-Part
    -retiming_algorithm pan     ;# new: "pan" (default) or "l_s"
    -solution_file ...
```

When the flag is on, TritonPart's static-STA per-net slacks are replaced by
slacks computed under an optimal retiming hypothesis. Nets that would have
slack under retiming get higher partition cost (kept together); nets that
have slack regardless get lower cost (free to cut).

## What this is NOT

- **It is not retiming.** No flip-flop is ever moved, added, or removed.
  The engine computes where FFs *would* move (Pan's r* values) but only
  consumes that information to score the per-net sequential slack. The
  netlist is unchanged.
- **It is not physical-aware placement.** The engine produces slacks; the
  placer (gpl/RePlAce) places cells. The "physical" part is just that real
  wire delays from OpenSTA go into the slack computation.

## How it's structured

```
src/par/src/
  RetimingSlack.{h,cpp}      Standalone Leiserson-Saxe + Pan c-retiming engine.
                             No OpenROAD dependencies. ~700 lines.
  RetimingExtract.{h,cpp}    Adapter: TritonPart hypergraph -> engine graph.
  TritonPart.{cpp,h}         Path-B seam in BuildTimingPaths: harvests real
                             STA delays, calls the engine, writes back slacks.
  PartitionMgr.{cpp,h}       Threads the two new flags through the API.
  partitionmgr.tcl           Tcl interface (-retiming_aware_flag,
                             -retiming_algorithm).
  partitionmgr.i             SWIG signature update.

src/par/test/
  RetimingSlack_test.cpp     14 unit tests (8 L-S + 6 Pan).
  rta_apply_partition.tcl    Wraps a TritonPart .part file into ODB
                             dbRegion + dbGroup fences for global_placement.
  rta_bench.tcl              Reference bench driver.

test/
  asap7_aig_pipeline.sh      Logic-only retime baseline via yosys + ABC
                             (used to control for synthesis quality).
  aes_asap7_bench.tcl        Multi-arm post-GPL bench (baseline / roundtrip /
                             abc-retime / RTA).
  aes_asap7_route_bench.tcl  End-to-end PD harness through detailed_route,
                             with the drop-fences-before-repair_design fix.
  gcd_asap7_bench.tcl        Smaller equivalent for gcd_asap7.
  rta_*_sweep.sh             Multi-seed sweep runners; CSV emit.
```

## Algorithms implemented

### Leiserson–Saxe (1991)
- Algorithm WD: Floyd–Warshall over the lex pair
  `(regs, -d(u)-wire(e))`, O(V³) all-pairs.
- Algorithm OPT: binary search over distinct D values + Bellman-Ford
  feasibility on the constraint graph.
- Min-feasible-period computation.

### Pan c-retiming (ICCD 1997)
- CTCHECK: longest-path relaxation
  `s(v) = max over in-edges of (s(u) - regs + (d(v) + wire(e))/φ)`.
- Real-valued binary search on the period φ.
- Theorem 3 integer rounding: `r(v) = ⌈s(v)⌉ − 1` (anchors at 0).
- LegalizeRetiming safety net for edge cases producing `w_r(e) < 0`.
- Implemented via SPFA (Shortest-Path Faster Algorithm) on the relaxation:
  queue-based push relaxation with per-node pop-count divergence detection.
  Same fixpoint as dense Bellman-Ford, asymptotically faster on the
  feasible side.

Both engines accept real per-vertex delays (intrinsic gate delay from
`sta::TimingArc::intrinsicDelay()`) and real per-edge wire delays
(`Graph::arcDelay()` on wire arcs).

## Status & honest results

What works:
- Engine: faithful to the papers, 14 unit-test invariants hold, scales to
  ~16 k vertices in seconds with SPFA.
- Integration: clean Path-B seam in TritonPart; opt-in flag; no impact when
  off; netlist never mutated; FF count preserved end-to-end.
- Fence-drop fix for the DPL-0036 crash in the downstream PD flow.

What we learned (end-to-end aes_asap7 bench, both arms on the same
abc-remapped netlist for fair comparison):

| Stage | Baseline WNS (ps) | RTA WNS (ps) | Delta |
|---|---:|---:|---:|
| post-GPL | −2122 | −2261 | −139 (RTA worse) |
| post-repair_timing | −14.5 | −45.1 | −30.6 |
| post-DRT | −66.6 | −145.5 | **−78.9** |

The fence-region application of the partition output is the wrong consumer
for the sequential-slack signal — forcing cells into vertical strips hurts
what RePlAce's analytical placer would otherwise optimize globally. The
engine is the asset; the consumer needs to change. The exploration in
`docs/geo/` (separate branch) traces the path to feeding the slacks into
gpl's existing timing-driven net-reweighting loop instead.

## Build & run

Standard OpenROAD build (no extra dependencies). To run the multi-arm
bench from `test/`:

```bash
# Generate the controlled netlist (arm B baseline + arm C retime variant):
./asap7_aig_pipeline.sh aes_cipher_top aes_asap7.v results/aes M4

# Post-GPL bench:
BENCH_ARM=d BENCH_SEED=1 ../build/bin/openroad -no_init -no_splash \
    aes_asap7_bench.tcl > results/bench_aes_d_s1.log 2>&1

# End-to-end through detailed route:
BENCH_ARM=rta BENCH_SEED=1 ../build/bin/openroad -no_init -no_splash \
    aes_asap7_route_bench.tcl > results/route_aes_rta_s1.log 2>&1
```

## References

1. **Leiserson, C. E., & Saxe, J. B.** (1991). Retiming Synchronous
   Circuitry. *Algorithmica*, 6(1), 5–35.
   The foundational retiming paper. Algorithms WD and OPT.

2. **Pan, P.** (1997). Continuous Retiming: Algorithms and Applications.
   *Proc. IEEE International Conference on Computer Design (ICCD)*.
   Continuous-relaxation c-retiming + CTCHECK feasibility test.
   The algorithm implemented in `RetimingSlack.cpp::PanCtcheck`.

3. **Cong, J., & Lim, S. K.** (2000). Physical Planning with Retiming.
   *Proc. ICCAD*. Introduces the GEO method — geometric realization of
   retiming during placement.

4. **Cong, J., & Lim, S. K.** (2004). Retiming-Based Timing Analysis with
   an Application to Mincut-Based Global Placement. *IEEE TCAD*, 23(12).
   Full TCAD treatment of GEO; the source for the geometric-FF-placement
   idea.

5. **Bustany, I., Gasanov, E., Gupta, P., et al.** (TritonPart authors,
   various). TritonPart is the existing OpenROAD K-way hypergraph
   partitioner this work extends. See `src/par/` upstream and the OpenROAD
   docs.

## License

SPDX-License-Identifier: BSD-3-Clause. Inherits OpenROAD's license. See
[LICENSE](LICENSE).
