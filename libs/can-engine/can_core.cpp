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
#include <string>
#include <unordered_map>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

namespace {

constexpr size_t   kStageCap            = 1u << 20; // staging records: u16 len | u8 dev | u8 ep | f64 host_ms | bytes
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

// Maps the free-running 32-bit microsecond adapter clock onto the page timeline. Two sources feed it:
//
//  * the frames themselves: the offset is the lowest (host time - adapter time) seen. Host time only ever adds
//    latency, so the minimum is the best estimate of the true offset. It may rise slowly (drift allowance) so that a
//    systematic clock drift cannot pin it forever.
//  * clock samples (AddSample): the worker asks the adapter for its timestamp (GS_USB_BREQ_TIMESTAMP) about once a
//    second and measures the round trip. The adapter latched its clock somewhere inside the round trip, so the
//    middle of it is the host time of the sample, uncertain by half the round trip. Samples with a long round trip
//    are discarded (the shortest ones are the accurate ones), a line through the rest gives the offset and the drift
//    of the adapter's quartz against the browser clock, and the scatter of the samples around it is the spread of
//    the synchronisation.
//
// Once three good samples exist the line is used for the frame times (never later than the moment the frame was
// handled); until then, and for adapters without samples, the frame minimum is. Output never goes backwards.
struct ClockSync {
    static constexpr int    kRing = 64;                 // samples kept (about a minute at one per second)
    static constexpr double kRttSlack = 1.5;            // a sample is good if its round trip is below min * this + kRttAddMs
    static constexpr double kRttAddMs = 0.3;
    static constexpr double kMinFitSpanMs = 4000.0;     // the drift is only fitted over at least this long a time
    static constexpr double kMaxDrift = 5e-4;           // 500 ppm: beyond that the fit is noise, not a quartz
    static constexpr int32_t kBackJitterUs = 1000000;   // timestamps this much older than the newest one are jitter, not a restart

    struct Sample { double x_ms, off_ms, rtt_ms; };

    bool init = false;            // `unwrapped_us` holds a valid reference
    bool frame_init = false;      // the frame minimum has its first observation
    uint32_t last_hw_us = 0;
    uint64_t unwrapped_us = 0;    // the newest timestamp seen (frame or sample), extended to 64 bit
    double offset_ms = 0.0;
    double last_host_ms = 0.0;
    double last_out_ms = 0.0;

    Sample ring[kRing] = {};
    int ring_n = 0, ring_head = 0;
    bool fit_valid = false;
    bool push_init = false;
    uint32_t push_last_hw = 0;
    uint64_t push_unwrapped = 0;
    bool push_mode = false;       // the samples are records the adapter pushed (PEAK calibration), not answers to a request
    double fit_a = 0.0, fit_b = 0.0;                    // offset(x) = a + b * x, x = adapter time in ms
    double st_total = 0.0, st_accepted = 0.0, st_jitter_us = 0.0, st_max_resid_us = 0.0;
    double st_rtt_min = 0.0, st_rtt_last = 0.0, st_offset_ms = 0.0, st_drift_ppm = 0.0;

    void Reset() { *this = ClockSync(); }

    // Extends a 32-bit timestamp to milliseconds relative to the first one. False: the clock went back by more than
    // the jitter allowance, i.e. the adapter restarted.
    bool Unwrap(uint32_t hw_us, double& hw_ms) {
        if (!init) {
            init = true;
            last_hw_us = hw_us;
            unwrapped_us = hw_us;
            hw_ms = static_cast<double>(unwrapped_us) / 1000.0;
            return true;
        }
        const int32_t d = static_cast<int32_t>(hw_us - last_hw_us);     // wrap-safe
        if (d >= 0) {
            last_hw_us = hw_us;
            unwrapped_us += static_cast<uint32_t>(d);
            hw_ms = static_cast<double>(unwrapped_us) / 1000.0;
            return true;
        }
        if (d > -kBackJitterUs) {                                       // a frame older than the newest sample
            hw_ms = (static_cast<double>(unwrapped_us) + static_cast<double>(d)) / 1000.0;
            return true;
        }
        return false;
    }

