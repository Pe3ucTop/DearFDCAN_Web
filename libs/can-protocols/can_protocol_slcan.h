#pragma once
// SLCAN: the ASCII protocol of serial CAN adapters (Lawicel CAN232/CANUSB, USBtin, CANable "slcan" firmware, ...).
// Every command and every frame is one line terminated by '\r'. Two dialects are handled:
//
//   Lawicel   classic CAN only:  Sn bit rate code, O / L / C open and close, t T r R frames
//   CANable   the extended dialect of the CANable 2.5 firmware (STM32G4/G0 "Slcan"): additionally the CAN FD
//             frame letters d D (no BRS) and b B (BRS), exact bit timing with s<brp>,<seg1>,<seg2>,<sjw>
//             (nominal) and y... (data), modes ON / OS / OI / OE, feedback mode MF, an ESI suffix 'S'
//             and a version/limits line answered to 'V'.
//
// Frame lines (receive and transmit):  <type><id><dlc>[data][S]\r
//   type  t standard data   T extended data   r standard remote   R extended remote
//         d standard FD     D extended FD     b standard FD+BRS   B extended FD+BRS
//   id    3 (lower case type) or 8 (upper case type) hex digits
//   dlc   one hex digit (the DLC code, 9..F are the CAN FD lengths 12..64)
//   data  2 hex digits per byte (not for remote frames)
//   S     ESI flag (CANable, when enabled with "MS")
//
// Replies to commands carry no id: a command is answered by '\r' (ok) or BEL (error), with feedback mode by
// "#\r" / "#<code>\r"; Lawicel answers a transmitted frame with 'z\r' / 'Z\r'. Text answers (the 'V' line) and
// anything else that is not a frame is reported as a "reply token" (see ParseRxStream).
//
// The values follow the CANable 2.5 firmware sources (Firmware/Slcan/control.c, buffer.c, settings.h).
#include "can_protocol_common.h"

#include <string>
#include <vector>

enum SlcanDialect : int {
    SLCAN_DIALECT_LAWICEL = 0,   // classic CAN only
    SLCAN_DIALECT_CANABLE = 1    // CAN FD capable, exact bit timing
};

// Opcodes the page uses for the request id of the command transfers it sends to a serial adapter (they come back
// as the request of the acknowledgement). SLCAN_CMD_VERSION is also the "request" of the version reply.
constexpr int SLCAN_CMD_VERSION    = 0x56;    // 'V': answered with a text line instead of an acknowledge
constexpr int SLCAN_CMD_ID_START   = 0x300;   // first command of the start sequence
constexpr int SLCAN_CMD_ID_STOP    = 0x400;   // stop sequence

// What the 'V' command reports
struct SlcanVersionInfo {
    bool valid = false;
    SlcanDialect dialect = SLCAN_DIALECT_LAWICEL;
    std::string board, mcu, firmware, slcan_version, serial, version_text;   // version_text: the classic "V1013" payload
    uint32_t clock_mhz = 0;
    uint32_t channels = 1;
    CanBitTimingLimits nominal = {};   // from "Limits:" (CANable only)
    CanBitTimingLimits data = {};
};

namespace CanProtocolSlcan {

    // Bit-timing limits of the CANable firmware before the adapter told its own (STM32G4 FDCAN)
    CanBitTimingLimits DefaultNominalLimits();
    CanBitTimingLimits DefaultDataLimits();

    // Splits the incoming byte stream into lines and decodes them. Frames are appended to `out`; every other line is
    // appended to `replies` (if not null) as one token terminated by '\n':
    //   "K"        success   ('\r' alone, "#\r", 'z\r', 'Z\r')
    //   "E<c>"     error     (BEL gives "E-", "#<code>\r" gives "E<code>")
    //   "L<text>"  any other line, control characters replaced by '?' and shortened to 200 characters
    // `state` holds the unfinished line between two calls and has to be Reset() when the stream was interrupted.
    // Frames get no timestamp: the caller stamps them with the arrival time.
    void ParseRxStream(const uint8_t* buffer, int length, std::vector<CanMessage>& out, CanRxStreamState& state,
                       std::string* replies);

    // Decodes one line without its terminator; returns true and fills `msg` if it is a frame.
    bool ParseFrameLine(const uint8_t* line, size_t length, CanMessage& msg);

    // The command line of a frame, empty if it cannot be sent in the dialect (FD frame on a classic adapter)
    std::vector<uint8_t> BuildTxBuffer(const CanMessage& msg, SlcanDialect dialect = SLCAN_DIALECT_CANABLE);

    // Parses the answer to 'V': "+Board: ...\tClock: 160\tLimits: ...\r" (CANable) or "V1013" (classic)
    bool ParseVersionLine(const std::string& line, SlcanVersionInfo& out);

    // Lawicel bit-rate code for a bit rate in bit/s ('0'..'8'), 0 if the rate has no code
    char ClassicRateCode(uint32_t bitrate_bps);

    // The commands that bring a closed adapter on the bus, one command line each (including the terminator).
    //   CANable: C, MF, s<..>, y<..> (FD only), O / OS / OI
    //   Lawicel: C, S<code>, O / L
    // A bit rate without a Lawicel code makes the call fail (empty result).
    std::vector<std::string> BuildStartCommands(SlcanDialect dialect, bool fd, bool listen_only, bool loopback,
                                                const CanBittiming& nominal, const CanBittiming& data, uint32_t nominal_bps);

    // "C\r" (closing is never answered)
    std::vector<std::string> BuildStopCommands();

    // True for command lines that are not answered ("C\r")
    bool CommandHasNoReply(const std::string& line);

} // namespace CanProtocolSlcan
