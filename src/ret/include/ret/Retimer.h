
#pragma once

#include "odb/db.h"
#include "utl/Logger.h"

// Updating interconnect delays by
//  estimating parasitics
#include "est/EstimateParasitics.h"

// Determining potential legalized candidate cells
//  along interconnects
// Might use for wire retiming
#include "est/SteinerTree.h"

// STA
#include "sta/Graph.hh"
#include "sta/Path.hh"
#include "sta/StaMain.hh"
#include "sta/StaState.hh"
#include "db_sta/dbSta.hh"
#include "db_sta/dbNetwork.hh"

// Resizer for buffering?

#include "ret/RetimingGraph.h"

namespace ret {

class RetimingGraph;

class Retimer
{
    public:
    Retimer(odb::dbDatabase* db, 
            sta::dbSta* sta, 
            est::EstimateParasitics* estimate_parasitics,
            utl::Logger* logger);

    // Entry point
    void layoutAwareRetime();

    private:
    // odb
    odb::dbBlock* getBlock();

    odb::dbDatabase* db_ = nullptr;
    sta::dbSta* sta_ = nullptr;
    est::EstimateParasitics* estimate_parasitics_ = nullptr;
    utl::Logger* logger_ = nullptr;
};

} // namespace ret