    double Map(uint32_t hw_us, double host_ms) {
        double hw_ms;
        if (!Unwrap(hw_us, hw_ms)) {            // clock went backwards: adapter restarted
            Reset();
            return Map(hw_us, host_ms);
        }
        if (!frame_init) {
            frame_init = true;
            offset_ms = host_ms - hw_ms;
            last_host_ms = host_ms;
            last_out_ms = host_ms;
            if (!fit_valid) return host_ms;
            last_out_ms = 0.0;
        } else {
            if (host_ms > last_host_ms) {
                // the minimum may creep up by the assumed drift; with pushed samples the measured drift of the adapter's quartz is used
                // (it is negative when the adapter runs fast), so the minimum follows the clock in both directions
                const double rate = (push_mode && fit_valid) ? fit_b : kDriftAllowance;
                offset_ms += rate * (host_ms - last_host_ms);
                last_host_ms = host_ms;
            }
            const double observed = host_ms - hw_ms;
            if (observed < offset_ms) offset_ms = observed;
        }
        double t;
        if (fit_valid && !push_mode) {
            t = hw_ms + fit_a + fit_b * hw_ms;
            if (t > host_ms) t = host_ms;       // a frame cannot be handled before it happened
        } else {
            t = hw_ms + offset_ms;
        }
        if (t < last_out_ms) t = last_out_ms;
        last_out_ms = t;
        return t;
    }

    // One answer of GS_USB_BREQ_TIMESTAMP: the adapter clock, the page time in the middle of the request and its round trip
    void AddSample(uint32_t hw_us, double host_mid_ms, double rtt_ms) {
        double x;
        if (!Unwrap(hw_us, x)) {
            Reset();
            Unwrap(hw_us, x);
        }
        if (rtt_ms < 0.0) rtt_ms = 0.0;
        push_mode = false;
        st_total += 1.0;
        st_rtt_last = rtt_ms;
        ring[ring_head] = Sample{x, host_mid_ms - x, rtt_ms};
        ring_head = (ring_head + 1) % kRing;
        if (ring_n < kRing) ++ring_n;
        Refit();
    }

    // A record the adapter pushed (PEAK calibration): its clock value and the arrival time of the transfer that carried it. There is no
    // round trip; the delay between the adapter's clock reading and the arrival is what varies, and it is only ever positive, so the
    // samples whose offset is lowest (after taking out the drift) are the accurate ones. Their excess over the lowest plays the part
    // of the round trip. The result describes the adapter's clock rate and the scatter of the delay; it is not used to place frames,
    // because the clock of these records is not guaranteed to be the one of the frame timestamps (the frame minimum keeps doing that,
    // following the measured drift).
    void AddPushSample(uint32_t hw_us, double host_ms) {
        // Own 64 bit extension: the base of these timestamps may differ from the frames' (see above), so the two must not share one
        double x;
        if (!push_init || static_cast<int32_t>(hw_us - push_last_hw) < 0) {
            if (push_init && static_cast<int32_t>(hw_us - push_last_hw) > -kBackJitterUs) return;   // a late older record: ignore
            push_init = true;                                   // first record, or the clock restarted: start over
            ring_n = ring_head = 0;
            push_unwrapped = hw_us;
        } else {
            push_unwrapped += static_cast<uint32_t>(hw_us - push_last_hw);
        }
        push_last_hw = hw_us;
        x = static_cast<double>(push_unwrapped) / 1000.0;
        push_mode = true;
        st_total += 1.0;
        ring[ring_head] = Sample{x, host_ms - x, 0.0};
        ring_head = (ring_head + 1) % kRing;
        if (ring_n < kRing) ++ring_n;
        Refit();
    }

