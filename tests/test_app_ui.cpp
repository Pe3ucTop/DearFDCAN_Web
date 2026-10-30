// Headless test of the adapter sequences driven by AppUI (open, capabilities, start, acknowledgements, stop, transmit,
// reports) for gs_usb (legacy and Elmue), PEAK and SLCAN adapters. It uses the real Dear ImGui and a recording
// replacement of WebUsbBridge, so what is checked is exactly what the page would send to the worker.
// Build and run: make test (CTest target test_app_ui, see tests/CMakeLists.txt)
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "webusb_bridge.h"
#include "imgui.h"
#define private public
#include "app_ui.h"
#include "clipboard.h"
#undef private

struct Call {
    std::string kind;
    int dev = 0, request = 0, id = -1, value = 0, length = 0, a = 0, b = 0, c = 0;
    std::vector<uint8_t> data;
};
static std::vector<Call> g_calls;
static UsbDeviceStats g_stats;

WebUsbBridge& WebUsbBridge::GetInstance() { static WebUsbBridge b; return b; }
void WebUsbBridge::Initialize() {}
void WebUsbBridge::RequestDevice() {}
void WebUsbBridge::RequestSerialPort(int) {}
void WebUsbBridge::SetSerialBaud(int, int) {}
void WebUsbBridge::ConnectDevice(int) {}
void WebUsbBridge::DisconnectDevice(int) {}
void WebUsbBridge::RemoveDevice(int) {}
void WebUsbBridge::StartRxPump(int dev, int ep, int size, int flags) { Call c; c.kind = "rx_start"; c.dev = dev; c.a = ep; c.b = size; c.c = flags; g_calls.push_back(c); }
void WebUsbBridge::StopRxPump(int dev) { Call c; c.kind = "rx_stop"; c.dev = dev; g_calls.push_back(c); }
void WebUsbBridge::TransferOut(int dev, int ep, const uint8_t* d, int n) { Call c; c.kind = "tx"; c.dev = dev; c.a = ep; c.data.assign(d, d + n); g_calls.push_back(c); }
void WebUsbBridge::ControlTransferOut(int dev, uint8_t request, uint16_t value, uint16_t, const uint8_t* d, int n, int id) {
    Call c; c.kind = "out"; c.dev = dev; c.request = request; c.value = value; c.id = id; c.data.assign(d, d + n); g_calls.push_back(c);
}
void WebUsbBridge::ControlTransferIn(int dev, uint8_t request, uint16_t value, uint16_t, uint16_t length) {
    Call c; c.kind = "in"; c.dev = dev; c.request = request; c.value = value; c.length = length; g_calls.push_back(c);
}
void WebUsbBridge::CommandOut(int dev, int id, const uint8_t* d, int n) { Call c; c.kind = "cmd"; c.dev = dev; c.id = id; c.data.assign(d, d + n); g_calls.push_back(c); }
void WebUsbBridge::ClearIdTable() {}
void WebUsbBridge::ClearTrace() {}
void WebUsbBridge::SetTraceEnabled(bool) {}
void WebUsbBridge::SetTxFifoSlots(int dev, int slots) { Call c; c.kind = "txfifo"; c.dev = dev; c.value = slots; g_calls.push_back(c); }
const UsbDeviceStats& WebUsbBridge::GetDeviceStats(int) const { return g_stats; }
static double g_now_ms = 1000.0;
double emscripten_get_now() { return g_now_ms; }

static int g_fail = 0, g_n = 0;
#define CHECK(c) do { ++g_n; if (!(c)) { ++g_fail; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static std::vector<Call> take() { auto v = g_calls; g_calls.clear(); return v; }
static uint32_t u32at(const std::vector<uint8_t>& v, size_t o) { uint32_t x; std::memcpy(&x, v.data() + o, 4); return x; }
static std::string text(const Call& c) { return std::string(c.data.begin(), c.data.end()); }

static AppUI* g_app = nullptr;
static void frame() {
    ImGui::GetIO().DisplaySize = ImVec2(1400, 900);
    ImGui::GetIO().DeltaTime = 1.0f / 60;
    ImGui::NewFrame();
    g_app->Render();
    ImGui::Render();
}

