// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#pragma once

#include <vector>

#include "RetimingSlack.h"
#include "Utilities.h"  // for par::VertexType

namespace utl {
class Logger;
}

namespace par {

// Diagnostic block returned to the caller for logging. Lets BuildTimingPaths
// surface phi_opt vs target period and feasibility without re-running the
// engine.
struct RetimingDiagnostics
{
  double period_used = 0.0;
  double phi_opt = 0.0;
  bool phi_opt_valid = false;
  bool feasible = false;
  int num_engine_nodes = 0;
  int num_engine_edges = 0;
  int num_anchored_nodes = 0;
  int num_register_edges = 0;
  // Sourcing summary: lets the caller log how many vertices/edges were
  // populated from real OpenSTA delays vs the placeholder fallback.
  int num_nodes_with_real_delay = 0;
  int num_edges_with_real_wire_delay = 0;
};

// Build a retiming graph from TritonPart's vertex/hyperedge data structures
// and run the sequential-slack engine. Returns one raw (un-normalized)
// sequential slack per hyperedge, indexed by hyperedge_id; the caller is
// responsible for normalizing to TritonPart's (-inf, 1.0] contract.
//
// Engine inputs:
//   num_vertices, num_hyperedges, hyperedges -- same shape as the
//     TritonPart member fields of the same names.
//   vertex_types -- per-vertex VertexType; drives the engine's anchor /
//     register-boundary classification (only kCombStdCell is retimable;
//     ports, sequential cells, macros are anchored).
//   node_delays  -- optional. If non-empty (size == num_vertices), each
//     entry is the per-vertex intrinsic gate delay in seconds (e.g. max
//     comb-arc intrinsic delay from OpenSTA's timing arcs). If empty,
//     the adapter falls back to the v1 placeholder (uniform 1.0 for
//     combinational, 0.0 elsewhere -- unitless logic-depth, NOT seconds).
//   wire_delays  -- optional. If non-empty (size == num_hyperedges),
//     each entry is the per-net interconnect delay in seconds (e.g.
//     max wire-arc delay across driver-sink pairs from OpenSTA). If
//     empty, defaults to 0.0 on every engine edge.
//   algorithm    -- which engine to run (default kPan).
//   target_period -- if > 0, slack is reported at that period; otherwise
//     the engine computes phi_opt and uses it.
//   diag, logger -- diagnostics and logging.
//
// Wire-delay fallback (when wire_delays is empty): a fanout-proxy
// (wire ~= k * fanout, k = 1ps) is used on each engine edge so the
// computation is well-defined even without STA harvesting. This is
// documented as a fallback in docs/rta-part/implementation.md.
std::vector<float> ComputeSequentialSlackPerHyperedge(
    int num_vertices,
    int num_hyperedges,
    const std::vector<std::vector<int>>& hyperedges,
    const std::vector<VertexType>& vertex_types,
    const std::vector<double>& node_delays,
    const std::vector<double>& wire_delays,
    rta::RetimingAlgorithm algorithm,
    double target_period,
    RetimingDiagnostics* diag,
    utl::Logger* logger);

}  // namespace par
