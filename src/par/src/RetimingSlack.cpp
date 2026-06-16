// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include "RetimingSlack.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <queue>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace par::rta {

namespace {

// Lex-ordered pair used by Algorithm WD's all-pairs shortest path. The first
// component is the register count along the path (non-negative integer); the
// second is the negation of the cumulative source-node delay along the path
// (so minimizing it maximizes the delay over register-minimal paths). For an
// edge u -> v with delay d(u) and register count w, the edge weight is
// (w, -d(u)); a path's weight is the componentwise sum.
struct LexW
{
  int regs;
  double minus_delay;
  bool operator<(const LexW& other) const
  {
    if (regs != other.regs) {
      return regs < other.regs;
    }
    return minus_delay < other.minus_delay;
  }
  bool operator==(const LexW& other) const
  {
    return regs == other.regs && minus_delay == other.minus_delay;
  }
  LexW operator+(const LexW& other) const
  {
    return {regs + other.regs, minus_delay + other.minus_delay};
  }
};

constexpr int kInf = WDResult::kInfRegs;
constexpr double kNegInf = WDResult::kNegInfDelay;

const LexW kLexInf{kInf, 0.0};

}  // namespace

bool RetimingGraph::Validate(std::string* err) const
{
  auto fail = [err](const std::string& msg) {
    if (err) {
      *err = msg;
    }
    return false;
  };
  if (num_nodes < 0) {
    return fail("num_nodes negative");
  }
  if (static_cast<int>(node_delay.size()) != num_nodes) {
    return fail("node_delay size != num_nodes");
  }
  for (int v = 0; v < num_nodes; ++v) {
    if (!(node_delay[v] >= 0.0)) {  // also rejects NaN
      std::ostringstream oss;
      oss << "node_delay[" << v << "] = " << node_delay[v]
          << " is not >= 0";
      return fail(oss.str());
    }
  }
  for (std::size_t i = 0; i < edges.size(); ++i) {
    const auto& e = edges[i];
    if (e.u < 0 || e.u >= num_nodes || e.v < 0 || e.v >= num_nodes) {
      std::ostringstream oss;
      oss << "edges[" << i << "] endpoint out of range: u=" << e.u
          << " v=" << e.v;
      return fail(oss.str());
    }
    if (e.regs < 0) {
      std::ostringstream oss;
      oss << "edges[" << i << "] has negative register count " << e.regs;
      return fail(oss.str());
    }
  }
  if (!is_anchor.empty()
      && static_cast<int>(is_anchor.size()) != num_nodes) {
    return fail("is_anchor size != num_nodes");
  }
  // Detect register-free directed cycles via DFS over the comb subgraph.
  std::vector<std::vector<int>> comb_succ(num_nodes);
  for (const auto& e : edges) {
    if (e.regs == 0) {
      comb_succ[e.u].push_back(e.v);
    }
  }
  enum Mark { kUnvisited, kOnStack, kDone };
  std::vector<int> mark(num_nodes, kUnvisited);
  std::vector<int> stack;
  for (int start = 0; start < num_nodes; ++start) {
    if (mark[start] != kUnvisited) {
      continue;
    }
    // Iterative DFS to avoid blowing the call stack on large graphs.
    struct Frame
    {
      int v;
      std::size_t next_succ;
    };
    std::vector<Frame> frames;
    frames.push_back({start, 0});
    mark[start] = kOnStack;
    while (!frames.empty()) {
      Frame& f = frames.back();
      if (f.next_succ < comb_succ[f.v].size()) {
        const int w = comb_succ[f.v][f.next_succ++];
        if (mark[w] == kOnStack) {
          std::ostringstream oss;
          oss << "register-free cycle detected through node " << w;
          return fail(oss.str());
        }
        if (mark[w] == kUnvisited) {
          mark[w] = kOnStack;
          frames.push_back({w, 0});
        }
      } else {
        mark[f.v] = kDone;
        frames.pop_back();
      }
    }
  }
  return true;
}

