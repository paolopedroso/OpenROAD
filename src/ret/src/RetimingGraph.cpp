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

void RetimingGraph::BuildRetimingGraph() 
{
    int added_edge_count = 0;
    auto block = db_->getChip()->getBlock(); // whole netlist
    
    for (auto net : block->getNets()) {
        auto driver = net->getFirstDriverInst();
        if (driver == nullptr) { 
            continue;
        }
        for (odb::dbITerm* it : net->getITerms()) {
            if (it->getIoType() == odb::dbIoType::INPUT) {
                odb::dbInst* load = it->getInst();
                RetimingGraph::AddEdge(driver, RetEdge{load, 0, 0.0});
                ++added_edge_count;
            }
        }
    }
    logger_->info(utl::RET, 2, "Added {} edges to ret graph.", added_edge_count);
}

} // namespace ret