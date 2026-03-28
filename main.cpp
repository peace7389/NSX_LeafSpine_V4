#include "simulator.h"
#include "trace.h"
#include "checker.h"
#include <iostream>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <streambuf>
#include <vector>
#include <string>
#include <cctype>
#include <algorithm>

// ─────────────────────────────────────────────
// TeeStreambuf — mirrors std::cout to a file
// ─────────────────────────────────────────────
class TeeStreambuf : public std::streambuf {
public:
    TeeStreambuf(std::streambuf* console, std::streambuf* file)
        : console_(console), file_(file) {}
protected:
    int overflow(int c) override {
        if (c == EOF) return !EOF;
        if (console_->sputc(c) == EOF) return EOF;
        if (file_->sputc(c)   == EOF) return EOF;
        return c;
    }
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        console_->sputn(s, n);
        file_->sputn(s, n);
        return n;
    }
private:
    std::streambuf* console_;
    std::streambuf* file_;
};

// ─────────────────────────────────────────────
// sim_config.json — optional interactive config
// ─────────────────────────────────────────────
struct CustomConfig {
    TopoConfig          topo;
    std::string         scenario_name = "Custom";
    std::vector<int>    parallel_threads = {};
    bool                verbose_trace = false;
};

struct ModeExecution {
    SimResult                  measured;
    std::vector<PacketRecord>  traced_records;
};

// Minimal JSON field extractors (no third-party deps)
static int json_int(const std::string& j, const std::string& key, int def) {
    auto p = j.find('"' + key + '"'); if (p == j.npos) return def;
    p = j.find(':', p) + 1;
    while (p < j.size() && std::isspace((unsigned char)j[p])) ++p;
    try { return std::stoi(j.substr(p)); } catch (...) { return def; }
}
static double json_dbl(const std::string& j, const std::string& key, double def) {
    auto p = j.find('"' + key + '"'); if (p == j.npos) return def;
    p = j.find(':', p) + 1;
    while (p < j.size() && std::isspace((unsigned char)j[p])) ++p;
    try { return std::stod(j.substr(p)); } catch (...) { return def; }
}
static bool json_bool(const std::string& j, const std::string& key, bool def) {
    auto p = j.find('"' + key + '"'); if (p == j.npos) return def;
    p = j.find(':', p) + 1;
    while (p < j.size() && std::isspace((unsigned char)j[p])) ++p;
    if (j.compare(p, 4, "true") == 0) return true;
    if (j.compare(p, 5, "false") == 0) return false;
    return def;
}
static std::string json_str(const std::string& j, const std::string& key, std::string def) {
    auto p = j.find('"' + key + '"'); if (p == j.npos) return def;
    p = j.find(':', p) + 1;
    while (p < j.size() && std::isspace((unsigned char)j[p])) ++p;
    if (p >= j.size() || j[p] != '"') return def;
    ++p; auto e = j.find('"', p);
    return e == j.npos ? def : j.substr(p, e - p);
}
static std::vector<int> json_int_array(const std::string& j, const std::string& key) {
    std::vector<int> v;
    auto p = j.find('"' + key + '"'); if (p == j.npos) return v;
    p = j.find('[', p); if (p == j.npos) return v;
    ++p;
    while (p < j.size()) {
        while (p < j.size() && (std::isspace((unsigned char)j[p]) || j[p] == ',')) ++p;
        if (p >= j.size() || j[p] == ']') break;
        if (std::isdigit((unsigned char)j[p]) || j[p] == '-')
            try { v.push_back(std::stoi(j.substr(p))); } catch (...) {}
        while (p < j.size() && j[p] != ',' && j[p] != ']') ++p;
    }
    return v;
}