WDResult ComputeWD(const RetimingGraph& g)
{
  const int n = g.num_nodes;
  // dist[u][v] holds the lex-shortest path weight u -> v in Algorithm WD
  // formulation.
  std::vector<std::vector<LexW>> dist(n, std::vector<LexW>(n, kLexInf));
  for (int v = 0; v < n; ++v) {
    dist[v][v] = LexW{0, 0.0};
  }
  for (const auto& e : g.edges) {
    // Lex pair (regs, -(d(u) + wire(e))): summed over a path it gives
    // (Sigma regs, -Sigma_{i<k} (d(v_i) + wire(e_i))). Together with
    // d(v_k) at the destination we recover total path delay
    //   D = d(v_k) + Sigma_{i<k} d(v_i) + Sigma wire(e_i).
    const LexW w{e.regs, -(g.node_delay[e.u] + e.wire_delay)};
    if (w < dist[e.u][e.v]) {
      dist[e.u][e.v] = w;
    }
  }
  // Floyd-Warshall under lex addition/comparison.
  for (int k = 0; k < n; ++k) {
    for (int i = 0; i < n; ++i) {
      if (dist[i][k].regs >= kInf) {
        continue;
      }
      for (int j = 0; j < n; ++j) {
        if (dist[k][j].regs >= kInf) {
          continue;
        }
        const LexW candidate = dist[i][k] + dist[k][j];
        if (candidate < dist[i][j]) {
          dist[i][j] = candidate;
        }
      }
    }
  }
  WDResult out;
  out.W.assign(n, std::vector<int>(n, kInf));
  out.D.assign(n, std::vector<double>(n, kNegInf));
  for (int i = 0; i < n; ++i) {
    for (int j = 0; j < n; ++j) {
      if (dist[i][j].regs >= kInf) {
        continue;
      }
      out.W[i][j] = dist[i][j].regs;
      out.D[i][j] = g.node_delay[j] - dist[i][j].minus_delay;
    }
  }
  return out;
}

