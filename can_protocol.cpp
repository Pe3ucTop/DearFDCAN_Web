#include "can_protocol.h"
#include <cstring>
#include <cmath>
#include <algorithm>

constexpr uint32_t CAN_EFF_FLAG = 0x80000000U;
constexpr uint32_t CAN_EFF_MASK = 0x1FFFFFFFU;
constexpr uint32_t CAN_SFF_MASK = 0x000007FFU;

constexpr uint8_t GS_CAN_FLAG_FD  = (1 << 1);
constexpr uint8_t GS_CAN_FLAG_BRS = (1 << 2);

uint8_t CanProtocolHandler::DlcToBytes(uint8_t dlc) {
    static const uint8_t lookup[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64};
    return lookup[dlc & 0x0F];
}

uint8_t CanProtocolHandler::BytesToDlc(uint8_t bytes) {
    if (bytes <= 8) return bytes;
    if (bytes <= 12) return 9;
    if (bytes <= 16) return 10;
    if (bytes <= 20) return 11;
    if (bytes <= 24) return 12;
    if (bytes <= 32) return 13;
    if (bytes <= 48) return 14;
    return 15;
}

CanTimingMetrics CanProtocolHandler::ComputeTimingMetrics(uint32_t fclk_hz, const GsDeviceBittiming& bt) {
    CanTimingMetrics m = {0.0, 0, 0, 0.0, 0.0};
    if (fclk_hz == 0 || bt.brp == 0) return m;

    // 1 Sync_Seg + Prop_Seg + Phase_Seg1 + Phase_Seg2
    m.tseg1_total = bt.prop_seg + bt.phase_seg1;
    m.total_tq = 1U + m.tseg1_total + bt.phase_seg2;
    if (m.total_tq == 0) return m;

    // Time Quantum duration in nanoseconds: (BRP / fclk) * 1e9
    m.tq_ns = (static_cast<double>(bt.brp) * 1e9) / static_cast<double>(fclk_hz);

    // Resulting bitrate in bps
    m.actual_bitrate = static_cast<double>(fclk_hz) / (static_cast<double>(bt.brp) * static_cast<double>(m.total_tq));

    // Sample point percentage: (1 + Prop_Seg + Phase_Seg1) / Total_TQ * 100
    m.sample_point_pct = (static_cast<double>(1U + m.tseg1_total) / static_cast<double>(m.total_tq)) * 100.0;

    return m;
}

bool CanProtocolHandler::AutoCalculateBittiming(
		uint32_t fclk_hz,
		uint32_t target_bitrate_bps,
		float sample_point_pct,
		GsDeviceBittiming& out_bt	)
{
    if (fclk_hz == 0 || target_bitrate_bps == 0) return false;

    double best_error = 1e18;
    uint32_t best_brp = 1;
    uint32_t best_total_tq = 16;

    // Search for optimal Prescaler (BRP) and Total TQ (between 8 and 80 tq)
    for (uint32_t total_tq = 8; total_tq <= 80; ++total_tq) {
        uint32_t brp = static_cast<uint32_t>(std::round(static_cast<double>(fclk_hz) / (static_cast<double>(target_bitrate_bps) * total_tq)));
        if (brp < 1 || brp > 1024) continue;

        double actual_bps = static_cast<double>(fclk_hz) / (static_cast<double>(brp) * total_tq);
        double rate_err = std::abs(actual_bps - static_cast<double>(target_bitrate_bps));

        // Prefer standard TQ counts around 16..20 if bitrate error is identical
        double tq_penalty = std::abs(static_cast<int>(total_tq) - 16) * 0.1;
        double score = rate_err + tq_penalty;

        if (score < best_error) {
            best_error = score;
            best_brp = brp;
            best_total_tq = total_tq;
        }
    }

    // Split Total TQ into TSEG1 (prop + seg1) and TSEG2 according to target Sample Point
    uint32_t sync_Plus_tseg1 = static_cast<uint32_t>(std::round((sample_point_pct / 100.0f) * best_total_tq));
    if (sync_Plus_tseg1 < 2) sync_Plus_tseg1 = 2;
    if (sync_Plus_tseg1 >= best_total_tq) sync_Plus_tseg1 = best_total_tq - 1;

    uint32_t tseg1 = sync_Plus_tseg1 - 1;
    uint32_t tseg2 = best_total_tq - 1 - tseg1;

    out_bt.brp        = best_brp;
    out_bt.prop_seg   = tseg1 / 2;
    out_bt.phase_seg1 = tseg1 - out_bt.prop_seg;
    out_bt.phase_seg2 = tseg2;
    out_bt.sjw        = std::min<uint32_t>(tseg2, 4U);

    return true;
}

std::vector<CanMessage> CanProtocolHandler::ParseRxBuffer(const uint8_t* buffer, int length) {
    std::vector<CanMessage> messages;
    if (length < 20) return messages;

    const GsHostFrame* frame = reinterpret_cast<const GsHostFrame*>(buffer);

    CanMessage msg;
    msg.is_tx_echo  = (frame->echo_id != 0xFFFFFFFFU);
    msg.is_extended = (frame->can_id & CAN_EFF_FLAG) != 0;
    msg.id          = frame->can_id & (msg.is_extended ? CAN_EFF_MASK : CAN_SFF_MASK);
    msg.is_fd       = (frame->flags & GS_CAN_FLAG_FD) != 0;
    msg.is_brs      = (frame->flags & GS_CAN_FLAG_BRS) != 0;
    msg.dlc         = frame->can_dlc & 0x0F;

    uint8_t data_len = msg.is_fd ? DlcToBytes(msg.dlc) : std::min<uint8_t>(msg.dlc, 8);
    msg.payload.assign(frame->data, frame->data + data_len);

    messages.push_back(msg);
    return messages;
}

std::vector<uint8_t> CanProtocolHandler::BuildTxBuffer(const CanMessage& msg) {
    GsHostFrame frame = {0};
    frame.echo_id = 0;

    frame.can_id = msg.id & (msg.is_extended ? CAN_EFF_MASK : CAN_SFF_MASK);
    if (msg.is_extended) frame.can_id |= CAN_EFF_FLAG;

    if (msg.is_fd) {
        frame.flags |= GS_CAN_FLAG_FD;
        if (msg.is_brs) frame.flags |= GS_CAN_FLAG_BRS;
    }

    frame.can_dlc = msg.dlc;
    uint8_t data_len = DlcToBytes(msg.dlc);
    std::memcpy(frame.data, msg.payload.data(), std::min((size_t)data_len, msg.payload.size()));

    size_t transfer_size = msg.is_fd ? sizeof(GsHostFrame) : 20;
    std::vector<uint8_t> out_buffer(transfer_size);
    std::memcpy(out_buffer.data(), &frame, transfer_size);

    return out_buffer;
}
