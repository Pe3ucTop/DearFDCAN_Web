#include "can_protocol.h"
#include <cstring>

// Mask for standard/extended ID flags
constexpr uint32_t CAN_EFF_FLAG = 0x80000000U;
constexpr uint32_t CAN_EFF_MASK = 0x1FFFFFFFU;
constexpr uint32_t CAN_SFF_MASK = 0x000007FFU;

// Mask for CAN FD frames
constexpr uint8_t GS_CAN_FLAG_FD = (1 << 1);

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

std::vector<CanMessage> CanProtocolHandler::ParseRxBuffer(const uint8_t* buffer, int length) {
    std::vector<CanMessage> messages;
    
    // Verify we have enough bytes for a basic frame header
    if (length < 20) return messages;

    const GsHostFrame* frame = reinterpret_cast<const GsHostFrame*>(buffer);
    
    CanMessage msg;
    msg.is_extended = (frame->can_id & CAN_EFF_FLAG) != 0;
    msg.id = frame->can_id & (msg.is_extended ? CAN_EFF_MASK : CAN_SFF_MASK);
    msg.is_fd = (frame->flags & GS_CAN_FLAG_FD) != 0;
    msg.dlc = frame->can_dlc;
    
    uint8_t data_len = DlcToBytes(msg.dlc);
    msg.payload.assign(frame->data, frame->data + data_len);
    
    messages.push_back(msg);
    return messages;
}

std::vector<uint8_t> CanProtocolHandler::BuildTxBuffer(const CanMessage& msg) {
    GsHostFrame frame = {0};
    frame.echo_id = 0;
    
    frame.can_id = msg.id & (msg.is_extended ? CAN_EFF_MASK : CAN_SFF_MASK);
    if (msg.is_extended) frame.can_id |= CAN_EFF_FLAG;
    
    if (msg.is_fd) frame.flags |= GS_CAN_FLAG_FD;
    
    frame.can_dlc = msg.dlc;
    uint8_t data_len = DlcToBytes(msg.dlc);
    
    std::memcpy(frame.data, msg.payload.data(), std::min((size_t)data_len, msg.payload.size()));
    
    // Calculate total buffer size to send (smaller for classic CAN to save bandwidth)
    size_t transfer_size = msg.is_fd ? sizeof(GsHostFrame) : 24;
    
    std::vector<uint8_t> out_buffer(transfer_size);
    std::memcpy(out_buffer.data(), &frame, transfer_size);
    
    return out_buffer;
}