// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026-2026, The OpenROAD Authors

#include "ret/Retimer.h"
#include "ret/RetimingGraph.h"
#include "odb/db.h"
#include "utl/Logger.h"
// STA
#include "sta/Graph.hh"
#include "sta/Path.hh"
#include "sta/StaMain.hh"
#include "sta/StaState.hh"
#include "db_sta/dbSta.hh"
#include "db_sta/dbNetwork.hh"
// EST
#include "est/EstimateParasitics.h"
// #include "est/SteinerTree.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ret {
    
RetimingGraph::RetimingGraph(odb::dbDatabase* db,
                sta::dbSta* sta,
                est::EstimateParasitics* estimate_parasitics,
                utl::Logger* logger)
    : db_(db),
      sta_(sta),
      estimate_parasitics_(estimate_parasitics),
      logger_(logger) {}

// RetimingGraph::~RetimingGraph() = default;

void RetimingGraph::AddEdge(odb::dbInst* source, RetEdge edge) 
{
    RetimingGraph::g_[source].push_back(edge);
}

void RetimingGraph::Tunnel(odb::dbInst* source, odb::dbInst* current, \
    int weight, std::unordered_set<odb::dbInst*>& visited) {
    for (odb::dbITerm* out_pin : current->getITerms()) {
        if (out_pin->getIoType() != odb::dbIoType::OUTPUT) {
            continue;
        }
        odb::dbNet* net = out_pin->getNet();
        if (net == nullptr) {
            continue;
        }
        for (odb::dbITerm* sink : net->getITerms()) {
            if (sink->getIoType() != odb::dbIoType::INPUT) {
                continue;
            }
            odb::dbInst* load = sink->getInst();
            if (load->getMaster()->isSequential()) {
                // FF chain detection
                auto it = visited.find(load);
                if (it != visited.end()) {
                    continue;
                }
                visited.insert(load);
                RetimingGraph::Tunnel(source, load, weight + 1, visited);
                visited.erase(load);
            } else {
                RetimingGraph::AddEdge(source, {load, weight, 0.0});
            }
        }
    }
}

void RetimingGraph::BuildRetimingGraph() 
{
    auto block = db_->getChip()->getBlock(); // whole netlist
    
    for (auto inst : block->getInsts()) {
        if (inst->getMaster()->isSequential()) {
            continue;
        }
        std::unordered_set<odb::dbInst*> visited;
        RetimingGraph::Tunnel(inst, inst, 0, visited);
    }

    logger_->info(utl::RET, 2, "Built graph with {} source nodes.", g_.size());
}

} // namespace ret