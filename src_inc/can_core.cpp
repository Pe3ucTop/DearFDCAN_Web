// can_core.cpp - CAN processing core running in the USB worker (separate wasm module).
//
// usb_worker.js copies completed bulk transfers into the staging buffer and calls core_process().
// The core parses the gs_usb frames, maps adapter timestamps to the page timeline, maintains the
// "Table by ID" aggregates (count, cycle time from hardware timestamps) and the trace queue, and
// core_publish() emits one compact snapshot with only what changed since the previous one.
// The page never sees individual frames of a busy bus: updates of one ID coalesce into one row.
#include "can_protocol.h"
#include "can_wire.h"

#include <cstring>
#include <deque>
#include <unordered_map>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

namespace {

constexpr size_t   kStageCap            = 1u << 20; // staging records: u16 len | u16 ep | f64 host_ms | bytes
constexpr size_t   kStageRecHeader      = 12;
constexpr uint32_t kMaxTablePerPublish  = 1024;
constexpr uint32_t kMaxTracePerPublish  = 2048;     // the page keeps at most 2000 trace rows anyway
constexpr size_t   kTraceQueueMax       = 8192;     // unsent trace entries kept while the page is busy
constexpr size_t   kMaxIds              = 65536;
constexpr double   kDriftAllowance      = 1e-4;     // 100 ppm adapter vs. browser clock drift

alignas(8) uint8_t g_stage[kStageCap];
alignas(8) uint8_t g_out[sizeof(SnapshotHeader)
                         + kMaxTablePerPublish * sizeof(WireTableRecord)
                         + kMaxTracePerPublish * sizeof(TraceEntry)];

struct Slot {
    IdTableEntry entry;
    bool dirty = false;
};

// Maps the free-running 32-bit microsecond adapter clock onto the page timeline.
// The offset is the lowest (host time - adapter time) seen: host time only ever adds latency, so the
// minimum is the best estimate of the true offset. It may rise slowly (drift allowance) so that a
// systematic clock drift cannot pin it forever. Output never goes backwards.
struct ClockSync {
    bool init = false;
    uint32_t last_hw_us = 0;
    uint64_t unwrapped_us = 0;
    double offset_ms = 0.0;
    double last_host_ms = 0.0;
    double last_out_ms = 0.0;

    void Reset() { *this = ClockSync(); }

    double Map(uint32_t hw_us, double host_ms) {
        if (!init) {
            init = true;
            last_hw_us = hw_us;
            unwrapped_us = hw_us;
            offset_ms = host_ms - static_cast<double>(unwrapped_us) / 1000.0;
            last_host_ms = host_ms;
            last_out_ms = host_ms;
            return host_ms;
        }
        const uint32_t step = hw_us - last_hw_us; // wrap-safe
        if (step >= 0x80000000U) {                // clock went backwards: adapter restarted
            Reset();
            return Map(hw_us, host_ms);
        }
        last_hw_us = hw_us;
        unwrapped_us += step;
        const double hw_ms = static_cast<double>(unwrapped_us) / 1000.0;
        if (host_ms > last_host_ms) {
            offset_ms += kDriftAllowance * (host_ms - last_host_ms);
            last_host_ms = host_ms;
        }
        const double observed = host_ms - hw_ms;
        if (observed < offset_ms) offset_ms = observed;
        double t = hw_ms + offset_ms;
        if (t < last_out_ms) t = last_out_ms;
        last_out_ms = t;
        return t;
    }
};

std::unordered_map<uint64_t, Slot> g_table;
std::vector<uint64_t> g_dirty;
std::deque<TraceEntry> g_trace;
std::vector<CanMessage> g_parsed;
CanProtocolHandler g_parser;
ClockSync g_clock;

bool g_trace_enabled = true;
bool g_force_publish = false;
uint32_t g_seq = 0;
uint32_t g_frames_total = 0;
uint32_t g_trace_dropped = 0;
uint32_t g_table_overflow = 0;
uint32_t g_table_clear_ack = 0;
uint32_t g_trace_clear_ack = 0;

void OnFrame(CanMessage& m, double host_ms) {
    // Frame time: adapter clock mapped onto the page timeline when available, else completion time
    const double t = m.has_hw_timestamp ? g_clock.Map(m.hw_timestamp_us, host_ms) : host_ms;
    m.host_time_ms = t;
    ++g_frames_total;

    if (g_trace_enabled) {
        if (g_trace.size() >= kTraceQueueMax) {
            g_trace.pop_front();
            ++g_trace_dropped;
        }
        TraceEntry te;
        te.timestamp_sec = t / 1000.0;
        te.msg = m;
        g_trace.push_back(te);
    }

    const uint64_t key = MakeCanIdKey(m.id, m.is_extended);
    auto it = g_table.find(key);
    if (it == g_table.end()) {
        if (g_table.size() >= kMaxIds) {
            ++g_table_overflow;
            return;
        }
        Slot& s = g_table[key];
        s.entry.last_msg = m;
        s.entry.prev_payload = m.payload;
        s.entry.count = 1;
        s.entry.last_rx_time_ms = t;
        s.dirty = true;
        g_dirty.push_back(key);
        return;
    }

    Slot& s = it->second;
    IdTableEntry& e = s.entry;

    // Cycle time: difference of adapter timestamps (exact, immune to USB/browser jitter) when both
    // frames carry one, otherwise the difference of the frame times
    double dt;
    bool valid = true;
    if (m.has_hw_timestamp && e.last_msg.has_hw_timestamp) {
        const uint32_t d = m.hw_timestamp_us - e.last_msg.hw_timestamp_us;
        valid = d < 0x80000000U;
        dt = static_cast<double>(d) / 1000.0;
    } else {
        dt = t - e.last_rx_time_ms;
        valid = dt >= 0.0;
    }
    if (valid) {
        e.cycle_time_ms = dt;
        if (e.count == 1 || dt < e.min_cycle_ms) e.min_cycle_ms = dt;
        if (e.count == 1 || dt > e.max_cycle_ms) e.max_cycle_ms = dt;
    }
    e.prev_payload = e.last_msg.payload;
    e.last_msg = m;
    e.last_rx_time_ms = t;
    e.count++;

    if (!s.dirty) {
        s.dirty = true;
        g_dirty.push_back(key);
    }
}

} // namespace

