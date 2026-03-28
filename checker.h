#pragma once
#include "trace.h"
#include <string>
#include <vector>
#include <map>
#include <sstream>
#include <iostream>
#include <iomanip>
#include <cmath>
#include <algorithm>

// ─────────────────────────────────────────────
// CheckConfig — the config values the checker
// needs, passed directly to avoid including
// topology.h (which would create a cycle).
// ─────────────────────────────────────────────
struct CheckConfig {
    int    total_nics;
    int    num_packets;
    double nic_interval_ns;
    double link_delay_ns;
    int    num_link_hops = 2;

    int    expected_total()      const { return total_nics * num_packets; }
    // Destination formula from topology.h build_topology():
    //   dst = (src + total_nics/2) % total_nics
    int    expected_dst(int src) const { return (src + total_nics / 2) % total_nics; }
    // Expected delivery time: packet pkt_id is emitted at pkt_id*nic_interval,
    // then crosses num_link_hops LinkModules, each adding link_delay_ns.
    // EgressModules add zero delay.
    double expected_arrive(int pkt_id) const {
        return pkt_id * nic_interval_ns + num_link_hops * link_delay_ns;
    }
};

// ─────────────────────────────────────────────
// CorrectnessChecker
//
// Call add_run() after each simulator run.
// Call print_summary() at the end of the scenario.
//
// Five tests per run:
//   1. packet_conservation
//   2. no_duplicate_packets
//   3. correct_destinations
//   4. arrival_times
//   5. serial_vs_parallel_packetwise  (parallel runs only)
// ─────────────────────────────────────────────
class CorrectnessChecker {
public:
    explicit CorrectnessChecker(const CheckConfig& cfg) : cfg_(cfg) {}

    // Call once per run (after the run completes and before run_start
    // of the next run clears the tracer's delivery_records).
    void add_run(const std::string& mode,
                 const std::vector<PacketRecord>& records)
    {
        RunResult rr;
        rr.mode    = mode;
        rr.records = records;

        rr.tests.push_back(check_conservation(records));
        rr.tests.push_back(check_no_duplicates(records));
        rr.tests.push_back(check_destinations(records));
        rr.tests.push_back(check_arrival_times(records));

        // First run is the serial baseline; parallel runs compare against it
        if (serial_records_.empty() && mode == "Serial") {
            serial_records_ = records;
        } else if (!serial_records_.empty()) {
            rr.tests.push_back(check_serial_vs_parallel(mode, records));
        }

        runs_.push_back(std::move(rr));
    }

    // Write the full correctness report to both stdout and the trace file.
    void print_summary(const std::string& scenario_label, Tracer& tracer) const {
        auto out = [&](const std::string& line) {
            std::cout << line << '\n';
            tracer.log_raw(line);
        };

        const std::string bar(62, '=');
        out("");
        out(bar);
        out("CORRECTNESS VALIDATION — " + scenario_label);
        out(bar);

        // ── Per-run detail blocks ────────────────────────────────────
        for (const auto& rr : runs_) {
            out("");
            out("MODE: " + rr.mode);
            for (const auto& t : rr.tests) {
                // Inline summary line: "  test_name  ...details...  PASS/FAIL"
                std::string verdict = t.passed ? "PASS" : "FAIL";
                out("  " + pad(t.name, 32) + t.inline_summary + "  " + verdict);
                if (!t.passed && !t.failure_detail.empty())
                    out(t.failure_detail);
            }
        }

        // ── Aggregated summary table ─────────────────────────────────
        out("");
        out(bar);
        out("CORRECTNESS TEST SUMMARY");
        out(bar);

        // Collect unique test names in order
        std::vector<std::string> test_names;
        for (const auto& rr : runs_)
            for (const auto& t : rr.tests)
                if (std::find(test_names.begin(), test_names.end(), t.name)
                    == test_names.end())
                    test_names.push_back(t.name);

        bool overall = true;
        for (const auto& name : test_names) {
            bool all_pass = true;
            for (const auto& rr : runs_)
                for (const auto& t : rr.tests)
                    if (t.name == name && !t.passed) all_pass = false;
            overall &= all_pass;
            out("TEST " + pad(name, 38) + (all_pass ? "PASS" : "FAIL"));
        }

        out("");
        out("OVERALL RESULT: " + std::string(overall ? "PASS" : "FAIL"));
        out(bar);
    }

private:
    // ── Internal types ───────────────────────────────────────────────
    struct TestResult {
        std::string name;
        bool        passed        = true;
        std::string inline_summary;   // one-liner printed on the same line
        std::string failure_detail;   // indented block printed only on FAIL
    };

    struct RunResult {
        std::string             mode;
        std::vector<TestResult> tests;
        std::vector<PacketRecord> records;
    };

    CheckConfig               cfg_;
    std::vector<PacketRecord> serial_records_;
    std::vector<RunResult>    runs_;

