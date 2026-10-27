#include "can_protocol_gsusb.h"
#include <cstring>
#include <algorithm>

// SocketCAN / gs_usb CAN ID flags
constexpr uint32_t GS_CAN_EFF_FLAG = 0x80000000U; // Extended Frame Format (29-bit)
constexpr uint32_t GS_CAN_RTR_FLAG = 0x40000000U; // Remote Transmission Request

// gs_host_frame flags
constexpr uint8_t GS_CAN_FLAG_FD  = (1 << 1);
constexpr uint8_t GS_CAN_FLAG_BRS = (1 << 2);
constexpr uint8_t GS_CAN_FLAG_ESI = (1 << 3);

// Parses exactly one gs_host_frame occupying `length` bytes (optionally followed by a timestamp)
static CanMessage ParseSingleFrame(const uint8_t* buffer, int length) {
    const GsHostFrame* frame = reinterpret_cast<const GsHostFrame*>(buffer);

    CanMessage msg;
    msg.is_tx_echo  = (frame->echo_id != 0xFFFFFFFFU);
    msg.is_extended = (frame->can_id & GS_CAN_EFF_FLAG) != 0;
    msg.is_rtr      = (frame->can_id & GS_CAN_RTR_FLAG) != 0;
    msg.id          = frame->can_id & (msg.is_extended ? CAN_ID_EFF_MASK : CAN_ID_SFF_MASK);
    msg.is_fd       = (frame->flags & GS_CAN_FLAG_FD) != 0;
    msg.is_brs      = (frame->flags & GS_CAN_FLAG_BRS) != 0;
    msg.is_esi      = (frame->flags & GS_CAN_FLAG_ESI) != 0;
    msg.dlc         = frame->can_dlc & 0x0F;

    uint8_t data_len = msg.is_fd ? CanProtocolUtils::DlcToBytes(msg.dlc) : std::min<uint8_t>(msg.dlc, 8);
    if (!msg.is_rtr) {
        // A malformed transfer may hold fewer data bytes than its flags/DLC announce (e.g. 24 bytes with the
        // FD flag): copy what is there and pad with zeros instead of reading past the end of the buffer.
        // The data area is bytes 12.. up to the timestamp (offset 20 / 76) or the end of the frame.
        const int data_end = (length == 24) ? 20 : (length >= 80 ? 76 : length);
        const int available = std::max(0, data_end - 12);
        const int copy = std::min<int>(data_len, available);
        msg.payload.assign(frame->data, frame->data + copy);
        msg.payload.resize(data_len, 0);
    }

    // Extract hardware timestamp if present (offset 20 for Classic CAN, offset 76 for CAN FD)
    if (length == 24) {
        std::memcpy(&msg.hw_timestamp_us, buffer + 20, sizeof(uint32_t));
        msg.has_hw_timestamp = true;
    } else if (length >= 80) {
        std::memcpy(&msg.hw_timestamp_us, buffer + 76, sizeof(uint32_t));
        msg.has_hw_timestamp = true;
    }
    return msg;
}

std::vector<CanMessage> CanProtocolGsUsb::ParseRxBuffer(const uint8_t* buffer, int length) {
    std::vector<CanMessage> messages;
    ParseRxBuffer(buffer, length, messages);
    return messages;
}

void CanProtocolGsUsb::ParseRxBuffer(const uint8_t* buffer, int length, std::vector<CanMessage>& out) {
    if (!buffer || length < 20) return;

    // Frame sizes: Classic 20 (+4 timestamp), FD 76 (+4 timestamp). A transfer of one of these exact
    // sizes is a single frame (this is what gs_usb firmware sends). Anything else is split into
    // equal frames only if the stride is unambiguous; otherwise the first frame is parsed as before.
    int stride = length;
    if (length != 20 && length != 24 && length != 76 && length != 80) {
        const int base = (buffer[10] & GS_CAN_FLAG_FD) ? 76 : 20;
        const bool with_ts    = (length % (base + 4)) == 0;
        const bool without_ts = (length % base) == 0;
        if (with_ts != without_ts) stride = with_ts ? base + 4 : base;
    }

    for (int off = 0; off + stride <= length; off += stride) {
        out.push_back(ParseSingleFrame(buffer + off, stride));
    }
}

std::vector<uint8_t> CanProtocolGsUsb::BuildTxBuffer(const CanMessage& msg, uint8_t channel) {
    GsHostFrame frame = {};
    // Unique echo id per frame (0xFFFFFFFF is reserved for received frames) so the TX echo
    // returned by the adapter can be matched with the request
    static uint32_t next_echo_id = 0;
    frame.echo_id = next_echo_id++;
    if (next_echo_id == 0xFFFFFFFFU) next_echo_id = 0;
    frame.channel = channel;

    frame.can_id = msg.id & (msg.is_extended ? CAN_ID_EFF_MASK : CAN_ID_SFF_MASK);
    if (msg.is_extended) frame.can_id |= GS_CAN_EFF_FLAG;
    if (msg.is_rtr && !msg.is_fd) frame.can_id |= GS_CAN_RTR_FLAG;

    if (msg.is_fd) {
        frame.flags |= GS_CAN_FLAG_FD;
        if (msg.is_brs) frame.flags |= GS_CAN_FLAG_BRS;
    }

    frame.can_dlc = msg.dlc & 0x0F;
    uint8_t data_len = msg.is_fd ? CanProtocolUtils::DlcToBytes(frame.can_dlc) : std::min<uint8_t>(frame.can_dlc, 8);

    if (!msg.is_rtr && !msg.payload.empty()) {
        std::memcpy(frame.data, msg.payload.data(), std::min<size_t>(data_len, msg.payload.size()));
    }

    size_t transfer_size = msg.is_fd ? sizeof(GsHostFrame) : 20;
    std::vector<uint8_t> out_buffer(transfer_size);
    std::memcpy(out_buffer.data(), &frame, transfer_size);

    return out_buffer;
}
