#pragma once
#include "topology.h"
#include <vector>
#include <thread>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <functional>
#include <numeric>

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

    // ── Serial ──────────────────────────────
    SimResult run_serial() {
        reset_topology();
        auto t0 = now();

        for (int tick = 0; tick < ticks_; ++tick) {
            double until = tick_time(tick);
            for (auto& stage : topo_.stages)
                for (Module* m : stage)
                    m->process(until);
        }

        double ms = elapsed_ms(t0);
        return { ms, count_delivered(), "Serial" };
    }

    // ── Parallel ────────────────────────────
    // All modules within a stage execute in parallel threads.
    // Stages are still serialized (barrier between stages).
    // This mirrors NSX's "systolic kernel launch order":
    // threads = SIMT warps, stages = kernel invocations.
    SimResult run_parallel(int num_threads = 0) {
        if (num_threads <= 0)
            num_threads = static_cast<int>(std::thread::hardware_concurrency());
        num_threads = std::max(1, num_threads);

        reset_topology();
        auto t0 = now();

        // Flatten all (stage_idx, module_ptr) pairs
        // so we can dispatch round-robin to threads
        for (int tick = 0; tick < ticks_; ++tick) {
            double until = tick_time(tick);

            for (auto& stage : topo_.stages) {
                if (stage.empty()) continue;

                int n = static_cast<int>(stage.size());
                int actual_threads = std::min(num_threads, n);

                if (actual_threads == 1) {
                    for (Module* m : stage) m->process(until);
                    continue;
                }

                // Partition stage across threads
                std::vector<std::thread> workers;
                workers.reserve(actual_threads);
                int chunk = (n + actual_threads - 1) / actual_threads;

                for (int t = 0; t < actual_threads; ++t) {
                    int start = t * chunk;
                    int end   = std::min(start + chunk, n);
                    if (start >= n) break;
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
        }

        double ms = elapsed_ms(t0);
        return { ms, count_delivered(), "Parallel(" + std::to_string(num_threads) + " threads)" };
    }

private:
    Topology& topo_;
    int       ticks_;

    double tick_time(int tick) const {
        return (tick + 1) * topo_.cfg.nic_interval * topo_.cfg.num_packets;
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
        for (auto& q : topo_.queues)    while (!q->empty()) q->pop();
        for (auto& q : topo_.sink_queues) while (!q->empty()) q->pop();
        // Reset MET to infinity
        double inf = std::numeric_limits<double>::infinity();
        for (auto& q : topo_.queues)      q->set_met(inf);
        for (auto& q : topo_.sink_queues) q->set_met(inf);
    }

    using Clock    = std::chrono::high_resolution_clock;
    using TimePoint = Clock::time_point;
    static TimePoint now() { return Clock::now(); }
    static double elapsed_ms(TimePoint t0) {
        return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    }
};
