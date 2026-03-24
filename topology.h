#pragma once
#include "module.h"
#include "queues.h"
#include <vector>
#include <memory>
#include <string>
#include <stdexcept>

// ─────────────────────────────────────────────
// Topology parameters
// ─────────────────────────────────────────────
struct TopoConfig {
    int num_spine      = 4;
    int num_leaf       = 4;
    int nics_per_leaf  = 4;   // total NICs = num_leaf * nics_per_leaf

    int    num_packets  = 100;  // packets per NIC
    double nic_interval = 10.0; // ns between packets
    double link_delay   = 5.0;  // ns per link

    // Derived
    int total_nics() const { return num_leaf * nics_per_leaf; }
    int nic_to_leaf(int nic_id) const { return nic_id / nics_per_leaf; }
};

// ─────────────────────────────────────────────
// Topology — owns all queues + modules
// ─────────────────────────────────────────────
struct Topology {
    TopoConfig cfg;

    // All queues (owned here)
    std::vector<std::unique_ptr<FlexQueue>> queues;

    // Modules in systolic processing order
    // Tiers: NIC → NIC-uplink-link → leaf-fwd → leaf-egress →
    //        spine-link → spine-fwd → spine-egress → (reverse optional)
    std::vector<std::unique_ptr<NICModule>>              nic_mods;
    std::vector<std::unique_ptr<LinkModule>>             nic_uplink_mods;  // NIC→leaf
    std::vector<std::unique_ptr<SwitchForwardingModule>> leaf_fwd_mods;
    std::vector<std::unique_ptr<EgressModule>>           leaf_egress_mods; // per spine port
    std::vector<std::unique_ptr<LinkModule>>             spine_uplink_mods;// leaf→spine
    std::vector<std::unique_ptr<SwitchForwardingModule>> spine_fwd_mods;
    std::vector<std::unique_ptr<EgressModule>>           spine_egress_mods;
    // Sink queues (destination NIC queues — count delivered packets)
    std::vector<std::unique_ptr<FlexQueue>>              sink_queues;

    // Ordered stages for serial / parallel execution
    std::vector<std::vector<Module*>> stages;
};