bool IsFeasiblePeriod(const RetimingGraph& g,
                      const WDResult& wd,
                      double T,
                      std::vector<int>* retiming_out)
{
  // Difference constraints over r:
  //   (i)   r(u) - r(v) <= w(e)             for every edge e: u -> v
  //   (ii)  r(u) - r(v) <= W[u][v] - 1      for every (u,v) with D[u][v] > T
  //   (iii) r(anchor) = r(canonical_anchor) for every anchored node (a star
  //         of bidirectional 0-weight constraints rooted at the canonical
  //         anchor; the canonical anchor is then forced to 0 after BF)
  // Each "r(a) - r(b) <= c" becomes a constraint-graph edge b -> a with
  // weight c. Add a super-source s with 0-weight edges to every node; run
  // Bellman-Ford. Feasible iff no negative cycle. Then r*(v) = dist[v],
  // shifted so the canonical anchor (if any) reads 0.
  const int n = g.num_nodes;
  struct CEdge
  {
    int from;
    int to;
    double w;
  };
  std::vector<CEdge> cons;
  cons.reserve(g.edges.size() + static_cast<std::size_t>(n) * n);
  for (const auto& e : g.edges) {
    cons.push_back({e.v, e.u, static_cast<double>(e.regs)});
  }
  for (int u = 0; u < n; ++u) {
    for (int v = 0; v < n; ++v) {
      if (wd.W[u][v] >= WDResult::kInfRegs) {
        continue;
      }
      if (wd.D[u][v] > T) {
        cons.push_back({v, u, static_cast<double>(wd.W[u][v] - 1)});
      }
    }
  }
  int canonical_anchor = -1;
  if (!g.is_anchor.empty()) {
    for (int v = 0; v < n; ++v) {
      if (g.is_anchor[v]) {
        if (canonical_anchor < 0) {
          canonical_anchor = v;
          continue;
        }
        // Pin r(v) = r(canonical_anchor) via bidirectional 0-weight
        // constraints. These do not create a register-free cycle in the
        // input graph because they live only in the constraint graph.
        cons.push_back({canonical_anchor, v, 0.0});
        cons.push_back({v, canonical_anchor, 0.0});
      }
    }
  }
  // Super-source s = n with 0-weight edge to every node; we initialize all
  // dist to 0 directly, equivalent to relaxing from s once.
  std::vector<double> dist(n, 0.0);
  // Bellman-Ford: relax for n iterations; on the (n+1)-th, any further
  // relaxation indicates a negative cycle => infeasible.
  for (int iter = 0; iter < n; ++iter) {
    bool changed = false;
    for (const auto& c : cons) {
      if (dist[c.from] + c.w < dist[c.to]) {
        dist[c.to] = dist[c.from] + c.w;
        changed = true;
      }
    }
    if (!changed) {
      break;
    }
  }
  for (const auto& c : cons) {
    if (dist[c.from] + c.w < dist[c.to] - 1e-9) {
      return false;
    }
  }
  if (retiming_out) {
    retiming_out->resize(n);
    // A uniform integer shift on r leaves every w_r(e) unchanged; use it to
    // pin the canonical anchor (if any) to exactly 0.
    double shift = 0.0;
    if (canonical_anchor >= 0) {
      shift = -dist[canonical_anchor];
    }
    for (int v = 0; v < n; ++v) {
      (*retiming_out)[v]
          = static_cast<int>(std::lround(dist[v] + shift));
    }
    // Sanity: verify legality (w_r >= 0) and that anchors land on 0.
    for (const auto& e : g.edges) {
      const int wr
          = e.regs + (*retiming_out)[e.v] - (*retiming_out)[e.u];
      if (wr < 0) {
        return false;
      }
    }
    if (!g.is_anchor.empty()) {
      for (int v = 0; v < n; ++v) {
        if (g.is_anchor[v] && (*retiming_out)[v] != 0) {
          return false;
        }
      }
    }
  }
  return true;
}

double ComputeMinFeasiblePeriod(const RetimingGraph& g, const WDResult& wd)
{
  // Candidate periods are the distinct finite D[u][v] values (Leiserson-Saxe:
  // the optimum is always one of these). Plus the global max delay as a
  // ceiling (always feasible: r* = 0 gives the static circuit at its own
  // critical-path period).
  const int n = g.num_nodes;
  std::vector<double> cands;
  cands.reserve(static_cast<std::size_t>(n) * n);
  for (int i = 0; i < n; ++i) {
    for (int j = 0; j < n; ++j) {
      if (wd.D[i][j] != WDResult::kNegInfDelay) {
        cands.push_back(wd.D[i][j]);
      }
    }
  }
  std::sort(cands.begin(), cands.end());
  cands.erase(std::unique(cands.begin(), cands.end()), cands.end());
  if (cands.empty()) {
    return -1.0;
  }
  // Binary search for the smallest feasible candidate.
  int lo = 0;
  int hi = static_cast<int>(cands.size()) - 1;
  int best = -1;
  if (!IsFeasiblePeriod(g, wd, cands[hi], nullptr)) {
    return -1.0;
  }
  while (lo <= hi) {
    const int mid = (lo + hi) / 2;
    if (IsFeasiblePeriod(g, wd, cands[mid], nullptr)) {
      best = mid;
      hi = mid - 1;
    } else {
      lo = mid + 1;
    }
  }
  return best >= 0 ? cands[best] : -1.0;
}