    // ── Helpers ──────────────────────────────────────────────────────
    static std::string pad(const std::string& s, int width) {
        if ((int)s.size() >= width) return s + " ";
        return s + std::string(width - (int)s.size(), ' ');
    }

    static std::string fmt(double v) {
        char buf[32]; snprintf(buf, sizeof(buf), "%.1f", v); return buf;
    }

    // ── Test 1: packet conservation ──────────────────────────────────
    TestResult check_conservation(const std::vector<PacketRecord>& recs) const {
        int expected  = cfg_.expected_total();
        int delivered = (int)recs.size();
        int missing   = expected - delivered;
        int extra     = delivered - expected;

        std::ostringstream sum;
        sum << "expected=" << expected
            << " delivered=" << delivered
            << " missing=" << std::max(0, missing);
        if (extra > 0) sum << " extra=" << extra;

        TestResult t;
        t.name           = "packet_conservation";
        t.passed         = (missing == 0 && extra == 0);
        t.inline_summary = sum.str();
        if (!t.passed) {
            std::ostringstream d;
            d << "    expected_packets  = " << expected  << "\n"
              << "    delivered_packets = " << delivered << "\n"
              << "    missing_packets   = " << std::max(0, missing);
            if (extra > 0) d << "\n    extra_packets     = " << extra;
            t.failure_detail = d.str();
        }
        return t;
    }

    // ── Test 2: no duplicate packets ─────────────────────────────────
    TestResult check_no_duplicates(const std::vector<PacketRecord>& recs) const {
        // Key: (src_nic, pkt_id) — globally unique per packet
        std::map<std::pair<int,int>, int> counts;
        for (const auto& r : recs) counts[{r.src_nic, r.pkt_id}]++;

        std::vector<std::pair<int,int>> dupes;
        for (const auto& kv : counts)
            if (kv.second > 1) dupes.push_back(kv.first);

        std::ostringstream sum;
        sum << "records=" << recs.size()
            << " unique=" << counts.size()
            << " dupes=";
        if (dupes.empty()) {
            sum << "none";
        } else {
            sum << "[";
            for (int i = 0; i < (int)dupes.size() && i < 3; ++i) {
                if (i) sum << ",";
                sum << "(src=" << dupes[i].first << ",pkt=" << dupes[i].second << ")";
            }
            if ((int)dupes.size() > 3) sum << "...";
            sum << "]";
        }

        TestResult t;
        t.name           = "no_duplicate_packets";
        t.passed         = dupes.empty();
        t.inline_summary = sum.str();
        if (!t.passed) {
            std::ostringstream d;
            d << "    delivered_records     = " << recs.size()    << "\n"
              << "    unique_packet_ids     = " << counts.size()  << "\n"
              << "    duplicate_packet_ids  = [";
            for (int i = 0; i < (int)dupes.size() && i < 10; ++i) {
                if (i) d << ", ";
                d << "(src=" << dupes[i].first
                  << ",pkt=" << dupes[i].second
                  << ")x" << counts[{dupes[i].first, dupes[i].second}];
            }
            if ((int)dupes.size() > 10) d << " ...";
            d << "]";
            t.failure_detail = d.str();
        }
        return t;
    }

    // ── Test 3: correct destinations ─────────────────────────────────
    TestResult check_destinations(const std::vector<PacketRecord>& recs) const {
        int wrong = 0;
        std::ostringstream mismatches;
        int shown = 0;

        for (const auto& r : recs) {
            int exp = cfg_.expected_dst(r.src_nic);
            if (r.dst_nic != exp) {
                ++wrong;
                if (shown++ < 5)
                    mismatches << "    packet(src=" << r.src_nic
                               << ",pkt=" << r.pkt_id
                               << "): expected_dst=" << exp
                               << " actual_dst=" << r.dst_nic << "\n";
            }
        }

        std::ostringstream sum;
        sum << "checked=" << recs.size() << " wrong=" << wrong;

        TestResult t;
        t.name           = "correct_destinations";
        t.passed         = (wrong == 0);
        t.inline_summary = sum.str();
        if (!t.passed) {
            std::ostringstream d;
            d << "    checked_packets       = " << recs.size() << "\n"
              << "    wrong_destination_count = " << wrong    << "\n"
              << "    mismatches (first 5):\n" << mismatches.str();
            if (wrong > 5) d << "    ... and " << (wrong - 5) << " more\n";
            t.failure_detail = d.str();
        }
        return t;
    }

