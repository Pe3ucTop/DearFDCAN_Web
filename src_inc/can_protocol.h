#pragma once
// Umbrella header of the CAN protocol layer. The definitions live in
//   can_protocol_common.h   protocol independent types (CanMessage, CanPayload, CanBittiming, ...) and helpers
//   can_protocol_gsusb.h    gs_usb (candleLight / CANable) structures, frame parsing and building
// and CanProtocolHandler below is the single entry point the rest of the program uses, so that the
// callers do not depend on which protocol backend does the work.
#include "can_protocol_common.h"
#include "can_protocol_gsusb.h"

class CanProtocolHandler {
public:
    // Parses one bulk IN transfer into CAN messages
    std::vector<CanMessage> ParseRxBuffer(const uint8_t* buffer, int length);
    // Same, appending to `out` (lets a caller reuse one vector and avoid per-transfer allocations)
    void ParseRxBuffer(const uint8_t* buffer, int length, std::vector<CanMessage>& out);
    // Builds the bulk OUT transfer of one frame
    std::vector<uint8_t> BuildTxBuffer(const CanMessage& msg);

    static CanTimingMetrics ComputeTimingMetrics(uint32_t fclk_hz, const CanBittiming& bt);
    static bool AutoCalculateBittiming(uint32_t fclk_hz, uint32_t target_bitrate_bps, float sample_point_pct, CanBittiming& out_bt);

    static uint8_t DlcToBytes(uint8_t dlc);
    static uint8_t BytesToDlc(uint8_t bytes);
};
