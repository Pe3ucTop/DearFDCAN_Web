#pragma once
// Umbrella header of the CAN protocol layer. The definitions live in
//   can_protocol_common.h   protocol independent types (CanMessage, CanPayload, CanBittiming, ...) and helpers
//   can_protocol_gsusb.h    gs_usb (candleLight / CANable) structures, frame parsing and building
//   can_protocol_pcan.h     PEAK PCAN-USB FD (uCAN) structures, frame parsing/building and command sequences
// and CanProtocolHandler below is the entry point the rest of the program uses for everything that
// depends on the protocol of an adapter, so that the callers do not depend on which backend does the work.
#include "can_protocol_common.h"
#include "can_protocol_gsusb.h"
#include "can_protocol_pcan.h"

class CanProtocolHandler {
public:
    // ---- protocol aware entry points ----

    // Parses one bulk IN transfer of the adapter's data endpoint into CAN messages, appended to `out`.
    // `state` is the adapter's stream reassembly state (used by protocols whose records can be split
    // across transfers, ignored by gs_usb).
    static void ParseRx(CanProtocolType type, const uint8_t* buffer, int length, std::vector<CanMessage>& out,
                        CanRxStreamState& state);

    // Builds the bulk OUT transfer of one frame for a CAN channel of the adapter
    static std::vector<uint8_t> BuildTx(CanProtocolType type, const CanMessage& msg, uint8_t channel = 0);

    // Search limited to the registers of the controller (see CanProtocolUtils)
    static bool AutoCalculateBittiming(uint32_t fclk_hz, uint32_t target_bitrate_bps, float sample_point_pct,
                                       const CanBitTimingLimits& limits, CanBittiming& out_bt);

    // ---- gs_usb shortcuts kept for callers that only deal with gs_usb ----
    std::vector<CanMessage> ParseRxBuffer(const uint8_t* buffer, int length);
    void ParseRxBuffer(const uint8_t* buffer, int length, std::vector<CanMessage>& out);
    std::vector<uint8_t> BuildTxBuffer(const CanMessage& msg);

    // ---- protocol independent helpers ----
    static CanTimingMetrics ComputeTimingMetrics(uint32_t fclk_hz, const CanBittiming& bt);
    static bool AutoCalculateBittiming(uint32_t fclk_hz, uint32_t target_bitrate_bps, float sample_point_pct, CanBittiming& out_bt);

    static uint8_t DlcToBytes(uint8_t dlc);
    static uint8_t BytesToDlc(uint8_t bytes);
};
