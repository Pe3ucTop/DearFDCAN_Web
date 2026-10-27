#pragma once
// Protocol independent CAN definitions shared by every USB-CAN backend (can_protocol_gsusb.h, ...), the
// worker core and the UI: frame/message types, bit-timing helpers, DLC helpers.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

// Maximum number of adapters handled at once. A device number (slot) is 0..kMaxDevices-1; the UI shows it 1-based.
constexpr int kMaxDevices = 8;

// Supported USB-CAN hardware protocol backends
enum CanProtocolType : int {
    PROTOCOL_GS_USB_CANDLELIGHT = 0
};

// SocketCAN style identifier masks
constexpr uint32_t CAN_ID_EFF_MASK = 0x1FFFFFFFU;
constexpr uint32_t CAN_ID_SFF_MASK = 0x000007FFU;

#pragma pack(push, 1)
// Generic bit-timing parameters (identical to the gs_usb wire format of BITTIMING / DATA_BITTIMING)
struct CanBittiming {
    uint32_t prop_seg;   // Propagation segment in tq
    uint32_t phase_seg1; // Phase buffer segment 1 in tq
    uint32_t phase_seg2; // Phase buffer segment 2 in tq
    uint32_t sjw;        // Synchronization jump width in tq
    uint32_t brp;        // Bit-rate prescaler (clock divider)
};
#pragma pack(pop)

static_assert(sizeof(CanBittiming) == 20, "CanBittiming is a wire format");

// Computed timing metrics for UI display
struct CanTimingMetrics {
    double tq_ns;            // Duration of 1 Time Quantum in nanoseconds
    uint32_t total_tq;       // Total Time Quanta per bit (1 + prop + seg1 + seg2)
    uint32_t tseg1_total;    // Combined TSEG1 (prop_seg + phase_seg1)
    double actual_bitrate;   // Resulting bitrate in bps
    double sample_point_pct; // Sample point position in %
};

// Fixed-capacity payload (CAN FD maximum is 64 bytes) with a std::vector-like surface.
// It is trivially copyable, so frames can travel between the worker and page wasm modules by memcpy
// and a frame never allocates on the heap.
struct CanPayload {
    static constexpr size_t kCapacity = 64;

    CanPayload() = default;
    CanPayload(const std::vector<uint8_t>& v) { assign(v.data(), v.data() + v.size()); }
    CanPayload& operator=(const std::vector<uint8_t>& v) { assign(v.data(), v.data() + v.size()); return *this; }

    size_t size() const { return len_; }
    bool empty() const { return len_ == 0; }
    void clear() { len_ = 0; }

    uint8_t& operator[](size_t i) { return bytes_[i]; }
    const uint8_t& operator[](size_t i) const { return bytes_[i]; }
    uint8_t* data() { return bytes_; }
    const uint8_t* data() const { return bytes_; }
    const uint8_t* begin() const { return bytes_; }
    const uint8_t* end() const { return bytes_ + len_; }

    void assign(const uint8_t* first, const uint8_t* last) {
        const size_t n = last > first ? static_cast<size_t>(last - first) : 0;
        len_ = static_cast<uint8_t>(n < kCapacity ? n : kCapacity);
        if (len_) std::memcpy(bytes_, first, len_);
    }

    void resize(size_t n, uint8_t fill = 0) {
        if (n > kCapacity) n = kCapacity;
        for (size_t i = len_; i < n; ++i) bytes_[i] = fill;
        len_ = static_cast<uint8_t>(n);
    }

    bool operator==(const CanPayload& o) const { return len_ == o.len_ && (len_ == 0 || std::memcmp(bytes_, o.bytes_, len_) == 0); }
    bool operator!=(const CanPayload& o) const { return !(*this == o); }

private:
    uint8_t bytes_[kCapacity] = {};
    uint8_t len_ = 0;
};

// Structure representing a parsed CAN / CAN FD message (plain data: no heap, trivially copyable)
struct CanMessage {
    uint32_t id = 0;
    uint32_t hw_timestamp_us = 0;   // adapter clock, valid if has_hw_timestamp
    double host_time_ms = 0.0;      // frame time in the page's performance.now() timeline (0 = unknown)
    bool is_extended = false;
    bool is_rtr = false;
    bool is_fd = false;
    bool is_brs = false;
    bool is_esi = false;
    bool is_tx_echo = false;
    bool has_hw_timestamp = false;
    uint8_t dlc = 0;
    uint8_t dev = 0;                // device number (slot) the frame was received from / is meant for
    CanPayload payload;
};

