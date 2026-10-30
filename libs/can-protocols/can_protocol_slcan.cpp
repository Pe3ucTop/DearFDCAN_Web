#include "can_protocol_slcan.h"

#include <cstdio>
#include <cstdlib>

namespace {

constexpr size_t kMaxText = 250;      // longest text reply token kept (a line is at most CanRxStreamState::kMaxRecord)

int HexValue(uint8_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

bool ParseHex(const uint8_t* s, size_t digits, uint32_t& value) {
    value = 0;
    for (size_t i = 0; i < digits; ++i) {
        const int v = HexValue(s[i]);
        if (v < 0) return false;
        value = (value << 4) | static_cast<uint32_t>(v);
    }
    return true;
}

char Nibble(unsigned v) { return "0123456789ABCDEF"[v & 0xF]; }

void AppendHex(std::vector<uint8_t>& out, uint32_t value, int digits) {
    for (int i = digits - 1; i >= 0; --i) out.push_back(static_cast<uint8_t>(Nibble(value >> (4 * i))));
}

void AppendLineReply(std::string& replies, const uint8_t* line, size_t n) {
    replies.push_back('L');
    for (size_t i = 0; i < n && i < kMaxText; ++i) {
        const uint8_t c = line[i];
        replies.push_back((c >= 0x20 && c < 0x7F) || c == '\t' ? static_cast<char>(c) : '?');
    }
    replies.push_back('\n');
}

// A non-frame line: acknowledge, error or text
void HandleReply(const uint8_t* line, size_t n, std::string* replies) {
    if (!replies) return;
    if (n == 0) { replies->append("K\n"); return; }
    if (n == 1 && (line[0] == 'z' || line[0] == 'Z')) { replies->append("K\n"); return; }    // Lawicel: frame sent
    if (line[0] == '#') {
        if (n == 1) { replies->append("K\n"); return; }
        replies->push_back('E');
        replies->push_back(static_cast<char>(line[1]));
        replies->push_back('\n');
        return;
    }
    AppendLineReply(*replies, line, n);
}

bool IsFrameLetter(uint8_t c) {
    switch (c) {
        case 't': case 'T': case 'r': case 'R': case 'd': case 'D': case 'b': case 'B': return true;
        default: return false;
    }
}

bool ParseUnsigned(const std::string& s, uint32_t& v) {
    if (s.empty()) return false;
    char* end = nullptr;
    const unsigned long x = std::strtoul(s.c_str(), &end, 10);
    if (end == s.c_str()) return false;
    v = static_cast<uint32_t>(x);
    return true;
}

} // namespace

namespace CanProtocolSlcan {

CanBitTimingLimits DefaultNominalLimits() { return CanBitTimingLimits{256, 128, 128, 512}; }
CanBitTimingLimits DefaultDataLimits()    { return CanBitTimingLimits{32, 16, 16, 32}; }

bool ParseFrameLine(const uint8_t* s, size_t n, CanMessage& msg) {
    if (n < 5 || !IsFrameLetter(s[0])) return false;
    const uint8_t type = s[0];
    const bool ext = type >= 'A' && type <= 'Z';
    const uint8_t lower = ext ? static_cast<uint8_t>(type + 32) : type;
    const bool remote = lower == 'r';
    const bool fd = lower == 'd' || lower == 'b';
    const bool brs = lower == 'b';
    const size_t id_digits = ext ? 8 : 3;
    if (n < 1 + id_digits + 1) return false;

    uint32_t id = 0, dlc = 0;
    if (!ParseHex(s + 1, id_digits, id)) return false;
    if (ext ? id > CAN_ID_EFF_MASK : id > CAN_ID_SFF_MASK) return false;
    if (!ParseHex(s + 1 + id_digits, 1, dlc)) return false;
    if (!fd && dlc > 8) return false;

    size_t pos = 1 + id_digits + 1;
    const size_t nbytes = remote ? 0 : CanProtocolUtils::DlcToBytes(static_cast<uint8_t>(dlc));
    if (n < pos + 2 * nbytes) return false;

    CanMessage m;
    m.id = id;
    m.is_extended = ext;
    m.is_rtr = remote;
    m.is_fd = fd;
    m.is_brs = brs;
    m.dlc = static_cast<uint8_t>(dlc);
    uint8_t data[CanPayload::kCapacity];
    for (size_t i = 0; i < nbytes; ++i) {
        uint32_t b = 0;
        if (!ParseHex(s + pos + 2 * i, 2, b)) return false;
        data[i] = static_cast<uint8_t>(b);
    }
    pos += 2 * nbytes;
    if (nbytes) m.payload.assign(data, data + nbytes);

    // What may follow: nothing, the ESI marker, or a 16-bit timestamp of the adapter (ignored)
    size_t rest = n - pos;
    if (rest >= 1 && s[n - 1] == 'S' && fd) { m.is_esi = true; --rest; }
    if (rest != 0 && rest != 4) return false;
    if (rest == 4) { uint32_t ts; if (!ParseHex(s + pos, 4, ts)) return false; }

    msg = m;
    return true;
}

void ParseRxStream(const uint8_t* buffer, int length, std::vector<CanMessage>& out, CanRxStreamState& state,
                   std::string* replies) {
    if (!buffer || length <= 0) return;
    if (state.frag_len > CanRxStreamState::kMaxRecord) state.frag_len = 0;

    for (int i = 0; i < length; ++i) {
        const uint8_t c = buffer[i];
        if (c == '\r') {
            if (state.discard) {
                state.discard = false;
            } else {
                CanMessage m;
                if (state.frag_len > 0 && IsFrameLetter(state.frag[0])) {
                    if (ParseFrameLine(state.frag, state.frag_len, m)) out.push_back(m);
                } else {
                    HandleReply(state.frag, state.frag_len, replies);
                }
            }
            state.frag_len = 0;
        } else if (c == 0x07) {
            if (state.frag_len == 0 && !state.discard && replies) replies->append("E-\n");
        } else if (c == '\n') {
            // line feeds are not part of the protocol; adapters that send "\r\n" are tolerated
        } else if (!state.discard) {
            if (state.frag_len < CanRxStreamState::kMaxRecord) {
                state.frag[state.frag_len++] = c;
            } else {
                state.discard = true;       // overlong garbage line: skip it up to the next terminator
                state.frag_len = 0;
            }
        }
    }
}

std::vector<uint8_t> BuildTxBuffer(const CanMessage& msg, SlcanDialect dialect) {
    std::vector<uint8_t> out;
    const bool fd = msg.is_fd;
    if (fd && dialect != SLCAN_DIALECT_CANABLE) return out;

    char type;
    if (fd)               type = msg.is_brs ? 'b' : 'd';
    else if (msg.is_rtr)  type = 'r';
    else                  type = 't';
    if (msg.is_extended) type = static_cast<char>(type - 32);

    out.reserve(1 + 8 + 1 + 2 * CanPayload::kCapacity + 1);
    out.push_back(static_cast<uint8_t>(type));
    AppendHex(out, msg.id & (msg.is_extended ? CAN_ID_EFF_MASK : CAN_ID_SFF_MASK), msg.is_extended ? 8 : 3);
    const unsigned dlc = fd ? (msg.dlc & 0x0F) : std::min<unsigned>(msg.dlc, 8);
    out.push_back(static_cast<uint8_t>(Nibble(dlc)));
    if (!(msg.is_rtr && !fd)) {
        const size_t nbytes = CanProtocolUtils::DlcToBytes(static_cast<uint8_t>(dlc));
        for (size_t i = 0; i < nbytes; ++i) AppendHex(out, i < msg.payload.size() ? msg.payload[i] : 0, 2);
    }
    out.push_back('\r');
    return out;
}

bool ParseVersionLine(const std::string& line, SlcanVersionInfo& out) {
    SlcanVersionInfo info;
    if (!line.empty() && line[0] == '+') {
        // "+Board: X\tMCU: Y\tDevID: n\tFirmware: n\tSlcan: n\tClock: 160\tChannels: 1\tQuartz: Yes\tLimits: a,b,..\tHAL: ..\tSerial: s"
        info.dialect = SLCAN_DIALECT_CANABLE;
        size_t pos = 1;
        while (pos < line.size()) {
            size_t end = line.find('\t', pos);
            if (end == std::string::npos) end = line.size();
            const std::string field = line.substr(pos, end - pos);
            pos = end + 1;
            const size_t colon = field.find(':');
            if (colon == std::string::npos) continue;
            const std::string key = field.substr(0, colon);
            size_t vstart = colon + 1;
            while (vstart < field.size() && field[vstart] == ' ') ++vstart;
            const std::string value = field.substr(vstart);
            uint32_t n = 0;
            if (key == "Board") info.board = value;
            else if (key == "MCU") info.mcu = value;
            else if (key == "Firmware") info.firmware = value;
            else if (key == "Slcan") info.slcan_version = value;
            else if (key == "Serial") info.serial = value;
            else if (key == "Clock") { if (ParseUnsigned(value, n)) info.clock_mhz = n; }
            else if (key == "Channels") { if (ParseUnsigned(value, n)) info.channels = n; }
            else if (key == "Limits") {
                uint32_t v[8];
                size_t p = 0;
                int count = 0;
                while (count < 8 && p <= value.size()) {
                    size_t comma = value.find(',', p);
                    if (comma == std::string::npos) comma = value.size();
                    if (!ParseUnsigned(value.substr(p, comma - p), v[count])) break;
                    ++count;
                    p = comma + 1;
                }
                if (count == 8) {
                    // brp_max, seg1_max, seg2_max, sjw_max for the nominal, then for the data phase
                    info.nominal = CanBitTimingLimits{v[1], v[2], v[3], v[0]};
                    info.data = CanBitTimingLimits{v[5], v[6], v[7], v[4]};
                }
            }
        }
        info.valid = info.clock_mhz > 0 && info.nominal.brp_max > 0;
        if (!info.valid) return false;
    } else if (line.size() >= 2 && line[0] == 'V') {
        // Lawicel "V1013": hardware 10, software 13
        info.dialect = SLCAN_DIALECT_LAWICEL;
        info.version_text = line.substr(1);
        info.valid = true;
    } else {
        return false;
    }
    out = info;
    return true;
}

char ClassicRateCode(uint32_t bitrate_bps) {
    switch (bitrate_bps) {
        case 10000:   return '0';
        case 20000:   return '1';
        case 50000:   return '2';
        case 100000:  return '3';
        case 125000:  return '4';
        case 250000:  return '5';
        case 500000:  return '6';
        case 800000:  return '7';
        case 1000000: return '8';
        default:      return 0;
    }
}

std::vector<std::string> BuildStartCommands(SlcanDialect dialect, bool fd, bool listen_only, bool loopback,
                                            const CanBittiming& nominal, const CanBittiming& data, uint32_t nominal_bps) {
    std::vector<std::string> cmds;
    char buf[96];
    cmds.push_back("C\r");                  // reset a previous session (also clears the adapter's flags)

    if (dialect == SLCAN_DIALECT_CANABLE) {
        cmds.push_back("MF\r");             // every command is answered with "#\r" or "#<error code>\r"
        if (fd) cmds.push_back("MS\r");     // report the ESI flag of received FD frames
        std::snprintf(buf, sizeof(buf), "s%u,%u,%u,%u\r", nominal.brp, nominal.prop_seg + nominal.phase_seg1,
                      nominal.phase_seg2, nominal.sjw);
        cmds.push_back(buf);
        if (fd) {
            std::snprintf(buf, sizeof(buf), "y%u,%u,%u,%u\r", data.brp, data.prop_seg + data.phase_seg1,
                          data.phase_seg2, data.sjw);
            cmds.push_back(buf);
        }
        cmds.push_back(loopback ? "OI\r" : (listen_only ? "OS\r" : "O\r"));
    } else {
        const char code = ClassicRateCode(nominal_bps);
        if (!code) return std::vector<std::string>();
        std::snprintf(buf, sizeof(buf), "S%c\r", code);
        cmds.push_back(buf);
        cmds.push_back(listen_only ? "L\r" : "O\r");
    }
    return cmds;
}

std::vector<std::string> BuildStopCommands() {
    return std::vector<std::string>{"C\r"};
}

bool CommandHasNoReply(const std::string& line) {
    return line == "C\r";
}

} // namespace CanProtocolSlcan
