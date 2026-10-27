#pragma once
// PEAK-System PCAN-USB FD family (PCAN-USB FD, PCAN-USB Pro FD, PCAN-Chip USB, PCAN-USB X6): the uCAN firmware
// protocol. Commands go to a bulk OUT endpoint as 8-byte records, CAN frames travel as variable length records
// on a bulk IN endpoint (received) and on one bulk OUT endpoint per CAN channel (transmitted).
//
// Layouts and values follow PEAK's Linux driver (pcanfd_core_user.h, pcanfd_usb_fw.h, pcanfd_usb.c).
#include "can_protocol_common.h"

// USB ids
constexpr uint16_t PEAK_USB_VENDOR_ID       = 0x0C72;
constexpr uint16_t PCAN_USBPROFD_PRODUCT_ID = 0x0011;
constexpr uint16_t PCAN_USBFD_PRODUCT_ID    = 0x0012;
constexpr uint16_t PCAN_USBCHIP_PRODUCT_ID  = 0x0013;
constexpr uint16_t PCAN_USBX6_PRODUCT_ID    = 0x0014;

// Endpoint numbers (without the direction bit) used when the firmware info does not list them
constexpr uint8_t PCAN_EPN_CMD_OUT  = 1;
constexpr uint8_t PCAN_EPN_CMD_IN   = 1;
constexpr uint8_t PCAN_EPN_MSG_IN   = 2;
constexpr uint8_t PCAN_EPN_MSG_OUT0 = 2;   // transmit endpoint of CAN channel 0
constexpr uint8_t PCAN_EPN_MSG_OUT1 = 3;   // transmit endpoint of CAN channel 1

// USB vendor requests (request type "vendor", recipient "other", wIndex 0)
constexpr uint8_t  PCAN_VREQ_INFO                 = 0;   // device -> host
constexpr uint8_t  PCAN_VREQ_FKT                  = 2;   // host -> device
constexpr uint16_t PCAN_VREQ_INFO_FIRMWARE        = 1;   // wValue of the firmware info record
constexpr uint16_t PCAN_VREQ_FKT_DRIVER_LOADED    = 5;   // wValue: "a driver is using the interface" notification
constexpr uint16_t PCAN_FWINFO_TYPE_EXT           = 2;   // record type from which the endpoint numbers are present

// A command transfer holds at most this many bytes (a shorter one is terminated with an end-of-collection record)
constexpr int PCAN_CMD_PACKET_SIZE = 512;

// uCAN command opcodes (low 10 bits of opcode_channel; bits 12..15 are the channel)
enum PcanOpcode : uint16_t {
    PCAN_CMD_NOP               = 0x000,
    PCAN_CMD_RESET_MODE        = 0x001,
    PCAN_CMD_NORMAL_MODE       = 0x002,
    PCAN_CMD_LISTEN_ONLY_MODE  = 0x003,
    PCAN_CMD_TIMING_SLOW       = 0x004,
    PCAN_CMD_TIMING_FAST       = 0x005,
    PCAN_CMD_SET_STD_FILTER    = 0x006,
    PCAN_CMD_FILTER_STD        = 0x008,   // old style filter row
    PCAN_CMD_TX_ABORT          = 0x009,
    PCAN_CMD_WR_ERR_CNT        = 0x00A,
    PCAN_CMD_SET_EN_OPTION     = 0x00B,
    PCAN_CMD_CLR_DIS_OPTION    = 0x00C,
    PCAN_CMD_RX_BARRIER        = 0x010,
    PCAN_CMD_END_OF_COLLECTION = 0x3FF,
    PCAN_USB_CMD_CLK_SET       = 0x080    // USB extension: CAN clock domain
};

// uCAN record types on the data endpoints
enum PcanMsgType : uint16_t {
    PCAN_MSG_CAN_RX          = 0x0001,
    PCAN_MSG_ERROR           = 0x0002,
    PCAN_MSG_STATUS          = 0x0003,
    PCAN_MSG_BUSLOAD         = 0x0004,
    PCAN_MSG_USB_CALIBRATION = 0x0100,
    PCAN_MSG_USB_OVERRUN     = 0x0101,
    PCAN_MSG_CACHE_CRITICAL  = 0x0102,
    PCAN_MSG_CAN_TX          = 0x1000,
    PCAN_MSG_CAN_TX_PAUSE    = 0x1002
};