static bool try_load_custom_config(CustomConfig& out) {
    std::ifstream f("sim_config.json");
    if (!f.is_open()) return false;
    std::string j((std::istreambuf_iterator<char>(f)),
                   std::istreambuf_iterator<char>());

    out.scenario_name           = json_str(j, "scenario_name",  "Custom");
    out.topo.num_spine          = json_int(j, "num_spine",        4);
    out.topo.num_leaf           = json_int(j, "num_leaf",         4);
    out.topo.nics_per_leaf      = json_int(j, "nics_per_leaf",    4);
    out.topo.num_packets        = json_int(j, "num_packets",    200);
    out.topo.nic_interval       = json_dbl(j, "nic_interval_ns", 10.0);
    out.topo.link_delay         = json_dbl(j, "link_delay_ns",    5.0);
    out.topo.num_tiers          = json_int(j, "num_tiers",         2);
    out.topo.num_agg            = json_int(j, "num_agg",           4);
    out.verbose_trace           = json_bool(j, "verbose_trace", false);
    auto pt = json_int_array(j, "parallel_threads");
    if (!pt.empty()) out.parallel_threads = pt;
    return true;
}

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
// Write topology nodes/links/routes to trace file
// ─────────────────────────────────────────────
static void log_topology(Tracer& tr, const TopoConfig& cfg) {
    const int N = cfg.total_nics();
    for (int i = 0; i < N; ++i)             tr.node_nic(i, cfg.nic_to_leaf(i));
    for (int l = 0; l < cfg.num_leaf; ++l)  tr.node_leaf(l);
    if (cfg.num_tiers == 3)
        for (int a = 0; a < cfg.num_agg; ++a) tr.node_agg(a);
    for (int s = 0; s < cfg.num_spine; ++s) tr.node_spine(s);
    for (int i = 0; i < N; ++i)             tr.link_nic_uplink(i, cfg.nic_to_leaf(i));
    for (int l = 0; l < cfg.num_leaf; ++l)
        for (int s = 0; s < cfg.num_spine; ++s)
            tr.link_leaf_to_spine(l, s);  // In 3-tier this logs ToR→Agg
    for (int s = 0; s < cfg.num_spine; ++s)
        for (int l = 0; l < cfg.num_leaf; ++l)
            tr.link_spine_to_leaf(s, l);
    for (int i = 0; i < N; ++i)
        tr.packet_route(i, (i + N / 2) % N);
    tr.blank_line();
}

// ─────────────────────────────────────────────
// Run one config and print comparison table
// ─────────────────────────────────────────────
static void run_comparison(const TopoConfig& cfg,
                            const std::string& label,
                            Tracer* tracer = nullptr,
                            int ticks = 1,
                            const std::vector<int>& extra_threads = {2, 4, 8})
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

    // Write scenario header and topology to trace file
    if (tracer) {
        tracer->scenario(label);
        tracer->config(cfg.num_spine, cfg.num_leaf, cfg.nics_per_leaf,
                       cfg.total_nics(), cfg.num_packets,
                       cfg.nic_interval, cfg.link_delay,
                       cfg.num_agg, cfg.num_tiers);
        log_topology(*tracer, cfg);
    }

    CheckConfig cc {
        cfg.total_nics(), cfg.num_packets,
        cfg.nic_interval, cfg.link_delay,
        cfg.num_link_hops()
    };
    CorrectnessChecker checker(cc);

    std::vector<SimResult> results;

    auto execute_mode = [&](auto&& run_fn) -> ModeExecution {
        // Warm up without tracer so the displayed benchmark is less dominated
        // by first-run effects such as cold caches or thread startup.
        // Reuse the same topology between warm-up and measurement so the
        // measured pass benefits from already-grown queue storage too.
        ModeExecution out;
        {
            Topology topo = build_topology(cfg);
            Simulator sim(topo, ticks);
            (void)run_fn(sim);
            out.measured = run_fn(sim);
        }

        if (tracer) {
            Topology topo = build_topology(cfg);
            Simulator sim(topo, ticks);
            sim.set_tracer(tracer);
            auto traced = run_fn(sim);
            out.measured.label = traced.label;
            out.measured.events_sent = traced.events_sent;
            out.traced_records = tracer->get_delivery_records();
        }

        return out;
    };

    // Serial
    {
        auto exec = execute_mode([](Simulator& sim) {
            return sim.run_serial();
        });
        results.push_back(exec.measured);
        print_result(exec.measured, total_pkts);
        if (tracer) checker.add_run(exec.measured.label, exec.traced_records);
    }

    std::vector<int> requested_threads;
    requested_threads.reserve(extra_threads.size());
    for (int nt : extra_threads) {
        if (std::find(requested_threads.begin(), requested_threads.end(), nt) == requested_threads.end())
            requested_threads.push_back(nt);
    }

    // Parallel — run exactly the requested thread-count modes.
    // nt <= 0 means "auto/hardware concurrency".
    for (int nt : requested_threads) {
        auto exec = execute_mode([&](Simulator& sim) {
            return nt <= 0 ? sim.run_parallel() : sim.run_parallel(nt);
        });
        results.push_back(exec.measured);
        print_result(exec.measured, total_pkts);
        if (tracer) checker.add_run(exec.measured.label, exec.traced_records);
    }

    std::cout << std::string(71, '-') << "\n";

    // Write comparison summary to trace file
    if (tracer) {
        tracer->comparison_start(label);
        for (auto& r : results)
            tracer->comparison_mode(r.label, r.wall_ms, r.events_sent, total_pkts);
        tracer->blank_line();
    }

    // Correctness validation report (stdout + trace file)
    if (tracer) checker.print_summary(label, *tracer);
}

