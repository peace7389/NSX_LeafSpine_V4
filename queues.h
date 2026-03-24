#pragma once
#include "event.h"
#include <queue>
#include <vector>
#include <mutex>
#include <optional>
#include <atomic>
#include <limits>
#include <cstring>

// ─────────────────────────────────────────────
// Thread-safe FIFO queue
// ─────────────────────────────────────────────
class FIFOQueue {
public:
    void push(const Event& e) {
        std::lock_guard<std::mutex> lg(mu_);
        q_.push(e);
    }

    std::optional<Event> pop() {
        std::lock_guard<std::mutex> lg(mu_);
        if (q_.empty()) return std::nullopt;
        Event e = q_.front(); q_.pop();
        return e;
    }

    std::optional<Event> peek() const {
        std::lock_guard<std::mutex> lg(mu_);
        if (q_.empty()) return std::nullopt;
        return q_.front();
    }

    bool empty() const {
        std::lock_guard<std::mutex> lg(mu_);
        return q_.empty();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lg(mu_);
        return q_.size();
    }

private:
    mutable std::mutex    mu_;
    std::queue<Event>     q_;
};

// ─────────────────────────────────────────────
// Thread-safe min-heap priority queue
// ─────────────────────────────────────────────
class PriorityQueue {
    using MinHeap = std::priority_queue<Event,
                                        std::vector<Event>,
                                        std::greater<Event>>;
public:
    void push(const Event& e) {
        std::lock_guard<std::mutex> lg(mu_);
        pq_.push(e);
    }

    std::optional<Event> pop() {
        std::lock_guard<std::mutex> lg(mu_);
        if (pq_.empty()) return std::nullopt;
        Event e = pq_.top(); pq_.pop();
        return e;
    }

    std::optional<Event> peek() const {
        std::lock_guard<std::mutex> lg(mu_);
        if (pq_.empty()) return std::nullopt;
        return pq_.top();
    }

    bool empty() const {
        std::lock_guard<std::mutex> lg(mu_);
        return pq_.empty();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lg(mu_);
        return pq_.size();
    }

private:
    mutable std::mutex mu_;
    MinHeap            pq_;
};

// ─────────────────────────────────────────────
// FlexQueue — FIFO for low fan-in,
//             FIFO + priority for high fan-in
//
// The MET value represents the "minimum enqueue
// time" promise this channel makes to downstream
// consumers (Section 3.3 of NSX paper).
// ─────────────────────────────────────────────
constexpr int FLEX_FAN_IN_THRESHOLD = 4;

class FlexQueue {
public:
    static uint64_t double_to_bits(double v) {
        uint64_t b; memcpy(&b, &v, 8); return b;
    }
    static double bits_to_double(uint64_t b) {
        double v; memcpy(&v, &b, 8); return v;
    }

    explicit FlexQueue(int fan_in)
        : fan_in_(fan_in),
          use_priority_(fan_in > FLEX_FAN_IN_THRESHOLD),
          met_(double_to_bits(std::numeric_limits<double>::infinity()))
    {}

    void push(const Event& e) {
        if (use_priority_) pq_.push(e);
        else               fifo_.push(e);
    }

    std::optional<Event> pop() {
        if (use_priority_) return pq_.pop();
        return fifo_.pop();
    }

    std::optional<Event> peek() const {
        if (use_priority_) return pq_.peek();
        return fifo_.peek();
    }

    bool empty() const {
        if (use_priority_) return pq_.empty();
        return fifo_.empty();
    }

    double get_met() const {
        return bits_to_double(met_.load(std::memory_order_acquire));
    }

    void set_met(double v) {
        met_.store(double_to_bits(v), std::memory_order_release);
    }

    bool uses_priority() const { return use_priority_; }

private:
    int            fan_in_;
    bool           use_priority_;
    FIFOQueue      fifo_;
    PriorityQueue  pq_;
    std::atomic<uint64_t> met_;
};