// Flags of CAN records (flags field)
enum PcanFrameFlags : uint16_t {
    PCAN_FLAG_API_SRR   = 0x80,   // echo of an own frame, returns the client value
    PCAN_FLAG_ESI       = 0x40,   // error state indicator (FD)
    PCAN_FLAG_BRS       = 0x20,   // bit rate switch (FD)
    PCAN_FLAG_EDL       = 0x10,   // extended data length: CAN FD frame
    PCAN_FLAG_SINGLE    = 0x08,   // single shot
    PCAN_FLAG_HW_SRR    = 0x04,   // loopback: own frame copied to the receive path
    PCAN_FLAG_EXT_ID    = 0x02,   // 29-bit identifier
    PCAN_FLAG_RTR       = 0x01    // remote frame
};

// Controller options (SET_EN_OPTION / CLR_DIS_OPTION, offset 2 of the option record)
enum PcanOptions : uint16_t {
    PCAN_OPTION_ERROR     = 0x0001,   // report error / status records
    PCAN_OPTION_BUSLOAD   = 0x0002,
    PCAN_OPTION_ISO_MODE  = 0x0004,   // ISO 11898-1:2015 CAN FD CRC
    PCAN_OPTION_LO_MODE   = 0x0008,   // Diag FD only
    PCAN_OPTION_20AB_MODE = 0x0010,   // force CAN 2.0 A/B frame format
    PCAN_OPTION_TS_SOF    = 0x0020,
    PCAN_OPTION_SELF_ACK  = 0x0040,
    PCAN_OPTION_BRS_IGN   = 0x0080
};

// USB options (offset 6 of the option record)
enum PcanUsbOptions : uint16_t {
    PCAN_USB_OPTION_CALIBRATION = 0x8000,
    PCAN_USB_OPTION_FAST_FWD    = 0x2000
};

// WR_ERR_CNT selection bits
constexpr uint16_t PCAN_WRERRCNT_TE = 0x4000;
constexpr uint16_t PCAN_WRERRCNT_RE = 0x8000;

// USB_CMD_CLK_SET modes; the PCAN-USB FD family runs its CAN cores from an 80 MHz domain
enum PcanClockId : uint8_t {
    PCAN_CLK_80MHZ = 0, PCAN_CLK_60MHZ = 1, PCAN_CLK_40MHZ = 2, PCAN_CLK_30MHZ = 3, PCAN_CLK_24MHZ = 4, PCAN_CLK_20MHZ = 5
};
constexpr uint32_t PCAN_CAN_CLOCK_HZ = 80000000U;

#pragma pack(push, 1)
// Generic 8-byte uCAN command
struct PcanCommand {
    uint16_t opcode_channel;
    uint16_t args[3];
};

// Nominal (slow) bit timing. Every field is "value - 1" and limited to the bit width in brackets.
struct PcanTimingSlow {
    uint16_t opcode_channel;
    uint8_t  ewl;            // error warning limit (96)
    uint8_t  sjw_t;          // sjw (7 bits) | triple sampling (bit 7)
    uint8_t  tseg2;          // (7 bits)
    uint8_t  tseg1;          // prop_seg + phase_seg1 (8 bits)
    uint16_t brp;            // (10 bits)
};

// CAN FD data phase (fast) bit timing
struct PcanTimingFast {
    uint16_t opcode_channel;
    uint8_t  unused;
    uint8_t  sjw;            // (4 bits)
    uint8_t  tseg2;          // (4 bits)
    uint8_t  tseg1;          // prop_seg + phase_seg1 (5 bits)
    uint16_t brp;            // (10 bits)
};

// SET_STD_FILTER: one row of the 11-bit identifier acceptance mask (64 rows x 32 bits)
struct PcanStdFilter {
    uint16_t opcode_channel;
    uint8_t  unused;
    uint8_t  idx;
    uint32_t mask;
};

// SET_EN_OPTION / CLR_DIS_OPTION
struct PcanOptionCmd {
    uint16_t opcode_channel;
    uint16_t mask;           // PcanOptions
    uint16_t unused;
    uint16_t usb_mask;       // PcanUsbOptions
};

// WR_ERR_CNT
struct PcanWrErrCnt {
    uint16_t opcode_channel;
    uint16_t sel_mask;
    uint8_t  tx_counter;
    uint8_t  rx_counter;
    uint16_t unused;
};

// USB_CMD_CLK_SET
struct PcanClockCmd {
    uint16_t opcode_channel;
    uint8_t  mode;           // PcanClockId
    uint8_t  unused[5];
};

// Header common to all records on the data endpoints
struct PcanMsgHeader {
    uint16_t size;           // size of the whole record in bytes
    uint16_t type;           // PcanMsgType
};

