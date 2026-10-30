// Tests of the ElmueSoft protocol layer (parser, builder, blobs, echoes, error decoding, a fuzzer) and of the core that uses it
// (transmit records staged by the worker, tx echoes, error frames, event lines).
#include "can_wire.h"
#include <cstdio>
#include <random>

extern "C" {
uint8_t* core_stage_ptr(); void core_process(int); int core_publish(); uint8_t* core_out_ptr();
void core_set_elmue(int, int); int core_take_events(int); uint8_t* core_reply_ptr();
}

static int g_fail = 0, g_n = 0;
#define CHECK(c) do { ++g_n; if (!(c)) { ++g_fail; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static void le32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back((x >> (8 * i)) & 0xFF); }
static std::vector<uint8_t> rxframe(uint32_t id, uint8_t flags, std::vector<uint8_t> data, bool ts = false, uint32_t t = 0) {
    std::vector<uint8_t> m = {0, ELM_MSG_RX_FRAME, flags}; le32(m, id); if (ts) le32(m, t);
    m.insert(m.end(), data.begin(), data.end()); m[0] = (uint8_t)m.size(); return m;
}
static std::vector<uint8_t> blob(int count, std::vector<std::vector<uint8_t>> msgs) {
    std::vector<uint8_t> b = {(uint8_t)count, ELM_MSG_RX_BLOB};
    for (auto& m : msgs) b.insert(b.end(), m.begin(), m.end());
    return b;
}

static void TestRxFrames() {
    { std::vector<CanMessage> o;
      auto m = rxframe(0x123, 0, {1,2,3,4,5,6,7,8});
      CanProtocolElmue::ParseRx(m.data(), (int)m.size(), false, o);
      CHECK(o.size() == 1); CHECK(o[0].id == 0x123 && !o[0].is_extended && !o[0].is_fd && o[0].dlc == 8 && o[0].payload.size() == 8 && o[0].payload[7] == 8);
      CHECK(!o[0].has_hw_timestamp && !o[0].is_tx_echo); }
    { std::vector<CanMessage> o;
      auto m = rxframe(0x80000000u | 0x1ABCDEF0u, ELM_FRM_FDF | ELM_FRM_BRS | ELM_FRM_ESI, std::vector<uint8_t>(12, 0x5A), true, 0xDEADBEEF);
      CanProtocolElmue::ParseRx(m.data(), (int)m.size(), true, o);
      CHECK(o.size() == 1 && o[0].is_extended && o[0].id == 0x1ABCDEF0 && o[0].is_fd && o[0].is_brs && o[0].is_esi);
      CHECK(o[0].dlc == 9 && o[0].payload.size() == 12); CHECK(o[0].has_hw_timestamp && o[0].hw_timestamp_us == 0xDEADBEEF); }
    { std::vector<CanMessage> o;  // FD, 64 bytes
      auto m = rxframe(0x7FF, ELM_FRM_FDF, std::vector<uint8_t>(64, 0x11));
      CanProtocolElmue::ParseRx(m.data(), (int)m.size(), false, o);
      CHECK(o.size() == 1 && o[0].dlc == 15 && o[0].payload.size() == 64 && !o[0].is_brs); }
    { std::vector<CanMessage> o;  // remote frames, with the DLC in the first data byte and without
      auto a = rxframe(0x40000000u | 0x55, 0, {3}); auto b = rxframe(0x40000000u | 0x56, 0, {});
      CanProtocolElmue::ParseRx(a.data(), (int)a.size(), false, o); CanProtocolElmue::ParseRx(b.data(), (int)b.size(), false, o);
      CHECK(o.size() == 2 && o[0].is_rtr && o[0].dlc == 3 && o[0].payload.empty() && o[1].is_rtr && o[1].dlc == 0); }
    { std::vector<CanMessage> o;  // a classic frame with more than 8 bytes is clamped
      auto m = rxframe(0x1, 0, std::vector<uint8_t>(20, 7));
      CanProtocolElmue::ParseRx(m.data(), (int)m.size(), false, o);
      CHECK(o.size() == 1 && o[0].dlc == 8 && o[0].payload.size() == 8); }
}

