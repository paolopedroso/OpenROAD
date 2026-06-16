// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include "RetimingExtract.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <vector>

#include "utl/Logger.h"

namespace par {

namespace {

bool IsAnchorVertex(VertexType t)
{
  // Only combinational std cells are retimable. Ports anchor the design
  // boundary; sequential std cells and macros are register-bearing
  // boundaries that must not shift.
  return t != kCombStdCell;
}

bool IsRegisterBoundary(VertexType t)
{
  // The presence of a sequential cell or hard macro as the LOAD of a net
  // means the net crosses a register boundary. Ports do not contribute a
  // register; they are pure timing boundaries handled by the anchor.
  return t == kSeqStdCell || t == kMacro;
}

// Fanout-proxy fallback for wire delay when the caller did not source real
// values from OpenSTA. The constant is intentionally small (~1 ps per fanout
// hop) so the proxy contributes order-of-magnitude correct, sub-cell-delay
// values rather than dominating the engine. Documented as a proxy in
// docs/rta-part/implementation.md.
constexpr double kWireFanoutProxyPerHop = 1e-12;  // seconds

}  // namespace

std::vector<float> ComputeSequentialSlackPerHyperedge(
    int num_vertices,
    int num_hyperedges,
    const std::vector<std::vector<int>>& hyperedges,
    const std::vector<VertexType>& vertex_types,
    const std::vector<double>& node_delays_in,
    const std::vector<double>& wire_delays_in,
    rta::RetimingAlgorithm algorithm,
    double target_period,
    RetimingDiagnostics* diag,
    utl::Logger* logger)
{
  std::vector<float> result(num_hyperedges,
                            static_cast<float>(target_period > 0
                                                   ? target_period
                                                   : 1.0));
  if (diag) {
    *diag = RetimingDiagnostics{};
    diag->period_used = target_period;
  }

  if (num_vertices <= 0 || num_hyperedges <= 0
      || static_cast<int>(vertex_types.size()) != num_vertices) {
    if (logger) {
      logger->warn(utl::PAR,
                   39,
                   "RTA-Part: extraction skipped (vertices={}, hyperedges={}, "
                   "vertex_types.size={})",
                   num_vertices,
                   num_hyperedges,
                   vertex_types.size());
    }
    return result;
  }

  const bool have_real_node_delays
      = static_cast<int>(node_delays_in.size()) == num_vertices;
  const bool have_real_wire_delays
      = static_cast<int>(wire_delays_in.size()) == num_hyperedges;

  // --- Build the engine graph -------------------------------------------
  rta::RetimingGraph g;
  g.num_nodes = num_vertices;
  g.node_delay.assign(num_vertices, 0.0);
  g.is_anchor.assign(num_vertices, false);
  int num_anchored = 0;
  int num_nodes_real = 0;
  for (int v = 0; v < num_vertices; ++v) {
    if (have_real_node_delays && node_delays_in[v] >= 0.0) {
      g.node_delay[v] = node_delays_in[v];
      if (node_delays_in[v] > 0.0) {
        ++num_nodes_real;
      }
    } else if (vertex_types[v] == kCombStdCell) {
      g.node_delay[v] = 1.0;  // v1 placeholder (unitless logic depth)
    } else {
      g.node_delay[v] = 0.0;
    }
    if (IsAnchorVertex(vertex_types[v])) {
      g.is_anchor[v] = true;
      ++num_anchored;
    }
  }

  // hyperedge[e] = {driver_vertex_id, load1, load2, ...}. Skip degenerate
  // hyperedges; they cannot contribute to slack.
  std::vector<std::vector<int>> hyperedge_to_engine_edges(num_hyperedges);
  int num_register_edges = 0;
  int num_edges_real_wire = 0;
  for (int e = 0; e < num_hyperedges; ++e) {
    const auto& vs = hyperedges[e];
    if (vs.size() < 2) {
      continue;
    }
    const int driver = vs[0];
    if (driver < 0 || driver >= num_vertices) {
      continue;
    }
    // Wire delay for this hyperedge: real if provided, else fanout-proxy.
    double wire_for_edge = 0.0;
    if (have_real_wire_delays && wire_delays_in[e] >= 0.0) {
      wire_for_edge = wire_delays_in[e];
      if (wire_delays_in[e] > 0.0) {
        ++num_edges_real_wire;
      }
    } else {
      // (vs.size() - 1) is the fanout (# loads on this net).
      wire_for_edge = kWireFanoutProxyPerHop
                      * static_cast<double>(vs.size() - 1);
    }
    for (std::size_t i = 1; i < vs.size(); ++i) {
      const int load = vs[i];
      if (load < 0 || load >= num_vertices || load == driver) {
        continue;
      }
      const int regs = IsRegisterBoundary(vertex_types[load]) ? 1 : 0;
      if (regs > 0) {
        ++num_register_edges;
      }
      const int eidx = static_cast<int>(g.edges.size());
      g.edges.push_back({driver, load, regs, wire_for_edge});
      hyperedge_to_engine_edges[e].push_back(eidx);
    }
  }

  if (diag) {
    diag->num_engine_nodes = g.num_nodes;
    diag->num_engine_edges = static_cast<int>(g.edges.size());
    diag->num_anchored_nodes = num_anchored;
    diag->num_register_edges = num_register_edges;
    diag->num_nodes_with_real_delay = num_nodes_real;
    diag->num_edges_with_real_wire_delay = num_edges_real_wire;
  }

  // --- Run the engine ---------------------------------------------------
  rta::RetimingOptions opts;
  opts.target_period = target_period;
  opts.algorithm = algorithm;
  const rta::RetimingSlackResult res = rta::ComputeSequentialSlack(g, opts);
  if (diag) {
    diag->feasible = res.feasible;
    diag->period_used = res.period_used;
    diag->phi_opt = res.phi_opt;
    diag->phi_opt_valid = res.phi_opt_valid;
  }

  if (!res.feasible) {
    if (logger) {
      logger->warn(utl::PAR,
                   40,
                   "RTA-Part: retiming infeasible at T={:.3e}; falling back "
                   "to neutral slack on every hyperedge.",
                   target_period);
    }
    return result;
  }

  // --- Aggregate per-hyperedge slack ------------------------------------
  const double T = res.period_used;
  for (int e = 0; e < num_hyperedges; ++e) {
    const auto& eidxs = hyperedge_to_engine_edges[e];
    if (eidxs.empty()) {
      result[e] = static_cast<float>(T);  // unconstrained -> 1.0 after norm
      continue;
    }
    double worst = std::numeric_limits<double>::infinity();
    for (const int idx : eidxs) {
      if (res.edge_slack[idx] < worst) {
        worst = res.edge_slack[idx];
      }
    }
    result[e] = static_cast<float>(worst);
  }
  return result;
}

}  // namespace par
