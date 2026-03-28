#pragma once
#include "event.h"
#include "queues.h"
#include "trace.h"
#include <vector>
#include <string>
#include <functional>
#include <limits>
#include <atomic>
#include <cstdint>
#include <cstring>

// ─────────────────────────────────────────────
// Abstract Module (NSX "granular module execution")
// Each module owns its input FlexQueues.
// Output is handled by writing to downstream
// modules' input queues directly.
// ─────────────────────────────────────────────
class Module {
public:
    explicit Module(std::string name) : name_(std::move(name)), cur_time_(0.0) {}
    virtual ~Module() = default;

    virtual void process(double until_time) = 0;
    virtual void reset() { cur_time_ = 0.0; }

    // Downstream modules this module forwards events to
    void add_downstream(FlexQueue* q) { out_queues_.push_back(q); }
    void add_in_queue(FlexQueue* q)   { in_queues_.push_back(q);  }

    const std::string& name() const { return name_; }
    double cur_time()         const { return cur_time_; }

    void set_tracer(Tracer* t) { tracer_ = t; }

    // MET of this module = min MET over all its input queues
    double compute_met() const {
        double m = std::numeric_limits<double>::infinity();
        for (auto* q : in_queues_) m = std::min(m, q->get_met());
        return m;
    }

protected:
    std::string              name_;
    double                   cur_time_;
    Tracer*                  tracer_ = nullptr;
    std::vector<FlexQueue*>  in_queues_;   // owned by topology builder
    std::vector<FlexQueue*>  out_queues_;  // downstream queues (not owned)

    void emit(const Event& e) {
        for (auto* q : out_queues_) q->push(e);
    }
};

// ─────────────────────────────────────────────
// NICModule — packet source
// Generates `num_packets` packets at regular
// intervals starting at time 0.
// ─────────────────────────────────────────────
class NICModule : public Module {
public:
    NICModule(int id, int num_packets, double interval_ns, int dst_nic,
              FlexQueue* uplink_q)
        : Module("NIC-" + std::to_string(id)),
          id_(id), num_packets_(num_packets),
          interval_(interval_ns), dst_nic_(dst_nic),
          uplink_q_(uplink_q), next_seq_(0)
    {}

    void process(double until_time) override {
        // Generate pending packets up to until_time
        while (next_seq_ < num_packets_) {
            double ts = next_seq_ * interval_;
            if (ts > until_time) break;
            Event e{ ts, id_, dst_nic_, next_seq_, 0 };
            uplink_q_->push(e);
            if (tracer_) tracer_->pkt_emit(name_, e);
            cur_time_ = ts;
            ++next_seq_;
        }
        // Set MET promise: next packet timestamp
        double next_ts = (next_seq_ < num_packets_)
                         ? next_seq_ * interval_
                         : std::numeric_limits<double>::infinity();
        uplink_q_->set_met(next_ts);
    }

    void reset() override {
        Module::reset();
        next_seq_ = 0;
    }

private:
    int         id_, num_packets_, dst_nic_, next_seq_;
    double      interval_;
    FlexQueue*  uplink_q_;
};

// ─────────────────────────────────────────────
// LinkModule — propagation delay
// Reads from in_queue, adds delay, writes to
// out_queue.
// ─────────────────────────────────────────────
class LinkModule : public Module {
public:
    LinkModule(std::string name, double delay_ns,
               FlexQueue* in_q, FlexQueue* out_q)
        : Module(std::move(name)),
          delay_(delay_ns), in_q_(in_q), out_q_(out_q)
    {
        add_in_queue(in_q_);
    }

    void process(double until_time) override {
        double met = compute_met();
        auto ev = in_q_->pop();
        while (ev && ev->timestamp <= until_time) {
            Event fwd = *ev;
            fwd.timestamp += delay_;
            fwd.hop++;
            out_q_->push(fwd);
            if (tracer_) tracer_->pkt_fwd(name_, *ev, fwd.timestamp);
            cur_time_ = ev->timestamp;
            ev = in_q_->pop();
        }
        // Promise: earliest future event + delay
        out_q_->set_met(met + delay_);
    }

    void reset() override {
        Module::reset();
    }

private:
    double      delay_;
    FlexQueue*  in_q_;
    FlexQueue*  out_q_;
};

// ─────────────────────────────────────────────
// SwitchForwardingModule
// High fan-in switch core — uses FlexQueue
// (FIFO or FIFO+priority depending on fan_in).
// Routes by destination: picks output port.
// ─────────────────────────────────────────────
class SwitchForwardingModule : public Module {
public:
    // route_fn: given dst_nic, returns output FlexQueue* to use
    using RouteFn = std::function<FlexQueue*(int dst_nic)>;

    SwitchForwardingModule(std::string name,
                           std::vector<FlexQueue*> in_queues,
                           RouteFn route_fn)
        : Module(std::move(name)),
          flex_q_(static_cast<int>(in_queues.size())),
          route_fn_(std::move(route_fn))
    {
        for (auto* q : in_queues) add_in_queue(q);
    }

    void process(double until_time) override {
        // Step 1: drain all ingress queues into flex_q_
        for (auto* q : in_queues_) {
            auto ev = q->pop();
            while (ev) {
                flex_q_.push(*ev);
                ev = q->pop();
            }
        }

        // Step 2: process flex_q_ in timestamp order up to until_time
        double met = compute_met();
        auto ev = flex_q_.pop();
        while (ev && ev->timestamp <= until_time) {
            FlexQueue* out = route_fn_(ev->dst_nic);
            if (out) {
                Event fwd = *ev;
                fwd.hop++;
                out->push(fwd);
                if (tracer_) tracer_->pkt_route(name_, fwd);
            }
            cur_time_ = ev->timestamp;
            ev = flex_q_.pop();
        }
        if (ev) flex_q_.push(*ev);  // put back unprocessed

        // Propagate MET to all output queues
        for (auto* q : out_queues_) q->set_met(met);
    }

    void reset() override {
        Module::reset();
        flex_q_.clear();
        flex_q_.set_met(std::numeric_limits<double>::infinity());
    }

private:
    FlexQueue  flex_q_;  // internal merge queue (the "flex" part)
    RouteFn    route_fn_;
};

// ─────────────────────────────────────────────
// EgressModule — simple per-port output buffer
// Forwards everything it receives (acts as
// an egress buffer / scheduler placeholder).
// ─────────────────────────────────────────────
class EgressModule : public Module {
public:
    EgressModule(std::string name, FlexQueue* in_q, FlexQueue* out_q,
                 bool is_sink = false)
        : Module(std::move(name)), in_q_(in_q), out_q_(out_q), is_sink_(is_sink)
    {
        add_in_queue(in_q_);
    }

    void process(double until_time) override {
        double met = compute_met();
        auto ev = in_q_->pop();
        while (ev && ev->timestamp <= until_time) {
            out_q_->push(*ev);
            if (tracer_) {
                if (is_sink_) tracer_->pkt_delivered(name_, *ev);
                else          tracer_->pkt_egress(name_, *ev);
            }
            cur_time_ = ev->timestamp;
            ev = in_q_->pop();
        }
        out_q_->set_met(met);
    }

    void reset() override {
        Module::reset();
    }

private:
    FlexQueue* in_q_;
    FlexQueue* out_q_;
    bool       is_sink_;
};
