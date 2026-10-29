#pragma once
// The ElmueSoft "CANable 2.5" extension of the gs_usb (candleLight) protocol: https://github.com/Elmue/CANable-2.5-firmware-Slcan-and-Candlelight
//
// The adapter is still driven with the gs_usb control requests, but the bulk transfers use a compact message format
// (kHeader: size + type, then only the bytes that are needed) instead of the 80 byte kHostFrame. The mode is switched on
// by the flag ELM_DevFlagProtocolElmue of GS_USB_BREQ_MODE; with ELM_DevFlagSendUsbBlobs the adapter packs several received
// messages into one USB transfer (up to 2048 bytes). Besides CAN frames the adapter sends tx echoes (one marker byte instead
// of the whole frame), error reports, bus load and text messages. Extra control requests: board info, hardware acceptance
// filters, bus load report, termination resistor, separate bit-timing limits of the data phase.
//
// Layouts follow candlelight_def.h of the firmware (MIT licensed, ElmueSoft / Hubert Denkmair).
#include "can_protocol_common.h"
#include "can_protocol_gsusb.h"

#include <string>
#include <vector>

// Control requests (bRequest) added by the Elmue protocol; the requests below 20 exist in the generic gs_usb protocol too
enum ElmBreq : uint8_t {
    GS_USB_BREQ_BT_CONST_EXT    = 11,   // bit timing limits of the nominal and the data phase (kCapabilityFD)
    GS_USB_BREQ_SET_TERMINATION = 12,   // u32: 1 = 120 ohm resistor on
    GS_USB_BREQ_GET_TERMINATION = 13,
    GS_USB_BREQ_GET_STATE       = 14,   // deprecated polling of the bus state (the Elmue firmware reports errors by itself)
    ELM_BREQ_GET_BOARD_INFO     = 20,   // these need the Elmue protocol to be switched on by the last GS_USB_BREQ_MODE
    ELM_BREQ_SET_FILTER         = 21,
    ELM_BREQ_GET_LAST_ERROR     = 22,   // u8 eFeedback of the previous request
    ELM_BREQ_SET_BUSLOAD_REPORT = 23    // u8: report interval in 100 ms steps, 0 = off
};

// Feature / mode flags beyond the ones in can_protocol_gsusb.h
constexpr uint32_t GS_CAN_FEATURE_BT_CONST_EXT = 1u << 10;   // GS_USB_BREQ_BT_CONST_EXT is answered
constexpr uint32_t GS_CAN_FEATURE_TERMINATION  = 1u << 11;   // the termination resistor can be switched
constexpr uint32_t GS_CAN_FEATURE_GET_STATE    = 1u << 13;
constexpr uint32_t ELM_FEATURE_PROTOCOL        = 1u << 14;   // Elmue protocol (as a mode flag: switch it on)
constexpr uint32_t ELM_FEATURE_USB_BLOBS       = 1u << 15;   // several messages in one USB transfer

// Message types of the Elmue bulk protocol
enum ElmMsgType : uint8_t {
    ELM_MSG_TX_FRAME = 10,   // host -> adapter
    ELM_MSG_TX_ECHO  = 11,   // the adapter sent the frame with this marker
    ELM_MSG_RX_FRAME = 12,
    ELM_MSG_ERROR    = 13,
    ELM_MSG_STRING   = 14,
    ELM_MSG_BUSLOAD  = 15,
    ELM_MSG_TX_BLOB  = 16,
    ELM_MSG_RX_BLOB  = 17
};

constexpr int ELM_MAX_BLOB_SIZE = 2048;          // size of the biggest USB transfer of the protocol
constexpr size_t ELM_RX_HEADER = 7;              // size, type, flags, can_id[4]
constexpr size_t ELM_TX_HEADER = 8;              // size, type, flags, can_id[4], marker

// eFrameFlags of the messages and eCanIdFlags of can_id
constexpr uint8_t  ELM_FRM_FDF = 0x02, ELM_FRM_BRS = 0x04, ELM_FRM_ESI = 0x08;
constexpr uint32_t ELM_ID_RTR  = 0x40000000U, ELM_ID_29BIT = 0x80000000U;

// Host acceptance filters (ELM_BREQ_SET_FILTER, operation codes)
enum ElmFilterOp : uint8_t {
    ELM_FIL_HOST_CLEAR   = 0,
    ELM_FIL_HOST_PASS_11 = 1,
    ELM_FIL_HOST_PASS_29 = 2
};
constexpr int ELM_MAX_HOST_FILTERS = 8;

// The 8 + 2 data bytes of an error message: eErrFlagsCanID (err_id, 32 bit) and eErrFlagsByte1.. (err_data[8])
constexpr uint32_t ELM_ERID_TX_TIMEOUT = 0x0001, ELM_ERID_ARBITRATION = 0x0002, ELM_ERID_NO_ACK = 0x0020, ELM_ERID_BUS_OFF = 0x0040,
                   ELM_ERID_BUS_ERROR = 0x0080, ELM_ERID_RESTARTED = 0x0100, ELM_ERID_CRC = 0x0200;