// Received CAN / CAN FD frame: 28 bytes of header, then the data
struct PcanRxMsg {
    uint16_t size;
    uint16_t type;           // PCAN_MSG_CAN_RX
    uint32_t ts_low;         // timestamp in microseconds, low 32 bits
    uint32_t ts_high;
    uint32_t tag_low;
    uint32_t tag_high;
    uint8_t  channel_dlc;    // channel (bits 0..3), DLC (bits 4..7)
    uint8_t  client;         // echo value of own frames
    uint16_t flags;          // PcanFrameFlags
    uint32_t can_id;
    uint8_t  data[64];
};

// Transmitted CAN / CAN FD frame: 20 bytes of header, then the data; the record is padded to 4 bytes
struct PcanTxMsg {
    uint16_t size;
    uint16_t type;           // PCAN_MSG_CAN_TX
    uint32_t tag_low;
    uint32_t tag_high;
    uint8_t  channel_dlc;    // channel (bits 0..3), DLC (bits 4..7)
    uint8_t  client;         // echoed back when PCAN_FLAG_API_SRR is set
    uint16_t flags;          // PcanFrameFlags
    uint32_t can_id;
    uint8_t  data[64];
};

// Reply of the firmware info request (vendor request PCAN_VREQ_INFO, wValue PCAN_VREQ_INFO_FIRMWARE)
struct PcanFwInfo {
    uint16_t size_of;        // size of the record the firmware returned
    uint16_t type;           // endpoint numbers are valid from PCAN_FWINFO_TYPE_EXT
    uint8_t  hw_type;        // MCU type
    uint8_t  bl_version[3];  // bootloader version
    uint8_t  hw_version;     // PCB revision
    uint8_t  fw_version[3];  // firmware version
    uint32_t dev_id[2];      // device id per CAN channel
    uint32_t ser_no;         // serial number
    uint32_t flags;
    uint8_t  cmd_out_ep;     // (type >= EXT) endpoint numbers
    uint8_t  cmd_in_ep;
    uint8_t  data_out_ep[2]; // transmit endpoint of CAN channel 0 / 1
    uint8_t  data_in_ep;     // receive endpoint
    uint8_t  can_bus_count;  // number of CAN channels
    uint8_t  instance_idx;
    uint8_t  dummy;
    uint8_t  guid[16];
    uint8_t  fw_version_adv[32];
};
#pragma pack(pop)

static_assert(sizeof(PcanCommand) == 8 && sizeof(PcanTimingSlow) == 8 && sizeof(PcanTimingFast) == 8 &&
              sizeof(PcanStdFilter) == 8 && sizeof(PcanOptionCmd) == 8 && sizeof(PcanWrErrCnt) == 8 &&
              sizeof(PcanClockCmd) == 8, "uCAN commands are 8 bytes");
static_assert(offsetof(PcanRxMsg, can_id) == 24 && offsetof(PcanRxMsg, data) == 28, "RX record header is 28 bytes");
static_assert(offsetof(PcanTxMsg, can_id) == 16 && offsetof(PcanTxMsg, data) == 20, "TX record header is 20 bytes");
static_assert(sizeof(PcanFwInfo) == 84, "firmware info is 84 bytes");

// Bit-timing limits of the controller registers
inline const CanBitTimingLimits& PcanNominalLimits() { static const CanBitTimingLimits l = {256, 128, 128, 1024}; return l; }
inline const CanBitTimingLimits& PcanDataLimits()    { static const CanBitTimingLimits l = {32, 16, 16, 1024}; return l; }

class CanProtocolPcan {
public:
    // Parses a bulk IN transfer of the data endpoint. Records split across two transfers are reassembled
    // through `state` (keep one per adapter and call Reset() when the stream restarts). Only the CAN frames
    // of `channel` are returned: other channels and non-frame records (status, error, calibration) are skipped.
    static void ParseRxStream(const uint8_t* buffer, int length, std::vector<CanMessage>& out,
                              CanRxStreamState& state, uint8_t channel = 0);

    // Builds the bulk OUT transfer for one frame on a channel (the record plus a terminating empty record).
    // The frame is sent with a request to echo it back (flag API_SRR) tagged with an incrementing 8-bit
    // client value, so that the trace shows it as a transmitted frame.
    static std::vector<uint8_t> BuildTxBuffer(const CanMessage& msg, uint8_t channel = 0);

    // Command transfers that open a channel (clock, error counters, options, bit timing, accept-all filter, bus on).
    // Send them in order to the command endpoint; each is at most PCAN_CMD_PACKET_SIZE bytes.
    static std::vector<std::vector<uint8_t>> BuildStartCommands(uint8_t channel, bool enable_fd, bool listen_only,
                                                                const CanBittiming& nominal, const CanBittiming& data);

    // Command transfers that close a channel (bus off, options cleared)
    static std::vector<std::vector<uint8_t>> BuildStopCommands(uint8_t channel);
};