static void TestGsUsb(AppUI& app) {
    UsbDeviceInfo info; info.vid = 0x1D50; info.pid = 0x606F; info.product = "CANable 2.5"; info.serial = "S1";
    app.OnDeviceAdded(0, info);
    DeviceOpenInfo oi; oi.protocol = PROTOCOL_GS_USB_CANDLELIGHT; oi.endpoints = "IN 1, OUT 2";
    app.OnDeviceOpened(0, oi);
    auto c = take();
    CHECK(c.size() == 2 && c[0].kind == "in" && c[0].request == GS_USB_BREQ_BT_CONST && c[1].request == GS_USB_BREQ_DEVICE_CONFIG);

    // BT_CONST of a CANable 2.5 (G431 at 160 MHz)
    GsDeviceBtConst bc = {};
    bc.feature = GS_CAN_FEATURE_LISTEN_ONLY | GS_CAN_FEATURE_LOOP_BACK | GS_CAN_FEATURE_HW_TIMESTAMP | GS_CAN_FEATURE_IDENTIFY | GS_CAN_FEATURE_FD |
                 GS_CAN_FEATURE_BT_CONST_EXT | GS_CAN_FEATURE_TERMINATION | ELM_FEATURE_PROTOCOL | ELM_FEATURE_USB_BLOBS;
    bc.fclk_can = 160000000; bc.tseg1_max = 256; bc.tseg2_max = 128; bc.sjw_max = 128; bc.brp_max = 512; bc.brp_inc = 1;
    app.OnControlDataReceived(0, GS_USB_BREQ_BT_CONST, reinterpret_cast<uint8_t*>(&bc), sizeof(bc));
    c = take();
    CHECK(app.devices_[0].elmue_supported);
    CHECK(c.size() == 3 && c[0].kind == "out" && c[0].request == GS_USB_BREQ_MODE && c[0].id == 0x700 && u32at(c[0].data, 0) == 0 && u32at(c[0].data, 4) == ELM_FEATURE_PROTOCOL);
    CHECK(c[1].kind == "in" && c[1].request == ELM_BREQ_GET_BOARD_INFO && c[1].length == 56);
    CHECK(c[2].kind == "in" && c[2].request == GS_USB_BREQ_BT_CONST_EXT && c[2].length == 72);
    app.OnControlOutDone(0, 0x700, true);

    GsDeviceBtConstExt ext = {}; ext.feature = bc.feature; ext.fclk_can = 160000000;
    ext.nominal = {1, 256, 1, 128, 128, 1, 512, 1}; ext.data = {1, 32, 1, 16, 16, 1, 32, 1};
    app.OnControlDataReceived(0, GS_USB_BREQ_BT_CONST_EXT, reinterpret_cast<uint8_t*>(&ext), sizeof(ext));
    uint8_t board[56] = {}; board[0] = 0x68; board[1] = 4; std::memcpy(board + 2, "STM32G431xx", 12); std::memcpy(board + 27, "Multiboard", 11); board[52] = 1;
    app.OnControlDataReceived(0, ELM_BREQ_GET_BOARD_INFO, board, 56);
    DeviceSlot& d = app.devices_[0];
    CHECK(d.has_bt_ext && d.has_board && std::string(d.board.board_name) == "Multiboard");
    CHECK(d.data_bt.brp <= 32 && d.data_bt.phase_seg2 <= 16 && d.data_bt.prop_seg + d.data_bt.phase_seg1 <= 32);      // data phase within the real limits
    GsDeviceConfig cfg = {}; app.OnControlDataReceived(0, GS_USB_BREQ_DEVICE_CONFIG, reinterpret_cast<uint8_t*>(&cfg), sizeof(cfg));
    take();

    // start in Elmue mode
    d.enable_fd = true;
    app.StartCanChannel(0);
    c = take();
    std::vector<Call> outs, ins; Call rx;
    for (auto& x : c) { if (x.kind == "out") outs.push_back(x); else if (x.kind == "in") ins.push_back(x); else if (x.kind == "rx_start") rx = x; }
    CHECK(outs.size() == 6);
    CHECK(outs[0].request == GS_USB_BREQ_HOST_FORMAT && outs[0].id == 0x600);
    CHECK(outs[1].request == GS_USB_BREQ_MODE && outs[1].id == 0x500 && u32at(outs[1].data, 0) == 0 && u32at(outs[1].data, 4) == ELM_FEATURE_PROTOCOL);
    CHECK(outs[2].request == GS_USB_BREQ_BITTIMING && outs[2].id == 0x501 && outs[2].data.size() == 20);
    CHECK(outs[3].request == GS_USB_BREQ_DATA_BITTIMING && outs[3].id == 0x502);
    CHECK(outs[4].request == ELM_BREQ_SET_BUSLOAD_REPORT && outs[4].id == 0x601 && outs[4].data[0] == 10);
    CHECK(outs[5].request == GS_USB_BREQ_MODE && outs[5].id == 0x503 && u32at(outs[5].data, 0) == 1);
    const uint32_t want = GS_CAN_FEATURE_FD | ELM_FEATURE_PROTOCOL | ELM_FEATURE_USB_BLOBS | GS_CAN_FEATURE_HW_TIMESTAMP;
    CHECK(u32at(outs[5].data, 4) == want);
    CHECK(ins.size() == 4 && ins[0].request == ELM_BREQ_GET_LAST_ERROR && ins[0].length == 1);
    CHECK(rx.kind == "rx_start" && rx.b == 2048 && rx.c == (WebUsbBridge::kRxElmue | WebUsbBridge::kRxTimestamps | WebUsbBridge::kRxTxBlobs));
    CHECK(d.channel_opening && d.elmue_active && d.hw_timestamps_active);
    for (auto& x : outs) app.OnControlOutDone(0, x.id, true);
    CHECK(d.channel_open && !d.channel_opening && app.IsBusOn(0));
    uint8_t ok = 2; for (int i = 0; i < 4; ++i) app.OnControlDataReceived(0, ELM_BREQ_GET_LAST_ERROR, &ok, 1);
    CHECK(d.channel_open && d.feedback_labels.empty());

    // transmit uses the Elmue message, a new marker for every frame
    CanMessage m; m.id = 0x123; m.dlc = 2; m.payload.resize(2, 0x55);
    CHECK(app.SendFrame(m, kTargetAllDevices) == 1);
    c = take(); CHECK(c.size() == 1 && c[0].kind == "tx" && c[0].data.size() == 10 && c[0].data[1] == ELM_MSG_TX_FRAME && c[0].data[0] == 10);
    app.SendFrame(m, 0); auto c2 = take(); CHECK(c2.size() == 1 && c2[0].data[7] != c[0].data[7]);

    // reports from the adapter
    app.OnAdapterEvents(0, "U37\nTfirmware hello\nE00000080:0008040000000500\nE00000060:0020010000000000\n");
    CHECK(d.bus_load == 37 && d.adapter_text == "firmware hello" && d.error_reports == 2 && d.bus_state == ELM_BUS_OFF);
    app.OnAdapterEvents(0, "garbage\nE1\n\nEZZZZZZZZ:00\nU\n");
    CHECK(d.error_reports == 2);
    app.OnAdapterEvents(0, "S2\n"); CHECK(d.bus_state == ELM_BUS_PASSIVE);
    app.OnAdapterEvents(0, "S9\n"); CHECK(d.bus_state == ELM_BUS_OFF);
    app.OnAdapterEvents(0, "S0\n");
    g_stats.sync.valid = true; g_stats.sync.samples = 40; g_stats.sync.accepted = 22; g_stats.sync.drift_ppm = 12.5; g_stats.sync.jitter_us = 180;
    g_stats.sync.worst_us = 420; g_stats.sync.rtt_min_ms = 0.9; g_stats.sync.rtt_last_ms = 1.4;
    frame(); frame();

    // stop: the reset carries the Elmue flag
    app.StopCanChannel(0); c = take();
    CHECK(c.size() == 2 && c[0].kind == "rx_stop" && c[1].request == GS_USB_BREQ_MODE && c[1].id == 0x700 && u32at(c[1].data, 4) == ELM_FEATURE_PROTOCOL);
    CHECK(!d.channel_open && !d.elmue_active);

    // host filters, and the adapter rejecting a parameter (the feedback read says so)
    d.use_filters = true; d.filters[0] = {true, false, 0x123, 0x7FF}; d.filters[1] = {true, true, 0x18DA00F1, 0x1FFF00FF}; d.filters[3] = {false, false, 0, 0};
    app.StartCanChannel(0); c = take();
    outs.clear(); ins.clear(); for (auto& x : c) { if (x.kind == "out") outs.push_back(x); else if (x.kind == "in") ins.push_back(x); }
    int fil = 0; for (auto& x : outs) if (x.request == ELM_BREQ_SET_FILTER) ++fil;
    CHECK(fil == 3);
    size_t fi = 0; while (fi < outs.size() && outs[fi].request != ELM_BREQ_SET_FILTER) ++fi;
    if (fi + 2 < outs.size()) {
        CHECK(outs[fi].data.size() == 17 && outs[fi].data[0] == ELM_FIL_HOST_CLEAR);
        CHECK(outs[fi + 1].data[0] == ELM_FIL_HOST_PASS_11 && u32at(outs[fi + 1].data, 1) == 0x123 && u32at(outs[fi + 1].data, 5) == 0x7FF);
        CHECK(outs[fi + 2].data[0] == ELM_FIL_HOST_PASS_29 && u32at(outs[fi + 2].data, 1) == 0x18DA00F1 && u32at(outs[fi + 2].data, 5) == 0x1FFF00FF);
    }
    uint8_t bad = ':';
    app.OnControlDataReceived(0, ELM_BREQ_GET_LAST_ERROR, &ok, 1);
    app.OnControlDataReceived(0, ELM_BREQ_GET_LAST_ERROR, &bad, 1);
    CHECK(!d.channel_opening && !d.channel_open && app.notice_.find("rejected") != std::string::npos);
    // a failed required control transfer stops the start as well, an optional one does not
    take(); d.use_filters = false; app.StartCanChannel(0); take();
    app.OnControlOutDone(0, 0x600, false); CHECK(d.channel_opening);
    app.OnControlOutDone(0, 0x601, false); CHECK(d.channel_opening);
    app.OnControlOutDone(0, 0x501, false); CHECK(!d.channel_opening);
    take();

    // Elmue switched off by the user: the legacy sequence
    d.use_elmue = false; d.enable_fd = true;
    app.StartCanChannel(0); c = take();
    outs.clear(); rx = Call(); for (auto& x : c) { if (x.kind == "out") outs.push_back(x); else if (x.kind == "rx_start") rx = x; }
    CHECK(outs.size() == 4);
    if (outs.size() == 4) {
        CHECK(outs[0].request == GS_USB_BREQ_HOST_FORMAT && outs[1].request == GS_USB_BREQ_BITTIMING && outs[2].request == GS_USB_BREQ_DATA_BITTIMING && outs[3].request == GS_USB_BREQ_MODE);
        for (auto& x : outs) CHECK(x.id == -1);
        CHECK(u32at(outs[3].data, 4) == (GS_CAN_FEATURE_FD | GS_CAN_FEATURE_HW_TIMESTAMP));
    }
    CHECK(rx.b == 0 && rx.c == WebUsbBridge::kRxTimestamps && !d.elmue_active);
    app.OnControlOutDone(0, GS_USB_BREQ_HOST_FORMAT, true); app.OnControlOutDone(0, GS_USB_BREQ_BITTIMING, true); app.OnControlOutDone(0, GS_USB_BREQ_DATA_BITTIMING, true);
    CHECK(d.channel_opening); app.OnControlOutDone(0, GS_USB_BREQ_MODE, true); CHECK(d.channel_open);
    d.use_hw_timestamps = false; app.StopCanChannel(0); take(); app.StartCanChannel(0); c = take(); for (auto& x : c) if (x.kind == "rx_start") rx = x;
    CHECK(rx.c == 0);
    app.OnControlOutDone(0, GS_USB_BREQ_MODE, true); app.SendFrame(m, 0); c = take();
    CHECK(c.size() == 1 && c[0].data.size() == 20 && c[0].data[8] == 2);       // legacy 20 byte frame, dlc 2
    frame();
    app.StopCanChannel(0); c = take(); CHECK(c.size() == 2 && c[1].id == 0x700 && u32at(c[1].data, 4) == ELM_FEATURE_PROTOCOL);

    // termination switch
    d.termination_on = false; app.SetTermination(0, true); c = take();
    CHECK(c.size() == 1 && c[0].request == GS_USB_BREQ_SET_TERMINATION && u32at(c[0].data, 0) == 1 && c[0].id == 0x700 && d.termination_on);

    // a plain legacy adapter (no Elmue flag): no Elmue requests
    UsbDeviceInfo li; li.vid = 0x1D50; li.pid = 0x606F; app.OnDeviceAdded(1, li); app.OnDeviceOpened(1, oi); take();
    GsDeviceBtConst lc = {}; lc.feature = GS_CAN_FEATURE_FD | GS_CAN_FEATURE_HW_TIMESTAMP; lc.fclk_can = 80000000; lc.tseg1_max = 16; lc.tseg2_max = 8; lc.sjw_max = 4; lc.brp_max = 1024; lc.brp_inc = 1;
    app.OnControlDataReceived(1, GS_USB_BREQ_BT_CONST, reinterpret_cast<uint8_t*>(&lc), sizeof(lc));
    CHECK(!app.devices_[1].elmue_supported && take().empty());
    app.StartCanChannel(1); c = take(); int n_out = 0; for (auto& x : c) if (x.kind == "out") { ++n_out; CHECK(x.id == -1); }
    CHECK(n_out == 4);
    app.OnControlOutDone(1, GS_USB_BREQ_MODE, true); CHECK(app.devices_[1].channel_open);
    app.StopCanChannel(1); c = take(); CHECK(c.size() == 2 && c[1].id == -1 && u32at(c[1].data, 4) == 0);
    frame();
    app.RemoveDevice(0); app.RemoveDevice(1);
}