// --- Pan c-retiming (ICCD 1997) -----------------------------------------
//
// CTCHECK feasibility test: c-retiming s: V -> R with s(PI) = 0 forced,
// s(PO) <= 1 required. Under target period phi, each edge u -> v carries
// an effective longest-path weight
//   w1(e) = (node_delay[v] + wire_delay(e)) / phi - regs(e)
// and the relaxation
//   s(v) = max over incoming edges u->v of (s(u) + w1(e))
//        = max over incoming of (s(u) - regs(e) + (d(v) + wire(e))/phi)
// converges in |U|+1 sweeps under an FVS pseudo-order. Pan's paper allows
// any correct longest-path relaxation order (FVS is a recommended schedule,
// not a correctness requirement). We use SPFA -- Bellman-Ford with a queue
// of changed nodes, push-style relaxation -- which computes the identical
// fixpoint as dense BF but only touches nodes whose s actually changed.
// Divergence detection uses the standard SPFA test: any node popped from
// the queue more than n times implies a positive cycle under w1, so phi
// is infeasible. Anchored nodes in our engine play the role of both PIs
// (initial s = 0, never updated) and POs (any candidate push into an
// anchor must satisfy <= 1 + epsilon, else infeasible by Theorem 1).
bool PanCtcheck(const RetimingGraph& g,
                double phi,
                std::vector<double>* pan_s_out)
{
  const int n = g.num_nodes;
  if (n == 0 || !(phi > 0.0)) {
    return false;
  }
  constexpr double kNegInfS = -std::numeric_limits<double>::infinity();
  constexpr double kSlackTol = 1e-9;
  const bool has_anchor = !g.is_anchor.empty();

  // Build per-source-node outgoing edge index for push-style relaxation.
  std::vector<std::vector<int>> out_edges(n);
  for (int i = 0; i < static_cast<int>(g.edges.size()); ++i) {
    const auto& e = g.edges[i];
    if (e.u >= 0 && e.u < n && e.v >= 0 && e.v < n) {
      out_edges[e.u].push_back(i);
    }
  }

  std::vector<double> s(n, kNegInfS);
  std::vector<char> in_queue(n, 0);
  std::vector<int> pop_count(n, 0);
  std::queue<int> q;
  // Seed: anchored nodes start at s=0 and are the only initial finite-s
  // sources of relaxation (matches Pan's PI semantics).
  for (int v = 0; v < n; ++v) {
    if (has_anchor && g.is_anchor[v]) {
      s[v] = 0.0;
      q.push(v);
      in_queue[v] = 1;
    }
  }

  while (!q.empty()) {
    const int u = q.front();
    q.pop();
    in_queue[u] = 0;
    // SPFA divergence test: a node can be relaxed at most n-1 times before
    // either converging or being on a positive cycle. Popping > n times
    // proves the cycle exists -> infeasible.
    if (++pop_count[u] > n) {
      return false;
    }
    const double s_u = s[u];
    if (s_u == kNegInfS) {
      continue;  // shouldn't happen post-seed but be defensive
    }
    for (const int eidx : out_edges[u]) {
      const auto& e = g.edges[eidx];
      const int v = e.v;
      const double cand
          = s_u - static_cast<double>(e.regs)
            + (g.node_delay[v] + e.wire_delay) / phi;
      if (has_anchor && g.is_anchor[v]) {
        // s(PO) <= 1 (Theorem 1). Anchors are pinned at 0; just verify the
        // bound. A push that exceeds 1+eps proves phi infeasible.
        if (cand > 1.0 + kSlackTol) {
          return false;
        }
        // Don't update or enqueue: the anchor's s stays at 0.
        continue;
      }
      if (cand > s[v] + kSlackTol) {
        s[v] = cand;
        if (!in_queue[v]) {
          q.push(v);
          in_queue[v] = 1;
        }
      }
    }
  }
  if (pan_s_out) {
    *pan_s_out = std::move(s);
  }
  return true;
}

