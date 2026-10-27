#include "can_protocol.h"

// Facade over the protocol backends. Only gs_usb exists so far, so every call is forwarded to it;
// the backend will be selected per adapter once more protocols are available.

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
