#include "can_protocol_elmue.h"
#include <cstdio>
#include <cstring>

namespace {

uint32_t Le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// Payload bytes of a CAN FD frame are always one of the DLC lengths
uint8_t PadToDlcLength(size_t n) {
    return CanProtocolUtils::DlcToBytes(CanProtocolUtils::BytesToDlc(static_cast<uint8_t>(n > 64 ? 64 : n)));
}

constexpr size_t kMaxTextEvent = 200;
constexpr size_t kMaxEventsPerTransfer = 4096;   // a firmware that floods the host with text cannot make the buffer grow without end

// One complete message at b[0..size); unknown or too short ones are skipped
void ParseMessage(const uint8_t* b, int size, bool with_ts, std::vector<CanMessage>& out, std::string* events, CanMessage* echoes) {
    const uint8_t type = b[1];
    const size_t ts_len = with_ts ? 4 : 0;

    if (type == ELM_MSG_RX_FRAME) {
        if (static_cast<size_t>(size) < ELM_RX_HEADER + ts_len) return;
        const uint8_t flags = b[2];
        const uint32_t can_id = Le32(b + 3);
        CanMessage m;
        m.is_extended = (can_id & ELM_ID_29BIT) != 0;
        m.is_rtr      = (can_id & ELM_ID_RTR) != 0;
        m.id          = can_id & (m.is_extended ? CAN_ID_EFF_MASK : CAN_ID_SFF_MASK);
        m.is_fd       = (flags & ELM_FRM_FDF) != 0;
        m.is_brs      = m.is_fd && (flags & ELM_FRM_BRS) != 0;
        m.is_esi      = m.is_fd && (flags & ELM_FRM_ESI) != 0;
        if (with_ts) {
            m.hw_timestamp_us = Le32(b + ELM_RX_HEADER);
            m.has_hw_timestamp = true;
        }
        const uint8_t* data = b + ELM_RX_HEADER + ts_len;
        size_t n = static_cast<size_t>(size) - ELM_RX_HEADER - ts_len;
        if (m.is_rtr) {
            // A remote frame carries its DLC in the first data byte (none: DLC 0)
            m.dlc = n >= 1 ? (data[0] & 0x0F) : 0;
        } else {
            if (n > (m.is_fd ? 64u : 8u)) n = m.is_fd ? 64 : 8;
            m.dlc = m.is_fd ? CanProtocolUtils::BytesToDlc(static_cast<uint8_t>(n)) : static_cast<uint8_t>(n);
            m.payload.assign(data, data + n);
            m.payload.resize(m.is_fd ? CanProtocolUtils::DlcToBytes(m.dlc) : n, 0);
        }
        out.push_back(m);
        return;
    }

    if (type == ELM_MSG_TX_ECHO) {
        if (static_cast<size_t>(size) < 3 + ts_len) return;
        const uint8_t marker = b[2];
        if (!echoes || marker == 0 || !echoes[marker].is_tx_echo) return;
        CanMessage m = echoes[marker];
        echoes[marker] = CanMessage();
        m.tx_marker = marker;
        if (with_ts) {
            m.hw_timestamp_us = Le32(b + 3);
            m.has_hw_timestamp = true;
        }
        out.push_back(m);
        return;
    }

    if (type == ELM_MSG_ERROR) {
        if (static_cast<size_t>(size) < 14 + ts_len) return;
        const uint32_t err_id = Le32(b + 2);
        CanMessage m;
        m.is_error = true;
        m.id = err_id & CAN_ID_SFF_MASK;
        m.dlc = 8;
        m.payload.assign(b + 6, b + 14);
        if (with_ts) {
            m.hw_timestamp_us = Le32(b + 14);
            m.has_hw_timestamp = true;
        }
        out.push_back(m);
        if (events && events->size() < kMaxEventsPerTransfer) {
            char line[40];
            std::snprintf(line, sizeof(line), "E%08X:", err_id);
            *events += line;
            for (int i = 0; i < 8; ++i) {
                std::snprintf(line, sizeof(line), "%02X", b[6 + i]);
                *events += line;
            }
            *events += '\n';
        }
        return;
    }

    if (type == ELM_MSG_BUSLOAD) {
        if (size < 3 || !events || events->size() >= kMaxEventsPerTransfer) return;
        *events += "U" + std::to_string(b[2]) + "\n";
        return;
    }

    if (type == ELM_MSG_STRING) {
        if (size <= 2 || !events || events->size() >= kMaxEventsPerTransfer) return;
        std::string text;
        for (int i = 2; i < size && text.size() < kMaxTextEvent; ++i) {
            const uint8_t c = b[i];
            if (c == 0) break;
            text += (c >= 32 && c < 127) ? static_cast<char>(c) : ' ';
        }
        while (!text.empty() && text.back() == ' ') text.pop_back();
        if (!text.empty()) *events += "T" + text + "\n";
    }
}

} // namespace