double ComputeMinFeasiblePeriodPan(const RetimingGraph& g, double tol)
{
  // Lower bound: largest single (node + max-incident-wire) since any gate
  // needs at least that much per period. Even cruder: max node_delay or
  // max wire_delay alone.
  double max_d = 0.0;
  for (const double d : g.node_delay) {
    if (d > max_d) {
      max_d = d;
    }
  }
  double max_wire = 0.0;
  for (const auto& e : g.edges) {
    if (e.wire_delay > max_wire) {
      max_wire = e.wire_delay;
    }
  }
  double lo = std::max(max_d + max_wire, 0.0);
  // Upper bound: sum of all delays (very generous; effectively unbounded
  // unrolling).
  double up = 0.0;
  for (const double d : g.node_delay) {
    up += d;
  }
  for (const auto& e : g.edges) {
    up += e.wire_delay;
  }
  if (lo <= 0.0 && up <= 0.0) {
    // All-zero delays: pick a small unit.
    lo = 0.0;
    up = 1.0;
  }
  if (up <= lo) {
    up = lo * 2.0 + 1.0;
  }
  // Verify the upper bound is actually feasible; otherwise the graph has
  // a positive cycle under w1 even at large phi -> infeasible.
  if (!PanCtcheck(g, up, nullptr)) {
    return -1.0;
  }
  // Real-valued binary search to absolute tolerance tol.
  while (up - lo > tol) {
    const double mid = 0.5 * (lo + up);
    if (PanCtcheck(g, mid, nullptr)) {
      up = mid;
    } else {
      lo = mid;
    }
  }
  return up;
}

std::vector<int> PanRoundRetiming(const RetimingGraph& g,
                                  const std::vector<double>& s)
{
  const int n = g.num_nodes;
  std::vector<int> r(n, 0);
  const bool has_anchor = !g.is_anchor.empty();
  for (int v = 0; v < n; ++v) {
    if (has_anchor && g.is_anchor[v]) {
      r[v] = 0;  // Theorem 3: PIs / POs pinned to 0.
    } else if (v < static_cast<int>(s.size())
               && std::isfinite(s[v])) {
      r[v] = static_cast<int>(std::ceil(s[v])) - 1;
    } else {
      r[v] = 0;  // Unreachable from any PI (s remained -inf): leave at 0.
    }
  }
  return r;
}