static void TestPcan(AppUI& app) {
    UsbDeviceInfo info; info.vid = 0x0C72; info.pid = 0x0012; info.product = "PCAN-USB FD";
    app.OnDeviceAdded(2, info);
    DeviceOpenInfo oi; oi.protocol = PROTOCOL_PEAK_PCAN_FD; oi.endpoints = "x"; oi.info = "fw=1.2.3;bl=4.5.6;hw=3;mcu=2;ser=777;ch=1";
    app.OnDeviceOpened(2, oi);
    DeviceSlot& d = app.devices_[2];
    CHECK(take().empty());                                            // the worker read the firmware info already: nothing to ask
    CHECK(d.protocol == PROTOCOL_PEAK_PCAN_FD && d.pcan_fw == "1.2.3" && d.pcan_bl == "4.5.6" && d.pcan_serial == "777" && d.pcan_channels == "1");
    CHECK(d.has_bt_const && d.bt_const.fclk_can == 80000000U && d.nom_bt.brp > 0);

    d.enable_fd = true;
    app.StartCanChannel(2);
    auto c = take();
    std::vector<Call> cmds; Call rx; for (auto& x : c) { if (x.kind == "cmd") cmds.push_back(x); else if (x.kind == "rx_start") rx = x; }
    CHECK(cmds.size() >= 3 && cmds.front().id == 0x100 && rx.kind == "rx_start" && rx.c == 0 && d.channel_opening);
    for (size_t i = 0; i < cmds.size(); ++i) CHECK(cmds[i].id == 0x100 + static_cast<int>(i));
    for (size_t i = 0; i + 1 < cmds.size(); ++i) app.OnControlOutDone(2, cmds[i].id, true);
    CHECK(d.channel_opening && !d.channel_open);
    app.OnControlOutDone(2, cmds.back().id, true);
    CHECK(d.channel_open && app.IsBusOn(2));

    // PEAK frames are the same for every adapter and carry an echo request
    CanMessage m; m.id = 0x321; m.dlc = 1; m.payload.resize(1, 7);
    CHECK(app.SendFrame(m, 2) == 1); c = take(); CHECK(c.size() == 1 && c[0].kind == "tx" && c[0].data.size() >= 24);
    app.OnAdapterEvents(2, "U12\nS1\nE00000080:0000010000000102\n");
    CHECK(d.bus_load == 12 && d.bus_state == ELM_BUS_WARNING && d.bus_tx_errors == 1 && d.bus_rx_errors == 2);
    g_stats.sync.valid = true; g_stats.sync.pushed = true; g_stats.sync.samples = 30; g_stats.sync.accepted = 12; g_stats.sync.drift_ppm = -40; g_stats.sync.jitter_us = 90;
    frame();
    g_stats = UsbDeviceStats();

    app.StopCanChannel(2); c = take();
    int stop_cmds = 0; bool stopped_rx = false; for (auto& x : c) { if (x.kind == "cmd") { CHECK(x.id == 0x200 + stop_cmds); ++stop_cmds; } if (x.kind == "rx_stop") stopped_rx = true; }
    CHECK(stopped_rx && stop_cmds >= 2 && !d.channel_open);

    // a failed command rolls the start back
    app.StartCanChannel(2); c = take(); cmds.clear(); for (auto& x : c) if (x.kind == "cmd") cmds.push_back(x);
    app.OnControlOutDone(2, cmds[1].id, false); CHECK(!d.channel_opening && !d.channel_open);
    take();
    app.RemoveDevice(2);
}

