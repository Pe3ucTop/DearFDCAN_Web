#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

// Maximum number of adapters handled at once. A device number (slot) is 0..kMaxDevices-1; the UI shows it 1-based.
constexpr int kMaxDevices = 8;

// gs_usb control request codes (bRequest)
enum GsUsbBreq : uint8_t {
    GS_USB_BREQ_HOST_FORMAT    = 0,
    GS_USB_BREQ_BITTIMING      = 1,
    GS_USB_BREQ_MODE           = 2,
    GS_USB_BREQ_BERR           = 3,
    GS_USB_BREQ_BT_CONST       = 4,
    GS_USB_BREQ_DEVICE_CONFIG  = 5,
    GS_USB_BREQ_TIMESTAMP      = 6,
    GS_USB_BREQ_IDENTIFY       = 7,
    GS_USB_BREQ_DATA_BITTIMING = 10
};

// Feature flags in GsDeviceBtConst::feature and GsDeviceMode::flags
enum GsCanFeature : uint32_t {
    GS_CAN_FEATURE_LISTEN_ONLY  = (1 << 0),
    GS_CAN_FEATURE_LOOP_BACK    = (1 << 1),
    GS_CAN_FEATURE_TRIPLE_SAMPLE = (1 << 2),
    GS_CAN_FEATURE_ONE_SHOT     = (1 << 3),
    GS_CAN_FEATURE_HW_TIMESTAMP = (1 << 4),
    GS_CAN_FEATURE_IDENTIFY     = (1 << 5),
    GS_CAN_FEATURE_USER_ID      = (1 << 6),
    GS_CAN_FEATURE_PAD_PKTS     = (1 << 7),
    GS_CAN_FEATURE_FD           = (1 << 8)
};

enum GsCanMode : uint32_t {
    GS_CAN_MODE_RESET = 0,
    GS_CAN_MODE_START = 1
};

#pragma pack(push, 1)
// Hardware bit-timing limits returned by GS_USB_BREQ_BT_CONST
struct GsDeviceBtConst {
    uint32_t feature;
    uint32_t fclk_can;  // CAN peripheral clock in Hz (e.g., 80000000)
    uint32_t tseg1_min;
    uint32_t tseg1_max;
    uint32_t tseg2_min;
    uint32_t tseg2_max;
    uint32_t sjw_max;
    uint32_t brp_min;
    uint32_t brp_max;
    uint32_t brp_inc;
};

// Adapter description returned by GS_USB_BREQ_DEVICE_CONFIG
struct GsDeviceConfig {
    uint8_t  reserved1;
    uint8_t  reserved2;
    uint8_t  reserved3;
    uint8_t  icount;      // number of CAN interfaces minus one
    uint32_t sw_version;  // firmware version
    uint32_t hw_version;  // hardware version
};

// Bit-timing parameters sent via GS_USB_BREQ_BITTIMING / GS_USB_BREQ_DATA_BITTIMING
struct GsDeviceBittiming {
    uint32_t prop_seg;   // Propagation segment in tq
    uint32_t phase_seg1; // Phase buffer segment 1 in tq
    uint32_t phase_seg2; // Phase buffer segment 2 in tq
    uint32_t sjw;        // Synchronization jump width in tq
    uint32_t brp;        // Bit-rate prescaler (clock divider)
};

// Mode command structure sent via GS_USB_BREQ_MODE
struct GsDeviceMode {
    uint32_t mode;
    uint32_t flags;
};

// gs_usb frame layout
struct GsHostFrame {
    uint32_t echo_id;
    uint32_t can_id;
    uint8_t  can_dlc;
    uint8_t  channel;
    uint8_t  flags;
    uint8_t  reserved;
    uint8_t  data[64];
};
#pragma pack(pop)

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

class CanProtocolHandler {
public:
    // Parses one bulk IN transfer. gs_usb normally delivers exactly one frame per transfer; if the
    // buffer holds several equally sized frames they are all returned.
    std::vector<CanMessage> ParseRxBuffer(const uint8_t* buffer, int length);
    // Same, appending to `out` (lets a caller reuse one vector and avoid per-transfer allocations)
    void ParseRxBuffer(const uint8_t* buffer, int length, std::vector<CanMessage>& out);
    std::vector<uint8_t> BuildTxBuffer(const CanMessage& msg);

    static CanTimingMetrics ComputeTimingMetrics(uint32_t fclk_hz, const GsDeviceBittiming& bt);
    static bool AutoCalculateBittiming(uint32_t fclk_hz, uint32_t target_bitrate_bps, float sample_point_pct, GsDeviceBittiming& out_bt);

    static uint8_t DlcToBytes(uint8_t dlc);
    static uint8_t BytesToDlc(uint8_t bytes);
};