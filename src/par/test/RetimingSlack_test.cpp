// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// Standalone unit tests for src/par/src/RetimingSlack.{h,cpp}.
//
// No OpenROAD dependencies; build (from repo root) with:
//   g++ -std=c++17 -O0 -g -Wall -Wextra -Isrc/par/src
//       src/par/src/RetimingSlack.cpp src/par/test/RetimingSlack_test.cpp
//       -o /tmp/RetimingSlack_test
//   /tmp/RetimingSlack_test
// Each test prints "[PASS] name" or "[FAIL] name: reason"; process exits
// non-zero if any test fails.

#include "RetimingSlack.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace {

int g_pass = 0;
int g_fail = 0;
std::string g_current_test;

void StartTest(const std::string& name)
{
  g_current_test = name;
}

void FailWith(const std::string& msg)
{
  std::printf("[FAIL] %s: %s\n", g_current_test.c_str(), msg.c_str());
  ++g_fail;
}

void Pass()
{
  std::printf("[PASS] %s\n", g_current_test.c_str());
  ++g_pass;
}

// Returns true if the predicate holds; otherwise registers a failure and
// returns false so the caller can early-out.
bool Expect(bool cond, const std::string& msg)
{
  if (cond) {
    return true;
  }
  FailWith(msg);
  return false;
}

bool NearlyEqual(double a, double b, double tol = 1e-9)
{
  return std::fabs(a - b) <= tol;
}

bool ExpectNear(double a, double b, const std::string& label, double tol = 1e-9)
{
  if (NearlyEqual(a, b, tol)) {
    return true;
  }
  std::ostringstream oss;
  oss << label << ": expected ~" << b << ", got " << a;
  FailWith(oss.str());
  return false;
}

// --- Test 1: Validate rejects a register-free directed cycle ---
void Test_ValidateRejectsCombCycle()
{
  StartTest("ValidateRejectsCombCycle");
  par::rta::RetimingGraph g;
  g.num_nodes = 2;
  g.node_delay = {1.0, 1.0};
  g.edges = {{0, 1, 0}, {1, 0, 0}};
  std::string err;
  if (!Expect(!g.Validate(&err), "expected Validate to reject comb cycle"))
    return;
  if (!Expect(!err.empty(), "expected non-empty error message"))
    return;
  Pass();
}

// --- Test 2: Validate accepts a register on the cycle ---
void Test_ValidateAcceptsRegisterCycle()
{
  StartTest("ValidateAcceptsRegisterCycle");
  par::rta::RetimingGraph g;
  g.num_nodes = 2;
  g.node_delay = {1.0, 1.0};
  g.edges = {{0, 1, 0}, {1, 0, 1}};
  std::string err;
  if (!Expect(g.Validate(&err),
              std::string("Validate rejected legal graph: ") + err))
    return;
  Pass();
}