static void TestSlcan(AppUI& app) {
    UsbDeviceInfo info; info.is_serial = true; info.product = "serial";
    app.OnDeviceAdded(3, info);
    DeviceSlot& d = app.devices_[3];
    CHECK(d.protocol == PROTOCOL_SLCAN && d.is_serial);
    DeviceOpenInfo oi; oi.protocol = PROTOCOL_SLCAN;
    app.OnDeviceOpened(3, oi);
    auto c = take();
    CHECK(c.size() == 1 && c[0].kind == "cmd" && c[0].id == SLCAN_CMD_VERSION && text(c[0]) == "V\r");

    const std::string v = "+Board: CANable\tMCU: STM32G431\tDevID: 1\tFirmware: 2.5\tSlcan: 1\tClock: 160\tChannels: 1\tQuartz: Yes\tLimits: 512,256,128,128,32,32,16,16\tHAL: 1\tSerial: ABC";
    app.OnControlDataReceived(3, SLCAN_CMD_VERSION, reinterpret_cast<const uint8_t*>(v.data()), static_cast<int>(v.size()));
    CHECK(d.slcan_probed && d.slcan_dialect == SLCAN_DIALECT_CANABLE && d.bt_const.fclk_can == 160000000U && d.slcan_info.nominal.brp_max == 512);
    CHECK(app.EffectiveDialect(d) == SLCAN_DIALECT_CANABLE);

    d.enable_fd = true;
    app.StartCanChannel(3); c = take();
    std::vector<Call> cmds; Call rx; for (auto& x : c) { if (x.kind == "cmd") cmds.push_back(x); else if (x.kind == "rx_start") rx = x; }
    CHECK(cmds.size() >= 4 && cmds[0].id == SLCAN_CMD_ID_START && text(cmds[0]) == "C\r" && d.channel_opening);
    bool has_s = false, has_y = false; for (auto& x : cmds) { if (text(x)[0] == 's') has_s = true; if (text(x)[0] == 'y') has_y = true; }
    CHECK(has_s && has_y);
    for (size_t i = 0; i < cmds.size(); ++i) CHECK(cmds[i].id == SLCAN_CMD_ID_START + static_cast<int>(i));
    for (size_t i = 0; i + 1 < cmds.size(); ++i) app.OnControlOutDone(3, cmds[i].id, true);
    CHECK(!d.channel_open); app.OnControlOutDone(3, cmds.back().id, true); CHECK(d.channel_open);

    CanMessage m; m.id = 0x123; m.dlc = 2; m.payload.resize(2, 0xAB);
    CHECK(app.SendFrame(m, 3) == 1); c = take(); CHECK(c.size() == 1 && c[0].kind == "tx" && text(c[0]) == "t1232ABAB\r");
    app.StopCanChannel(3); c = take();
    bool c_cmd = false; for (auto& x : c) if (x.kind == "cmd" && x.id == SLCAN_CMD_ID_STOP && text(x) == "C\r") c_cmd = true;
    CHECK(c_cmd && !d.channel_open);

    // a rejected start command
    app.StartCanChannel(3); c = take(); cmds.clear(); for (auto& x : c) if (x.kind == "cmd") cmds.push_back(x);
    app.OnControlOutDone(3, cmds[2].id, false); CHECK(!d.channel_opening && app.notice_.find("rejected") != std::string::npos);
    take();

    // a classic (Lawicel) adapter: a bit rate without a code cannot be started, FD frames cannot be sent
    d.slcan_dialect_choice = 2; d.target_nom_kbps = 333; d.enable_fd = false;
    app.StartCanChannel(3); c = take();
    CHECK(!d.channel_opening && app.notice_.find("supports only the bit rates") != std::string::npos);
    d.target_nom_kbps = 500; app.StartCanChannel(3); c = take(); cmds.clear(); for (auto& x : c) if (x.kind == "cmd") cmds.push_back(x);
    CHECK(cmds.size() >= 3 && text(cmds[1]) == "S6\r");
    for (auto& x : cmds) app.OnControlOutDone(3, x.id, true);
    CHECK(d.channel_open);
    CanMessage fd = m; fd.is_fd = true; fd.dlc = 9; fd.payload.resize(12, 1);
    CHECK(app.SendFrame(fd, 3) == 0); CHECK(app.notice_.find("classic") != std::string::npos);
    take(); frame();
    app.RemoveDevice(3);
}