// Aggregated entry for the "Table by ID" window (PCAN-View Receive Table style)
struct IdTableEntry {
    CanMessage last_msg;
    CanPayload prev_payload;
    uint64_t count = 0;
    double last_rx_time_ms = 0.0;
    double cycle_time_ms = 0.0;
    double min_cycle_ms = 0.0;
    double max_cycle_ms = 0.0;
};

// Chronological trace entry for the Linear Receive window
struct TraceEntry {
    double timestamp_sec = 0.0;
    CanMessage msg;
};

static_assert(std::is_trivially_copyable<CanPayload>::value, "CanPayload must be memcpy-able");
static_assert(std::is_trivially_copyable<CanMessage>::value, "CanMessage must be memcpy-able");
static_assert(std::is_trivially_copyable<IdTableEntry>::value, "IdTableEntry must be memcpy-able");
static_assert(std::is_trivially_copyable<TraceEntry>::value, "TraceEntry must be memcpy-able");

// DLC and bit-timing helpers that do not depend on the USB protocol
namespace CanProtocolUtils {

    inline uint8_t DlcToBytes(uint8_t dlc) {
        static const uint8_t lookup[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64};
        return lookup[dlc & 0x0F];
    }

    inline uint8_t BytesToDlc(uint8_t bytes) {
        if (bytes <= 8) return bytes;
        if (bytes <= 12) return 9;
        if (bytes <= 16) return 10;
        if (bytes <= 20) return 11;
        if (bytes <= 24) return 12;
        if (bytes <= 32) return 13;
        if (bytes <= 48) return 14;
        return 15;
    }

    inline CanTimingMetrics ComputeTimingMetrics(uint32_t fclk_hz, const CanBittiming& bt) {
        CanTimingMetrics m = {0.0, 0, 0, 0.0, 0.0};
        if (fclk_hz == 0 || bt.brp == 0) return m;

        m.tseg1_total = bt.prop_seg + bt.phase_seg1;
        m.total_tq = 1U + m.tseg1_total + bt.phase_seg2;
        if (m.total_tq == 0) return m;

        m.tq_ns = (static_cast<double>(bt.brp) * 1e9) / static_cast<double>(fclk_hz);
        m.actual_bitrate = static_cast<double>(fclk_hz) / (static_cast<double>(bt.brp) * static_cast<double>(m.total_tq));
        m.sample_point_pct = (static_cast<double>(1U + m.tseg1_total) / static_cast<double>(m.total_tq)) * 100.0;

        return m;
    }

    inline bool AutoCalculateBittiming(uint32_t fclk_hz, uint32_t target_bitrate_bps, float sample_point_pct, CanBittiming& out_bt) {
        if (fclk_hz == 0 || target_bitrate_bps == 0) return false;

        double best_error = 1e18;
        uint32_t best_brp = 1;
        uint32_t best_total_tq = 16;

        for (uint32_t total_tq = 8; total_tq <= 80; ++total_tq) {
            uint32_t brp = static_cast<uint32_t>(std::round(static_cast<double>(fclk_hz) / (static_cast<double>(target_bitrate_bps) * total_tq)));
            if (brp < 1 || brp > 1024) continue;

            double actual_bps = static_cast<double>(fclk_hz) / (static_cast<double>(brp) * total_tq);
            double rate_err = std::abs(actual_bps - static_cast<double>(target_bitrate_bps));
            double tq_penalty = std::abs(static_cast<int>(total_tq) - 16) * 0.1;
            double score = rate_err + tq_penalty;

            if (score < best_error) {
                best_error = score;
                best_brp = brp;
                best_total_tq = total_tq;
            }
        }

        uint32_t sync_plus_tseg1 = static_cast<uint32_t>(std::round((sample_point_pct / 100.0f) * best_total_tq));
        if (sync_plus_tseg1 < 2) sync_plus_tseg1 = 2;
        if (sync_plus_tseg1 >= best_total_tq) sync_plus_tseg1 = best_total_tq - 1;

        uint32_t tseg1 = sync_plus_tseg1 - 1;
        uint32_t tseg2 = best_total_tq - 1 - tseg1;

        out_bt.brp        = best_brp;
        out_bt.prop_seg   = tseg1 / 2;
        out_bt.phase_seg1 = tseg1 - out_bt.prop_seg;
        out_bt.phase_seg2 = tseg2;
        out_bt.sjw        = std::min<uint32_t>(tseg2, 4U);

        return true;
    }

} // namespace CanProtocolUtils
