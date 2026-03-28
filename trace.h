#pragma once
#include "event.h"
#include <fstream>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>

// ─────────────────────────────────────────────
// PacketRecord — one delivery event, captured
// for every run regardless of verbose flag.
// Used by CorrectnessChecker.
// ─────────────────────────────────────────────
struct PacketRecord {
    int    src_nic;
    int    dst_nic;
    int    pkt_id;
    double delivered_time_ns;
};

// ─────────────────────────────────────────────
// Tracer — writes structured simulation trace
// to a text file for HTML visualization.
//
// verbose=true : log every packet event
//                (EMIT / FWD / ROUTE / EGRESS / DELIVERED)
// verbose=false: log stage/tick/run boundaries only
//                (recommended for large scenarios)
// ─────────────────────────────────────────────
class Tracer {
public:
    explicit Tracer(const std::string& path, bool verbose = true)
        : out_(path), verbose_(verbose) {}

    bool ok() const { return out_.is_open(); }

    // ── Scenario / topology (written once per scenario) ──────────────
    void scenario(const std::string& label) {
        write("SCENARIO \"" + label + "\"");
    }

    void config(int num_spine, int num_leaf, int nics_per_leaf,
                int total_nics, int num_packets,
                double nic_interval_ns, double link_delay_ns,
                int num_agg = 0, int num_tiers = 2) {
        char buf[256];
        snprintf(buf, sizeof(buf),
            "CONFIG num_spine=%d num_leaf=%d nics_per_leaf=%d "
            "total_nics=%d num_packets=%d "
            "nic_interval_ns=%.1f link_delay_ns=%.1f num_agg=%d num_tiers=%d",
            num_spine, num_leaf, nics_per_leaf,
            total_nics, num_packets,
            nic_interval_ns, link_delay_ns, num_agg, num_tiers);
        write(buf);
    }

    void node_nic(int nic_id, int leaf_id) {
        char buf[64];
        snprintf(buf, sizeof(buf), "NODE NIC id=%d leaf=%d", nic_id, leaf_id);
        write(buf);
    }

    void node_leaf(int leaf_id) {
        char buf[64];
        snprintf(buf, sizeof(buf), "NODE LEAF id=%d", leaf_id);
        write(buf);
    }

    void node_agg(int agg_id) {
        char buf[64];
        snprintf(buf, sizeof(buf), "NODE AGG id=%d", agg_id);
        write(buf);
    }

    void node_spine(int spine_id) {
        char buf[64];
        snprintf(buf, sizeof(buf), "NODE SPINE id=%d", spine_id);
        write(buf);
    }

    void link_nic_uplink(int nic_id, int leaf_id) {
        char buf[64];
        snprintf(buf, sizeof(buf), "LINK NIC_UPLINK nic=%d leaf=%d", nic_id, leaf_id);
        write(buf);
    }

    void link_leaf_to_spine(int leaf_id, int spine_id) {
        char buf[64];
        snprintf(buf, sizeof(buf), "LINK LEAF_TO_SPINE leaf=%d spine=%d", leaf_id, spine_id);
        write(buf);
    }

    void link_spine_to_leaf(int spine_id, int leaf_id) {
        char buf[64];
        snprintf(buf, sizeof(buf), "LINK SPINE_TO_LEAF spine=%d leaf=%d", spine_id, leaf_id);
        write(buf);
    }

    void packet_route(int src_nic, int dst_nic) {
        char buf[64];
        snprintf(buf, sizeof(buf), "PACKET_ROUTE src=%d dst=%d", src_nic, dst_nic);
        write(buf);
    }

    void blank_line() { write(""); }

    // ── Delivery record access (for CorrectnessChecker) ──────────────
    const std::vector<PacketRecord>& get_delivery_records() const {
        return delivery_records_;
    }

    // Write a line directly (used by CorrectnessChecker to write report
    // into the same trace file alongside the event log).
    void log_raw(const std::string& line) { write(line); }

    // ── Run boundary ──────────────────────────────────────────────────
    void run_start(const std::string& mode, int ticks) {
        delivery_records_.clear();   // fresh slate for each run
        run_t0_ = now_us();
        write("RUN_START mode=" + mode + " ticks=" + std::to_string(ticks));
    }

    void run_end(const std::string& mode, double wall_ms,
                 uint64_t delivered, uint64_t total) {
        double rate = total > 0 ? 100.0 * delivered / total : 0.0;
        char buf[256];
        snprintf(buf, sizeof(buf),
            "RUN_END mode=%s wall_ms=%.3f delivered=%llu total=%llu rate=%.1f%%",
            mode.c_str(), wall_ms,
            (unsigned long long)delivered,
            (unsigned long long)total, rate);
        write(buf);
        write("");
    }

    // ── Tick boundary ─────────────────────────────────────────────────
    void tick_start(int tick, double until_ns) {
        char buf[64];
        snprintf(buf, sizeof(buf), "TICK %d until_ns=%.1f", tick, until_ns);
        write(buf);
    }

    void tick_end(int tick) {
        write("TICK_END " + std::to_string(tick));
    }

    // ── Stage boundary ─────────────────────────────────────────────────
    void stage_start(int idx, const char* name, int num_modules) {
        stage_event_count_.store(0, std::memory_order_relaxed);
        uint64_t t = now_us();
        stage_t0_ = t;
        char buf[128];
        snprintf(buf, sizeof(buf),
            "  STAGE_START stage=%d name=%s modules=%d rel_us=%llu",
            idx, name, num_modules,
            (unsigned long long)(t - run_t0_));
        write(buf);
    }