// The event log: lines from the worker, connections, losses with their reasons; the window renders
static void TestEventLog(AppUI& app) {
    app.log_.clear();
    app.OnLog(1, 2, "control transfer failed: NetworkError: A transfer error has occurred.");
    app.OnLog(1, 1, "recovering: RX errors");
    UsbDeviceInfo info; info.vid = 0x1D50; info.pid = 0x606F;
    app.OnDeviceAdded(4, info);
    app.OnDeviceClosed(4, "device disconnected");
    app.OnDeviceClosed(4, "closed by host");
    app.OnNotice("Device 5: something to tell");
    CHECK(app.log_.size() == 5);
    CHECK(app.log_[0].level == 2 && app.log_[0].text.find("dev 2: control transfer failed") != std::string::npos);
    CHECK(app.log_[1].level == 1 && app.log_[2].level == 2 && app.log_[2].text.find("dev 5: closed: device disconnected") != std::string::npos);
    CHECK(app.log_[3].level == 0 && app.log_[3].text.find("dev 5: closed") != std::string::npos && app.log_[4].level == 1);
    app.OnLog(0, 1, std::string(2000, 'x'));
    CHECK(app.log_.back().text.size() < 500);
    for (int i = 0; i < 1000; ++i) app.OnLog(0, 1, "line");
    CHECK(app.log_.size() == 600 && app.log_dirty_);
    app.show_log_window_ = true;
    app.log_.push_front({-1, "---- marker ----"});
    frame(); frame();
    app.SaveLog();
    CHECK(!app.log_dirty_);
    app.show_log_window_ = false;
    app.RemoveDevice(4);

    // the clipboard: ImGui's functions are replaced; the stand-ins of the browser calls return nothing (idle, empty)
    BrowserClipboard::Install();
    ImGui::SetClipboardText("text");
    const char* pasted = ImGui::GetClipboardText();
    CHECK(pasted && pasted[0] == 0);
    CHECK(BrowserClipboard::GetState() == BrowserClipboard::State::Idle && BrowserClipboard::StatusText().empty());
    app.show_log_window_ = true;
    frame();
    app.show_log_window_ = false;
}

