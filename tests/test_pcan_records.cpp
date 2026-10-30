// PEAK PCAN-USB FD: the error, status, bus load, calibration and overrun records, the option bytes of the start and stop sequences, and the
// core that turns the calibration records into drift / spread statistics (without using them to place frames).
#include "can_wire.h"
#include <cstdio>
#include <random>

extern "C" {
uint8_t* core_stage_ptr(); void core_process(int); int core_publish(); uint8_t* core_out_ptr(); void core_set_protocol(int, int); void core_reset_clock(int);
double* core_clock_stats(int); int core_take_events(int); uint8_t* core_reply_ptr();
}
static int g_fail = 0, g_n = 0;
#define CHECK(c) do { ++g_n; if (!(c)) { ++g_fail; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static void put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back((x >> (8 * i)) & 255); }
static std::vector<uint8_t> rec(uint16_t type, uint32_t ts, std::vector<uint8_t> body, size_t total = 16) {
    std::vector<uint8_t> r = {(uint8_t)total, (uint8_t)(total >> 8), (uint8_t)type, (uint8_t)(type >> 8)}; put32(r, ts); put32(r, 0);
    r.insert(r.end(), body.begin(), body.end()); r.resize(total, 0); return r; }
// records back to back, padded to 4 bytes, ended by an empty record
static std::vector<uint8_t> concat(std::initializer_list<std::vector<uint8_t>> l) {
    std::vector<uint8_t> o; for (auto& x : l) o.insert(o.end(), x.begin(), x.end()); o.resize((o.size() + 3) & ~3u); o.insert(o.end(), 4, 0); return o; }

static void TestParser() {
    std::vector<CanMessage> out; CanRxStreamState st; std::string ev; std::vector<uint32_t> cal; PcanExtras ex{&ev, &cal};
    auto err_bit_tx = rec(PCAN_MSG_ERROR, 1000, {0x80 | (PCAN_ERR_BIT << 4) | 0, 0x10, 130, 97});
    auto err_stuff = rec(PCAN_MSG_ERROR, 1100, {(PCAN_ERR_STUFF << 4) | 0, 0, 5, 6});
    auto err_dec = rec(PCAN_MSG_ERROR, 1200, {(PCAN_ERR_COUNTER_DEC << 4) | 0, 0, 3, 2});
    auto err_other_ch = rec(PCAN_MSG_ERROR, 1300, {1, 0, 9, 9});
    auto st_pass = rec(PCAN_MSG_STATUS, 2000, {PCAN_ST_PASSIVE | 0}); auto st_off = rec(PCAN_MSG_STATUS, 2100, {PCAN_ST_BUSOFF});
    auto st_ok = rec(PCAN_MSG_STATUS, 2200, {PCAN_ST_RB}); auto st_warn = rec(PCAN_MSG_STATUS, 2300, {PCAN_ST_WARNING});
    auto bl = rec(PCAN_MSG_BUSLOAD, 3000, {0, 0, 0x00, 0x08});     // 0x0800 = 2048 -> 50 %
    auto cl = rec(PCAN_MSG_USB_CALIBRATION, 4000, {5, 0});
    auto ov = rec(PCAN_MSG_USB_OVERRUN, 5000, {0});
    auto b = concat({err_bit_tx, err_stuff, err_dec, err_other_ch, st_pass, st_off, st_ok, st_warn, bl, cl, ov});
    CanProtocolPcan::ParseRxStream(b.data(), (int)b.size(), out, st, 0, &ex);
    CHECK(out.size() == 6);   // 2 errors + 4 status
    CHECK(out[0].is_error && out[0].id == 0x80 && out[0].payload[2] == 0x81 && out[0].payload[6] == 130 && out[0].payload[7] == 97 && out[0].hw_timestamp_us == 1000);
    CHECK(out[1].payload[2] == 0x04);
    CHECK(out[2].is_error && out[2].payload[1] == 0x30 && out[3].id == 0x40 && out[4].payload[1] == 0x40 && out[5].payload[1] == 0x0C);
    CHECK(ev.find("U50\n") != std::string::npos);
    CHECK(ev.find("E00000000:000000000000" "0302\n") != std::string::npos);                 // counters decreased: counters only
    CHECK(ev.find("S0\n") != std::string::npos && ev.find("S1\n") != std::string::npos && ev.find("S2\n") != std::string::npos && ev.find("S3\n") != std::string::npos);
    CHECK(ev.find("receive overrun") != std::string::npos);
    CHECK(cal.size() == 1 && cal[0] == 4000);
    // decoded like the Elmue reports
    uint32_t id; uint8_t d[8]; size_t p = ev.find("E00000080:"); CHECK(CanProtocolElmue::ParseErrorEvent(ev.substr(p + 1, 25), id, d));
    auto info = CanProtocolElmue::DecodeError(id, d);
    CHECK(info.tx_errors == 130 && info.rx_errors == 97 && info.text.find("bit error") != std::string::npos && info.text.find("error while transmitting") != std::string::npos);
    // without extras the stream behaves as before: no frames from info records
    std::vector<CanMessage> o2; CanRxStreamState s2; CanProtocolPcan::ParseRxStream(b.data(), (int)b.size(), o2, s2, 0); CHECK(o2.empty());
    // a record split between two transfers still gets through
    std::vector<CanMessage> o3; CanRxStreamState s3; std::string e3; PcanExtras x3{&e3, nullptr};
    CanProtocolPcan::ParseRxStream(bl.data(), 7, o3, s3, 0, &x3); CHECK(e3.empty());
    CanProtocolPcan::ParseRxStream(bl.data() + 7, (int)bl.size() - 7, o3, s3, 0, &x3); CHECK(e3 == "U50\n");
    // fuzz (the sanitizers watch)
    std::mt19937 rng(3);
    for (int i = 0; i < 100000; ++i) {
        std::vector<uint8_t> r(rng() % 200); for (auto& x : r) x = rng();
        if (r.size() > 4 && (i & 1)) { r[0] = 16 + (rng() % 4) * 4; r[1] = 0; r[2] = 1 + rng() % 4; r[3] = (i & 2) ? 1 : 0; }
        std::vector<CanMessage> o; CanRxStreamState s; std::string e; std::vector<uint32_t> c; PcanExtras x{&e, &c};
        CanProtocolPcan::ParseRxStream(r.data(), (int)r.size(), o, s, 0, &x);
    }
}