// ─────────────────────────────────────────────
// Build a 2-tier leaf/spine topology
//
// Logical wiring:
//   NIC[i] → (link) → Leaf[leaf_id].fwd → (link per spine) →
//   Spine[s].fwd → sink_queue[dst_nic]
//
// For simplicity, all traffic is sent from NIC[i] to
// NIC[(i + total_nics/2) % total_nics] (opposite half).
// ─────────────────────────────────────────────
inline Topology build_topology(const TopoConfig& cfg) {
    Topology topo;
    topo.cfg = cfg;

    const int N   = cfg.total_nics();
    const int NL  = cfg.num_leaf;
    const int NS  = cfg.num_spine;
    const int NPL = cfg.nics_per_leaf;

    auto make_q = [&](int fan_in) -> FlexQueue* {
        topo.queues.push_back(std::make_unique<FlexQueue>(fan_in));
        return topo.queues.back().get();
    };

    // ── Sink queues (one per NIC destination) ──
    topo.sink_queues.resize(N);
    for (int i = 0; i < N; ++i)
        topo.sink_queues[i] = std::make_unique<FlexQueue>(NS); // fan-in = num_spine

    // ── NIC uplink queues: NIC → Leaf (one queue per NIC) ──
    //    fan_in=1 (one NIC per queue) → FIFO
    std::vector<FlexQueue*> nic_to_leaf_q(N);
    for (int i = 0; i < N; ++i)
        nic_to_leaf_q[i] = make_q(1);

    // ── Leaf ingress queues (NIC-side links output):
    //    same as nic_to_leaf_q after link delay
    std::vector<FlexQueue*> leaf_ingress_q(N); // per-NIC port on leaf
    for (int i = 0; i < N; ++i)
        leaf_ingress_q[i] = make_q(1);

    // ── Leaf→Spine queues: one per (leaf, spine) pair ──
    //    fan_in = NPL (all NICs on that leaf) → may use priority
    // leaf_to_spine_q[leaf][spine]
    std::vector<std::vector<FlexQueue*>> leaf_to_spine_q(NL, std::vector<FlexQueue*>(NS));
    for (int l = 0; l < NL; ++l)
        for (int s = 0; s < NS; ++s)
            leaf_to_spine_q[l][s] = make_q(NPL);

    // ── Spine ingress queues: one per (spine, leaf) uplink ──
    //    (after link delay) fan_in=1
    // spine_ingress_q[spine][leaf]
    std::vector<std::vector<FlexQueue*>> spine_ingress_q(NS, std::vector<FlexQueue*>(NL));
    for (int s = 0; s < NS; ++s)
        for (int l = 0; l < NL; ++l)
            spine_ingress_q[s][l] = make_q(1);

    // ── Spine→Leaf (egress) queues ──
    //    spine_to_leaf_q[spine][leaf], fan_in = NL (all leaves going to this spine)
    std::vector<std::vector<FlexQueue*>> spine_to_leaf_q(NS, std::vector<FlexQueue*>(NL));
    for (int s = 0; s < NS; ++s)
        for (int l = 0; l < NL; ++l)
            spine_to_leaf_q[s][l] = make_q(NL);

    // ── Leaf downlink ingress (from spine): leaf_down_q[leaf][spine] ──
    std::vector<std::vector<FlexQueue*>> leaf_down_q(NL, std::vector<FlexQueue*>(NS));
    for (int l = 0; l < NL; ++l)
        for (int s = 0; s < NS; ++s)
            leaf_down_q[l][s] = make_q(1);

    // ═══════════════════════════════════════════
    // Stage 0: NIC modules
    // ═══════════════════════════════════════════
    for (int i = 0; i < N; ++i) {
        int dst = (i + N / 2) % N; // send to opposite NIC
        topo.nic_mods.push_back(std::make_unique<NICModule>(
            i, cfg.num_packets, cfg.nic_interval, dst, nic_to_leaf_q[i]));
    }

    // ═══════════════════════════════════════════
    // Stage 1: NIC uplink link modules (NIC → Leaf ingress)
    // ═══════════════════════════════════════════
    for (int i = 0; i < N; ++i) {
        topo.nic_uplink_mods.push_back(std::make_unique<LinkModule>(
            "UplinkNIC-" + std::to_string(i),
            cfg.link_delay,
            nic_to_leaf_q[i], leaf_ingress_q[i]));
    }

    // ═══════════════════════════════════════════
    // Stage 2: Leaf forwarding modules
    // One per leaf — all NIC ports on that leaf feed in
    // ═══════════════════════════════════════════
    for (int l = 0; l < NL; ++l) {
        // Collect ingress queues for this leaf
        std::vector<FlexQueue*> ins;
        for (int p = 0; p < NPL; ++p) ins.push_back(leaf_ingress_q[l * NPL + p]);

        // Route: pick spine based on dst_nic (simple hash: spine = dst_leaf % NS)
        auto route_fn = [cfg_copy = cfg, l, ltq = leaf_to_spine_q, NS](int dst_nic) mutable -> FlexQueue* {
            int dst_leaf = cfg_copy.nic_to_leaf(dst_nic);
            if (dst_leaf == l) return nullptr; // local — skip (simplified)
            int spine = dst_leaf % NS;
            return ltq[l][spine];
        };

        topo.leaf_fwd_mods.push_back(std::make_unique<SwitchForwardingModule>(
            "LeafFwd-" + std::to_string(l), ins, route_fn));

        // Register output queues for MET propagation
        for (int s = 0; s < NS; ++s)
            topo.leaf_fwd_mods.back()->add_downstream(leaf_to_spine_q[l][s]);
    }

    // ═══════════════════════════════════════════
    // Stage 3: Leaf → Spine link modules
    // ═══════════════════════════════════════════
    for (int l = 0; l < NL; ++l) {
        for (int s = 0; s < NS; ++s) {
            topo.spine_uplink_mods.push_back(std::make_unique<LinkModule>(
                "Spine-Link-L" + std::to_string(l) + "-S" + std::to_string(s),
                cfg.link_delay,
                leaf_to_spine_q[l][s], spine_ingress_q[s][l]));
        }
    }

    // ═══════════════════════════════════════════
    // Stage 4: Spine forwarding modules
    // ═══════════════════════════════════════════
    for (int s = 0; s < NS; ++s) {
        std::vector<FlexQueue*> ins(spine_ingress_q[s].begin(), spine_ingress_q[s].end());

        auto route_fn = [cfg_copy = cfg, stlq = spine_to_leaf_q, s](int dst_nic) mutable -> FlexQueue* {
            int dst_leaf = cfg_copy.nic_to_leaf(dst_nic);
            return stlq[s][dst_leaf];
        };

        topo.spine_fwd_mods.push_back(std::make_unique<SwitchForwardingModule>(
            "SpineFwd-" + std::to_string(s), ins, route_fn));

        for (int l = 0; l < NL; ++l)
            topo.spine_fwd_mods.back()->add_downstream(spine_to_leaf_q[s][l]);
    }

    // ═══════════════════════════════════════════
    // Stage 5: Spine → Leaf downlink links
    // ═══════════════════════════════════════════
    for (int s = 0; s < NS; ++s) {
        for (int l = 0; l < NL; ++l) {
            topo.spine_egress_mods.push_back(std::make_unique<EgressModule>(
                "SpineEgr-S" + std::to_string(s) + "-L" + std::to_string(l),
                spine_to_leaf_q[s][l], leaf_down_q[l][s]));
        }
    }

    // ═══════════════════════════════════════════
    // Stage 6: Leaf egress → sink queues (delivery)
    // ═══════════════════════════════════════════
    // One EgressModule per (leaf, spine) downlink, routes to sink_queue[dst_nic]
    // For simplicity: we use a leaf-level aggregator via a dedicated egress
    // that writes directly to per-NIC sink queues.
    for (int l = 0; l < NL; ++l) {
        for (int s = 0; s < NS; ++s) {
            // leaf_down_q[l][s] → sink_queues (aggregated into a shared delivery stage)
            // We re-use a simple link with 0 delay as a demux placeholder
            topo.leaf_egress_mods.push_back(std::make_unique<EgressModule>(
                "LeafEgr-L" + std::to_string(l) + "-S" + std::to_string(s),
                leaf_down_q[l][s], topo.sink_queues[l * NPL].get())); // simplified: deliver to first NIC in leaf
        }
    }

    // ═══════════════════════════════════════════
    // Build ordered stages for the simulator
    // ═══════════════════════════════════════════
    auto as_ptrs = [](auto& v) {
        std::vector<Module*> r;
        for (auto& p : v) r.push_back(p.get());
        return r;
    };

    topo.stages = {
        as_ptrs(topo.nic_mods),
        as_ptrs(topo.nic_uplink_mods),
        as_ptrs(topo.leaf_fwd_mods),
        as_ptrs(topo.spine_uplink_mods),
        as_ptrs(topo.spine_fwd_mods),
        as_ptrs(topo.spine_egress_mods),
        as_ptrs(topo.leaf_egress_mods),
    };

    return topo;
}
