#include "can_protocol_pcan.h"
#include <cstring>
#include <algorithm>
#include <utility>

namespace {

// opcode in bits 0..9, channel in bits 12..15
inline uint16_t OpcodeChannel(uint16_t opcode, uint8_t channel) {
    return static_cast<uint16_t>((opcode & 0x03FFU) | ((channel & 0x0FU) << 12));
}

inline size_t AlignUp4(size_t v) { return (v + 3U) & ~static_cast<size_t>(3U); }

// Collects commands into transfers of at most PCAN_CMD_PACKET_SIZE bytes. A transfer that is not full is
// terminated with an end-of-collection record, which is "all 64 bits set to 1" in the firmware's eyes.
class CommandTransfers {
public:
    template <typename T>
    void Add(const T& cmd) {
        static_assert(sizeof(T) == 8, "uCAN commands are 8 bytes");
        if (cur_.size() + sizeof(T) > static_cast<size_t>(PCAN_CMD_PACKET_SIZE)) Flush();
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&cmd);
        cur_.insert(cur_.end(), p, p + sizeof(T));
    }

    void Flush() {
        if (cur_.empty()) return;
        if (cur_.size() < static_cast<size_t>(PCAN_CMD_PACKET_SIZE)) cur_.insert(cur_.end(), 8, 0xFF);
        out_.push_back(std::move(cur_));
        cur_.clear();
    }

    std::vector<std::vector<uint8_t>> Take() { Flush(); return std::move(out_); }

private:
    std::vector<uint8_t> cur_;
    std::vector<std::vector<uint8_t>> out_;
};

PcanCommand SimpleCommand(uint16_t opcode, uint8_t channel) {
    PcanCommand c = {};
    c.opcode_channel = OpcodeChannel(opcode, channel);
    return c;
}

PcanOptionCmd OptionCommand(uint16_t opcode, uint8_t channel, uint16_t mask, uint16_t usb_mask) {
    PcanOptionCmd c = {};
    c.opcode_channel = OpcodeChannel(opcode, channel);
    c.mask = mask;
    c.usb_mask = usb_mask;
    return c;
}

uint32_t Clamp(uint32_t v, uint32_t lo, uint32_t hi) { return std::min(std::max(v, lo), hi); }

PcanTimingSlow SlowTiming(uint8_t channel, const CanBittiming& bt) {
    const CanBitTimingLimits& lim = PcanNominalLimits();
    PcanTimingSlow t = {};
    t.opcode_channel = OpcodeChannel(PCAN_CMD_TIMING_SLOW, channel);
    t.ewl   = 96;
    t.sjw_t = static_cast<uint8_t>((Clamp(bt.sjw, 1, lim.sjw_max) - 1U) & 0x7FU);   // triple sampling off
    t.tseg2 = static_cast<uint8_t>((Clamp(bt.phase_seg2, 1, lim.tseg2_max) - 1U) & 0x7FU);
    t.tseg1 = static_cast<uint8_t>((Clamp(bt.prop_seg + bt.phase_seg1, 1, lim.tseg1_max) - 1U) & 0xFFU);
    t.brp   = static_cast<uint16_t>((Clamp(bt.brp, 1, lim.brp_max) - 1U) & 0x3FFU);
    return t;
}

PcanTimingFast FastTiming(uint8_t channel, const CanBittiming& bt) {
    const CanBitTimingLimits& lim = PcanDataLimits();
    PcanTimingFast t = {};
    t.opcode_channel = OpcodeChannel(PCAN_CMD_TIMING_FAST, channel);
    t.sjw   = static_cast<uint8_t>((Clamp(bt.sjw, 1, lim.sjw_max) - 1U) & 0x0FU);
    t.tseg2 = static_cast<uint8_t>((Clamp(bt.phase_seg2, 1, lim.tseg2_max) - 1U) & 0x0FU);
    t.tseg1 = static_cast<uint8_t>((Clamp(bt.prop_seg + bt.phase_seg1, 1, lim.tseg1_max) - 1U) & 0x1FU);
    t.brp   = static_cast<uint16_t>((Clamp(bt.brp, 1, lim.brp_max) - 1U) & 0x3FFU);
    return t;
}

