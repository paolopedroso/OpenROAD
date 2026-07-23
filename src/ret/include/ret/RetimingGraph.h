// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026-2026, The OpenROAD Authors

#pragma once

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

/* Notes
dbITerm -> database Instance Terminal

*/

struct RetEdge
{
    odb::dbInst* target;
    int weight;
    float wireDelay;
    odb::dbITerm* driverPin;
    odb::dbITerm* sinkPin;
};

class RetimingGraph
{
    public:
    RetimingGraph(odb::dbDatabase* db,
                  sta::dbSta* sta,
                  est::EstimateParasitics* estimate_parasitics,
                  utl::Logger* logger);
    
    std::unordered_map<odb::dbInst*, std::vector<RetEdge>> g_;
    std::unordered_map<odb::dbInst*, float> node_delays_;
    
    void AddEdge(odb::dbInst* source, RetEdge edge);
    void BuildRetimingGraph();
    void Tunnel(odb::dbInst* source, odb::dbInst* current, \
        int weight, std::unordered_set<odb::dbInst*>& visited);
    void ComputeDelays();
    float PinArrival(odb::dbITerm* pin);

    odb::dbDatabase* db_ = nullptr;
    sta::dbSta* sta_ = nullptr;
    est::EstimateParasitics* estimate_parasitics_ = nullptr;
    utl::Logger* logger_ = nullptr;
};


} // namespace ret