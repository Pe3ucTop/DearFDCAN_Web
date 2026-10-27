#pragma once
#include <cstdint>
#include <vector>

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
    GS_CAN_FEATURE_HW_TIMESTAMP = (1 << 4),
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

// Structure representing a parsed CAN / CAN FD message
struct CanMessage {
    uint32_t id = 0;
    bool is_extended = false;
    bool is_rtr = false;
    bool is_fd = false;
    bool is_brs = false;
    bool is_esi = false;
    bool is_tx_echo = false;
    uint8_t dlc = 0;
    uint32_t hw_timestamp_us = 0;
    std::vector<uint8_t> payload;
};

class CanProtocolHandler {
public:
    std::vector<CanMessage> ParseRxBuffer(const uint8_t* buffer, int length);
    std::vector<uint8_t> BuildTxBuffer(const CanMessage& msg);

    static CanTimingMetrics ComputeTimingMetrics(uint32_t fclk_hz, const GsDeviceBittiming& bt);
    static bool AutoCalculateBittiming(uint32_t fclk_hz, uint32_t target_bitrate_bps, float sample_point_pct, GsDeviceBittiming& out_bt);

    static uint8_t DlcToBytes(uint8_t dlc);
    static uint8_t BytesToDlc(uint8_t bytes);
};