// The adapter's own error reports reach the event log, at most one line a second
static void TestAdapterErrorLog(AppUI& app) {
    UsbDeviceInfo info; info.vid = 0x1D50; info.pid = 0x606F;
    DeviceOpenInfo oi; oi.protocol = PROTOCOL_GS_USB_CANDLELIGHT;
    app.OnDeviceAdded(7, info);
    app.OnDeviceOpened(7, oi);
    take();
    app.log_.clear();
    g_now_ms += 5000.0;
    app.OnAdapterEvents(7, "E00000080:0002000000000000\n");
    CHECK(app.devices_[7].error_reports == 1);
    CHECK(app.log_.size() == 1 && app.log_[0].level == 1 && app.log_[0].text.find("dev 8: adapter error report: ") != std::string::npos &&
          app.log_[0].text.find("TX buffer overflow") != std::string::npos);
    app.OnAdapterEvents(7, "E00000080:0002000000000000\nE00000080:0002000000000000\n");
    CHECK(app.log_.size() == 1);                       // within a second: not logged again
    g_now_ms += 1500.0;
    app.OnAdapterEvents(7, "E00000080:0002000000000000\n");
    CHECK(app.log_.size() == 2 && app.log_[1].text.find("(+3)") != std::string::npos);      // the ones in between are counted
    app.RemoveDevice(7);
}

