// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026-2026, The OpenROAD Authors

#include "ret/Retimer.h"
#include "ret/RetimingGraph.h"
#include "odb/db.h"
#include "utl/Logger.h"
// STA
#include "sta/Clock.hh"
#include "sta/Delay.hh"
#include "sta/Graph.hh"
#include "sta/Path.hh"
#include "sta/Mode.hh"
#include "sta/Sdc.hh"
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

void RetimingGraph::GetClockPeriod()
{
    clock_period_ = 0.0;
    for (sta::Clock* clk : sta_->cmdMode()->sdc()->clocks()) {
        clock_period_ = std::max(clock_period_, clk->period());
    }
    // if (!clock_period_) {
    //     logger_->error(utl::RET, 1, "");
    // }
}

void RetimingGraph::AddEdge(odb::dbInst* source, RetEdge edge) 
{
    RetimingGraph::g_[source].push_back(edge);
}

float RetimingGraph::PinArrival(odb::dbITerm* db_pin)
{
    sta::Pin* sta_pin = sta_->getDbNetwork()->dbToSta(db_pin);
    sta::Vertex *vertex, *bidirect_drvr;
    sta_->ensureGraph()->pinVertices(sta_pin, vertex, bidirect_drvr);
    if (vertex == nullptr) {
        return 0.0f;
    }
    float rise = sta::delayAsFloat(sta_->arrival(
        vertex, sta::RiseFall::rise()->asRiseFallBoth(),
        sta_->scenes(), sta::MinMax::max()));
    float fall = sta::delayAsFloat(sta_->arrival(
        vertex, sta::RiseFall::fall()->asRiseFallBoth(),
        sta_->scenes(), sta::MinMax::max()));

    return std::max(rise, fall);
}

/*
Starts with source node and tunnels through sequential logic
while counting how many registers there are in that path.
*/
void RetimingGraph::Tunnel(odb::dbInst* source, odb::dbInst* current, \
    int weight, std::unordered_set<odb::dbInst*>& visited) 
{
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
                RetimingGraph::AddEdge(source, {load, weight, 0.0, out_pin, sink});
            }
        }
    }
}

/*
Computes and stores both node and interconnect delays.
node_delays_ = max_output - max_input
RetEdge e, e.wireDelay = e.sinkPin - e.driverPin
*/
void RetimingGraph::ComputeDelays() 
{
    for (auto& [inst, edges] : g_) {
        float node_delay = 0.0;
        float max_input = 0.0;
        float max_output = 0.0;
        for (odb::dbITerm* iterm : inst->getITerms()) {
            if (iterm->getIoType() == odb::dbIoType::INPUT) {
                max_input = std::max(max_input, RetimingGraph::PinArrival(iterm));
            }
            if (iterm->getIoType() == odb::dbIoType::OUTPUT) {
                max_output = std::max(max_output, RetimingGraph::PinArrival(iterm));
            }
        }
        node_delay = max_output - max_input;
        node_delays_[inst] = node_delay;
        for (RetEdge& e : edges) {
            if (e.weight == 0) {
                e.wireDelay = RetimingGraph::PinArrival(e.sinkPin) - \
                                RetimingGraph::PinArrival(e.driverPin);
            }
        }
    }
}

/*
Compute register distances
*/
void RetimingGraph::ComputeLv()
{
    std::unordered_map<odb::dbInst*, int> indeg;
    for (auto& [u, edges] : g_) {
        for (RetEdge& e : edges) {
            indeg[e.target]++;
        }
    }
    for (auto& pair : g_) {
        odb::dbInst* u = pair.first;
        if (indeg.count(u) == 0) {// Input Port
            Lv_[u] = 0;
        }
    }
    //Bellman-Ford
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& [u, edges] : g_) {
            for (RetEdge& e : edges) {
                if (Lv_.count(u) == 0) {// Input Port
                    continue;
                }
                odb::dbInst* v = e.target;
                int cand = Lv_[u] + e.weight;
                if (Lv_.count(v) == 0 || cand < Lv_[v]) {
                    Lv_[v] = cand;
                    changed = true;
                }
            }
        }
    }
}