// --- Test 3: Pure combinational chain matches static expectation ---
// 3-node chain 0 -> 1 -> 2, each delay 1.0, no registers. PI=node 0,
// PO=node 2 are anchored so the engine cannot "free-pipeline" the chain
// by adding registers at the boundary. Expected:
//   phi_opt = 3.0, edge slacks all 0 at T = 3.0, all 1 at T = 4.0.
void Test_PureCombChain()
{
  StartTest("PureCombChain");
  par::rta::RetimingGraph g;
  g.num_nodes = 3;
  g.node_delay = {1.0, 1.0, 1.0};
  g.edges = {{0, 1, 0}, {1, 2, 0}};
  g.is_anchor = {true, false, true};

  par::rta::WDResult wd = par::rta::ComputeWD(g);
  if (!ExpectNear(wd.D[0][2], 3.0, "D[0][2]"))
    return;
  if (!Expect(wd.W[0][2] == 0, "W[0][2] expected 0"))
    return;

  const double phi = par::rta::ComputeMinFeasiblePeriod(g, wd);
  if (!ExpectNear(phi, 3.0, "phi_opt"))
    return;

  par::rta::RetimingOptions opts;
  opts.algorithm = par::rta::RetimingAlgorithm::kLeisersonSaxe;
  opts.target_period = 3.0;
  par::rta::RetimingSlackResult r3 = par::rta::ComputeSequentialSlack(g, opts);
  if (!Expect(r3.feasible, "T=3 should be feasible"))
    return;
  if (!ExpectNear(r3.edge_slack[0], 0.0, "edge(0,1) slack @ T=3"))
    return;
  if (!ExpectNear(r3.edge_slack[1], 0.0, "edge(1,2) slack @ T=3"))
    return;

  opts.target_period = 4.0;
  par::rta::RetimingSlackResult r4 = par::rta::ComputeSequentialSlack(g, opts);
  if (!Expect(r4.feasible, "T=4 should be feasible"))
    return;
  if (!ExpectNear(r4.edge_slack[0], 1.0, "edge(0,1) slack @ T=4"))
    return;
  if (!ExpectNear(r4.edge_slack[1], 1.0, "edge(1,2) slack @ T=4"))
    return;
  Pass();
}

// --- Test 4: Single-register feedback ring — classic retiming example ---
// 3-node ring 0 -> 1 -> 2 -> 0, each delay 1.0, register on the back-edge.
// Cycle has 1 register and total delay 3, so phi_opt = 3. At T = 3 every
// edge sits exactly on the critical path (slack 0). At T = 4 every edge has
// slack 1.
void Test_SingleRegisterRing()
{
  StartTest("SingleRegisterRing");
  par::rta::RetimingGraph g;
  g.num_nodes = 3;
  g.node_delay = {1.0, 1.0, 1.0};
  g.edges = {{0, 1, 0}, {1, 2, 0}, {2, 0, 1}};

  par::rta::WDResult wd = par::rta::ComputeWD(g);
  const double phi = par::rta::ComputeMinFeasiblePeriod(g, wd);
  if (!ExpectNear(phi, 3.0, "phi_opt"))
    return;

  par::rta::RetimingOptions opts;
  opts.algorithm = par::rta::RetimingAlgorithm::kLeisersonSaxe;
  opts.target_period = 3.0;
  par::rta::RetimingSlackResult r3 = par::rta::ComputeSequentialSlack(g, opts);
  if (!Expect(r3.feasible, "T=3 should be feasible"))
    return;
  for (std::size_t i = 0; i < g.edges.size(); ++i) {
    std::ostringstream oss;
    oss << "edge " << i << " slack @ T=3";
    if (!ExpectNear(r3.edge_slack[i], 0.0, oss.str()))
      return;
  }

  opts.target_period = 4.0;
  par::rta::RetimingSlackResult r4 = par::rta::ComputeSequentialSlack(g, opts);
  if (!Expect(r4.feasible, "T=4 should be feasible"))
    return;
  for (std::size_t i = 0; i < g.edges.size(); ++i) {
    std::ostringstream oss;
    oss << "edge " << i << " slack @ T=4";
    if (!ExpectNear(r4.edge_slack[i], 1.0, oss.str()))
      return;
  }
  Pass();
}