// The Benchmark window: a sender and a receiver, the frames follow the echoes, the result is made from the Receive Table counters
static void TestBenchmark(AppUI& app) {
    UsbDeviceInfo info; info.vid = 0x1D50; info.pid = 0x606F;
    DeviceOpenInfo oi; oi.protocol = PROTOCOL_GS_USB_CANDLELIGHT;
    for (int dev : {5, 6}) {
        app.OnDeviceAdded(dev, info);
        app.OnDeviceOpened(dev, oi);
        app.devices_[dev].connected = true;
        app.devices_[dev].channel_open = true;
    }
    take();
    app.log_.clear();

    // refused starts
    app.bench_tx_ = kTargetAllDevices;
    app.StartBenchmark();
    CHECK(!app.bench_.Active() && !app.bench_note_.empty());
    app.bench_tx_ = 5; app.bench_rx_ = 5;
    app.StartBenchmark();
    CHECK(!app.bench_.Active() && app.bench_note_.find("another adapter") != std::string::npos);

    app.bench_rx_ = 6; app.bench_duration_ = 2; app.bench_window_ = 50; app.bench_bytes_ = 8; app.bench_fd_ = false;
    app.StartBenchmark();
    CHECK(app.bench_.Active() && app.bench_note_.empty());
    const uint32_t id = app.bench_.TestId();
    {   // the FIFO limit of the run goes to the worker at the start (the default 64 here) and back to 64 at the end
        int fifo_calls = 0;
        for (const Call& c : g_calls) if (c.kind == "txfifo" && c.dev == 5 && c.value == 64) ++fifo_calls;
        CHECK(fifo_calls == 1);
    }
    app.show_bench_window_ = true;
    frame();

    auto tx_count = [&] { size_t n = 0; for (const Call& c : take()) if (c.kind == "tx" && c.dev == 5) ++n; return n; };
    g_now_ms += 16.0;
    app.ProcessBenchmark(app.GetNowMs());
    CHECK(tx_count() == 50);                                   // the window is filled
    g_now_ms += 16.0;
    app.ProcessBenchmark(app.GetNowMs());
    CHECK(tx_count() == 0);                                    // no echo yet

    IdTableEntry e;
    e.count = 30;
    app.id_table_[MakeCanIdKey(5, id, false)] = e;             // 30 echoes of the sender
    g_now_ms += 16.0;
    app.ProcessBenchmark(app.GetNowMs());
    CHECK(tx_count() == 30);
    frame();

    e.count = 80;
    app.id_table_[MakeCanIdKey(5, id, false)] = e;
    IdTableEntry r;
    r.count = 80;
    r.max_cycle_ms = 4.0;
    app.id_table_[MakeCanIdKey(6, id, false)] = r;
    g_now_ms += 2100.0;
    app.ProcessBenchmark(app.GetNowMs());                      // the time is over
    CHECK(app.bench_.state() == TxBenchmark::State::Draining);
    g_now_ms += 1300.0;
    app.ProcessBenchmark(app.GetNowMs());
    CHECK(app.bench_.state() == TxBenchmark::State::Done);
    const BenchResult& res = app.bench_.result();
    CHECK(res.sent == 80 && res.echoed == 80 && res.received == 80 && res.lost == 0 && res.has_receiver);
    CHECK(!app.log_.empty() && app.log_.back().text.find("benchmark finished: 80 sent, 80 echoed") != std::string::npos && app.log_.back().level == 0);
    CHECK(app.BenchReportText().find("lost 0") != std::string::npos);
    frame();

    // a lost frame is reported as an error
    g_calls.clear();
    app.bench_fifo_ = 32;
    app.StartBenchmark();
    CHECK(app.bench_run_fifo_ == 32);
    {
        bool lowered = false;
        for (const Call& c : g_calls) if (c.kind == "txfifo" && c.dev == 5 && c.value == 32) lowered = true;
        CHECK(lowered);
    }
    const uint32_t id2 = app.bench_.TestId();
    CHECK(id2 != id);                                          // every run has IDs of its own
    app.bench_.Stop(app.GetNowMs(), "stopped");
    IdTableEntry e2; e2.count = 10;
    app.id_table_[MakeCanIdKey(5, id2, false)] = e2;
    IdTableEntry r2; r2.count = 9;
    app.id_table_[MakeCanIdKey(6, id2, false)] = r2;
    g_now_ms += 1300.0;
    app.ProcessBenchmark(app.GetNowMs());
    CHECK(app.bench_.state() == TxBenchmark::State::Done && app.bench_.result().lost == 1 && app.log_.back().level == 2);
    {
        bool restored = false;       // the full FIFO is given back when the run is over
        for (const Call& c : g_calls) if (c.kind == "txfifo" && c.dev == 5 && c.value == 64) restored = true;
        CHECK(restored);
        CHECK(app.BenchReportText().find("32 of 64 slots") != std::string::npos);
    }

    app.show_bench_window_ = false;
    app.RemoveDevice(5); app.RemoveDevice(6);
}