    // ── Test 4: arrival times ─────────────────────────────────────────
    // Expected latency = num_link_hops × link_delay_ns.
    // EgressModules add zero delay — verified by reading module.h.
    // Rule: exact equality (epsilon = 1e-6 ns).
    TestResult check_arrival_times(const std::vector<PacketRecord>& recs) const {
        constexpr double kEpsilon = 1e-6;
        double expected_latency = cfg_.num_link_hops * cfg_.link_delay_ns;

        int mismatches = 0;
        std::ostringstream detail;
        int shown = 0;

        for (const auto& r : recs) {
            double expected = cfg_.expected_arrive(r.pkt_id);
            if (std::abs(r.delivered_time_ns - expected) > kEpsilon) {
                ++mismatches;
                if (shown++ < 5)
                    detail << "    packet(src=" << r.src_nic
                           << ",pkt=" << r.pkt_id
                           << "): expected=" << fmt(expected)
                           << " actual=" << fmt(r.delivered_time_ns) << "\n";
            }
        }

        std::ostringstream sum;
        sum << "checked=" << recs.size()
            << " mismatches=" << mismatches
            << " rule=emit+" << fmt(expected_latency) << "ns";

        TestResult t;
        t.name           = "arrival_times";
        t.passed         = (mismatches == 0);
        t.inline_summary = sum.str();
        if (!t.passed) {
            std::ostringstream d;
            d << "    tolerance_rule        = exact equality (epsilon=1e-6 ns)\n"
              << "    expected_latency      = " << fmt(expected_latency) << " ns"
              << "  (= " << cfg_.num_link_hops << " x link_delay = "
              << cfg_.num_link_hops << " x "
              << fmt(cfg_.link_delay_ns) << " ns)\n"
              << "    checked_packets       = " << recs.size()   << "\n"
              << "    timing_mismatches     = " << mismatches    << "\n"
              << "    mismatches (first 5):\n" << detail.str();
            if (mismatches > 5) d << "    ... and " << (mismatches - 5) << " more\n";
            t.failure_detail = d.str();
        }
        return t;
    }

    // ── Test 5: serial vs parallel packet-by-packet ───────────────────
    TestResult check_serial_vs_parallel(
            const std::string& parallel_mode,
            const std::vector<PacketRecord>& parallel_recs) const
    {
        // Build lookup: (src, pkt_id) → record
        std::map<std::pair<int,int>, const PacketRecord*> serial_map;
        for (const auto& r : serial_records_)
            serial_map[{r.src_nic, r.pkt_id}] = &r;

        std::map<std::pair<int,int>, const PacketRecord*> parallel_map;
        for (const auto& r : parallel_recs)
            parallel_map[{r.src_nic, r.pkt_id}] = &r;

        constexpr double kEpsilon = 1e-6;
        int mismatches = 0;
        std::ostringstream detail;
        int shown = 0;

        // Check every serial packet against parallel
        for (const auto& sr : serial_records_) {
            auto it = parallel_map.find({sr.src_nic, sr.pkt_id});
            if (it == parallel_map.end()) {
                ++mismatches;
                if (shown++ < 5)
                    detail << "    packet(src=" << sr.src_nic
                           << ",pkt=" << sr.pkt_id
                           << "): present in serial, missing in parallel\n";
                continue;
            }
            const auto& pr = *it->second;
            bool dst_ok  = (sr.dst_nic == pr.dst_nic);
            bool time_ok = (std::abs(sr.delivered_time_ns - pr.delivered_time_ns) < kEpsilon);
            if (!dst_ok || !time_ok) {
                ++mismatches;
                if (shown++ < 5) {
                    detail << "    packet(src=" << sr.src_nic
                           << ",pkt=" << sr.pkt_id << "): "
                           << "serial=(dst=" << sr.dst_nic
                           << ",time=" << fmt(sr.delivered_time_ns) << ") "
                           << "parallel=(dst=" << pr.dst_nic
                           << ",time=" << fmt(pr.delivered_time_ns) << ")\n";
                }
            }
        }

        // Also flag packets present in parallel but not in serial
        for (const auto& pr : parallel_recs) {
            if (serial_map.find({pr.src_nic, pr.pkt_id}) == serial_map.end()) {
                ++mismatches;
                if (shown++ < 5)
                    detail << "    packet(src=" << pr.src_nic
                           << ",pkt=" << pr.pkt_id
                           << "): present in parallel, missing in serial\n";
            }
        }

        std::ostringstream sum;
        sum << "serial=" << serial_records_.size()
            << " parallel=" << parallel_recs.size()
            << " mismatches=" << mismatches;

        TestResult t;
        t.name           = "serial_vs_parallel_packetwise";
        t.passed         = (mismatches == 0);
        t.inline_summary = sum.str();
        if (!t.passed) {
            std::ostringstream d;
            d << "    serial_packets        = " << serial_records_.size()  << "\n"
              << "    parallel_packets      = " << parallel_recs.size()    << "\n"
              << "    mismatched_packets    = " << mismatches               << "\n"
              << "    mismatches (first 5):\n"  << detail.str();
            if (mismatches > 5) d << "    ... and " << (mismatches - 5) << " more\n";
            t.failure_detail = d.str();
        }
        return t;
    }
};