void CanProtocolElmue::ParseRx(const uint8_t* buffer, int length, bool with_timestamp, std::vector<CanMessage>& out,
                               std::string* events, CanMessage* echoes) {
    if (!buffer || length < 2) return;

    int off = 0;
    int remaining_in_blob = -1;               // -1: not a blob, the messages are consumed until the buffer ends
    if (buffer[1] == ELM_MSG_RX_BLOB) {
        remaining_in_blob = buffer[0];
        off = 2;
    }
    while (off + 2 <= length && remaining_in_blob != 0) {
        const int size = buffer[off];
        if (size < 2 || off + size > length) break;       // truncated or corrupt: nothing sensible follows
        ParseMessage(buffer + off, size, with_timestamp, out, events, echoes);
        off += size;
        if (remaining_in_blob > 0) --remaining_in_blob;
    }
}

std::vector<uint8_t> CanProtocolElmue::BuildTxBuffer(const CanMessage& msg, uint8_t* marker_out) {
    // Markers 1..255 identify the frame in its tx echo; 0 would mean "no echo". One counter for all adapters: a
    // marker only has to be unique among the frames of one adapter that are still waiting for their echo.
    static uint8_t next_marker = 0;
    if (++next_marker == 0) next_marker = 1;

    uint32_t can_id = msg.id & (msg.is_extended ? CAN_ID_EFF_MASK : CAN_ID_SFF_MASK);
    if (msg.is_extended) can_id |= ELM_ID_29BIT;
    if (msg.is_rtr && !msg.is_fd) can_id |= ELM_ID_RTR;

    uint8_t flags = 0;
    if (msg.is_fd) {
        flags |= ELM_FRM_FDF;
        if (msg.is_brs) flags |= ELM_FRM_BRS;
    }

    std::vector<uint8_t> data;
    if (msg.is_rtr && !msg.is_fd) {
        if ((msg.dlc & 0x0F) != 0) data.push_back(msg.dlc & 0x0F);       // remote frames: none or one byte, the DLC
    } else {
        const size_t want = msg.is_fd ? CanProtocolUtils::DlcToBytes(msg.dlc & 0x0F) : std::min<size_t>(msg.dlc & 0x0F, 8);
        data.assign(msg.payload.begin(), msg.payload.begin() + std::min(want, msg.payload.size()));
        data.resize(msg.is_fd ? PadToDlcLength(want) : want, 0);
    }

    std::vector<uint8_t> out(ELM_TX_HEADER + data.size());
    out[0] = static_cast<uint8_t>(out.size());
    out[1] = ELM_MSG_TX_FRAME;
    out[2] = flags;
    std::memcpy(&out[3], &can_id, 4);
    out[7] = next_marker;
    if (!data.empty()) std::memcpy(&out[ELM_TX_HEADER], data.data(), data.size());
    if (marker_out) *marker_out = next_marker;
    return out;
}

