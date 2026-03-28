#pragma once
#include "topology.h"
#include "trace.h"
#include <vector>
#include <thread>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <functional>
#include <numeric>

static constexpr const char* kStageNames[] = {
    "NIC_MODULES",    // 0
    "NIC_UPLINKS",    // 1
    "TOR_FWD",        // 2  (2-tier: LEAF_FWD)
    "TOR_TO_AGG",     // 3  (2-tier: SPINE_UPLINKS)
    "AGG_FWD_UP",     // 4  (2-tier: SPINE_FWD)
    "AGG_TO_CORE",    // 5  (3-tier only)
    "CORE_FWD",       // 6  (3-tier only / 2-tier: SPINE_EGRESS)
    "CORE_EGRESS",    // 7  (3-tier only / 2-tier: LEAF_EGRESS)
    "AGG_FWD_DOWN",   // 8  (3-tier only)
    "AGG_TO_TOR",     // 9  (3-tier only)
    "TOR_EGRESS",     // 10 (3-tier only)
};
static constexpr int kNumStageNames = 11;

// ─────────────────────────────────────────────
// SimResult — timing + event counts
// ─────────────────────────────────────────────
struct SimResult {
    double   wall_ms     = 0.0;
    uint64_t events_sent = 0;
    std::string label;
};

// ─────────────────────────────────────────────
// Simulator
// Implements:
//   run_serial()   — one module at a time per tick
//   run_parallel() — all modules in a stage in
//                    parallel threads per tick
// Both follow the same systolic stage order.
// ─────────────────────────────────────────────
class Simulator {
public:
    explicit Simulator(Topology& topo, int ticks)
        : topo_(topo), ticks_(ticks) {}

    void set_tracer(Tracer* t) {
        tracer_ = t;
        if (!t) return;
        for (auto& stage : topo_.stages)
            for (Module* m : stage)
                m->set_tracer(t);
    }

    // ── Serial ──────────────────────────────
    SimResult run_serial() {
        reset_topology();
        auto t0 = now();
        if (tracer_) tracer_->run_start("Serial", ticks_);

        for (int tick = 0; tick < ticks_; ++tick) {
            double until = tick_time(tick);
            if (tracer_) tracer_->tick_start(tick, until);
            int si = 0;
            for (auto& stage : topo_.stages) {
                const char* sname = si < kNumStageNames ? kStageNames[si] : "UNKNOWN";
                if (tracer_) tracer_->stage_start(si, sname,
                                                   static_cast<int>(stage.size()));
                auto sw = now();
                for (Module* m : stage)
                    m->process(until);
                if (tracer_) tracer_->stage_end(si, sname, elapsed_us(sw));
                ++si;
            }
            if (tracer_) tracer_->tick_end(tick);
        }

        double ms = elapsed_ms(t0);
        uint64_t delivered = count_delivered();
        uint64_t total = (uint64_t)topo_.cfg.total_nics() * topo_.cfg.num_packets;
        if (tracer_) tracer_->run_end("Serial", ms, delivered, total);
        return { ms, delivered, "Serial" };
    }

    // ── Parallel ────────────────────────────
    // All modules within a stage execute in parallel threads.
    // Stages are still serialized (barrier between stages).
    // This mirrors NSX's "systolic kernel launch order":
    // threads = SIMT warps, stages = kernel invocations.
    SimResult run_parallel(int num_threads = 0) {
        const bool hw_mode = num_threads <= 0;
        if (hw_mode)
            num_threads = static_cast<int>(std::thread::hardware_concurrency());
        num_threads = std::max(1, num_threads);

        std::string label = hw_mode
            ? "Parallel(hw threads=" + std::to_string(num_threads) + ")"
            : "Parallel(" + std::to_string(num_threads) + " threads)";
        reset_topology();
        auto t0 = now();
        if (tracer_) tracer_->run_start(label, ticks_);

        // Flatten all (stage_idx, module_ptr) pairs
        // so we can dispatch round-robin to threads
        for (int tick = 0; tick < ticks_; ++tick) {
            double until = tick_time(tick);
            if (tracer_) tracer_->tick_start(tick, until);
            int si = 0;

            for (auto& stage : topo_.stages) {
                if (stage.empty()) { ++si; continue; }

                int n = static_cast<int>(stage.size());
                int actual_threads = std::min(num_threads, n);
                const char* sname = si < kNumStageNames ? kStageNames[si] : "UNKNOWN";

                if (tracer_) tracer_->stage_start(si, sname, n);
                auto sw = now();

                if (actual_threads == 1) {
                    for (Module* m : stage) m->process(until);
                } else {
                    // Partition stage across threads
                    std::vector<std::thread> workers;
                    workers.reserve(actual_threads);
                    int chunk = (n + actual_threads - 1) / actual_threads;

                    for (int t = 0; t < actual_threads; ++t) {
                        int start = t * chunk;
                        int end   = std::min(start + chunk, n);
                        if (start >= n) break;
                        if (tracer_) {
                            std::vector<std::string> names;
                            names.reserve(end - start);
                            for (int i = start; i < end; ++i)
                                names.push_back(stage[i]->name());
                            tracer_->thread_assignment(t, names);
                        }
                        workers.emplace_back([&, start, end, until]() {
                            for (int i = start; i < end; ++i)
                                stage[i]->process(until);
                        });
                    }
                    for (auto& w : workers) w.join();
                    // ↑ Implicit barrier: all threads in stage finish
                    //   before next stage begins — same as NSX's
                    //   "decentralized MET" synchronization point.
                }

                if (tracer_) tracer_->stage_end(si, sname, elapsed_us(sw));
                ++si;
            }
            if (tracer_) tracer_->tick_end(tick);
        }

        double ms = elapsed_ms(t0);
        uint64_t delivered = count_delivered();
        uint64_t total = (uint64_t)topo_.cfg.total_nics() * topo_.cfg.num_packets;
        if (tracer_) tracer_->run_end(label, ms, delivered, total);
        return { ms, delivered, label };
    }

private:
    Topology& topo_;
    int       ticks_;
    Tracer*   tracer_ = nullptr;

    double tick_time(int tick) const {
        // Give the last injected packet enough simulated time to traverse
        // the full end-to-end path within the current tick.
        return (tick + 1) * topo_.cfg.nic_interval * topo_.cfg.num_packets
             + topo_.cfg.num_link_hops() * topo_.cfg.link_delay;
    }

    uint64_t count_delivered() const {
        uint64_t total = 0;
        for (auto& sq : topo_.sink_queues) {
            // peek repeatedly — non-destructive count
            FlexQueue* q = sq.get();
            // We can't call size() directly; drain into count
            auto ev = q->pop();
            while (ev) { ++total; ev = q->pop(); }
        }
        return total;
    }

    void reset_topology() {
        // Drain all queues and reset module times
        for (auto& q : topo_.queues)      q->clear();
        for (auto& q : topo_.sink_queues) q->clear();
        // Reset MET to infinity
        double inf = std::numeric_limits<double>::infinity();
        for (auto& q : topo_.queues)      q->set_met(inf);
        for (auto& q : topo_.sink_queues) q->set_met(inf);
        for (auto& stage : topo_.stages)
            for (Module* m : stage)
                m->reset();
    }

    using Clock    = std::chrono::high_resolution_clock;
    using TimePoint = Clock::time_point;
    static TimePoint now() { return Clock::now(); }
    static double elapsed_ms(TimePoint t0) {
        return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    }
    static double elapsed_us(TimePoint t0) {
        return std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
    }
};