// --- Test 5: Defining property — retiming relieves a static-critical net ---
// 4-node ring with delays [1, 5, 1, 1] and registers [0, 0, 1, 1] on edges
// (0,1), (1,2), (2,3), (3,0). Static critical period = 7 (segment {0,1,2});
// phi_opt = 5 (limited by gate 1 with delay 5). At T = 5:
//   - static (r* = 0): all edges have slack -2 (violating);
//   - sequential: edges (2,3) and (3,0) have slack +2 — retiming has moved
//     the registers so that gate 1 sits in its own combinational region,
//     freeing the downstream nets.
void Test_DefiningProperty()
{
  StartTest("DefiningProperty");
  par::rta::RetimingGraph g;
  g.num_nodes = 4;
  g.node_delay = {1.0, 5.0, 1.0, 1.0};
  g.edges = {{0, 1, 0}, {1, 2, 0}, {2, 3, 1}, {3, 0, 1}};

  par::rta::WDResult wd = par::rta::ComputeWD(g);
  const double phi = par::rta::ComputeMinFeasiblePeriod(g, wd);
  if (!ExpectNear(phi, 5.0, "phi_opt"))
    return;

  const double T = 5.0;
  std::vector<int> zero(g.num_nodes, 0);
  par::rta::RetimingSlackResult sta_r
      = par::rta::ComputeSlackAtRetiming(g, zero, T);
  if (!Expect(sta_r.feasible, "static (r=0) should compute"))
    return;

  par::rta::RetimingOptions opts;
  opts.algorithm = par::rta::RetimingAlgorithm::kLeisersonSaxe;
  opts.target_period = T;
  par::rta::RetimingSlackResult seq_r
      = par::rta::ComputeSequentialSlack(g, opts);
  if (!Expect(seq_r.feasible, "sequential T=5 should be feasible"))
    return;

  // Defining property: edges that retiming relieves have strictly larger
  // sequential slack than static slack at the same target period.
  for (std::size_t i = 2; i <= 3; ++i) {
    std::ostringstream oss;
    oss << "seq_slack[" << i << "] (" << seq_r.edge_slack[i]
        << ") should exceed static_slack[" << i << "] ("
        << sta_r.edge_slack[i] << ")";
    if (!Expect(seq_r.edge_slack[i] > sta_r.edge_slack[i] + 1e-9, oss.str()))
      return;
  }

  // Concrete sequential values for this hand-derived case.
  if (!ExpectNear(seq_r.edge_slack[2], 2.0, "seq edge(2,3) slack @ T=5"))
    return;
  if (!ExpectNear(seq_r.edge_slack[3], 2.0, "seq edge(3,0) slack @ T=5"))
    return;
  // Static violation on every edge at T=5.
  for (std::size_t i = 0; i < g.edges.size(); ++i) {
    std::ostringstream oss;
    oss << "static edge " << i << " slack @ T=5";
    if (!ExpectNear(sta_r.edge_slack[i], -2.0, oss.str()))
      return;
  }
  Pass();
}

// --- Test 6: Infeasible target period is rejected ---
void Test_InfeasiblePeriodRejected()
{
  StartTest("InfeasiblePeriodRejected");
  par::rta::RetimingGraph g;
  g.num_nodes = 4;
  g.node_delay = {1.0, 5.0, 1.0, 1.0};
  g.edges = {{0, 1, 0}, {1, 2, 0}, {2, 3, 1}, {3, 0, 1}};

  par::rta::WDResult wd = par::rta::ComputeWD(g);
  // T = 4 is strictly below phi_opt = 5; must be infeasible.
  if (!Expect(!par::rta::IsFeasiblePeriod(g, wd, 4.0),
              "T=4 should be infeasible (< phi_opt=5)"))
    return;
  if (!Expect(par::rta::IsFeasiblePeriod(g, wd, 5.0),
              "T=5 should be feasible (= phi_opt)"))
    return;

  par::rta::RetimingOptions opts;
  opts.algorithm = par::rta::RetimingAlgorithm::kLeisersonSaxe;
  opts.target_period = 1.0;
  par::rta::RetimingSlackResult r
      = par::rta::ComputeSequentialSlack(g, opts);
  if (!Expect(!r.feasible, "ComputeSequentialSlack should report infeasible"))
    return;
  Pass();
}