// One CAN frame record (types are not asserted here: the caller checked size and type)
void DecodeCanRx(const uint8_t* rec, int size, std::vector<CanMessage>& out, uint8_t channel) {
    const PcanRxMsg* rx = reinterpret_cast<const PcanRxMsg*>(rec);
    if (static_cast<uint8_t>(rx->channel_dlc & 0x0F) != channel) return;   // another CAN channel of the adapter

    CanMessage msg;
    msg.is_extended     = (rx->flags & PCAN_FLAG_EXT_ID) != 0;
    msg.is_rtr          = (rx->flags & PCAN_FLAG_RTR) != 0;
    msg.is_fd           = (rx->flags & PCAN_FLAG_EDL) != 0;
    msg.is_brs          = msg.is_fd && (rx->flags & PCAN_FLAG_BRS) != 0;
    msg.is_esi          = msg.is_fd && (rx->flags & PCAN_FLAG_ESI) != 0;
    msg.is_tx_echo      = (rx->flags & (PCAN_FLAG_API_SRR | PCAN_FLAG_HW_SRR)) != 0;
    msg.id              = rx->can_id & (msg.is_extended ? CAN_ID_EFF_MASK : CAN_ID_SFF_MASK);
    msg.dlc             = (rx->channel_dlc >> 4) & 0x0F;
    msg.hw_timestamp_us = rx->ts_low;
    msg.has_hw_timestamp = true;

    if (!msg.is_rtr) {
        const uint8_t data_len = msg.is_fd ? CanProtocolUtils::DlcToBytes(msg.dlc) : std::min<uint8_t>(msg.dlc, 8);
        const int available = std::max(0, size - static_cast<int>(offsetof(PcanRxMsg, data)));
        msg.payload.assign(rx->data, rx->data + std::min<int>(data_len, available));
        msg.payload.resize(data_len, 0);   // a record shorter than its DLC announces is padded, never over-read
    }
    out.push_back(msg);
}

void HandleRecord(const uint8_t* rec, int size, std::vector<CanMessage>& out, uint8_t channel) {
    uint16_t type = 0;
    std::memcpy(&type, rec + 2, sizeof(type));
    if (type == PCAN_MSG_CAN_RX && size >= static_cast<int>(offsetof(PcanRxMsg, data))) {
        DecodeCanRx(rec, size, out, channel);
    }
    // status, error, bus load, calibration, overrun and cache records are not used yet
}

} // namespace

void CanProtocolPcan::ParseRxStream(const uint8_t* buffer, int length, std::vector<CanMessage>& out,
                                    CanRxStreamState& state, uint8_t channel) {
    if (!buffer || length <= 0) return;
    const int max_record = static_cast<int>(CanRxStreamState::kMaxRecord);
    int pos = 0;

    // 1. finish the record that started at the end of the previous transfer
    while (state.frag_len > 0 && pos < length) {
        if (state.frag_len < 2) {                       // not even the size field is complete yet
            state.frag[state.frag_len++] = buffer[pos++];
            continue;
        }
        uint16_t size = 0;
        std::memcpy(&size, state.frag, sizeof(size));
        if (size < sizeof(PcanMsgHeader) || size > max_record || state.frag_len > size) {   // out of sync: drop the rest
            state.Reset();
            return;
        }
        const int take = std::min<int>(size - state.frag_len, length - pos);
        std::memcpy(state.frag + state.frag_len, buffer + pos, take);
        state.frag_len = static_cast<uint16_t>(state.frag_len + take);
        pos += take;
        if (state.frag_len == size) {
            HandleRecord(state.frag, size, out, channel);
            state.Reset();
        }
    }

    // 2. whole records of this transfer
    while (pos + 2 <= length) {
        uint16_t size = 0;
        std::memcpy(&size, buffer + pos, sizeof(size));
        if (size == 0) break;                           // empty record: end of the list
        if (size < sizeof(PcanMsgHeader) || size > max_record) {
            state.Reset();                              // garbage: nothing can be trusted any more
            return;
        }
        if (pos + size > length) {                      // the record continues in the next transfer
            state.frag_len = static_cast<uint16_t>(length - pos);
            std::memcpy(state.frag, buffer + pos, state.frag_len);
            return;
        }
        HandleRecord(buffer + pos, size, out, channel);
        pos += size;
    }
    if (pos + 1 == length && length > 0 && state.frag_len == 0) {
        // a single trailing byte can only be the first half of a size field
        state.frag[0] = buffer[pos];
        state.frag_len = 1;
    }
}