static void TestBlobsAndMessages() {
    { std::vector<CanMessage> o; std::string ev;
      auto b = blob(3, {rxframe(0x10,0,{1}), rxframe(0x11,0,{2,3}), rxframe(0x12,0,{})});
      CanProtocolElmue::ParseRx(b.data(), (int)b.size(), false, o, &ev);
      CHECK(o.size() == 3 && o[0].id == 0x10 && o[1].payload.size() == 2 && o[2].id == 0x12);
      // frame_count smaller than the data: the rest is ignored; larger: stops at the end
      std::vector<CanMessage> p; auto b2 = blob(1, {rxframe(0x10,0,{1}), rxframe(0x11,0,{2})});
      CanProtocolElmue::ParseRx(b2.data(), (int)b2.size(), false, p); CHECK(p.size() == 1);
      std::vector<CanMessage> q; auto b3 = blob(9, {rxframe(0x10,0,{1}), rxframe(0x11,0,{2})});
      CanProtocolElmue::ParseRx(b3.data(), (int)b3.size(), false, q); CHECK(q.size() == 2);
      std::vector<CanMessage> r; auto c1 = rxframe(0x20,0,{1}), c2 = rxframe(0x21,0,{2}); c1.insert(c1.end(), c2.begin(), c2.end());
      CanProtocolElmue::ParseRx(c1.data(), (int)c1.size(), false, r); CHECK(r.size() == 2); }
    { std::vector<CanMessage> o; std::string ev;
      std::vector<uint8_t> bl = {3, ELM_MSG_BUSLOAD, 42};
      std::vector<uint8_t> st = {0, ELM_MSG_STRING}; for (char c : std::string("Hello\nWorld\x01 ")) st.push_back((uint8_t)c); st[0] = (uint8_t)st.size();
      std::vector<uint8_t> er = {0, ELM_MSG_ERROR}; le32(er, ELM_ERID_BUS_OFF | ELM_ERID_NO_ACK);
      const uint8_t d[8] = {0, 0x20, 0x01, 0, 0, 0x08, 130, 97}; er.insert(er.end(), d, d + 8); er[0] = (uint8_t)er.size();
      auto b = blob(3, {bl, st, er});
      CanProtocolElmue::ParseRx(b.data(), (int)b.size(), false, o, &ev);
      CHECK(ev.find("U42\n") == 0); CHECK(ev.find("THello World\n") != std::string::npos);
      CHECK(o.size() == 1 && o[0].is_error && o[0].payload.size() == 8 && o[0].id == 0x60);
      size_t p = ev.find("E00000060:"); uint32_t id = 0; uint8_t dd[8];
      CHECK(p != std::string::npos && CanProtocolElmue::ParseErrorEvent(ev.substr(p + 1, ev.find('\n', p) - p - 1), id, dd));
      CHECK(id == 0x60 && dd[1] == 0x20 && dd[6] == 130 && dd[7] == 97);
      ElmErrorInfo info = CanProtocolElmue::DecodeError(id, dd);
      CHECK(info.state_known && info.state == ELM_BUS_OFF && info.tx_errors == 130 && info.rx_errors == 97);
      CHECK(info.text.find("bus off") != std::string::npos && info.text.find("no ACK") != std::string::npos && info.text.find("TX passive") != std::string::npos);
      CHECK(info.text.find("adapter TX buffer full") == std::string::npos && info.text.find("bit error") != std::string::npos);
      // state transitions
      uint8_t w[8] = {0, 0x04}; CHECK(CanProtocolElmue::DecodeError(0x80, w).state == ELM_BUS_WARNING);
      uint8_t pa[8] = {0, 0x10}; CHECK(CanProtocolElmue::DecodeError(0x80, pa).state == ELM_BUS_PASSIVE);
      uint8_t ba[8] = {0, 0x40}; auto bi = CanProtocolElmue::DecodeError(0x04, ba); CHECK(bi.state_known && bi.state == ELM_BUS_ACTIVE);
      uint8_t none[8] = {}; CHECK(!CanProtocolElmue::DecodeError(0x80, none).state_known);
      CHECK(!CanProtocolElmue::ParseErrorEvent("0000006:00", id, dd)); CHECK(!CanProtocolElmue::ParseErrorEvent("0000006G:0020010000000000", id, dd)); }
}