// --- Test 7: Auto phi_opt path (target_period < 0) ---
void Test_AutoPhiOpt()
{
  StartTest("AutoPhiOpt");
  par::rta::RetimingGraph g;
  g.num_nodes = 3;
  g.node_delay = {1.0, 1.0, 1.0};
  g.edges = {{0, 1, 0}, {1, 2, 0}, {2, 0, 1}};

  par::rta::RetimingOptions opts;  // default target_period = -1
  opts.algorithm = par::rta::RetimingAlgorithm::kLeisersonSaxe;
  par::rta::RetimingSlackResult r = par::rta::ComputeSequentialSlack(g, opts);
  if (!Expect(r.feasible, "auto-phi_opt path should be feasible"))
    return;
  if (!Expect(r.phi_opt_valid, "phi_opt_valid should be true"))
    return;
  if (!ExpectNear(r.phi_opt, 3.0, "phi_opt"))
    return;
  if (!ExpectNear(r.period_used, 3.0, "period_used"))
    return;
  Pass();
}

// --- Test 8: Single feed-forward register, retiming moves it ---
// 3-gate feed-forward: in -> 0 -> 1 -> 2 -> out, delays [5, 2, 5], single
// register on edge (1, 2). Static critical: comb segment {0, 1} delay 7.
// phi_opt = 7 (the {2} segment is delay 5, but 0->1 is 5+2=7 and remains
// even after retiming since we have only one register; best we can do is
// split into {0, 1} (7) and {2} (5), or {0} (5) and {1, 2} (7)). So phi_opt
// = 7, but for any T >= 7 the engine should be feasible and r=0 valid.
void Test_FeedforwardSingleRegister()
{
  StartTest("FeedforwardSingleRegister");
  par::rta::RetimingGraph g;
  g.num_nodes = 3;
  g.node_delay = {5.0, 2.0, 5.0};
  g.edges = {{0, 1, 0}, {1, 2, 1}};
  g.is_anchor = {true, false, true};

  par::rta::WDResult wd = par::rta::ComputeWD(g);
  const double phi = par::rta::ComputeMinFeasiblePeriod(g, wd);
  if (!ExpectNear(phi, 7.0, "phi_opt"))
    return;

  par::rta::RetimingOptions opts;
  opts.algorithm = par::rta::RetimingAlgorithm::kLeisersonSaxe;
  opts.target_period = 7.0;
  par::rta::RetimingSlackResult r = par::rta::ComputeSequentialSlack(g, opts);
  if (!Expect(r.feasible, "T=7 should be feasible"))
    return;
  // Slacks must all be >= 0 at the feasible period.
  for (std::size_t i = 0; i < g.edges.size(); ++i) {
    std::ostringstream oss;
    oss << "edge " << i << " slack >= 0 @ T=7";
    if (!Expect(r.edge_slack[i] >= -1e-9, oss.str()))
      return;
  }
  Pass();
}

// =====================================================================
// Pan c-retiming tests (Phase 2.5 additions)
// =====================================================================

// --- Pan #1: single gate PI -> v -> PO with delay d. phi >= d iff feasible.
// Direct check of CTCHECK + binary search on the simplest possible circuit.
void Test_Pan_SingleGate()
{
  StartTest("Pan_SingleGate");
  par::rta::RetimingGraph g;
  g.num_nodes = 3;
  g.node_delay = {0.0, 4.0, 0.0};  // gate v=1 has delay 4; PI/PO are 0
  g.edges = {{0, 1, 0}, {1, 2, 0}};
  g.is_anchor = {true, false, true};

  // CTCHECK: phi = 4 must be feasible, phi = 3.9 must not.
  if (!Expect(par::rta::PanCtcheck(g, 4.0), "Pan: T=4 should be feasible"))
    return;
  if (!Expect(!par::rta::PanCtcheck(g, 3.9), "Pan: T=3.9 should be infeasible"))
    return;

  // ComputeMinFeasiblePeriodPan should land at ~4 within tolerance.
  const double phi = par::rta::ComputeMinFeasiblePeriodPan(g, 1e-3);
  if (!ExpectNear(phi, 4.0, "Pan phi_opt", 1e-2))
    return;
  Pass();
}