namespace {

// Forward/backward sequential STA on the retimed graph at period T. An edge
// with w_r == 0 is combinational; an edge with w_r > 0 carries at least one
// register (we treat any positive count as a register boundary, which is the
// correct STA model: arrival resets at register output, required is T at
// register input).
struct RetimedSTA
{
  std::vector<double> arrival;
  std::vector<double> required;
};

RetimedSTA RunRetimedSTA(const RetimingGraph& g,
                         const std::vector<int>& retiming,
                         double T)
{
  const int n = g.num_nodes;
  // Build per-node comb predecessors and successors after applying retiming.
  // Each comb edge carries its wire_delay so the arrival/required passes can
  // accumulate it on top of node_delay.
  struct CombHop
  {
    int node;
    double wire_delay;
  };
  std::vector<std::vector<CombHop>> comb_pred(n), comb_succ(n);
  std::vector<bool> has_reg_out(n, false);
  for (const auto& e : g.edges) {
    const int wr = e.regs + retiming[e.v] - retiming[e.u];
    if (wr == 0) {
      comb_pred[e.v].push_back({e.u, e.wire_delay});
      comb_succ[e.u].push_back({e.v, e.wire_delay});
    } else {
      has_reg_out[e.u] = true;
    }
  }
  // Topological order of the comb subgraph (Kahn's). A feasible retiming
  // guarantees acyclicity here.
  std::vector<int> in_deg(n, 0);
  for (int v = 0; v < n; ++v) {
    in_deg[v] = static_cast<int>(comb_pred[v].size());
  }
  std::vector<int> topo;
  topo.reserve(n);
  std::queue<int> q;
  for (int v = 0; v < n; ++v) {
    if (in_deg[v] == 0) {
      q.push(v);
    }
  }
  while (!q.empty()) {
    const int v = q.front();
    q.pop();
    topo.push_back(v);
    for (const auto& hop : comb_succ[v]) {
      if (--in_deg[hop.node] == 0) {
        q.push(hop.node);
      }
    }
  }
  // If topo.size() != n, the comb subgraph has a cycle — shouldn't happen
  // for a feasible retiming. Fall through; arrivals on the cyclic part will
  // remain at the initialized "fresh start" value.
  RetimedSTA out;
  out.arrival.assign(n, 0.0);
  out.required.assign(n, T);
  // Forward pass: a(v) = d(v) + max over comb edges u->v of (a(u) + wire(e)).
  for (const int v : topo) {
    double max_pred = 0.0;
    for (const auto& hop : comb_pred[v]) {
      const double cand = out.arrival[hop.node] + hop.wire_delay;
      if (cand > max_pred) {
        max_pred = cand;
      }
    }
    out.arrival[v] = g.node_delay[v] + max_pred;
  }
  // Backward pass: req(v) = min over outgoing edges of either
  //   - req(w) - d(w) - wire(e) when the edge v->w is combinational
  //   - T                        when the edge carries a register
  //   - T                        when v has no outgoing edges (primary out)
  for (auto it = topo.rbegin(); it != topo.rend(); ++it) {
    const int v = *it;
    double best = std::numeric_limits<double>::infinity();
    bool any = false;
    for (const auto& hop : comb_succ[v]) {
      const double cand
          = out.required[hop.node] - g.node_delay[hop.node] - hop.wire_delay;
      if (cand < best) {
        best = cand;
      }
      any = true;
    }
    if (has_reg_out[v]) {
      if (T < best) {
        best = T;
      }
      any = true;
    }
    if (!any) {
      best = T;  // primary output
    }
    out.required[v] = best;
  }
  return out;
}

}  // namespace

RetimingSlackResult ComputeSlackAtRetiming(const RetimingGraph& g,
                                           const std::vector<int>& retiming,
                                           double T)
{
  RetimingSlackResult out;
  out.period_used = T;
  if (static_cast<int>(retiming.size()) != g.num_nodes) {
    out.feasible = false;
    return out;
  }
  // Legality: w_r(e) = w(e) + r(v) - r(u) must be non-negative.
  for (const auto& e : g.edges) {
    const int wr = e.regs + retiming[e.v] - retiming[e.u];
    if (wr < 0) {
      out.feasible = false;
      return out;
    }
  }
  const RetimedSTA sta = RunRetimedSTA(g, retiming, T);
  out.feasible = true;
  out.retiming = retiming;
  out.arrival = sta.arrival;
  out.required = sta.required;
  out.edge_slack.assign(g.edges.size(), 0.0);
  for (std::size_t i = 0; i < g.edges.size(); ++i) {
    const auto& e = g.edges[i];
    const int wr = e.regs + retiming[e.v] - retiming[e.u];
    if (wr == 0) {
      // Combinational edge: slack at v's input via u and the net wire =
      //   required(v) - d(v) - arrival(u) - wire(e).
      out.edge_slack[i] = out.required[e.v] - g.node_delay[e.v]
                          - out.arrival[e.u] - e.wire_delay;
    } else {
      // Register-bearing edge: the more constraining of register-input slack
      // (data must arrive at the FF.D within T of clock; the wire adds delay
      // on this leg) and register-output slack (downstream side).
      const double slack_in = T - out.arrival[e.u] - e.wire_delay;
      const double slack_out = out.required[e.v] - g.node_delay[e.v];
      out.edge_slack[i] = std::min(slack_in, slack_out);
    }
  }
  return out;
}