bool CanProtocolElmue::ParseTxFrame(const uint8_t* b, int length, CanMessage& msg, uint8_t& marker) {
    if (!b || length < static_cast<int>(ELM_TX_HEADER) || b[1] != ELM_MSG_TX_FRAME || b[0] > length) return false;
    const int size = b[0];
    if (size < static_cast<int>(ELM_TX_HEADER)) return false;
    const uint8_t flags = b[2];
    const uint32_t can_id = Le32(b + 3);
    marker = b[7];

    msg = CanMessage();
    msg.is_tx_echo  = true;
    msg.is_extended = (can_id & ELM_ID_29BIT) != 0;
    msg.is_rtr      = (can_id & ELM_ID_RTR) != 0;
    msg.id          = can_id & (msg.is_extended ? CAN_ID_EFF_MASK : CAN_ID_SFF_MASK);
    msg.is_fd       = (flags & ELM_FRM_FDF) != 0;
    msg.is_brs      = msg.is_fd && (flags & ELM_FRM_BRS) != 0;
    msg.tx_marker   = marker;
    size_t n = static_cast<size_t>(size) - ELM_TX_HEADER;
    const uint8_t* data = b + ELM_TX_HEADER;
    if (msg.is_rtr) {
        msg.dlc = n >= 1 ? (data[0] & 0x0F) : 0;
    } else {
        if (n > 64) n = 64;
        msg.dlc = msg.is_fd ? CanProtocolUtils::BytesToDlc(static_cast<uint8_t>(n)) : static_cast<uint8_t>(std::min<size_t>(n, 8));
        msg.payload.assign(data, data + n);
    }
    return true;
}

std::vector<uint8_t> CanProtocolElmue::BuildFilter(ElmFilterOp op, uint32_t filter, uint32_t mask) {
    ElmFilter f = {};
    f.operation = op;
    f.filter = filter;
    f.mask = mask;
    std::vector<uint8_t> out(sizeof(f));
    std::memcpy(out.data(), &f, sizeof(f));
    return out;
}

uint32_t CanProtocolElmue::ModeFlags(uint32_t features, bool want_timestamp) {
    uint32_t flags = ELM_FEATURE_PROTOCOL;
    if (features & ELM_FEATURE_USB_BLOBS) flags |= ELM_FEATURE_USB_BLOBS;
    if (want_timestamp && (features & GS_CAN_FEATURE_HW_TIMESTAMP)) flags |= GS_CAN_FEATURE_HW_TIMESTAMP;
    return flags;
}

bool CanProtocolElmue::ParseBoardInfo(const uint8_t* data, int length, ElmBoardInfo& info) {
    if (!data || length < static_cast<int>(ELM_BOARD_INFO_MIN)) return false;
    info = ElmBoardInfo();
    std::memcpy(&info, data, std::min<size_t>(static_cast<size_t>(length), sizeof(info)));
    info.mcu_name[sizeof(info.mcu_name) - 1] = 0;
    info.board_name[sizeof(info.board_name) - 1] = 0;
    if (length < static_cast<int>(sizeof(info))) info.board_flags = 0;
    return true;
}

std::string CanProtocolElmue::BoardFlagsText(uint32_t flags) {
    std::string t;
    auto add = [&](const char* s) { if (!t.empty()) t += ", "; t += s; };
    if (flags & ELM_BOARD_QUARTZ) add("quartz");
    if (flags & ELM_BOARD_USB_HIGHSPEED) add("USB high speed");
    if (flags & ELM_BOARD_LIN) add("LIN");
    return t;
}