extern "C" {

EMSCRIPTEN_KEEPALIVE uint8_t* core_stage_ptr() { return g_stage; }
EMSCRIPTEN_KEEPALIVE int core_stage_cap() { return static_cast<int>(kStageCap); }
EMSCRIPTEN_KEEPALIVE uint8_t* core_out_ptr() { return g_out; }
EMSCRIPTEN_KEEPALIVE int core_out_cap() { return static_cast<int>(sizeof(g_out)); }

// Parse `used` bytes of staged transfer records
EMSCRIPTEN_KEEPALIVE void core_process(int used) {
    if (used < 0) return;
    if (static_cast<size_t>(used) > kStageCap) used = static_cast<int>(kStageCap);
    int off = 0;
    while (off + static_cast<int>(kStageRecHeader) <= used) {
        uint16_t len = 0;
        double host_ms = 0.0;
        std::memcpy(&len, g_stage + off, sizeof(len));
        std::memcpy(&host_ms, g_stage + off + 4, sizeof(host_ms));
        off += static_cast<int>(kStageRecHeader);
        if (off + len > used) break;

        g_parsed.clear();
        g_parser.ParseRxBuffer(g_stage + off, len, g_parsed);
        for (CanMessage& m : g_parsed) OnFrame(m, host_ms);
        off += len;
    }
}

// Write everything that changed since the previous call into the out buffer.
// Returns the snapshot size in bytes, 0 if there is nothing to report.
EMSCRIPTEN_KEEPALIVE int core_publish() {
    if (g_dirty.empty() && g_trace.empty() && !g_force_publish) return 0;
    g_force_publish = false;

    uint8_t* const base = g_out;
    uint8_t* w = base + sizeof(SnapshotHeader);

    uint32_t n_table = 0;
    size_t consumed = 0;
    while (consumed < g_dirty.size() && n_table < kMaxTablePerPublish) {
        const uint64_t key = g_dirty[consumed++];
        auto it = g_table.find(key);
        if (it == g_table.end() || !it->second.dirty) continue;
        WireTableRecord rec;
        rec.key = key;
        rec.entry = it->second.entry;
        std::memcpy(w, &rec, sizeof(rec));
        w += sizeof(rec);
        it->second.dirty = false;
        ++n_table;
    }
    g_dirty.erase(g_dirty.begin(), g_dirty.begin() + static_cast<std::ptrdiff_t>(consumed));

    if (g_trace.size() > kMaxTracePerPublish) {
        const size_t skip = g_trace.size() - kMaxTracePerPublish;
        g_trace.erase(g_trace.begin(), g_trace.begin() + static_cast<std::ptrdiff_t>(skip));
        g_trace_dropped += static_cast<uint32_t>(skip);
    }
    const uint32_t n_trace = static_cast<uint32_t>(g_trace.size());
    for (const TraceEntry& te : g_trace) {
        std::memcpy(w, &te, sizeof(te));
        w += sizeof(te);
    }
    g_trace.clear();

    SnapshotHeader h;
    std::memset(&h, 0, sizeof(h));
    h.magic = kSnapshotMagic;
    h.seq = g_seq++;
    h.n_table = n_table;
    h.n_trace = n_trace;
    h.table_rec_size = sizeof(WireTableRecord);
    h.trace_rec_size = sizeof(TraceEntry);
    h.table_clear_ack = g_table_clear_ack;
    h.trace_clear_ack = g_trace_clear_ack;
    h.trace_dropped_total = g_trace_dropped;
    h.frames_total = g_frames_total;
    h.table_overflow_total = g_table_overflow;
    std::memcpy(base, &h, sizeof(h));

    return static_cast<int>(w - base);
}

// Commands from the page. Each clear is acknowledged in the following snapshots, which lets the page
// discard snapshots that were produced before the clear.
EMSCRIPTEN_KEEPALIVE void core_clear_table() {
    g_table.clear();
    g_dirty.clear();
    ++g_table_clear_ack;
    g_force_publish = true;
}

EMSCRIPTEN_KEEPALIVE void core_clear_trace() {
    g_trace.clear();
    ++g_trace_clear_ack;
    g_force_publish = true;
}

EMSCRIPTEN_KEEPALIVE void core_set_trace_enabled(int enable) {
    g_trace_enabled = enable != 0;
    if (!g_trace_enabled) g_trace.clear();
}

// Forget the adapter clock correlation (call when a CAN channel is (re)started)
EMSCRIPTEN_KEEPALIVE void core_reset_clock() { g_clock.Reset(); }

} // extern "C"