    void stage_end(int idx, const char* name, double wall_us) {
        uint64_t events = stage_event_count_.load(std::memory_order_relaxed);
        char buf[128];
        snprintf(buf, sizeof(buf),
            "  STAGE_END stage=%d name=%s events=%llu wall_us=%.1f",
            idx, name,
            (unsigned long long)events, wall_us);
        write(buf);
    }

    // ── Thread assignment (parallel mode only) ─────────────────────────
    void thread_assignment(int tid, const std::vector<std::string>& names) {
        std::string csv;
        for (size_t i = 0; i < names.size(); ++i) {
            if (i) csv += ',';
            csv += names[i];
        }
        write("    THREAD " + std::to_string(tid) + " modules=" + csv);
    }

    // ── Per-packet events (gated by verbose_) ──────────────────────────
    // stage_event_count_ is always incremented regardless of verbose
    // so STAGE_END reports accurate counts even when verbose=false.

    void pkt_emit(const std::string& mod, const Event& e) {
        stage_event_count_.fetch_add(1, std::memory_order_relaxed);
        if (!verbose_) return;
        char buf[256];
        snprintf(buf, sizeof(buf),
            "    EMIT module=%s src=%d dst=%d pkt=%d hop=%d time_ns=%.1f",
            mod.c_str(), e.src_nic, e.dst_nic,
            e.payload_id, e.hop, e.timestamp);
        write(buf);
    }

    void pkt_fwd(const std::string& mod, const Event& in_ev, double time_out_ns) {
        stage_event_count_.fetch_add(1, std::memory_order_relaxed);
        if (!verbose_) return;
        char buf[256];
        snprintf(buf, sizeof(buf),
            "    FWD module=%s src=%d dst=%d pkt=%d hop=%d "
            "time_in_ns=%.1f time_out_ns=%.1f",
            mod.c_str(), in_ev.src_nic, in_ev.dst_nic,
            in_ev.payload_id, in_ev.hop,
            in_ev.timestamp, time_out_ns);
        write(buf);
    }

    void pkt_route(const std::string& mod, const Event& e) {
        stage_event_count_.fetch_add(1, std::memory_order_relaxed);
        if (!verbose_) return;
        char buf[256];
        snprintf(buf, sizeof(buf),
            "    ROUTE module=%s src=%d dst=%d pkt=%d hop=%d time_ns=%.1f",
            mod.c_str(), e.src_nic, e.dst_nic,
            e.payload_id, e.hop, e.timestamp);
        write(buf);
    }

    void pkt_egress(const std::string& mod, const Event& e) {
        stage_event_count_.fetch_add(1, std::memory_order_relaxed);
        if (!verbose_) return;
        char buf[256];
        snprintf(buf, sizeof(buf),
            "    EGRESS module=%s src=%d dst=%d pkt=%d hop=%d time_ns=%.1f",
            mod.c_str(), e.src_nic, e.dst_nic,
            e.payload_id, e.hop, e.timestamp);
        write(buf);
    }

    void pkt_delivered(const std::string& mod, const Event& e) {
        stage_event_count_.fetch_add(1, std::memory_order_relaxed);
        // Always collect record regardless of verbose flag.
        // Use the file mutex so parallel module threads don't race on the vector.
        if (verbose_) {
            char buf[256];
            snprintf(buf, sizeof(buf),
                "    DELIVERED module=%s src=%d dst=%d pkt=%d hop=%d time_ns=%.1f",
                mod.c_str(), e.src_nic, e.dst_nic,
                e.payload_id, e.hop, e.timestamp);
            std::lock_guard<std::mutex> lg(mu_);
            delivery_records_.push_back({ e.src_nic, e.dst_nic, e.payload_id, e.timestamp });
            out_ << buf << '\n';
        } else {
            std::lock_guard<std::mutex> lg(mu_);
            delivery_records_.push_back({ e.src_nic, e.dst_nic, e.payload_id, e.timestamp });
        }
    }

    // ── Comparison summary ──────────────────────────────────────────────
    void comparison_start(const std::string& scenario_label) {
        write("COMPARISON scenario=\"" + scenario_label + "\"");
    }

    void comparison_mode(const std::string& mode_label, double wall_ms,
                         uint64_t delivered, uint64_t total) {
        double rate = total > 0 ? 100.0 * delivered / total : 0.0;
        char buf[256];
        snprintf(buf, sizeof(buf),
            "  MODE %-35s wall_ms=%8.3f delivered=%llu rate=%.1f%%",
            mode_label.c_str(), wall_ms,
            (unsigned long long)delivered, rate);
        write(buf);
    }

private:
    std::ofstream             out_;
    std::mutex                mu_;
    bool                      verbose_;
    std::atomic<uint64_t>     stage_event_count_{0};
    uint64_t                  run_t0_   = 0;
    uint64_t                  stage_t0_ = 0;
    std::vector<PacketRecord> delivery_records_;

    static uint64_t now_us() {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count();
    }

    void write(const std::string& line) {
        std::lock_guard<std::mutex> lg(mu_);
        out_ << line << '\n';
    }
};
