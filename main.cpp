#include "simulator.h"
#include <iostream>
#include <iomanip>
#include <vector>
#include <string>

// ─────────────────────────────────────────────
// Print a SimResult row
// ─────────────────────────────────────────────
static void print_result(const SimResult& r, uint64_t total_possible) {
    double deliver_rate = total_possible > 0
        ? 100.0 * r.events_sent / total_possible : 0.0;
    std::cout << std::left  << std::setw(30) << r.label
              << std::right << std::setw(12) << std::fixed << std::setprecision(3) << r.wall_ms << " ms"
              << std::setw(15) << r.events_sent  << " pkts"
              << std::setw(10) << std::fixed << std::setprecision(1) << deliver_rate << "%\n";
}

// ─────────────────────────────────────────────
// Run one config and print comparison table
// ─────────────────────────────────────────────
static void run_comparison(const TopoConfig& cfg,
                            const std::string& label,
                            int ticks = 1)
{
    std::cout << "\n╔══════════════════════════════════════════════════════╗\n";
    std::cout << "║  " << label << "\n";
    std::cout << "║  Spine=" << cfg.num_spine
              << "  Leaf=" << cfg.num_leaf
              << "  NICs/leaf=" << cfg.nics_per_leaf
              << "  TotalNICs=" << cfg.total_nics()
              << "  Pkts/NIC=" << cfg.num_packets << "\n";
    std::cout << "╚══════════════════════════════════════════════════════╝\n";
    std::cout << std::left << std::setw(30) << "Mode"
              << std::right << std::setw(14) << "Wall time"
              << std::setw(17) << "Delivered"
              << std::setw(10) << "Rate" << "\n";
    std::cout << std::string(71, '-') << "\n";

    uint64_t total_pkts =
        static_cast<uint64_t>(cfg.total_nics()) * cfg.num_packets;

    // Serial
    {
        Topology topo = build_topology(cfg);
        Simulator sim(topo, ticks);
        auto r = sim.run_serial();
        print_result(r, total_pkts);
    }

    // Parallel — hardware concurrency
    {
        Topology topo = build_topology(cfg);
        Simulator sim(topo, ticks);
        auto r = sim.run_parallel(); // default: hw threads
        print_result(r, total_pkts);
    }

    // Parallel — 2, 4, 8 threads
    for (int nt : {2, 4, 8}) {
        Topology topo = build_topology(cfg);
        Simulator sim(topo, ticks);
        auto r = sim.run_parallel(nt);
        print_result(r, total_pkts);
    }

    std::cout << std::string(71, '-') << "\n";
}

// ─────────────────────────────────────────────
// main
// ─────────────────────────────────────────────
int main() {
    std::cout << "═══════════════════════════════════════════════════════════\n";
    std::cout << "  NSX-inspired Leaf/Spine Network Simulator (C++)\n";
    std::cout << "  Core ideas: Granular modules | FlexQueue | MET | MT\n";
    std::cout << "═══════════════════════════════════════════════════════════\n";

    // ── Scenario A: Small (paper's debug scale) ──
    run_comparison({
        .num_spine     = 4,
        .num_leaf      = 4,
        .nics_per_leaf = 4,
        .num_packets   = 200,
        .nic_interval  = 10.0,
        .link_delay    = 5.0,
    }, "Scenario A: 4 Spine x 4 Leaf x 4 NICs/leaf = 16 NICs total");

    // ── Scenario B: Medium ──
    run_comparison({
        .num_spine     = 4,
        .num_leaf      = 8,
        .nics_per_leaf = 8,
        .num_packets   = 500,
        .nic_interval  = 10.0,
        .link_delay    = 5.0,
    }, "Scenario B: 4 Spine x 8 Leaf x 8 NICs/leaf = 64 NICs total");

    // ── Scenario C: Large ──
    run_comparison({
        .num_spine     = 8,
        .num_leaf      = 16,
        .nics_per_leaf = 16,
        .num_packets   = 1000,
        .nic_interval  = 10.0,
        .link_delay    = 5.0,
    }, "Scenario C: 8 Spine x 16 Leaf x 16 NICs/leaf = 256 NICs total");

    return 0;
}
