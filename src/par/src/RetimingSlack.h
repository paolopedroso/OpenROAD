// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#pragma once

#include <limits>
#include <string>
#include <vector>

namespace par::rta {

// A directed combinational/sequential graph used by the retiming-aware
// sequential-slack engine. Nodes 0..num_nodes-1 are combinational gates with
// propagation delay node_delay[v] >= 0. Each edge (u, v) carries a
// non-negative integer register count `regs` (the number of flip-flops on the
// connection u -> v). The graph need not be acyclic, but every directed cycle
// must contain at least one register; a register-free cycle has infinite
// clock period and is rejected by Validate().
struct RetimingGraph
{
  int num_nodes = 0;
  std::vector<double> node_delay;
  struct Edge
  {
    int u = 0;
    int v = 0;
    int regs = 0;
    // Wire (interconnect) delay for this engine edge, in the same time
    // unit as node_delay. Adds to the path's combinational delay just like
    // node_delay does, so retiming sees physically meaningful slack when
    // the adapter sources real values from OpenSTA (instead of the
    // uniform-0 default used by tests).
    double wire_delay = 0.0;
  };
  std::vector<Edge> edges;
  // Anchored nodes: r*(v) is pinned to 0 (no retiming). Use this for primary
  // inputs, primary outputs, or any other circuit boundary that must not
  // shift in time. Without anchors a feed-forward subgraph can be
  // "pipelined for free" by lowering r at the source — mathematically legal
  // in Leiserson-Saxe but physically meaningless for partitioning. When
  // is_anchor is empty (size 0), no anchoring is applied; otherwise it must
  // have size num_nodes.
  std::vector<bool> is_anchor;

  // Returns true if num_nodes/node_delay/edges/is_anchor are mutually
  // consistent and there is no register-free directed cycle. On false, *err
  // (if non-null) is filled with a short human-readable reason.
  bool Validate(std::string* err = nullptr) const;
};

// Auxiliary quantities from Leiserson-Saxe Algorithm WD:
//   W[u][v] = minimum register count over any directed path u -> v
//   D[u][v] = maximum total delay over those register-minimal u -> v paths
// Diagonal: W[v][v] = 0, D[v][v] = node_delay[v]. Unreachable pairs are
// marked W == kInfRegs and D == kNegInfDelay.
struct WDResult
{
  static constexpr int kInfRegs = std::numeric_limits<int>::max() / 4;
  static constexpr double kNegInfDelay
      = -std::numeric_limits<double>::infinity();
  std::vector<std::vector<int>> W;
  std::vector<std::vector<double>> D;
};

WDResult ComputeWD(const RetimingGraph& g);

// Difference-constraint feasibility test (Leiserson-Saxe Theorem 7).
// Returns true iff some legal retiming r* achieves clock period T. On true,
// *retiming_out (if non-null) is filled with one such integer r*. Uses
// Bellman-Ford on the dual constraint graph; rejects on negative cycle.
bool IsFeasiblePeriod(const RetimingGraph& g,
                      const WDResult& wd,
                      double T,
                      std::vector<int>* retiming_out = nullptr);

// Minimum achievable clock period over all legal retimings (Phi_opt). Binary
// search over the sorted distinct finite D[u][v] values (the optimal period
// is always one of these per Leiserson-Saxe). Returns -1 if no finite period
// is feasible (e.g. a register-free cycle slipped past Validate).
double ComputeMinFeasiblePeriod(const RetimingGraph& g, const WDResult& wd);

// Pan c-retiming CTCHECK feasibility (Pan, ICCD 1997, Theorem 1 / Fig 2-3).
// Returns true iff phi is achievable, i.e. s(PO) <= 1 for every anchored
// node under the longest-path relaxation
//   s(v) = max over edges u->v of { s(u) - regs + (node_delay[v] + wire_delay)/phi }
// with s(PI) = 0 and s(other) = -inf. If feasible and pan_s_out is non-null,
// it is populated with the converged c-retiming potentials s; the integer
// retiming r* by Theorem 3 rounding is r(v) = ceil(s(v)) - 1 for internal
// nodes and 0 for anchored nodes.
bool PanCtcheck(const RetimingGraph& g,
                double phi,
                std::vector<double>* pan_s_out = nullptr);

// Minimum feasible clock period via real-valued binary search over phi,
// using PanCtcheck as the feasibility oracle. tol is the absolute tolerance
// on phi. Returns -1 if no finite phi is feasible.
double ComputeMinFeasiblePeriodPan(const RetimingGraph& g, double tol = 1e-6);

// Pan Theorem-3 rounding from c-retiming potentials s to integer r*.
//   r(v) = 0                if g.is_anchor[v] is true
//   r(v) = ceil(s(v)) - 1   otherwise
// The rounded retiming achieves a discrete clock period < phi + D where
// D = max node_delay.
std::vector<int> PanRoundRetiming(const RetimingGraph& g,
                                  const std::vector<double>& s);

// Which feasibility/min-period engine to use.
//   kLeisersonSaxe: Algorithm WD + Bellman-Ford difference-constraint test,
//                   O(V^3) WD computation. Integer-exact retiming. Limited
//                   to ~10k vertices in practice but useful as a cross-check
//                   oracle for Pan.
//   kPan:           c-retiming (Pan, ICCD 1997) + CTCHECK feasibility +
//                   real-valued binary search for phi_opt + Theorem-3
//                   rounding to integer r*. O(n+m) per CTCHECK pass; scales
//                   to large designs. The default for the project's flow.
enum class RetimingAlgorithm
{
  kLeisersonSaxe,
  kPan,
};

struct RetimingOptions
{
  // Target clock period at which sequential slack is reported.
  //   target_period > 0: use that value directly (must be feasible).
  //   target_period <= 0: compute Phi_opt and use it as T.
  double target_period = -1.0;
  // Comparison fuzz when testing D[u][v] > T. Lex-order on (regs, -delay)
  // makes the WD answers numerically clean for integer/rational inputs.
  double tolerance = 1e-12;
  // Which engine to use. Defaults to the scalable Pan c-retiming path.
  RetimingAlgorithm algorithm = RetimingAlgorithm::kPan;
};

struct RetimingSlackResult
{
  bool feasible = false;       // false if target_period was infeasible
  double period_used = 0.0;    // T at which arrivals/required/slack hold
  double phi_opt = 0.0;        // computed when target_period <= 0
  bool phi_opt_valid = false;
  std::vector<int> retiming;   // r*(v); empty if !feasible
  std::vector<double> arrival;   // a(v) at node OUTPUT; empty if !feasible
  std::vector<double> required;  // req(v) at node OUTPUT
  std::vector<double> edge_slack;  // per RetimingGraph::edges entry, in order
};

// End-to-end: compute a feasible retiming for T, apply it, run sequential STA
// at T on the retimed graph, and report per-edge sequential slack. The
// defining property: a net that an optimal retiming can relieve shows large
// sequential slack even when its (fixed-register) static slack is small.
RetimingSlackResult ComputeSequentialSlack(const RetimingGraph& g,
                                           const RetimingOptions& opts);

// Apply a user-supplied retiming (must be legal, i.e. w_r(e) >= 0 on every
// edge) at period T and report arrivals/required/edge slacks. Used both
// internally by ComputeSequentialSlack and by callers that want static slack
// (retiming = all zeros) without re-running the WD/BF machinery.
RetimingSlackResult ComputeSlackAtRetiming(const RetimingGraph& g,
                                           const std::vector<int>& retiming,
                                           double T);

}  // namespace par::rta
