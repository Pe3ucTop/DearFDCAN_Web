#pragma once
// gs_usb (candleLight / CANable) protocol: control requests, structures and bulk frame format.
#include "can_protocol_common.h"

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

// The gs_usb bit-timing structure is the generic one
using GsDeviceBittiming = CanBittiming;

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

class CanProtocolGsUsb {
public:
    // Parses one bulk IN transfer. gs_usb normally delivers exactly one frame per transfer; if the
    // buffer holds several equally sized frames they are all returned.
    static std::vector<CanMessage> ParseRxBuffer(const uint8_t* buffer, int length);
    // Same, appending to `out` (lets a caller reuse one vector and avoid per-transfer allocations)
    static void ParseRxBuffer(const uint8_t* buffer, int length, std::vector<CanMessage>& out);

    // Builds the bulk OUT transfer of one frame for the given adapter channel
    static std::vector<uint8_t> BuildTxBuffer(const CanMessage& msg, uint8_t channel = 0);
};