ElmErrorInfo CanProtocolElmue::DecodeError(uint32_t err_id, const uint8_t d[8]) {
    ElmErrorInfo info;
    info.tx_errors = d[6];
    info.rx_errors = d[7];

    auto add = [&](const char* s) { if (!info.text.empty()) info.text += ", "; info.text += s; };

    if (err_id & ELM_ERID_TX_TIMEOUT)  add("TX timeout");
    if (err_id & ELM_ERID_ARBITRATION) add("arbitration lost");
    if (err_id & ELM_ERID_NO_ACK)      add("no ACK");
    if (err_id & ELM_ERID_BUS_OFF)     add("bus off");
    if (err_id & ELM_ERID_BUS_ERROR)   add("bus error");
    if (err_id & ELM_ERID_RESTARTED)   add("controller restarted");
    if (err_id & ELM_ERID_CRC)         add("CRC error");

    // data[1] eErrFlagsByte1: controller state changes (bit 0, 1: legacy buffer overflow flags)
    const uint8_t b1 = d[1];
    if (b1 & 0x01) add("RX buffer overflow");
    if (b1 & 0x02) add("TX buffer overflow");
    if (b1 & 0x04) add("RX warning level");
    if (b1 & 0x08) add("TX warning level");
    if (b1 & 0x10) add("RX passive");
    if (b1 & 0x20) add("TX passive");
    if (b1 & 0x40) add("bus active again");

    // data[2] eErrFlagsByte2: protocol violations
    const uint8_t b2 = d[2];
    if (b2 & 0x01) add("bit error");
    if (b2 & 0x02) add("form error");
    if (b2 & 0x04) add("stuff error");
    if (b2 & 0x08) add("cannot send dominant");
    if (b2 & 0x10) add("cannot send recessive");
    if (b2 & 0x20) add("bus overload");
    if (b2 & 0x40) add("active error announcement");
    if (b2 & 0x80) add("error while transmitting");

    // data[5] eErrorAppFlags of the firmware
    const uint8_t b5 = d[5];
    if (b5 & 0x01) add("adapter RX failure (CAN faster than the firmware)");
    if (b5 & 0x02) add("adapter TX failure");
    if (b5 & 0x04) add("adapter TX buffer full");
    if (b5 & 0x08) add("USB IN overflow (CAN faster than USB)");
    if (b5 & 0x10) add("TX not acknowledged for 500 ms");

    if (err_id & ELM_ERID_BUS_OFF) {
        info.state = ELM_BUS_OFF;
        info.state_known = true;
    } else if (b1 & 0x30) {
        info.state = ELM_BUS_PASSIVE;
        info.state_known = true;
    } else if (b1 & 0x0C) {
        info.state = ELM_BUS_WARNING;
        info.state_known = true;
    } else if ((b1 & 0x40) || (err_id & ELM_ERID_RESTARTED)) {
        info.state = ELM_BUS_ACTIVE;
        info.state_known = true;
    }
    return info;
}

bool CanProtocolElmue::ParseErrorEvent(const std::string& body, uint32_t& err_id, uint8_t data[8]) {
    // "<8 hex>:<16 hex>"
    if (body.size() != 8 + 1 + 16 || body[8] != ':') return false;
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    uint32_t id = 0;
    for (int i = 0; i < 8; ++i) {
        const int v = hex(body[i]);
        if (v < 0) return false;
        id = (id << 4) | static_cast<uint32_t>(v);
    }
    for (int i = 0; i < 8; ++i) {
        const int hi = hex(body[9 + 2 * i]);
        const int lo = hex(body[10 + 2 * i]);
        if (hi < 0 || lo < 0) return false;
        data[i] = static_cast<uint8_t>(hi * 16 + lo);
    }
    err_id = id;
    return true;
}

const char* CanProtocolElmue::FeedbackText(uint8_t code) {
    switch (code) {
        case 2:   return "ok";
        case '1': return "invalid command";
        case '2': return "invalid parameter";
        case '3': return "the adapter must be open";
        case '4': return "the adapter must be closed";
        case '5': return "error in the processor library";
        case '6': return "not supported by the board";
        case '7': return "TX buffer full";
        case '8': return "bus is off";
        case '9': return "no TX in listen-only mode";
        case ':': return "baud rate not set";
        case ';': return "programming the option bytes failed";
        case '<': return "reset required";
        case '=': return "parameter out of range";
        default:  return "unknown error";
    }
}
