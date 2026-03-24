#pragma once
#include <cstdint>

struct Event {
    double   timestamp;   // simulation time (ns)
    int      src_nic;     // originating NIC id
    int      dst_nic;     // destination NIC id
    int      payload_id;  // packet identifier
    int      hop;         // hop count (debug)

    bool operator>(const Event& o) const { return timestamp > o.timestamp; }
    bool operator<(const Event& o) const { return timestamp < o.timestamp; }
};
