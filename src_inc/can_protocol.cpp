#include "can_protocol.h"

// Facade over the protocol backends: the protocol-aware calls pick the backend of an adapter, the
// shortcuts without a protocol argument go to gs_usb.

void CanProtocolHandler::ParseRx(CanProtocolType type, const uint8_t* buffer, int length, std::vector<CanMessage>& out,
                                 CanRxStreamState& state, std::string* replies) {
    switch (type) {
        case PROTOCOL_PEAK_PCAN_FD:
            CanProtocolPcan::ParseRxStream(buffer, length, out, state);
            break;
        case PROTOCOL_SLCAN:
            CanProtocolSlcan::ParseRxStream(buffer, length, out, state, replies);
            break;
        case PROTOCOL_GS_USB_CANDLELIGHT:
        default:
            CanProtocolGsUsb::ParseRxBuffer(buffer, length, out);
            break;
    }
}

std::vector<uint8_t> CanProtocolHandler::BuildTx(CanProtocolType type, const CanMessage& msg, uint8_t channel) {
    switch (type) {
        case PROTOCOL_PEAK_PCAN_FD:
            return CanProtocolPcan::BuildTxBuffer(msg, channel);
        case PROTOCOL_SLCAN:
            return CanProtocolSlcan::BuildTxBuffer(msg, channel == SLCAN_DIALECT_LAWICEL ? SLCAN_DIALECT_LAWICEL : SLCAN_DIALECT_CANABLE);
        case PROTOCOL_GS_USB_CANDLELIGHT:
        default:
            return CanProtocolGsUsb::BuildTxBuffer(msg, channel);
    }
}

bool CanProtocolHandler::AutoCalculateBittiming(uint32_t fclk_hz, uint32_t target_bitrate_bps, float sample_point_pct,
                                                const CanBitTimingLimits& limits, CanBittiming& out_bt) {
    return CanProtocolUtils::AutoCalculateBittiming(fclk_hz, target_bitrate_bps, sample_point_pct, limits, out_bt);
}

std::vector<CanMessage> CanProtocolHandler::ParseRxBuffer(const uint8_t* buffer, int length) {
    return CanProtocolGsUsb::ParseRxBuffer(buffer, length);
}

void CanProtocolHandler::ParseRxBuffer(const uint8_t* buffer, int length, std::vector<CanMessage>& out) {
    CanProtocolGsUsb::ParseRxBuffer(buffer, length, out);
}

std::vector<uint8_t> CanProtocolHandler::BuildTxBuffer(const CanMessage& msg) {
    return CanProtocolGsUsb::BuildTxBuffer(msg);
}

CanTimingMetrics CanProtocolHandler::ComputeTimingMetrics(uint32_t fclk_hz, const CanBittiming& bt) {
    return CanProtocolUtils::ComputeTimingMetrics(fclk_hz, bt);
}

bool CanProtocolHandler::AutoCalculateBittiming(uint32_t fclk_hz, uint32_t target_bitrate_bps, float sample_point_pct, CanBittiming& out_bt) {
    return CanProtocolUtils::AutoCalculateBittiming(fclk_hz, target_bitrate_bps, sample_point_pct, out_bt);
}

uint8_t CanProtocolHandler::DlcToBytes(uint8_t dlc) {
    return CanProtocolUtils::DlcToBytes(dlc);
}

uint8_t CanProtocolHandler::BytesToDlc(uint8_t bytes) {
    return CanProtocolUtils::BytesToDlc(bytes);
}