// ─────────────────────────────────────────────
// main
// ─────────────────────────────────────────────
int main() {
    // Mirror all std::cout output to sim_output.txt
    std::ofstream out_file("sim_output.txt");
    TeeStreambuf tee(std::cout.rdbuf(), out_file.rdbuf());
    std::cout.rdbuf(&tee);

    std::cout << "═══════════════════════════════════════════════════════════\n";
    std::cout << "  NSX-inspired Leaf/Spine Network Simulator (C++)\n";
    std::cout << "  Core ideas: Granular modules | FlexQueue | MET | MT\n";
    std::cout << "═══════════════════════════════════════════════════════════\n";

    // ── Check for interactive config from browser ──
    CustomConfig custom;
    if (try_load_custom_config(custom)) {
        std::cout << "  [Mode: interactive config from sim_config.json]\n";
        const auto& c = custom.topo;
        std::string label = custom.scenario_name + ": "
            + std::to_string(c.num_spine) + " Spine x "
            + std::to_string(c.num_leaf)  + " Leaf x "
            + std::to_string(c.nics_per_leaf) + " NICs/leaf = "
            + std::to_string(c.total_nics()) + " NICs total";
        Tracer tracer_custom("sim_trace_custom.txt", custom.verbose_trace);
        run_comparison(c, label, &tracer_custom, 1, custom.parallel_threads);
        return 0;
    }

    // ── Default: three fixed scenarios ──
    // Scenario A: verbose=true  (16 NICs × 200 pkts = 3,200 — manageable)
    // Scenario B: verbose=false (64 NICs × 500 pkts = 32,000 — summary only)
    // Scenario C: verbose=false (256 NICs × 1000 pkts = 256,000 — summary only)
    Tracer tracer_a("sim_trace_a.txt", true);
    Tracer tracer_b("sim_trace_b.txt", false);
    Tracer tracer_c("sim_trace_c.txt", false);

    // ── Scenario A: Small (paper's debug scale) ──
    run_comparison({
        .num_spine     = 4,
        .num_leaf      = 4,
        .nics_per_leaf = 4,
        .num_packets   = 200,
        .nic_interval  = 10.0,
        .link_delay    = 5.0,
    }, "Scenario A: 4 Spine x 4 Leaf x 4 NICs/leaf = 16 NICs total",
       &tracer_a);

    // ── Scenario B: Medium ──
    run_comparison({
        .num_spine     = 4,
        .num_leaf      = 8,
        .nics_per_leaf = 8,
        .num_packets   = 500,
        .nic_interval  = 10.0,
        .link_delay    = 5.0,
    }, "Scenario B: 4 Spine x 8 Leaf x 8 NICs/leaf = 64 NICs total",
       &tracer_b);

    // ── Scenario C: Large ──
    run_comparison({
        .num_spine     = 8,
        .num_leaf      = 16,
        .nics_per_leaf = 16,
        .num_packets   = 1000,
        .nic_interval  = 10.0,
        .link_delay    = 5.0,
    }, "Scenario C: 8 Spine x 16 Leaf x 16 NICs/leaf = 256 NICs total",
       &tracer_c);

    return 0;
}