#pragma pack(push, 1)
// ELM_BREQ_GET_BOARD_INFO. Firmware before February 2026 answers with 52 bytes (no board_flags).
struct ElmBoardInfo {
    uint16_t mcu_device_id;
    char     mcu_name[25];
    char     board_name[25];
    uint32_t board_flags;     // ElmBoardFlags
};

// ELM_BREQ_SET_FILTER (the bridge fields are for multi channel adapters and unused here)
struct ElmFilter {
    uint8_t  operation;       // ElmFilterOp
    uint32_t filter;
    uint32_t mask;
    uint8_t  index;
    uint8_t  dest_channel;
    uint8_t  reserved[6];
};

// One bit-timing limit block of GS_USB_BREQ_BT_CONST / _EXT, field order as in GsDeviceBtConst after fclk_can
struct ElmTimeLimits {
    uint32_t seg1_min, seg1_max, seg2_min, seg2_max, sjw_max, brp_min, brp_max, brp_inc;
};

// GS_USB_BREQ_BT_CONST_EXT: limits of the nominal and of the data phase
struct GsDeviceBtConstExt {
    uint32_t      feature;
    uint32_t      fclk_can;
    ElmTimeLimits nominal;
    ElmTimeLimits data;
};
#pragma pack(pop)

static_assert(sizeof(ElmBoardInfo) == 56, "ElmBoardInfo is a wire format");
static_assert(sizeof(ElmFilter) == 17, "ElmFilter is a wire format");
static_assert(sizeof(GsDeviceBtConstExt) == 72, "GsDeviceBtConstExt is a wire format");

constexpr uint32_t ELM_BOARD_QUARTZ = 1u << 0, ELM_BOARD_USB_HIGHSPEED = 1u << 1, ELM_BOARD_LIN = 1u << 2;
constexpr size_t   ELM_BOARD_INFO_MIN = 52;

// Bus state derived from the error messages (eBusState of the legacy protocol, without "stopped")
enum ElmBusState : uint8_t { ELM_BUS_ACTIVE = 0, ELM_BUS_WARNING = 1, ELM_BUS_PASSIVE = 2, ELM_BUS_OFF = 3 };

// What an error message says about the bus
struct ElmErrorInfo {
    ElmBusState state = ELM_BUS_ACTIVE;
    bool state_known = false;       // false: the message does not change the bus state (e.g. a plain bus error)
    uint8_t rx_errors = 0;          // error counters of the controller
    uint8_t tx_errors = 0;
    std::string text;               // comma separated description, "" if nothing was recognised
};

class CanProtocolElmue {
public:
    // Parses one bulk IN transfer: a blob (frame_count + type, then that many messages) or one/several plain messages.
    // CAN frames and tx echoes are appended to `out`; the other messages become lines of `events` (when given):
    //   "U<percent>\n"           bus load
    //   "T<text>\n"              text message of the firmware (control characters replaced by spaces)
    //   "E<err_id 8 hex>:<data 16 hex>\n"   error report (see DecodeError)
    // `with_timestamp` must match GS_DevFlagTimestamp of the running channel (the adapter then appends 4 bytes to rx frames,
    // echoes and errors). `echoes` is a table of 256 frames indexed by the tx marker: a marker found there (is_tx_echo set)
    // turns the echo message into that frame and the table entry is cleared; without a table echoes are dropped.
    static void ParseRx(const uint8_t* buffer, int length, bool with_timestamp, std::vector<CanMessage>& out,
                        std::string* events = nullptr, CanMessage* echoes = nullptr);

    // Builds the bulk OUT transfer of one frame: header + padded data, with the next marker (1..255, 0 is "no echo")
    static std::vector<uint8_t> BuildTxBuffer(const CanMessage& msg, uint8_t* marker_out = nullptr);

    // Decodes a transmit message built by BuildTxBuffer back into the frame and its marker (the core remembers the frames
    // until their echo comes). Returns false if the buffer is not a tx frame message.
    static bool ParseTxFrame(const uint8_t* buffer, int length, CanMessage& msg, uint8_t& marker);

    // ELM_BREQ_SET_FILTER payload of a host pass filter (extended: 29 bit)
    static std::vector<uint8_t> BuildFilter(ElmFilterOp op, uint32_t filter = 0, uint32_t mask = 0);

    // The flags of GS_USB_BREQ_MODE for a start in Elmue mode, from the features the adapter reported
    static uint32_t ModeFlags(uint32_t features, bool want_timestamp);

    static bool ParseBoardInfo(const uint8_t* data, int length, ElmBoardInfo& info);
    static std::string BoardFlagsText(uint32_t flags);

    // Decodes the payload of an "E" event line (and the CAN message form of an error report)
    static ElmErrorInfo DecodeError(uint32_t err_id, const uint8_t data[8]);
    // Parses "E<id>:<data>" (without the leading letter and the newline)
    static bool ParseErrorEvent(const std::string& body, uint32_t& err_id, uint8_t data[8]);

    // Name of a firmware feedback code (ELM_BREQ_GET_LAST_ERROR answer)
    static const char* FeedbackText(uint8_t code);
};