static void TestEchoAndBuilders() {
    { std::vector<CanMessage> o; std::vector<CanMessage> tbl(256);
      CanMessage s; s.id = 0x321; s.dlc = 2; s.payload.assign((const uint8_t*)"\x01\x02", (const uint8_t*)"\x01\x02" + 2);
      auto tx = CanProtocolElmue::BuildTxBuffer(s); uint8_t mk = 0; CanMessage back;
      CHECK(CanProtocolElmue::ParseTxFrame(tx.data(), (int)tx.size(), back, mk) && mk == tx[7] && mk != 0);
      CHECK(back.id == 0x321 && back.dlc == 2 && back.payload.size() == 2 && back.is_tx_echo);
      tbl[mk] = back;
      std::vector<uint8_t> e1 = {3, ELM_MSG_TX_ECHO, mk}; std::vector<uint8_t> e2 = {3, ELM_MSG_TX_ECHO, (uint8_t)(mk + 1)};
      auto b = blob(2, {e1, e2});
      CanProtocolElmue::ParseRx(b.data(), (int)b.size(), false, o, nullptr, tbl.data());
      CHECK(o.size() == 1 && o[0].is_tx_echo && o[0].id == 0x321 && o[0].tx_marker == mk); CHECK(!tbl[mk].is_tx_echo);
      std::vector<CanMessage> o2; std::vector<uint8_t> e3 = {7, ELM_MSG_TX_ECHO, mk, 0x78,0x56,0x34,0x12}; tbl[mk] = back;
      CanProtocolElmue::ParseRx(e3.data(), (int)e3.size(), true, o2, nullptr, tbl.data());
      CHECK(o2.size() == 1 && o2[0].has_hw_timestamp && o2[0].hw_timestamp_us == 0x12345678);
      std::vector<CanMessage> o3; CanProtocolElmue::ParseRx(e1.data(), (int)e1.size(), false, o3); CHECK(o3.empty()); }
    { CanMessage s; s.id = 0x1FFFFFFF; s.is_extended = true; s.is_fd = true; s.is_brs = true; s.dlc = 9;
      s.payload.resize(12, 0xAB); auto t = CanProtocolElmue::BuildTxBuffer(s);
      CHECK(t.size() == 8 + 12 && t[0] == 20 && t[1] == ELM_MSG_TX_FRAME && t[2] == (ELM_FRM_FDF | ELM_FRM_BRS));
      uint32_t id = t[3] | (t[4] << 8) | (t[5] << 16) | ((uint32_t)t[6] << 24); CHECK(id == (0x1FFFFFFFu | ELM_ID_29BIT));
      s = CanMessage(); s.id = 0x100; s.is_rtr = true; s.dlc = 4; t = CanProtocolElmue::BuildTxBuffer(s);
      CHECK(t.size() == 9 && t[8] == 4); id = t[3] | (t[4] << 8) | (t[5] << 16) | ((uint32_t)t[6] << 24); CHECK(id == (0x100u | ELM_ID_RTR));
      s.dlc = 0; t = CanProtocolElmue::BuildTxBuffer(s); CHECK(t.size() == 8);
      s = CanMessage(); s.id = 5; s.dlc = 3; s.payload.resize(1, 9); t = CanProtocolElmue::BuildTxBuffer(s); CHECK(t.size() == 11 && t[8] == 9 && t[9] == 0);   // short payload padded
      s = CanMessage(); s.id = 5; s.is_fd = true; s.dlc = 15; s.payload.resize(64, 1); t = CanProtocolElmue::BuildTxBuffer(s); CHECK(t.size() == 72 && t[0] == 72);
      // markers: 255 values, never 0
      std::vector<bool> seen(256, false); int zero = 0; for (int i = 0; i < 600; ++i) { uint8_t m; CanProtocolElmue::BuildTxBuffer(s, &m); if (!m) ++zero; seen[m] = true; }
      CHECK(zero == 0); int cnt = 0; for (int i = 1; i < 256; ++i) cnt += seen[i]; CHECK(cnt == 255); }
    { ElmBoardInfo bi; uint8_t raw[56] = {}; raw[0] = 0x68; raw[1] = 0x04; std::memcpy(raw + 2, "STM32G431xx", 12); std::memcpy(raw + 27, "Multiboard", 11); raw[52] = 3;
      CHECK(CanProtocolElmue::ParseBoardInfo(raw, 56, bi) && bi.mcu_device_id == 0x468 && std::string(bi.board_name) == "Multiboard" && std::string(bi.mcu_name) == "STM32G431xx" && bi.board_flags == 3);
      CHECK(CanProtocolElmue::ParseBoardInfo(raw, 52, bi) && bi.board_flags == 0); CHECK(!CanProtocolElmue::ParseBoardInfo(raw, 51, bi));
      CHECK(CanProtocolElmue::BoardFlagsText(3) == "quartz, USB high speed");
      auto f = CanProtocolElmue::BuildFilter(ELM_FIL_HOST_PASS_29, 0x1234, 0x1FFF0000);
      CHECK(f.size() == 17 && f[0] == 2 && f[1] == 0x34 && f[2] == 0x12 && f[5] == 0 && f[6] == 0 && f[7] == 0xFF && f[8] == 0x1F);
      CHECK(CanProtocolElmue::ModeFlags(GS_CAN_FEATURE_HW_TIMESTAMP | ELM_FEATURE_USB_BLOBS, true) == (ELM_FEATURE_PROTOCOL | ELM_FEATURE_USB_BLOBS | GS_CAN_FEATURE_HW_TIMESTAMP));
      CHECK(CanProtocolElmue::ModeFlags(GS_CAN_FEATURE_HW_TIMESTAMP, false) == ELM_FEATURE_PROTOCOL);
      CHECK(std::string(CanProtocolElmue::FeedbackText(':')) == "baud rate not set"); }
}