// --- Pan #2: anchored comb chain matches L-S (Theorem-3 within D).
void Test_Pan_AnchoredCombChain()
{
  StartTest("Pan_AnchoredCombChain");
  par::rta::RetimingGraph g;
  g.num_nodes = 3;
  g.node_delay = {1.0, 1.0, 1.0};
  g.edges = {{0, 1, 0}, {1, 2, 0}};
  g.is_anchor = {true, false, true};

  const par::rta::WDResult wd = par::rta::ComputeWD(g);
  const double phi_ls = par::rta::ComputeMinFeasiblePeriod(g, wd);
  const double phi_pan = par::rta::ComputeMinFeasiblePeriodPan(g, 1e-4);

  // Theorem-3: Pan's continuous phi may differ from L-S's discrete phi
  // by at most D = max node_delay (here D = 1.0). The structural source
  // of the gap on this circuit is that Pan's formula sums d(dest) per
  // edge but never sees d(source) at a PI -- equivalent to assuming the
  // PI has zero delay, which is Pan's standard convention. L-S treats
  // all node delays uniformly, so L-S = 3 (full comb chain), Pan = 2
  // (chain minus d(PI)). Both are valid feasibility statements and the
  // gap is bounded by D.
  std::ostringstream oss;
  oss << "Pan phi (" << phi_pan << ") vs L-S phi (" << phi_ls
      << ") differ by more than D = 1.0";
  if (!Expect(std::fabs(phi_pan - phi_ls) <= 1.0 + 1e-3, oss.str()))
    return;
  // Pan in [L-S - D, L-S].
  if (!Expect(phi_pan >= phi_ls - 1.0 - 1e-3
                  && phi_pan <= phi_ls + 1e-3,
              "Pan phi within Theorem-3 bound of L-S"))
    return;
  Pass();
}

// --- Pan #3: feed-forward register agrees with L-S (anchored).
void Test_Pan_FeedforwardRegister()
{
  StartTest("Pan_FeedforwardRegister");
  par::rta::RetimingGraph g;
  g.num_nodes = 3;
  g.node_delay = {5.0, 2.0, 5.0};
  g.edges = {{0, 1, 0}, {1, 2, 1}};
  g.is_anchor = {true, false, true};

  const par::rta::WDResult wd = par::rta::ComputeWD(g);
  const double phi_ls = par::rta::ComputeMinFeasiblePeriod(g, wd);
  const double phi_pan = par::rta::ComputeMinFeasiblePeriodPan(g, 1e-3);

  // Both should report phi_opt = 7 (max comb segment).
  if (!ExpectNear(phi_ls, 7.0, "L-S phi_opt"))
    return;
  // Pan continuous may approach 7 from above/below; allow Theorem-3 slack.
  std::ostringstream oss;
  oss << "Pan-vs-L-S phi agreement: |Pan " << phi_pan << " - L-S " << phi_ls
      << "| > D=5";
  const double D = 5.0;
  if (!Expect(std::fabs(phi_pan - phi_ls) <= D + 1e-3, oss.str()))
    return;
  Pass();
}

// --- Pan #4: end-to-end ComputeSequentialSlack via Pan path produces a
//     feasible result with non-negative slacks on every edge at phi_opt.
void Test_Pan_EndToEnd()
{
  StartTest("Pan_EndToEnd");
  par::rta::RetimingGraph g;
  g.num_nodes = 3;
  g.node_delay = {5.0, 2.0, 5.0};
  g.edges = {{0, 1, 0}, {1, 2, 1}};
  g.is_anchor = {true, false, true};

  par::rta::RetimingOptions opts;
  opts.algorithm = par::rta::RetimingAlgorithm::kPan;
  par::rta::RetimingSlackResult r = par::rta::ComputeSequentialSlack(g, opts);
  if (!Expect(r.feasible, "Pan end-to-end should be feasible"))
    return;
  if (!Expect(r.phi_opt_valid, "Pan phi_opt_valid should be true"))
    return;
  for (std::size_t i = 0; i < g.edges.size(); ++i) {
    std::ostringstream oss;
    oss << "Pan edge " << i << " slack >= 0 (got " << r.edge_slack[i] << ")";
    if (!Expect(r.edge_slack[i] >= -1e-6, oss.str()))
      return;
  }
  Pass();
}