// The big tables build the rows in view only, and a view that is in order is not sorted again
static void TestTableViews(AppUI& app) {
    for (int i = 0; i < 2000; ++i) {
        TraceEntry t;
        t.timestamp_sec = 1.0 + i * 0.001;
        t.msg.id = 0x100 + static_cast<uint32_t>(i);
        t.msg.dev = 0;
        t.msg.dlc = 8;
        t.msg.payload.resize(8);
        app.rx_trace_.push_back(t);
    }
    for (int i = 0; i < 1500; ++i) {
        IdTableEntry e;
        e.last_msg.id = 0x100 + static_cast<uint32_t>(i);
        e.last_msg.dev = 0;
        e.last_msg.dlc = 8;
        e.last_msg.payload.resize(8);
        e.count = 1;
        app.id_table_[MakeCanIdKey(0, e.last_msg.id, false)] = e;
    }
    app.auto_tile_layout_ = true;
    app.show_trace_window_ = true;
    app.show_id_table_window_ = true;
    frame(); frame();
    CHECK(app.trace_rows_built_ > 0 && app.trace_rows_built_ < 200);        // not 2000
    CHECK(app.id_table_rows_built_ > 0 && app.id_table_rows_built_ < 200);  // not 1500

    // a growing sequence is checked in one pass: n - 1 comparisons, no sort
    std::vector<int> v(1000);
    for (int i = 0; i < 1000; ++i) v[i] = i;
    int calls = 0;
    SortUnlessSorted(v.begin(), v.end(), [&](int a, int b) { ++calls; return a < b; });
    CHECK(calls == 999 && v.front() == 0 && v.back() == 999);
    v[500] = -1;                                                            // out of order: sorted
    SortUnlessSorted(v.begin(), v.end(), [](int a, int b) { return a < b; });
    CHECK(std::is_sorted(v.begin(), v.end()) && v.front() == -1);

    app.rx_trace_.clear();
    app.id_table_.clear();
    app.show_trace_window_ = false;
    app.show_id_table_window_ = false;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); unsigned char* px; int w, h; io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    AppUI app; g_app = &app;
    TestGsUsb(app);
    TestPcan(app);
    TestSlcan(app);
    TestEventLog(app);
    TestAdapterErrorLog(app);
    TestBenchmark(app);
    TestTableViews(app);
    std::printf("%d checks, %d failed\n", g_n, g_fail);
    return g_fail != 0;
}
