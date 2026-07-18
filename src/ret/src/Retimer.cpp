// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026-2026, The OpenROAD Authors

#include "ret/Retimer.h"

#include "odb/db.h"
#include "utl/Logger.h"
#include "est/EstimateParasitics.h"
#include "est/SteinerTree.h"

#include "sta/Graph.hh"
#include "sta/Path.hh"
#include "sta/StaMain.hh"
#include "sta/StaState.hh"
#include "db_sta/dbSta.hh"
#include "db_sta/dbNetwork.hh"

namespace ret {

Retimer::Retimer(odb::dbDatabase* db, 
                 sta::dbSta* sta,
                 est::EstimateParasitics* estimate_parasitics,
                 utl::Logger* logger)
    : db_(db), sta_(sta), estimate_parasitics_(estimate_parasitics), logger_(logger) {}

// Retimer::~Retimer() = default;

void Retimer::layoutAwareRetime()
{
    RetimingGraph graph(db_, sta_, estimate_parasitics_, logger_);
    graph.BuildRetimingGraph();
}


} // namespace ret