static void TestCommands() {
    CanBittiming bt = {0, 15, 4, 4, 5}; auto cmds = CanProtocolPcan::BuildStartCommands(0, true, false, bt, bt);
    bool found_set = false, found_clr = false;
    for (auto& t : cmds) for (size_t o = 0; o + 8 <= t.size(); o += 8) {
        uint16_t op, mask, usb; std::memcpy(&op, &t[o], 2); std::memcpy(&mask, &t[o + 2], 2); std::memcpy(&usb, &t[o + 6], 2); op &= 0x3FF;
        if (op == PCAN_CMD_SET_EN_OPTION) { found_set = true; CHECK((mask & PCAN_OPTION_BUSLOAD) && (mask & PCAN_OPTION_ERROR) && usb == PCAN_USB_OPTION_CALIBRATION); }
        if (op == PCAN_CMD_CLR_DIS_OPTION) { found_clr = true; CHECK(!(mask & PCAN_OPTION_BUSLOAD) && usb == PCAN_USB_OPTION_FAST_FWD); }
    }
    CHECK(found_set && found_clr);
    auto stop = CanProtocolPcan::BuildStopCommands(0); uint16_t mask = 0, usb = 0;
    for (auto& t : stop) for (size_t o = 0; o + 8 <= t.size(); o += 8) {
        uint16_t op; std::memcpy(&op, &t[o], 2);
        if ((op & 0x3FF) == PCAN_CMD_CLR_DIS_OPTION) { std::memcpy(&mask, &t[o + 2], 2); std::memcpy(&usb, &t[o + 6], 2); }
    }
    CHECK((mask & PCAN_OPTION_BUSLOAD) && usb == PCAN_USB_OPTION_CALIBRATION);
}

// The core: calibration records feed the clock statistics, events leave as lines, frames are still placed by the frame minimum
static void TestCore() {
    core_set_protocol(2, PROTOCOL_PEAK_PCAN_FD); core_reset_clock(2);
    std::mt19937 rng(5); std::uniform_real_distribution<double> U(0, 1); std::exponential_distribution<double> E(1.0 / 0.4);
    const double drift = -60e-6;                                 // the adapter clock runs 60 ppm slow
    const double ts0 = 4294967296.0 - 20.0e6;                    // wraps during the run
    uint8_t* stg = core_stage_ptr();
    for (int i = 1; i <= 70; ++i) {
        double t_true = 1000.0 + 1000.0 * i;
        uint32_t ts = (uint32_t)(uint64_t)(ts0 + (t_true - 1000.0) * 1000.0 * (1 + drift));
        double arrive = t_true + 0.3 + E(rng) + (U(rng) < 0.1 ? 8 * U(rng) : 0);
        auto r = concat({rec(PCAN_MSG_USB_CALIBRATION, ts, {1, 0}), rec(PCAN_MSG_BUSLOAD, ts, {0, 0, 0x00, 0x04})});
        uint16_t len = (uint16_t)r.size(); std::memcpy(stg, &len, 2); stg[2] = 2; stg[3] = 0x82; std::memcpy(stg + 4, &arrive, 8); std::memcpy(stg + 12, r.data(), len);
        core_process(12 + len);
    }
    double* s = core_clock_stats(2);
    std::printf("pcan calibration: records %.0f used %.0f drift %.1f ppm spread %.0f us worst %.0f us excess last %.2f ms push %.0f\n", s[0], s[1], s[3], s[4], s[7], s[6], s[8]);
    CHECK(s[0] == 70 && s[8] == 1 && s[1] >= 10);
    CHECK(s[3] > -85 && s[3] < -35);                              // -60 ppm
    CHECK(s[4] < 700);
    int n = core_take_events(2); std::string ev((const char*)core_reply_ptr(), n); CHECK(ev.find("U25\n") != std::string::npos);
    PcanRxMsg rx = {}; rx.size = 28; rx.type = PCAN_MSG_CAN_RX; rx.ts_low = (uint32_t)(uint64_t)(ts0 + 71000.0 * 1000.0 * (1 + drift)); rx.channel_dlc = 0x10; rx.can_id = 0x123;
    auto r = concat({std::vector<uint8_t>((uint8_t*)&rx, (uint8_t*)&rx + 28)});
    uint16_t len = (uint16_t)r.size(); double arrive = 72000.5; std::memcpy(stg, &len, 2); stg[2] = 2; stg[3] = 0x82; std::memcpy(stg + 4, &arrive, 8); std::memcpy(stg + 12, r.data(), len);
    core_process(12 + len); core_publish(); SnapshotHeader h; std::memcpy(&h, core_out_ptr(), sizeof(h)); CHECK(h.n_trace >= 1);
}

int main() {
    TestParser();
    TestCommands();
    TestCore();
    std::printf("%d checks, %d failed\n", g_n, g_fail);
    return g_fail != 0;
}