void RetimingGraph::ComputeArrivals() 
{
    arrivals_.clear();
    for (auto& pair : Lv_) {
        arrivals_[pair.first] = node_delays_[pair.first];
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& [u, edges] : g_) {
            for (RetEdge& e : edges) {
                odb::dbInst* v = e.target;
                int w_l = e.weight + label_[v] - label_[u];
                if (w_l != 0) { // Only combinational nodes
                    continue;
                }
                float cand = arrivals_[u] + e.wireDelay + node_delays_[v];
                if (cand > arrivals_[v]) {
                    arrivals_[v] = cand;
                    changed = true;
                }
            }
        }
    }
}

bool RetimingGraph::RunMinLag(float phi) 
{
    for (auto& [v, lv] : Lv_) {
        label_[v] = -lv;
    }
    int n = Lv_.size();
    for (int i = 0; i < n; i++) {
        RetimingGraph::ComputeArrivals(); // Fill arrival_
        bool feasible = true;
        for (auto& [v, av] : arrivals_) {
            if (av > phi) {
                label_[v] += 1;
                feasible = false;
            }
        }
        if (feasible) {
            logger_->info(utl::RET, 10, "MINLAG SUCCESS at phi={} in {} iterations", phi, i);
            return true;
        }
    }
    logger_->info(utl::RET, 11, "MINLAG FAILURE: phi={} infeasible", phi);
    return false;
}

double RetimingGraph::MinPeriod()
{
    double lo = 0.0;
    double hi = RetimingGraph::clock_period_;
    double eps = 1e-12;
    while (hi - lo > eps) {
        double mid = (lo + hi) / 2.0;
        if (RetimingGraph::RunMinLag(mid)) {
            hi = mid;
        } else {
            lo = mid;
        }
    }
    return hi;    
}

void RetimingGraph::BuildRetimingGraph() 
{
    auto block = db_->getChip()->getBlock(); // whole netlist
    RetimingGraph::GetClockPeriod();
    logger_->info(utl::RET, 12, "Max Clock Period: {}", clock_period_);
    
    for (auto inst : block->getInsts()) {
        if (inst->getMaster()->isSequential()) {
            continue;
        }
        std::unordered_set<odb::dbInst*> visited;
        RetimingGraph::Tunnel(inst, inst, 0, visited);
    }
    
    //////////////////////////////////////////////
    // Tunnel() Debug logs
    logger_->info(utl::RET, 2, "Built graph with {} source nodes.", g_.size());

    int total_edges = 0;
    int total_weights = 0;
    int max_weight = 0;
    for (auto& [src, edges] : g_) {
        for (const RetEdge& e : edges) {
            total_edges++;
            if (e.weight >= 1) {
                total_weights++;
                max_weight = std::max(max_weight, e.weight);
            }
        }
    }
    logger_->info(utl::RET, 3, "Total Edges: {}", total_edges);
    logger_->info(utl::RET, 4, "Total Weights: {}", total_weights);
    logger_->info(utl::RET, 5, "Max Weight: {}", max_weight);

    RetimingGraph::ComputeDelays();

    //////////////////////////////////////////////
    // ComputeDelays() Debug logs
    int calculated_nodes = 0;
    int calculated_edges = 0;
    for (auto& [inst, delay] : node_delays_) {
        if (delay >= 0) {
            calculated_nodes++;
        }
    }
    for (auto& [inst, edges] : g_) {
        for (const RetEdge& e : edges) {
            if (e.weight == 0 && e.wireDelay >= 0) {
                calculated_edges++;
            }
        }
    }
    logger_->info(utl::RET, 6, "Calculated Node Count: {}", calculated_nodes);
    logger_->info(utl::RET, 7, "Calculated Edge Count: {}", calculated_edges);

    RetimingGraph::ComputeLv();

    //////////////////////////////////////////////
    // ComputeLv() Debug logs    
    logger_->info(utl::RET, 8, "Found {} distinct nodes.", Lv_.size()); 

    RetimingGraph::ComputeArrivals();
    
    //////////////////////////////////////////////
    // ComputeArrivals() Debug logs    
    logger_->info(utl::RET, 9, "arrivals_.size(): {}.", arrivals_.size()); 

    double min_period = RetimingGraph::MinPeriod();

    //////////////////////////////////////////////
    // MinPeriod() Debug logs       
    logger_->info(utl::RET, 13, "Calculated MinPeriod: {}.", min_period); 


}

} // namespace ret