// --- Pan #5: infeasibility detected when phi is below max gate delay.
void Test_Pan_Infeasibility()
{
  StartTest("Pan_Infeasibility");
  par::rta::RetimingGraph g;
  g.num_nodes = 3;
  g.node_delay = {0.0, 10.0, 0.0};
  g.edges = {{0, 1, 0}, {1, 2, 0}};
  g.is_anchor = {true, false, true};

  if (!Expect(!par::rta::PanCtcheck(g, 5.0),
              "Pan: T=5 < d=10 should be infeasible"))
    return;

  par::rta::RetimingOptions opts;
  opts.algorithm = par::rta::RetimingAlgorithm::kPan;
  opts.target_period = 5.0;
  par::rta::RetimingSlackResult r = par::rta::ComputeSequentialSlack(g, opts);
  if (!Expect(!r.feasible, "Pan ComputeSequentialSlack: T=5 infeasible"))
    return;
  Pass();
}

// --- Pan #6: wire_delay actually enters the computation. Two-gate comb
// path with one big wire on a critical edge: phi_opt rises by the wire
// amount, and that edge's slack drops accordingly.
void Test_Pan_WireDelayMatters()
{
  StartTest("Pan_WireDelayMatters");
  par::rta::RetimingGraph g_no;
  g_no.num_nodes = 3;
  g_no.node_delay = {0.0, 2.0, 0.0};  // PI, gate (d=2), PO
  g_no.edges = {{0, 1, 0, 0.0}, {1, 2, 0, 0.0}};
  g_no.is_anchor = {true, false, true};

  par::rta::RetimingGraph g_yes = g_no;
  g_yes.edges[0].wire_delay = 3.0;  // big wire on PI -> gate

  const double phi_no = par::rta::ComputeMinFeasiblePeriodPan(g_no, 1e-3);
  const double phi_yes = par::rta::ComputeMinFeasiblePeriodPan(g_yes, 1e-3);

  // Without the wire, phi_opt is just the gate delay (=2). With the wire
  // (=3) on PI->gate, the path delay is 3+2=5.
  if (!ExpectNear(phi_no, 2.0, "Pan phi_opt without wire", 1e-2))
    return;
  if (!ExpectNear(phi_yes, 5.0, "Pan phi_opt with wire_delay=3", 1e-2))
    return;

  // L-S WD path-delay must also account for wire_delay.
  par::rta::WDResult wd_no = par::rta::ComputeWD(g_no);
  par::rta::WDResult wd_yes = par::rta::ComputeWD(g_yes);
  // D[0][2] is the longest path delay from node 0 to node 2.
  if (!ExpectNear(wd_no.D[0][2], 2.0, "L-S D[0][2] without wire"))
    return;
  if (!ExpectNear(wd_yes.D[0][2], 5.0, "L-S D[0][2] with wire_delay=3"))
    return;
  Pass();
}

}  // namespace

int main()
{
  std::printf("RetimingSlack unit tests\n");
  std::printf("------------------------\n");
  Test_ValidateRejectsCombCycle();
  Test_ValidateAcceptsRegisterCycle();
  Test_PureCombChain();
  Test_SingleRegisterRing();
  Test_DefiningProperty();
  Test_InfeasiblePeriodRejected();
  Test_AutoPhiOpt();
  Test_FeedforwardSingleRegister();
  Test_Pan_SingleGate();
  Test_Pan_AnchoredCombChain();
  Test_Pan_FeedforwardRegister();
  Test_Pan_EndToEnd();
  Test_Pan_Infeasibility();
  Test_Pan_WireDelayMatters();
  std::printf("------------------------\n");
  std::printf("PASS: %d  FAIL: %d\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