// Helper: ensure the produced integer retiming is legal (w_r(e) >= 0 on
// every edge). Pan's Theorem-3 rounding is provably feasible when applied
// to the converged c-retiming potentials, but our graph may include
// edge cases (unreachable subgraphs, anchored-component boundaries) where
// the round produces a w_r < 0 on a single edge; in that case bump r on
// the offending tail by the deficit and re-check.
namespace {
bool LegalizeRetiming(const RetimingGraph& g, std::vector<int>& r)
{
  for (int pass = 0; pass < g.num_nodes + 1; ++pass) {
    bool ok = true;
    for (const auto& e : g.edges) {
      const int wr = e.regs + r[e.v] - r[e.u];
      if (wr < 0) {
        ok = false;
        // r is too large at e.u relative to e.v; lower r[e.u] by the deficit.
        // (Equivalent to raising r[e.v]; pick whichever doesn't violate
        // anchor pinning.)
        if (!g.is_anchor.empty() && g.is_anchor[e.u]
            && !g.is_anchor[e.v]) {
          r[e.v] += -wr;
        } else if (!g.is_anchor.empty() && g.is_anchor[e.v]
                   && !g.is_anchor[e.u]) {
          r[e.u] += wr;  // wr is negative; this lowers r[e.u]
        } else if (g.is_anchor.empty()
                   || (!g.is_anchor[e.u] && !g.is_anchor[e.v])) {
          r[e.u] += wr;  // both retimable: lower source
        } else {
          // Both anchored and edge is illegal under r=0,0: impossible.
          return false;
        }
      }
    }
    if (ok) {
      return true;
    }
  }
  return false;
}
}  // namespace

RetimingSlackResult ComputeSequentialSlack(const RetimingGraph& g,
                                           const RetimingOptions& opts)
{
  RetimingSlackResult out;
  std::string err;
  if (!g.Validate(&err)) {
    out.feasible = false;
    return out;
  }
  double T = opts.target_period;
  bool phi_opt_seen = false;
  double phi_opt_val = 0.0;
  std::vector<int> r_star;

  if (opts.algorithm == RetimingAlgorithm::kPan) {
    // ----- Pan c-retiming path -----
    if (!(T > 0.0)) {
      phi_opt_val = ComputeMinFeasiblePeriodPan(g, opts.tolerance);
      if (phi_opt_val < 0.0) {
        out.feasible = false;
        return out;
      }
      phi_opt_seen = true;
      // Theorem 3 says the discrete period after integer rounding can be up
      // to max(node_delay) larger than phi. Bump T by that so the rounded
      // r* satisfies the constraint when fed to the retimed STA.
      double max_d = 0.0;
      for (const double d : g.node_delay) {
        if (d > max_d) {
          max_d = d;
        }
      }
      T = phi_opt_val + max_d;
    }
    std::vector<double> s;
    if (!PanCtcheck(g, T, &s)) {
      out.feasible = false;
      out.period_used = T;
      return out;
    }
    r_star = PanRoundRetiming(g, s);
    if (!LegalizeRetiming(g, r_star)) {
      out.feasible = false;
      out.period_used = T;
      return out;
    }
  } else {
    // ----- Leiserson-Saxe path (existing) -----
    const WDResult wd = ComputeWD(g);
    if (!(T > 0.0)) {
      phi_opt_val = ComputeMinFeasiblePeriod(g, wd);
      if (phi_opt_val < 0.0) {
        out.feasible = false;
        return out;
      }
      phi_opt_seen = true;
      T = phi_opt_val;
    }
    if (!IsFeasiblePeriod(g, wd, T, &r_star)) {
      out.feasible = false;
      out.period_used = T;
      return out;
    }
  }

  // Apply r* and run retimed STA + per-edge slack. Common to both paths.
  out = ComputeSlackAtRetiming(g, r_star, T);
  if (phi_opt_seen) {
    out.phi_opt = phi_opt_val;
    out.phi_opt_valid = true;
  }
  return out;
}

}  // namespace par::rta