std::vector<uint8_t> CanProtocolPcan::BuildTxBuffer(const CanMessage& msg, uint8_t channel) {
    // The echo client value identifies the frame in the echo; 8 bits are all the record has
    static uint8_t next_client = 0;

    PcanTxMsg tx = {};
    tx.type = PCAN_MSG_CAN_TX;
    tx.can_id = msg.id & (msg.is_extended ? CAN_ID_EFF_MASK : CAN_ID_SFF_MASK);

    uint16_t flags = PCAN_FLAG_HW_SRR | PCAN_FLAG_API_SRR;      // ask for the frame to come back as an echo
    if (msg.is_extended) flags |= PCAN_FLAG_EXT_ID;
    if (msg.is_fd) {
        flags |= PCAN_FLAG_EDL;
        if (msg.is_brs) flags |= PCAN_FLAG_BRS;
    } else if (msg.is_rtr) {
        flags |= PCAN_FLAG_RTR;
    }
    tx.flags = flags;
    tx.client = next_client++;

    const uint8_t dlc = msg.dlc & 0x0F;
    tx.channel_dlc = static_cast<uint8_t>((channel & 0x0F) | (dlc << 4));

    const uint8_t data_len = msg.is_fd ? CanProtocolUtils::DlcToBytes(dlc) : std::min<uint8_t>(dlc, 8);
    const size_t payload = (msg.is_rtr && !msg.is_fd) ? 0U : data_len;
    if (payload && !msg.payload.empty()) {
        std::memcpy(tx.data, msg.payload.data(), std::min<size_t>(payload, msg.payload.size()));
    }

    const size_t record_size = AlignUp4(offsetof(PcanTxMsg, data) + payload);
    tx.size = static_cast<uint16_t>(record_size);

    // the record, then an empty record that ends the list in the USB transfer
    std::vector<uint8_t> out(record_size + sizeof(PcanMsgHeader), 0);
    std::memcpy(out.data(), &tx, std::min(record_size, sizeof(tx)));
    return out;
}

std::vector<std::vector<uint8_t>> CanProtocolPcan::BuildStartCommands(uint8_t channel, bool enable_fd, bool listen_only,
                                                                       const CanBittiming& nominal, const CanBittiming& data) {
    CommandTransfers cmds;

    // 1. CAN clock domain of the adapter and zeroed error counters
    PcanClockCmd clk = {};
    clk.opcode_channel = OpcodeChannel(PCAN_USB_CMD_CLK_SET, channel);
    clk.mode = PCAN_CLK_80MHZ;
    cmds.Add(clk);

    PcanWrErrCnt err_cnt = {};
    err_cnt.opcode_channel = OpcodeChannel(PCAN_CMD_WR_ERR_CNT, channel);
    err_cnt.sel_mask = PCAN_WRERRCNT_TE | PCAN_WRERRCNT_RE;
    cmds.Add(err_cnt);

    // 2. options: error reporting on, ISO CAN FD (or CAN 2.0 only frames), bus load and fast-forward off
    const uint16_t opt_clr = static_cast<uint16_t>(PCAN_OPTION_BUSLOAD | (enable_fd ? PCAN_OPTION_20AB_MODE : 0));
    const uint16_t opt_set = static_cast<uint16_t>(PCAN_OPTION_ERROR | (enable_fd ? PCAN_OPTION_ISO_MODE : PCAN_OPTION_20AB_MODE));
    cmds.Add(OptionCommand(PCAN_CMD_CLR_DIS_OPTION, channel, opt_clr, PCAN_USB_OPTION_FAST_FWD));
    cmds.Add(OptionCommand(PCAN_CMD_SET_EN_OPTION, channel, opt_set, 0));

    // 3. bit timing
    if (enable_fd) cmds.Add(FastTiming(channel, data));
    cmds.Add(SlowTiming(channel, nominal));
    cmds.Flush();

    // 4. accept every identifier: all 64 rows of the 11-bit acceptance filter
    for (uint16_t row = 0; row < 64; ++row) {
        PcanStdFilter f = {};
        f.opcode_channel = OpcodeChannel(PCAN_CMD_SET_STD_FILTER, channel);
        f.idx = static_cast<uint8_t>(row);
        f.mask = 0xFFFFFFFFU;
        cmds.Add(f);
    }
    cmds.Flush();

    // 5. bus on
    cmds.Add(SimpleCommand(listen_only ? PCAN_CMD_LISTEN_ONLY_MODE : PCAN_CMD_NORMAL_MODE, channel));
    return cmds.Take();
}

std::vector<std::vector<uint8_t>> CanProtocolPcan::BuildStopCommands(uint8_t channel) {
    CommandTransfers cmds;
    cmds.Add(SimpleCommand(PCAN_CMD_RESET_MODE, channel));
    cmds.Flush();
    cmds.Add(OptionCommand(PCAN_CMD_CLR_DIS_OPTION, channel, PCAN_OPTION_ERROR | PCAN_OPTION_BUSLOAD, 0));
    return cmds.Take();
}