// Random and half-valid input must never crash, overrun or produce more than 64 data bytes (run under the sanitizers)
static void TestFuzz() {
    std::mt19937 rng(1); std::vector<CanMessage> tbl(256);
    for (int it = 0; it < 200000; ++it) {
        std::vector<uint8_t> b(rng() % 140); for (auto& x : b) x = rng();
        if (it & 1 && b.size() > 2) b[1] = 10 + rng() % 9;
        if (it & 2 && b.size() > 2) b[0] = rng() % (b.size() + 3);
        std::vector<CanMessage> o; std::string ev; CanProtocolElmue::ParseRx(b.data(), (int)b.size(), it & 4, o, &ev, tbl.data());
        CanMessage m; uint8_t mk; CanProtocolElmue::ParseTxFrame(b.data(), (int)b.size(), m, mk);
        for (auto& x : o) CHECK(x.payload.size() <= 64);
    }
}

// The core: a transmit record staged by the worker is remembered until its echo arrives; error reports become frames; events leave as lines
static void TestCore() {
    core_set_elmue(0, 1);
    CanMessage s; s.id = 0x222; s.dlc = 1; s.payload.resize(1, 0xEE); uint8_t mk = 0;
    auto tx = CanProtocolElmue::BuildTxBuffer(s, &mk);
    uint8_t* st = core_stage_ptr(); int off = 0;
    auto put = [&](const std::vector<uint8_t>& bytes, uint8_t ep, double t) {
        uint16_t len = (uint16_t)bytes.size(); st[off] = len & 0xFF; st[off + 1] = len >> 8; st[off + 2] = 0; st[off + 3] = ep; std::memcpy(st + off + 4, &t, 8);
        std::memcpy(st + off + 12, bytes.data(), len); off += 12 + len; };
    put(tx, 0x02, 100.0);
    std::vector<uint8_t> echo = {7, ELM_MSG_TX_ECHO, mk}; le32(echo, 5000);
    std::vector<uint8_t> bl = {3, ELM_MSG_BUSLOAD, 17};
    std::vector<uint8_t> er = {0, ELM_MSG_ERROR}; le32(er, ELM_ERID_BUS_ERROR); const uint8_t d[8] = {0, 0x08, 0x04, 0, 0, 0, 12, 0}; er.insert(er.end(), d, d + 8); le32(er, 5100); er[0] = (uint8_t)er.size();
    auto f = rxframe(0x222, 0, {1, 2}, true, 5200);
    auto b = blob(4, {echo, bl, er, f});
    put(b, 0x82, 101.0);
    core_process(off);
    int n = core_take_events(0); std::string ev((const char*)core_reply_ptr(), n);
    CHECK(ev.find("U17\n") != std::string::npos && ev.find("E00000080:") != std::string::npos);
    int pub = core_publish(); CHECK(pub > 0);
    SnapshotHeader h; std::memcpy(&h, core_out_ptr(), sizeof(h));
    CHECK(h.n_trace == 3);   // echo, error, rx
    const uint8_t* tp = core_out_ptr() + sizeof(h) + h.n_table * sizeof(WireTableRecord);
    TraceEntry t0, t1, t2; std::memcpy(&t0, tp, sizeof(t0)); std::memcpy(&t1, tp + sizeof(t0), sizeof(t0)); std::memcpy(&t2, tp + 2 * sizeof(t0), sizeof(t0));
    CHECK(t0.msg.is_tx_echo && t0.msg.id == 0x222 && t0.msg.payload.size() == 1 && t0.msg.payload[0] == 0xEE && t0.msg.dev == 0);
    CHECK(t1.msg.is_error && t1.msg.id == 0x80 && t1.msg.dev == 0);
    CHECK(!t2.msg.is_error && !t2.msg.is_tx_echo && t2.msg.id == 0x222 && t2.msg.payload.size() == 2);
    CHECK(h.n_table == 2);   // row 0x222 (echo and rx coalesce) and the error row
}

int main() {
    TestRxFrames();
    TestBlobsAndMessages();
    TestEchoAndBuilders();
    TestFuzz();
    TestCore();
    std::printf("%d checks, %d failed\n", g_n, g_fail);
    return g_fail != 0;
}