    void Refit() {
        if (push_mode) {
            // slope of the lower envelope (the lowest offset of the older and of the newer half of the samples), then the excess over it
            auto at = [&](int i) -> Sample& { return ring[(ring_head - ring_n + i + 2 * kRing) % kRing]; };
            double b0 = 0.0;
            const int half = ring_n / 2;
            if (half >= 4) {
                int lo1 = 0, lo2 = half;
                for (int i = 0; i < half; ++i) if (at(i).off_ms < at(lo1).off_ms) lo1 = i;
                for (int i = half; i < ring_n; ++i) if (at(i).off_ms < at(lo2).off_ms) lo2 = i;
                const double dx = at(lo2).x_ms - at(lo1).x_ms;
                if (dx >= 8000.0) b0 = std::max(-kMaxDrift, std::min(kMaxDrift, (at(lo2).off_ms - at(lo1).off_ms) / dx));
            }
            double lowest = at(0).off_ms - b0 * at(0).x_ms;
            for (int i = 1; i < ring_n; ++i) lowest = std::min(lowest, at(i).off_ms - b0 * at(i).x_ms);
            for (int i = 0; i < ring_n; ++i) at(i).rtt_ms = (at(i).off_ms - b0 * at(i).x_ms) - lowest;
            st_rtt_last = at(ring_n - 1).rtt_ms;
        }
        double rtt_min = ring[0].rtt_ms;
        for (int i = 1; i < ring_n; ++i) rtt_min = std::min(rtt_min, ring[i].rtt_ms);
        st_rtt_min = rtt_min;
        // Good samples: those close to the shortest round trip, but at least the faster 40 % (a noisy host has no tight
        // cluster) and never more than a few times the shortest
        double sorted[kRing];
        for (int i = 0; i < ring_n; ++i) sorted[i] = ring[i].rtt_ms;
        const int k = (ring_n * 2) / 5;
        std::nth_element(sorted, sorted + k, sorted + ring_n);
        const double limit = std::max(rtt_min * kRttSlack + kRttAddMs, std::min(sorted[k], rtt_min * 4.0 + 1.0));

        int m = 0;
        double sx = 0.0, so = 0.0, xmin = 0.0, xmax = 0.0;
        for (int i = 0; i < ring_n; ++i) {
            const Sample& s = ring[i];
            if (s.rtt_ms > limit) continue;
            if (m == 0 || s.x_ms < xmin) xmin = s.x_ms;
            if (m == 0 || s.x_ms > xmax) xmax = s.x_ms;
            sx += s.x_ms;
            so += s.off_ms;
            ++m;
        }
        st_accepted = m;
        fit_valid = false;
        if (m == 0) return;

        const double mx = sx / m, mo = so / m;
        double b = 0.0;
        if (m >= 4 && xmax - xmin >= kMinFitSpanMs) {
            double sxx = 0.0, sxy = 0.0;
            for (int i = 0; i < ring_n; ++i) {
                const Sample& s = ring[i];
                if (s.rtt_ms > limit) continue;
                sxx += (s.x_ms - mx) * (s.x_ms - mx);
                sxy += (s.x_ms - mx) * (s.off_ms - mo);
            }
            if (sxx > 0.0) b = std::max(-kMaxDrift, std::min(kMaxDrift, sxy / sxx));
        }
        fit_a = mo - b * mx;
        fit_b = b;

        double ss = 0.0, worst = 0.0;
        for (int i = 0; i < ring_n; ++i) {
            const Sample& s = ring[i];
            if (s.rtt_ms > limit) continue;
            const double r = s.off_ms - (fit_a + fit_b * s.x_ms);
            ss += r * r;
            worst = std::max(worst, std::abs(r));
        }
        st_jitter_us = m >= 2 ? std::sqrt(ss / m) * 1000.0 : 0.0;
        st_max_resid_us = worst * 1000.0;
        st_drift_ppm = -fit_b * 1e6;        // the offset (page - adapter) falls while the adapter clock runs fast
        st_offset_ms = fit_a + fit_b * xmax;
        fit_valid = m >= 3;
    }
};

std::unordered_map<uint64_t, Slot> g_table;
std::vector<uint64_t> g_dirty;
std::deque<TraceEntry> g_trace;
std::vector<CanMessage> g_parsed;
CanProtocolType g_protocol[kMaxDevices] = {};   // protocol of every adapter (default gs_usb)
CanRxStreamState g_stream[kMaxDevices];         // records split across two transfers (PEAK uCAN)
ClockSync g_clock[kMaxDevices];          // every adapter has its own clock
uint32_t g_dev_frames[kMaxDevices] = {};

// SLCAN adapters (serial): no hardware timestamps; the lines that are not frames are command replies
std::string g_replies[kMaxDevices];             // reply tokens not yet fetched by the worker

// gs_usb adapters in Elmue mode: tx echoes carry only a marker, so the transmitted frames are remembered by marker
// (the worker stages every transmit message); bus load, text and error messages leave as event lines
bool g_elmue_ts[kMaxDevices] = {};              // the channel runs with GS_DevFlagTimestamp (4 extra bytes per message)
CanMessage g_echo[kMaxDevices][256];
std::string g_events[kMaxDevices];
double g_sync_out[9] = {};
std::vector<uint32_t> g_calibrations;           // PEAK calibration records of the transfer being parsed
bool g_rx_gate[kMaxDevices];                    // false: frames of the adapter are parsed but dropped
double g_prev_chunk_ms[kMaxDevices] = {};       // arrival time of the previous chunk of the adapter
constexpr size_t   kRepliesMax = 4096;
constexpr double   kSpreadMaxGapMs = 100.0;     // frames of a chunk are spread over at most this long a gap
uint8_t g_reply_out[kRepliesMax];
size_t g_reply_out_len = 0;
struct GateInit { GateInit() { for (bool& g : g_rx_gate) g = true; } } g_gate_init;

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
    const double t = m.has_hw_timestamp ? g_clock[m.dev].Map(m.hw_timestamp_us, host_ms) : host_ms;
    m.host_time_ms = t;
    ++g_frames_total;
    ++g_dev_frames[m.dev];

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

    // Error reports get rows of their own: a 29 bit identifier never has bit 31 set
    const uint64_t key = m.is_error ? MakeCanIdKey(m.dev, m.id | 0x80000000U, true) : MakeCanIdKey(m.dev, m.id, m.is_extended);
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
        const uint8_t dev = g_stage[off + 2];
        std::memcpy(&len, g_stage + off, sizeof(len));
        std::memcpy(&host_ms, g_stage + off + 4, sizeof(host_ms));
        off += static_cast<int>(kStageRecHeader);
        if (off + len > used) break;

        const uint8_t ep = g_stage[off - kStageRecHeader + 3];
        if (dev < kMaxDevices && !(ep & 0x80)) {
            // A message the host sent. Only the Elmue protocol needs it: the frame waits here for its echo
            if (g_protocol[dev] == PROTOCOL_GS_USB_ELMUE) {
                CanMessage sent;
                uint8_t marker = 0;
                if (CanProtocolElmue::ParseTxFrame(g_stage + off, len, sent, marker) && marker != 0) g_echo[dev][marker] = sent;
            }
        } else if (dev < kMaxDevices) {
            g_parsed.clear();
            const bool serial = g_protocol[dev] == PROTOCOL_SLCAN;
            if (g_protocol[dev] == PROTOCOL_GS_USB_ELMUE) {
                CanProtocolElmue::ParseRx(g_stage + off, len, g_elmue_ts[dev], g_parsed, &g_events[dev], g_echo[dev]);
                if (g_events[dev].size() > kRepliesMax) g_events[dev].erase(0, g_events[dev].size() - kRepliesMax);
            } else if (g_protocol[dev] == PROTOCOL_PEAK_PCAN_FD) {
                g_calibrations.clear();
                PcanExtras extras;
                extras.events = &g_events[dev];
                extras.calibrations = &g_calibrations;
                CanProtocolPcan::ParseRxStream(g_stage + off, len, g_parsed, g_stream[dev], 0, &extras);
                if (g_events[dev].size() > kRepliesMax) g_events[dev].erase(0, g_events[dev].size() - kRepliesMax);
                for (uint32_t ts : g_calibrations) g_clock[dev].AddPushSample(ts, host_ms);
            } else {
                CanProtocolHandler::ParseRx(g_protocol[dev], g_stage + off, len, g_parsed, g_stream[dev],
                                            serial ? &g_replies[dev] : nullptr);
            }
            if (g_replies[dev].size() > kRepliesMax) g_replies[dev].erase(0, g_replies[dev].size() - kRepliesMax);

            // A serial chunk holds frames that arrived since the previous chunk, not all at once: spread
            // them over that interval (in order) so cycle times are not quantized to the chunk rate
            const double prev = g_prev_chunk_ms[dev];
            const size_t n = g_parsed.size();
            const bool spread = serial && n > 1 && prev > 0.0 && host_ms > prev && host_ms - prev <= kSpreadMaxGapMs;
            if (serial) g_prev_chunk_ms[dev] = host_ms;
            size_t k = 0;
            for (CanMessage& m : g_parsed) {
                m.dev = dev;
                ++k;
                if (serial && !g_rx_gate[dev]) continue;
                OnFrame(m, spread ? prev + (host_ms - prev) * static_cast<double>(k) / static_cast<double>(n) : host_ms);
            }
        }
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
    std::memcpy(h.dev_frames, g_dev_frames, sizeof(h.dev_frames));
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

// Forget the clock correlation of one adapter (call when its CAN channel is (re)started)
EMSCRIPTEN_KEEPALIVE void core_reset_clock(int dev) {
    if (dev >= 0 && dev < kMaxDevices) g_clock[dev].Reset();
}

// Tell the core which protocol an adapter speaks (call before its first transfer is staged). Also starts
// a fresh receive stream.
EMSCRIPTEN_KEEPALIVE void core_set_protocol(int dev, int protocol) {
    if (dev < 0 || dev >= kMaxDevices) return;
    g_protocol[dev] = (protocol == PROTOCOL_PEAK_PCAN_FD || protocol == PROTOCOL_SLCAN || protocol == PROTOCOL_GS_USB_ELMUE)
                          ? static_cast<CanProtocolType>(protocol) : PROTOCOL_GS_USB_CANDLELIGHT;
    g_stream[dev].Reset();
    g_replies[dev].clear();
    g_prev_chunk_ms[dev] = 0.0;
    g_rx_gate[dev] = true;
    g_elmue_ts[dev] = false;
    g_events[dev].clear();
    for (CanMessage& e : g_echo[dev]) e = CanMessage();
}

// A gs_usb adapter that runs in Elmue mode (call before the channel is started, instead of core_set_protocol):
// `with_timestamp` tells whether the channel was started with GS_DevFlagTimestamp
EMSCRIPTEN_KEEPALIVE void core_set_elmue(int dev, int with_timestamp) {
    if (dev < 0 || dev >= kMaxDevices) return;
    core_set_protocol(dev, PROTOCOL_GS_USB_ELMUE);
    g_elmue_ts[dev] = with_timestamp != 0;
}

// Elmue: how many transmitted frames wait for their echo (staged by the worker, not yet echoed). The adapter's transmit FIFO holds a
// limited number of frames; the worker keeps the count of the frames it has put on the wire below that (staged minus still queued).
EMSCRIPTEN_KEEPALIVE int core_tx_pending(int dev) {
    if (dev < 0 || dev >= kMaxDevices) return 0;
    int n = 0;
    for (const CanMessage& e : g_echo[dev]) n += e.is_tx_echo ? 1 : 0;
    return n;
}

// Elmue: forget the frames that wait for an echo (the echoes are overdue and are taken as lost)
EMSCRIPTEN_KEEPALIVE void core_tx_forget(int dev) {
    if (dev < 0 || dev >= kMaxDevices) return;
    for (CanMessage& e : g_echo[dev]) e = CanMessage();
}

// One answer of GS_USB_BREQ_TIMESTAMP: the adapter clock (µs), the page time in the middle of the request
// and the round trip of the request (both ms, on the page timeline)
EMSCRIPTEN_KEEPALIVE void core_clock_sample(int dev, uint32_t hw_us, double host_mid_ms, double rtt_ms) {
    if (dev >= 0 && dev < kMaxDevices) g_clock[dev].AddSample(hw_us, host_mid_ms, rtt_ms);
}

// Statistics of the clock samples of an adapter as 9 doubles (valid until the next call; [8] = 1 for pushed samples, PEAK):
// [0] samples, [1] good samples, [2] offset (adapter -> page) ms, [3] drift ppm (+: the adapter clock runs fast), [4] spread (rms) µs,
// [5] shortest round trip ms, [6] last round trip ms, [7] largest deviation µs
EMSCRIPTEN_KEEPALIVE double* core_clock_stats(int dev) {
    std::memset(g_sync_out, 0, sizeof(g_sync_out));
    if (dev >= 0 && dev < kMaxDevices) {
        const ClockSync& c = g_clock[dev];
        g_sync_out[0] = c.st_total;
        g_sync_out[1] = c.st_accepted;
        g_sync_out[2] = c.st_offset_ms;
        g_sync_out[3] = c.st_drift_ppm;
        g_sync_out[4] = c.st_jitter_us;
        g_sync_out[5] = c.st_rtt_min;
        g_sync_out[6] = c.st_rtt_last;
        g_sync_out[7] = c.st_max_resid_us;
        g_sync_out[8] = c.push_mode ? 1.0 : 0.0;
    }
    return g_sync_out;
}

// Moves the pending event lines of an Elmue adapter ("U<percent>", "T<text>", "E<id>:<data>") into the reply buffer
// and returns their length; the bytes are at core_reply_ptr() until the next call.
EMSCRIPTEN_KEEPALIVE int core_take_events(int dev) {
    if (dev < 0 || dev >= kMaxDevices) return 0;
    std::string& r = g_events[dev];
    g_reply_out_len = r.size() < kRepliesMax ? r.size() : kRepliesMax;
    if (g_reply_out_len) std::memcpy(g_reply_out, r.data(), g_reply_out_len);
    r.clear();
    return static_cast<int>(g_reply_out_len);
}

// SLCAN adapters: frames are dropped while the gate is closed (the replies of commands still come through, the
// version request is answered before the CAN channel is started). USB adapters are gated by the worker.
EMSCRIPTEN_KEEPALIVE void core_set_rx_enabled(int dev, int enable) {
    if (dev < 0 || dev >= kMaxDevices) return;
    g_rx_gate[dev] = enable != 0;
    g_prev_chunk_ms[dev] = 0.0;
}

// Moves the pending reply tokens of an adapter ("K\n", "E<c>\n", "L<text>\n") into a fixed buffer and returns
// their length; the bytes are at core_reply_ptr() until the next call.
EMSCRIPTEN_KEEPALIVE int core_take_replies(int dev) {
    if (dev < 0 || dev >= kMaxDevices) return 0;
    std::string& r = g_replies[dev];
    g_reply_out_len = r.size() < kRepliesMax ? r.size() : kRepliesMax;
    if (g_reply_out_len) std::memcpy(g_reply_out, r.data(), g_reply_out_len);
    r.clear();
    return static_cast<int>(g_reply_out_len);
}

EMSCRIPTEN_KEEPALIVE uint8_t* core_reply_ptr() { return g_reply_out; }

// Drop the half-received record of an adapter's stream (call when its transfers were interrupted: the
// next transfer then starts at a record boundary again)
EMSCRIPTEN_KEEPALIVE void core_reset_stream(int dev) {
    if (dev >= 0 && dev < kMaxDevices) g_stream[dev].Reset();
}

} // extern "C"
