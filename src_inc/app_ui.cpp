#include "app_ui.h"
#include "webusb_bridge.h"
#include "imgui.h"
#include "url_params.h"
#include <GLFW/glfw3.h>
#include <emscripten.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <cmath>

// Column User IDs for ImGui table sorting specifications
enum IdTableColumnId : ImGuiID {
    ID_COL_CAN_ID = 1,
    ID_COL_TYPE,
    ID_COL_LENGTH,
    ID_COL_COUNT,
    ID_COL_CYCLE,
    ID_COL_DATA,
    ID_COL_DEV
};

enum TraceTableColumnId : ImGuiID {
    TRACE_COL_TIME = 1,
    TRACE_COL_DIR,
    TRACE_COL_CAN_ID,
    TRACE_COL_FORMAT,
    TRACE_COL_DLC,
    TRACE_COL_DATA,
    TRACE_COL_DEV
};

enum PeriodicTableColumnId : ImGuiID {
    JOB_COL_ENABLED = 1,
    JOB_COL_CAN_ID,
    JOB_COL_CYCLE,
    JOB_COL_SENT,
    JOB_COL_DATA,
    JOB_COL_ACTIONS,
    JOB_COL_DEV
};

enum HistoryTableColumnId : ImGuiID {
    HIST_COL_LAST_SENT = 1,
    HIST_COL_CAN_ID,
    HIST_COL_FORMAT,
    HIST_COL_DLC,
    HIST_COL_COUNT,
    HIST_COL_DATA,
    HIST_COL_OPS,
    HIST_COL_DEV
};

// Helper: Parse space-separated hexadecimal string into byte array
static std::vector<uint8_t> ParseHexPayload(const char* hex_str, size_t max_len) {
    std::vector<uint8_t> result;
    const char* p = hex_str;

    while (*p && result.size() < max_len) {
        while (*p == ' ' || *p == ',' || *p == '\t') p++;
        if (!*p) break;

        char* endptr = nullptr;
        unsigned long val = std::strtoul(p, &endptr, 16);
        if (endptr == p) break;

        result.push_back(static_cast<uint8_t>(val & 0xFF));
        p = endptr;
    }
    return result;
}

// Helper: Format payload into a space-separated hexadecimal string
static std::string FormatHexPayload(const CanPayload& data) {
    char buf[256] = {0};
    int offset = 0;
    for (size_t i = 0; i < data.size() && offset < 245; ++i) {
        offset += std::snprintf(buf + offset, sizeof(buf) - offset, "%02X%s",
                                data[i], (i + 1 < data.size()) ? " " : "");
    }
    return std::string(buf);
}

// Helper: Compute an integer rank for CAN frame format flags to allow sorting by frame type
static int GetFrameFormatRank(const CanMessage& m) {
    int rank = 0;
    if (m.is_extended) rank |= (1 << 0);
    if (m.is_fd)       rank |= (1 << 1);
    if (m.is_brs)      rank |= (1 << 2);
    if (m.is_rtr)      rank |= (1 << 3);
    return rank;
}

// Helper: Compare two payloads lexicographically (-1 if a < b, 1 if a > b, 0 if equal)
static int ComparePayloads(const CanPayload& a, const CanPayload& b) {
    size_t min_len = std::min(a.size(), b.size());
    if (min_len > 0) {
        int cmp = std::memcmp(a.data(), b.data(), min_len);
        if (cmp != 0) return (cmp < 0) ? -1 : 1;
    }
    if (a.size() < b.size()) return -1;
    if (a.size() > b.size()) return 1;
    return 0;
}

// Generic three-way comparison helper returning -1, 0, or +1
template <typename T>
static int ThreeWayCompare(const T& a, const T& b) {
    if (a < b) return -1;
    if (b < a) return 1;
    return 0;
}

// One colour per device number, used wherever a device number is shown
static ImVec4 DeviceColor(int dev) {
    static const ImVec4 palette[kMaxDevices] = {
        ImVec4(0.45f, 0.80f, 1.00f, 1.0f), ImVec4(1.00f, 0.75f, 0.35f, 1.0f),
        ImVec4(0.55f, 0.95f, 0.55f, 1.0f), ImVec4(0.95f, 0.55f, 0.85f, 1.0f),
        ImVec4(0.95f, 0.90f, 0.45f, 1.0f), ImVec4(0.70f, 0.65f, 1.00f, 1.0f),
        ImVec4(1.00f, 0.55f, 0.50f, 1.0f), ImVec4(0.60f, 0.90f, 0.85f, 1.0f)
    };
    return palette[(dev >= 0 && dev < kMaxDevices) ? dev : 0];
}

// Human readable list of the gs_usb feature bits an adapter reports
static std::string FeatureList(uint32_t features) {
    static const char* const names[] = {
        "Listen-only", "Loopback", "Triple-sample", "One-shot", "HW timestamp",
        "Identify", "User ID", "Pad packets", "CAN FD"
    };
    constexpr size_t kNames = sizeof(names) / sizeof(names[0]);

    std::string text;
    for (size_t i = 0; i < kNames; ++i) {
        if (features & (1u << i)) {
            if (!text.empty()) text += ", ";
            text += names[i];
        }
    }
    const uint32_t others = features & ~((1u << kNames) - 1u);
    if (others) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%s+0x%X", text.empty() ? "" : ", ", others);
        text += buf;
    }
    return text.empty() ? std::string("none") : text;
}

// State of an adapter for the Devices window
static const char* DeviceStateText(const DeviceSlot& d, ImVec4& color) {
    if (d.connected && d.channel_open)  { color = ImVec4(0.3f, 1.0f, 0.4f, 1.0f); return "BUS ON"; }
    if (d.channel_opening)              { color = ImVec4(1.0f, 0.85f, 0.3f, 1.0f); return "Starting..."; }
    if (d.connected)                    { color = ImVec4(0.4f, 0.9f, 1.0f, 1.0f); return "Connected, bus off"; }
    if (d.connecting)                   { color = ImVec4(1.0f, 0.85f, 0.3f, 1.0f); return "Connecting..."; }
    color = ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
    return "Disconnected";
}

// Reads one query parameter of the page URL. `which` indexes the name table in the JS body (a numeric
// index keeps the string marshalling out of it). Returns 0 if the parameter is absent.
enum UrlParam : int {
    URL_BAUDRATE = 0,
    URL_SAMPLE_POINT,
    URL_DATA_BAUDRATE,
    URL_DATA_SAMPLE_POINT
};

EM_JS(int, js_get_query_param, (int which, char* out, int out_size), {
    const names = ["baudrate", "sample_point", "data_baudrate", "data_sample_point"]; // order of UrlParam
    const value = new URLSearchParams(window.location.search).get(names[which]);
    if (value === null) return 0;
    stringToUTF8(value, out, out_size);
    return 1;
});

static bool GetUrlParam(UrlParam which, char* out, int out_size) {
    out[0] = '\0';
    return js_get_query_param(static_cast<int>(which), out, out_size) != 0;
}

AppUI::AppUI()
    : start_time_ms_(emscripten_get_now()),
      selected_dev_(-1),
      notice_time_ms_(0.0),
      auto_tile_layout_(true),
      show_hw_window_(true),
      show_tx_window_(true),
      show_history_window_(true),
      show_id_table_window_(true),
      show_trace_window_(true),
      tx_target_(kTargetAllDevices),
      tx_id_(0x123),
      tx_is_ext_(false),
      tx_is_fd_(true),
      tx_is_brs_(true),
      tx_is_rtr_(false),
      tx_dlc_(8),
      tx_cycle_ms_(0),
      tx_burst_count_(1),
      tx_auto_inc_byte_(-1),
      trace_paused_(false),
      trace_autoscroll_(true),
      trace_filter_id_(-1),
      trace_filter_dev_(-1),
      table_filter_dev_(-1),
      selected_history_index_(-1)
{
    std::strncpy(tx_payload_, "01 02 03 04 05 06 07 08", sizeof(tx_payload_) - 1);
    tx_payload_[sizeof(tx_payload_) - 1] = '\0';

    std::strncpy(tx_comment_, "Engine Status", sizeof(tx_comment_) - 1);
    tx_comment_[sizeof(tx_comment_) - 1] = '\0';
}

void AppUI::ApplyUrlParameters() {
    char text[64];

    auto apply_bitrate = [&](UrlParam which, const char* label, int& target_kbps) {
        if (!GetUrlParam(which, text, sizeof(text))) return;
        int kbps = 0;
        if (ParseBitrateKbps(text, kbps)) {
            target_kbps = kbps;
            std::printf("[URL] %s bit rate: %d kbit/s\n", label, kbps);
        } else {
            std::printf("[URL] ignored invalid %s bit rate '%s' (use e.g. 500k, 2M or 500000; %d..%d kbit/s)\n",
                        label, text, kMinBitrateKbps, kMaxBitrateKbps);
        }
    };
    auto apply_sample_point = [&](UrlParam which, const char* label, float& target_pct) {
        if (!GetUrlParam(which, text, sizeof(text))) return;
        float pct = 0.0f;
        if (ParseSamplePointPct(text, pct)) {
            target_pct = pct;
            std::printf("[URL] %s sample point: %.1f%%\n", label, pct);
        } else {
            std::printf("[URL] ignored invalid %s sample point '%s' (use e.g. 87.5 or 0.875; %.0f..%.0f %%)\n",
                        label, text, kMinSamplePointPct, kMaxSamplePointPct);
        }
    };

    // They are the defaults of every adapter added later; each adapter can still be changed on its own
    apply_bitrate(URL_BAUDRATE, "nominal", defaults_.target_nom_kbps);
    apply_sample_point(URL_SAMPLE_POINT, "nominal", defaults_.target_nom_sp);
    apply_bitrate(URL_DATA_BAUDRATE, "data", defaults_.target_data_kbps);
    apply_sample_point(URL_DATA_SAMPLE_POINT, "data", defaults_.target_data_sp);
}

double AppUI::GetNowMs() const {
    return emscripten_get_now() - start_time_ms_;
}

// ============================================================================
// Device slots
// ============================================================================

// Ids of the command transfers of PEAK start/stop sequences; their acknowledgements come back through
// OnControlOutDone() and are told apart from gs_usb request codes by being >= 0x100
static constexpr int kPcanCmdIdStart = 0x100;
static constexpr int kPcanCmdIdStop  = 0x200;

// Register limits of an adapter's bit timing: what the editors may enter and the automatic calculation may use
static const CanBitTimingLimits kGsLimits = {512, 128, 128, 1024};   // as wide as the editor always allowed
static CanBitTimingLimits NominalLimitsOf(const DeviceSlot& d) {
    return d.protocol == PROTOCOL_PEAK_PCAN_FD ? PcanNominalLimits() : kGsLimits;
}
static CanBitTimingLimits DataLimitsOf(const DeviceSlot& d) {
    return d.protocol == PROTOCOL_PEAK_PCAN_FD ? PcanDataLimits() : kGsLimits;
}

// Automatic calculation within the limits; if nothing fits gs_usb keeps the old search (it always produced
// some value), other protocols leave the previous values
static void CalculateTiming(CanProtocolType protocol, uint32_t fclk_hz, uint32_t bps, float sample_point,
                            const CanBitTimingLimits& limits, GsDeviceBittiming& bt) {
    if (CanProtocolHandler::AutoCalculateBittiming(fclk_hz, bps, sample_point, limits, bt)) return;
    if (protocol == PROTOCOL_GS_USB_CANDLELIGHT) CanProtocolHandler::AutoCalculateBittiming(fclk_hz, bps, sample_point, bt);
}

void AppUI::InitDeviceDefaults(DeviceSlot& d) const {
    d.target_nom_kbps = defaults_.target_nom_kbps;
    d.target_nom_sp = defaults_.target_nom_sp;
    d.target_data_kbps = defaults_.target_data_kbps;
    d.target_data_sp = defaults_.target_data_sp;
    d.enable_fd = defaults_.enable_fd;
    d.enable_loopback = defaults_.enable_loopback;
    d.enable_listen_only = defaults_.enable_listen_only;

    // Placeholder limits until the adapter answers GS_USB_BREQ_BT_CONST
    std::memset(&d.bt_const, 0, sizeof(d.bt_const));
    d.bt_const.fclk_can  = 80000000U; // Default 80 MHz clock
    d.bt_const.tseg1_min = 1;
    d.bt_const.tseg1_max = 256;
    d.bt_const.tseg2_min = 1;
    d.bt_const.tseg2_max = 128;
    d.bt_const.sjw_max   = 128;
    d.bt_const.brp_min   = 1;
    d.bt_const.brp_max   = 512;
    d.has_bt_const = false;
    d.has_config = false;
    std::memset(&d.config, 0, sizeof(d.config));

    RecalculateTimings(d);
}

void AppUI::RecalculateTimings(DeviceSlot& d) {
    CalculateTiming(d.protocol, d.bt_const.fclk_can, d.target_nom_kbps * 1000U, d.target_nom_sp, NominalLimitsOf(d), d.nom_bt);
    CalculateTiming(d.protocol, d.bt_const.fclk_can, d.target_data_kbps * 1000U, d.target_data_sp, DataLimitsOf(d), d.data_bt);
}

// Fixed properties of the adapter's protocol. A PEAK adapter has no bit-timing constants request: it runs
// its CAN cores from an 80 MHz domain and supports CAN FD, hardware timestamps and listen-only mode.
void AppUI::ApplyProtocol(DeviceSlot& d, CanProtocolType protocol) {
    d.protocol = protocol;
    if (protocol == PROTOCOL_PEAK_PCAN_FD) {
        std::memset(&d.bt_const, 0, sizeof(d.bt_const));
        d.bt_const.feature = GS_CAN_FEATURE_FD | GS_CAN_FEATURE_HW_TIMESTAMP | GS_CAN_FEATURE_LISTEN_ONLY;
        d.bt_const.fclk_can = PCAN_CAN_CLOCK_HZ;
        d.has_bt_const = true;
        d.enable_loopback = false;   // no internal loopback mode in this protocol
    }
    RecalculateTimings(d);
}

bool AppUI::IsBusOn(int dev) const {
    return ValidDevice(dev) && devices_[dev].used && devices_[dev].connected && devices_[dev].channel_open;
}

int AppUI::ConnectedDeviceCount() const {
    int n = 0;
    for (const DeviceSlot& d : devices_) {
        if (d.used && d.connected) ++n;
    }
    return n;
}

int AppUI::BusOnDeviceCount() const {
    int n = 0;
    for (int dev = 0; dev < kMaxDevices; ++dev) {
        if (IsBusOn(dev)) ++n;
    }
    return n;
}

std::string AppUI::DeviceLabel(int dev) const {
    if (!ValidDevice(dev)) return "?";
    const DeviceSlot& d = devices_[dev];
    std::string name = d.info.product;
    if (name.empty()) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%04X:%04X", d.info.vid, d.info.pid);
        name = buf;
    }
    std::string label = std::to_string(dev + 1) + ": " + name;
    if (!d.info.serial.empty()) label += " (S/N " + d.info.serial + ")";
    return label;
}

void AppUI::DeviceCombo(const char* label, int& value, const char* all_label, float width) {
    if (value >= 0 && (!ValidDevice(value) || !devices_[value].used)) value = kTargetAllDevices;   // slot removed meanwhile

    const std::string preview = (value < 0) ? std::string(all_label) : DeviceLabel(value);
    ImGui::SetNextItemWidth(width);
    if (ImGui::BeginCombo(label, preview.c_str())) {
        if (ImGui::Selectable(all_label, value < 0)) value = kTargetAllDevices;
        for (int dev = 0; dev < kMaxDevices; ++dev) {
            if (!devices_[dev].used) continue;
            ImVec4 color;
            const char* state = DeviceStateText(devices_[dev], color);
            const std::string text = DeviceLabel(dev) + "  [" + state + "]";
            ImGui::PushID(dev);
            if (ImGui::Selectable(text.c_str(), value == dev)) value = dev;
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
}

void AppUI::OnDeviceAdded(int dev, const UsbDeviceInfo& info) {
    if (!ValidDevice(dev)) return;
    DeviceSlot& d = devices_[dev];
    if (!d.used) InitDeviceDefaults(d);

    d.used = true;
    d.info = info;
    d.connecting = true;
    d.connected = false;
    d.channel_opening = false;
    d.channel_open = false;
    d.endpoints.clear();
    d.status.clear();

    if (selected_dev_ < 0 || !devices_[selected_dev_].used) selected_dev_ = dev;
}

void AppUI::OnDeviceOpened(int dev, const DeviceOpenInfo& open) {
    if (!ValidDevice(dev) || !devices_[dev].used) return;
    DeviceSlot& d = devices_[dev];
    d.connecting = false;
    d.connected = true;
    d.endpoints = open.endpoints;
    ApplyProtocol(d, open.protocol);

    if (open.protocol == PROTOCOL_PEAK_PCAN_FD) {
        // The worker already read the firmware info while opening: no requests to send
        d.pcan_fw = open.Get("fw");
        d.pcan_bl = open.Get("bl");
        d.pcan_hw = open.Get("hw");
        d.pcan_mcu = open.Get("mcu");
        d.pcan_serial = open.Get("ser");
        d.pcan_channels = open.Get("ch");
        return;
    }

    // gs_usb: query bit-timing constants and the adapter description from EP0
    auto& usb = WebUsbBridge::GetInstance();
    usb.ControlTransferIn(dev, GS_USB_BREQ_BT_CONST, 0, 0, sizeof(GsDeviceBtConst));
    usb.ControlTransferIn(dev, GS_USB_BREQ_DEVICE_CONFIG, 0, 0, sizeof(GsDeviceConfig));
}

void AppUI::OnDeviceClosed(int dev, const std::string& reason) {
    if (!ValidDevice(dev) || !devices_[dev].used) return;
    DeviceSlot& d = devices_[dev];
    d.connecting = false;
    d.connected = false;
    d.channel_opening = false;
    d.channel_open = false;
    d.status = (reason == "closed by host") ? std::string() : reason;
}

void AppUI::OnNotice(const std::string& text) {
    notice_ = text;
    notice_time_ms_ = GetNowMs();
}

void AppUI::OnControlDataReceived(int dev, uint8_t request, const uint8_t* data, int length) {
    if (!ValidDevice(dev) || !devices_[dev].used) return;
    DeviceSlot& d = devices_[dev];

    if (request == GS_USB_BREQ_BT_CONST && length >= static_cast<int>(sizeof(GsDeviceBtConst))) {
        std::memcpy(&d.bt_const, data, sizeof(GsDeviceBtConst));
        d.has_bt_const = true;
        if (d.bt_const.fclk_can > 0) RecalculateTimings(d);
    } else if (request == GS_USB_BREQ_DEVICE_CONFIG && length >= static_cast<int>(sizeof(GsDeviceConfig))) {
        std::memcpy(&d.config, data, sizeof(GsDeviceConfig));
        d.has_config = true;
    }
}

// Control OUT results arrive in submission order per adapter. The channel counts as open only after the
// MODE_START request was acknowledged; any failure of the start sequence rolls it back.
void AppUI::OnControlOutDone(int dev, int request, bool ok) {
    if (!ValidDevice(dev) || !devices_[dev].used) return;
    DeviceSlot& d = devices_[dev];
    if (!d.channel_opening) return;

    if (d.protocol == PROTOCOL_PEAK_PCAN_FD) {
        // The start sequence is a few command transfers; the channel is open after the last one was written
        if (request < kPcanCmdIdStart) return;
        if (!ok) {
            StopCanChannel(dev);
        } else if (request == d.pcan_start_last_id) {
            d.channel_opening = false;
            d.channel_open = true;
        }
        return;
    }

    if (!ok) {
        // Firmware may legitimately reject HOST_FORMAT (it is optional); everything else is fatal
        if (request != GS_USB_BREQ_HOST_FORMAT) StopCanChannel(dev);
        return;
    }
    if (request == GS_USB_BREQ_MODE) {
        d.channel_opening = false;
        d.channel_open = true;
    }
}

// The worker core owns parsing, per-ID aggregation (count, cycle time from adapter timestamps) and
// the trace queue. Rows arrive as complete latest states, so applying them is a plain overwrite.
// Core times are on the performance.now() timeline; subtract our start time to match GetNowMs().
void AppUI::OnSnapshot(const SnapshotView& snapshot) {
    for (uint32_t i = 0; i < snapshot.n_table; ++i) {
        WireTableRecord rec;
        std::memcpy(&rec, snapshot.table + static_cast<size_t>(i) * sizeof(rec), sizeof(rec));
        rec.entry.last_rx_time_ms -= start_time_ms_;
        id_table_[rec.key] = rec.entry;
    }

    if (trace_paused_) return;
    for (uint32_t i = 0; i < snapshot.n_trace; ++i) {
        TraceEntry entry;
        std::memcpy(&entry, snapshot.trace + static_cast<size_t>(i) * sizeof(entry), sizeof(entry));
        entry.timestamp_sec -= start_time_ms_ / 1000.0;
        rx_trace_.push_back(entry);
    }
    while (rx_trace_.size() > 2000) {
        rx_trace_.pop_front();
    }
}

void AppUI::StartCanChannel(int dev) {
    if (!ValidDevice(dev)) return;
    DeviceSlot& d = devices_[dev];
    if (!d.used || !d.connected || d.channel_opening || d.channel_open) return;
    auto& usb = WebUsbBridge::GetInstance();

    if (d.protocol == PROTOCOL_PEAK_PCAN_FD) {
        // Command transfers (clock, options, bit timing, acceptance filter, bus on), written in order; the
        // channel is open when the last one is done (see OnControlOutDone)
        const auto transfers = CanProtocolPcan::BuildStartCommands(0, d.enable_fd, d.enable_listen_only, d.nom_bt, d.data_bt);
        for (size_t i = 0; i < transfers.size(); ++i) {
            usb.CommandOut(dev, kPcanCmdIdStart + static_cast<int>(i), transfers[i].data(), static_cast<int>(transfers[i].size()));
        }
        d.pcan_start_last_id = kPcanCmdIdStart + static_cast<int>(transfers.size()) - 1;
        usb.StartRxPump(dev, 0, 0);
        d.channel_opening = true;
        return;
    }

    uint32_t host_format = 0x0000BEEFU;
    usb.ControlTransferOut(dev, GS_USB_BREQ_HOST_FORMAT, 1, 0, reinterpret_cast<const uint8_t*>(&host_format), sizeof(host_format));
    usb.ControlTransferOut(dev, GS_USB_BREQ_BITTIMING, 0, 0, reinterpret_cast<const uint8_t*>(&d.nom_bt), sizeof(d.nom_bt));

    if (d.enable_fd) {
        usb.ControlTransferOut(dev, GS_USB_BREQ_DATA_BITTIMING, 0, 0, reinterpret_cast<const uint8_t*>(&d.data_bt), sizeof(d.data_bt));
    }

    GsDeviceMode mode_cmd = {};
    mode_cmd.mode = GS_CAN_MODE_START;
    mode_cmd.flags = 0;
    if (d.enable_fd)          mode_cmd.flags |= GS_CAN_FEATURE_FD;
    if (d.enable_loopback)    mode_cmd.flags |= GS_CAN_FEATURE_LOOP_BACK;
    if (d.enable_listen_only) mode_cmd.flags |= GS_CAN_FEATURE_LISTEN_ONLY;
    // Ask the adapter to append its microsecond timestamp to every frame: cycle times are then
    // computed from the adapter clock instead of the (jittery) USB/browser arrival time
    if (d.bt_const.feature & GS_CAN_FEATURE_HW_TIMESTAMP) mode_cmd.flags |= GS_CAN_FEATURE_HW_TIMESTAMP;

    usb.ControlTransferOut(dev, GS_USB_BREQ_MODE, 0, 0, reinterpret_cast<const uint8_t*>(&mode_cmd), sizeof(mode_cmd));

    usb.StartRxPump(dev, 0, 0);   // endpoint and transfer size as detected by the worker
    d.channel_opening = true;     // channel_open is set when MODE_START is acknowledged
}

void AppUI::StopCanChannel(int dev) {
    if (!ValidDevice(dev)) return;
    DeviceSlot& d = devices_[dev];
    auto& usb = WebUsbBridge::GetInstance();
    usb.StopRxPump(dev);

    if (d.protocol == PROTOCOL_PEAK_PCAN_FD) {
        const auto transfers = CanProtocolPcan::BuildStopCommands(0);
        for (size_t i = 0; i < transfers.size(); ++i) {
            usb.CommandOut(dev, kPcanCmdIdStop + static_cast<int>(i), transfers[i].data(), static_cast<int>(transfers[i].size()));
        }
    } else {
        GsDeviceMode mode_cmd = {GS_CAN_MODE_RESET, 0};
        usb.ControlTransferOut(dev, GS_USB_BREQ_MODE, 0, 0, reinterpret_cast<const uint8_t*>(&mode_cmd), sizeof(mode_cmd));
    }
    d.channel_opening = false;
    d.channel_open = false;
}

// Closes the adapter, forgets its slot and everything that referred to it
void AppUI::RemoveDevice(int dev) {
    if (!ValidDevice(dev) || !devices_[dev].used) return;
    if (devices_[dev].channel_open || devices_[dev].channel_opening) StopCanChannel(dev);
    WebUsbBridge::GetInstance().RemoveDevice(dev);
    devices_[dev] = DeviceSlot();

    for (PeriodicTxJob& job : periodic_jobs_) {
        if (job.target == dev) job.enabled = false;   // its adapter is gone: do not silently retarget
    }
    if (tx_target_ == dev) tx_target_ = kTargetAllDevices;
    if (trace_filter_dev_ == dev) trace_filter_dev_ = -1;
    if (table_filter_dev_ == dev) table_filter_dev_ = -1;

    if (selected_dev_ == dev) {
        selected_dev_ = -1;
        for (int i = 0; i < kMaxDevices; ++i) {
            if (devices_[i].used) { selected_dev_ = i; break; }
        }
    }
}

// ============================================================================
// Transmit
// ============================================================================

CanMessage AppUI::BuildComposerMessage() const {
    CanMessage msg;
    msg.id = static_cast<uint32_t>(tx_id_) & (tx_is_ext_ ? 0x1FFFFFFFU : 0x7FFU);
    msg.is_extended = tx_is_ext_;
    msg.is_fd = tx_is_fd_;
    msg.is_brs = tx_is_fd_ && tx_is_brs_;
    msg.is_rtr = !tx_is_fd_ && tx_is_rtr_;
    msg.is_tx_echo = false;

    int clamped_dlc = std::clamp(tx_dlc_, 0, tx_is_fd_ ? 15 : 8);
    msg.dlc = static_cast<uint8_t>(clamped_dlc);

    size_t target_len = msg.is_fd ? CanProtocolHandler::DlcToBytes(msg.dlc) : std::min<uint8_t>(msg.dlc, 8);
    if (!msg.is_rtr) {
        msg.payload = ParseHexPayload(tx_payload_, target_len);
        if (msg.payload.size() < target_len) {
            msg.payload.resize(target_len, 0x00);
        }
    }
    return msg;
}

void AppUI::LoadMessageIntoComposer(const CanMessage& msg, int target, int cycle_ms, const std::string& comment) {
    tx_id_ = static_cast<int>(msg.id);
    tx_is_ext_ = msg.is_extended;
    tx_is_fd_ = msg.is_fd;
    tx_is_brs_ = msg.is_brs;
    tx_is_rtr_ = msg.is_rtr;
    tx_dlc_ = msg.dlc;
    tx_cycle_ms_ = cycle_ms;
    tx_target_ = (ValidDevice(target) && devices_[target].used) ? target : kTargetAllDevices;

    std::string hex = FormatHexPayload(msg.payload);
    std::strncpy(tx_payload_, hex.c_str(), sizeof(tx_payload_) - 1);
    tx_payload_[sizeof(tx_payload_) - 1] = '\0';

    std::strncpy(tx_comment_, comment.c_str(), sizeof(tx_comment_) - 1);
    tx_comment_[sizeof(tx_comment_) - 1] = '\0';

    show_tx_window_ = true;
}

bool AppUI::TargetHasOpenChannel(int target) const {
    return (target == kTargetAllDevices) ? BusOnDeviceCount() > 0 : IsBusOn(target);
}

// Sends one frame to one adapter, or to every adapter that is BUS ON. Returns the number of adapters.
int AppUI::SendFrame(const CanMessage& msg, int target) {
    // The transfer format depends on the adapter's protocol: build it once per protocol in use
    std::vector<uint8_t> usb_bytes[2];
    bool built[2] = {false, false};
    int sent = 0;
    for (int dev = 0; dev < kMaxDevices; ++dev) {
        if (target != kTargetAllDevices && target != dev) continue;
        if (!IsBusOn(dev)) continue;
        const CanProtocolType protocol = devices_[dev].protocol;
        const int slot = (protocol == PROTOCOL_PEAK_PCAN_FD) ? 1 : 0;
        if (!built[slot]) {
            usb_bytes[slot] = CanProtocolHandler::BuildTx(protocol, msg, 0);
            built[slot] = true;
        }
        WebUsbBridge::GetInstance().TransferOut(dev, 0, usb_bytes[slot].data(), static_cast<int>(usb_bytes[slot].size()));
        ++sent;
    }
    return sent;
}

void AppUI::TransmitSingleMessage(const CanMessage& msg, int target, bool record_in_history, int associated_cycle_ms, const std::string& comment) {
    SendFrame(msg, target);

    if (!record_in_history) return;

    double now_sec = GetNowMs() / 1000.0;

    // Update existing entry if identical message to the same target is already in history
    for (auto& item : tx_history_) {
        if (item.target == target &&
            item.msg.id == msg.id &&
            item.msg.is_extended == msg.is_extended &&
            item.msg.is_fd == msg.is_fd &&
            item.msg.is_brs == msg.is_brs &&
            item.msg.is_rtr == msg.is_rtr &&
            item.msg.dlc == msg.dlc &&
            item.msg.payload == msg.payload)
        {
            item.sent_count++;
            item.last_sent_sec = now_sec;
            if (!comment.empty()) item.comment = comment;
            return;
        }
    }

    TxHistoryEntry new_entry;
    new_entry.msg = msg;
    new_entry.target = target;
    new_entry.cycle_time_ms = associated_cycle_ms;
    new_entry.sent_count = 1;
    new_entry.last_sent_sec = now_sec;
    new_entry.comment = comment;
    tx_history_.push_back(new_entry);
}

void AppUI::ProcessPeriodicJobs(double now_ms) {
    for (auto& job : periodic_jobs_) {
        if (!job.enabled || job.cycle_ms <= 0) continue;
        if (!TargetHasOpenChannel(job.target)) continue;

        if (now_ms >= job.next_send_ms) {
            TransmitSingleMessage(job.msg, job.target, false);
            job.tx_count++;

            if (job.auto_inc_byte_index >= 0 &&
                job.auto_inc_byte_index < static_cast<int>(job.msg.payload.size()))
            {
                job.msg.payload[job.auto_inc_byte_index]++;
            }

            if (now_ms - job.next_send_ms > job.cycle_ms * 5) {
                job.next_send_ms = now_ms + job.cycle_ms;
            } else {
                job.next_send_ms += job.cycle_ms;
            }
        }
    }
}

float AppUI::RenderMenuBar() {
    float bar_height = 0.0f;
    if (ImGui::BeginMainMenuBar()) {
        bar_height = ImGui::GetWindowSize().y;

        if (ImGui::BeginMenu("View Windows")) {
            ImGui::MenuItem("Auto-Tile & Resize Windows with Browser", nullptr, &auto_tile_layout_);
            ImGui::Separator();
            ImGui::MenuItem("1. Devices & Bit-Timing Setup", nullptr, &show_hw_window_);
            ImGui::MenuItem("2. Receive Table by ID (Grouped Monitor)", nullptr, &show_id_table_window_);
            ImGui::MenuItem("3. Receive Trace (Chronological Stream)", nullptr, &show_trace_window_);
            ImGui::MenuItem("4. Transmit Frame & Periodic Jobs", nullptr, &show_tx_window_);
            ImGui::MenuItem("5. Sent Messages History", nullptr, &show_history_window_);
            ImGui::EndMenu();
        }

        ImGui::Separator();
        ImGui::Checkbox("Auto-Tile Resize", &auto_tile_layout_);
        ImGui::Separator();

        const int connected = ConnectedDeviceCount();
        if (connected > 0) {
            ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.4f, 1.0f), "USB: %d device%s connected, %d BUS ON",
                               connected, connected == 1 ? "" : "s", BusOnDeviceCount());
            const UsbStats& us = WebUsbBridge::GetInstance().GetStats();
            ImGui::Separator();
            ImGui::TextDisabled("frames %u  trace drop %u  gaps %u",
                                us.frames_total, us.trace_dropped, us.snapshot_gaps);
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "USB: no device connected");
        }

        ImGui::EndMainMenuBar();
    }
    return bar_height;
}

// Helper for Bit-Timing configuration UI
static void RenderBittimingEditor(
    const char* label_prefix,
    CanProtocolType protocol,
    uint32_t fclk_hz,
    const CanBitTimingLimits& limits,
    GsDeviceBittiming& bt,
    int& target_kbps,
    float& target_sp)
{
    ImGui::PushID(label_prefix);

    ImGui::PushItemWidth(85);
    ImGui::InputInt("Bitrate (kbps)", &target_kbps, 50, 250);
    target_kbps = std::max(10, target_kbps);
    ImGui::SameLine();
    ImGui::SliderFloat("Sample Point", &target_sp, 50.0f, 95.0f, "%.1f%%");
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Button("Auto-Calc")) {
        CalculateTiming(protocol, fclk_hz, static_cast<uint32_t>(target_kbps) * 1000U, target_sp, limits, bt);
    }

    int brp  = static_cast<int>(bt.brp);
    int prop = static_cast<int>(bt.prop_seg);
    int seg1 = static_cast<int>(bt.phase_seg1);
    int seg2 = static_cast<int>(bt.phase_seg2);
    int sjw  = static_cast<int>(bt.sjw);

    // The fields are limited by the controller registers: prop_seg + phase_seg1 share TSEG1
    const int tseg1_max = static_cast<int>(limits.tseg1_max);
    ImGui::PushItemWidth(70);
    if (ImGui::InputInt("BRP", &brp, 1, 5))   bt.brp = std::clamp(brp, 1, static_cast<int>(limits.brp_max));
    ImGui::SameLine();
    if (ImGui::InputInt("Prop", &prop, 1, 2)) bt.prop_seg = std::clamp(prop, 0, std::max(0, std::min(256, tseg1_max - seg1)));
    ImGui::SameLine();
    if (ImGui::InputInt("Seg1", &seg1, 1, 2)) bt.phase_seg1 = std::clamp(seg1, 1, std::max(1, std::min(256, tseg1_max - prop)));
    ImGui::SameLine();
    if (ImGui::InputInt("Seg2", &seg2, 1, 2)) bt.phase_seg2 = std::clamp(seg2, 1, static_cast<int>(limits.tseg2_max));
    ImGui::SameLine();
    if (ImGui::InputInt("SJW", &sjw, 1, 1))   bt.sjw = std::clamp(sjw, 1, static_cast<int>(limits.sjw_max));
    ImGui::PopItemWidth();

    CanTimingMetrics m = CanProtocolHandler::ComputeTimingMetrics(fclk_hz, bt);
    ImGui::TextColored(ImVec4(0.4f, 0.9f, 1.0f, 1.0f),
        "1 tq: %.1f ns | Total: %u tq (TSEG1:%u, TSEG2:%u) | Actual: %.1f kbps | SP: %.1f%%",
        m.tq_ns, m.total_tq, m.tseg1_total, bt.phase_seg2, m.actual_bitrate / 1000.0, m.sample_point_pct);

    ImGui::PopID();
}

// ============================================================================
// Window 1: Devices (connection, adapter information) & Bit-Timing Setup
// ============================================================================
void AppUI::RenderHardwareSetupWindow(float x, float y, float w, float h) {
    ImGuiCond cond = auto_tile_layout_ ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
    ImGui::SetNextWindowPos(ImVec2(x, y), cond);
    ImGui::SetNextWindowSize(ImVec2(w, h), cond);

    if (!ImGui::Begin("1. Devices & Bit-Timing Setup", &show_hw_window_)) {
        ImGui::End();
        return;
    }

    auto& usb = WebUsbBridge::GetInstance();

    // --- connection controls ---
    if (ImGui::Button("Add Device", ImVec2(110, 26))) {
        usb.RequestDevice();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(ConnectedDeviceCount() == 0);
    if (ImGui::Button("Start All", ImVec2(85, 26))) {
        for (int dev = 0; dev < kMaxDevices; ++dev) StartCanChannel(dev);
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop All", ImVec2(85, 26))) {
        for (int dev = 0; dev < kMaxDevices; ++dev) {
            if (devices_[dev].channel_open || devices_[dev].channel_opening) StopCanChannel(dev);
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (!notice_.empty() && GetNowMs() - notice_time_ms_ < 15000.0) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", notice_.c_str());
    } else {
        ImGui::TextDisabled("up to %d adapters", kMaxDevices);
    }

    // --- device list ---
    int used_count = 0;
    for (const DeviceSlot& d : devices_) {
        if (d.used) ++used_count;
    }

    if (used_count == 0) {
        ImGui::Separator();
        ImGui::TextDisabled("No devices yet. Press \"Add Device\" and pick an adapter in the browser dialog.");
    } else {
        const float row_h = ImGui::GetFrameHeightWithSpacing();
        const float list_h = row_h * static_cast<float>(std::min(used_count, 4) + 1) + 6.0f;
        const ImGuiTableFlags list_flags =
            ImGuiTableFlags_Borders |
            ImGuiTableFlags_RowBg |
            ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_Resizable |
            ImGuiTableFlags_SizingStretchProp;

        // VID:PID, USB rate and counters are in the information pane below; the list keeps what fits
        // next to each other in a narrow window: number, adapter, serial, state, actions
        if (ImGui::BeginTable("DeviceList", 5, list_flags, ImVec2(0.0f, list_h))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("#",       ImGuiTableColumnFlags_WidthFixed,   26.0f);
            ImGui::TableSetupColumn("Adapter", ImGuiTableColumnFlags_WidthStretch, 1.3f);
            ImGui::TableSetupColumn("Serial",  ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("State",   ImGuiTableColumnFlags_WidthFixed,   128.0f);
            ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed,   190.0f);
            ImGui::TableHeadersRow();

            int remove_dev = -1;
            for (int dev = 0; dev < kMaxDevices; ++dev) {
                DeviceSlot& d = devices_[dev];
                if (!d.used) continue;

                ImGui::PushID(dev);
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                char num[16];
                std::snprintf(num, sizeof(num), "%d", dev + 1);
                ImGui::PushStyleColor(ImGuiCol_Text, DeviceColor(dev));
                if (ImGui::Selectable(num, selected_dev_ == dev,
                                      ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
                    selected_dev_ = dev;
                }
                ImGui::PopStyleColor();

                ImGui::TableSetColumnIndex(1);
                if (d.info.product.empty()) {
                    ImGui::TextDisabled("(unnamed adapter)");
                } else {
                    ImGui::TextUnformatted(d.info.product.c_str());
                }

                ImGui::TableSetColumnIndex(2);
                if (d.info.serial.empty()) ImGui::TextDisabled("-");
                else ImGui::TextUnformatted(d.info.serial.c_str());

                ImGui::TableSetColumnIndex(3);
                ImVec4 state_color;
                const char* state_text = DeviceStateText(d, state_color);
                ImGui::TextColored(state_color, "%s", state_text);
                if (ImGui::IsItemHovered()) {
                    const UsbDeviceStats& ds = usb.GetDeviceStats(dev);
                    ImGui::SetTooltip("%04X:%04X  %s\nUSB %u transfers/s, %u frames%s%s",
                                      d.info.vid, d.info.pid, d.info.manufacturer.c_str(),
                                      d.connected ? ds.rx_per_sec : 0u, d.connected ? ds.frames : 0u,
                                      d.status.empty() ? "" : "\nLast event: ", d.status.c_str());
                }

                ImGui::TableSetColumnIndex(4);
                if (d.connecting) {
                    ImGui::BeginDisabled();
                    ImGui::SmallButton("Connecting");
                    ImGui::EndDisabled();
                } else if (!d.connected) {
                    if (ImGui::SmallButton("Connect")) usb.ConnectDevice(dev);
                } else {
                    if (ImGui::SmallButton("Disconnect")) {
                        if (d.channel_open || d.channel_opening) StopCanChannel(dev);
                        usb.DisconnectDevice(dev);
                    }
                    ImGui::SameLine();
                    if (d.channel_open) {
                        if (ImGui::SmallButton("Stop")) StopCanChannel(dev);
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stop the CAN channel (BUS OFF)");
                    } else {
                        ImGui::BeginDisabled(d.channel_opening);
                        if (ImGui::SmallButton("Start")) StartCanChannel(dev);
                        ImGui::EndDisabled();
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Start the CAN channel (BUS ON)");
                    }
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Remove")) remove_dev = dev;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Disconnect and forget this adapter");

                ImGui::PopID();
            }
            ImGui::EndTable();

            if (remove_dev >= 0) RemoveDevice(remove_dev);
        }
    }

    // --- details and settings of the selected adapter ---
    if (selected_dev_ < 0 || !ValidDevice(selected_dev_) || !devices_[selected_dev_].used) {
        ImGui::End();
        return;
    }

    const int dev = selected_dev_;
    DeviceSlot& d = devices_[dev];
    const UsbDeviceStats& ds = usb.GetDeviceStats(dev);

    // Collapsible so that the settings below stay reachable in a short window; "###" keeps the open/closed
    // state when another adapter is selected
    char title[64];
    std::snprintf(title, sizeof(title), "Device %d information###devinfo", dev + 1);

    if (ImGui::CollapsingHeader(title, ImGuiTreeNodeFlags_DefaultOpen) &&
        ImGui::BeginTable("DeviceInfo", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("k1", ImGuiTableColumnFlags_WidthFixed,   85.0f);
        ImGui::TableSetupColumn("v1", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("k2", ImGuiTableColumnFlags_WidthFixed,   85.0f);
        ImGui::TableSetupColumn("v2", ImGuiTableColumnFlags_WidthStretch, 1.0f);

        auto cell = [](const char* text) {
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(text);
        };
        auto label_cell = [](const char* text) {
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", text);
        };
        auto row = [&](const char* k1, const std::string& v1, const char* k2, const std::string& v2) {
            ImGui::TableNextRow();
            label_cell(k1);
            cell(v1.empty() ? "-" : v1.c_str());
            label_cell(k2);
            cell(v2.empty() ? "-" : v2.c_str());
        };
        auto fmt = [](const char* f, auto... args) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), f, args...);
            return std::string(buf);
        };

        ImVec4 state_color;
        const std::string state_text = DeviceStateText(d, state_color);
        ImGui::TableNextRow();
        label_cell("State");
        ImGui::TableNextColumn();
        ImGui::TextColored(state_color, "%s", state_text.c_str());
        label_cell("Last event");
        cell(d.status.empty() ? "-" : d.status.c_str());

        const bool pcan = d.protocol == PROTOCOL_PEAK_PCAN_FD;
        row("Protocol", d.connected ? std::string(CanProtocolName(d.protocol)) : std::string(),
            "Bootloader", d.pcan_bl);
        row("Adapter",  d.info.product,      "Manufacturer", d.info.manufacturer);
        row("Serial",   d.info.serial.empty() ? d.pcan_serial : d.info.serial,
            "VID:PID",  fmt("%04X:%04X", d.info.vid, d.info.pid));
        row("USB",      d.info.usb_version,  "Device rev.",  d.info.device_version);
        row("Firmware", pcan ? d.pcan_fw : (d.has_config ? fmt("v%u", d.config.sw_version) : std::string()),
            "Hardware", pcan ? (d.pcan_hw.empty() ? std::string() : "PCB " + d.pcan_hw + (d.pcan_mcu.empty() ? "" : ", MCU " + d.pcan_mcu))
                             : (d.has_config ? fmt("v%u", d.config.hw_version) : std::string()));
        row("CAN clock", d.has_bt_const ? fmt("%.1f MHz", d.bt_const.fclk_can / 1e6) : std::string("(not read yet)"),
            "CAN channels", pcan ? d.pcan_channels : (d.has_config ? fmt("%u", static_cast<unsigned>(d.config.icount) + 1u) : std::string()));
        row("Endpoints", d.endpoints, "Features", d.has_bt_const ? FeatureList(d.bt_const.feature) : std::string());
        row("USB/s",    d.connected ? fmt("%u transfers/s", ds.rx_per_sec) : std::string(),
            "Frames",   d.connected ? fmt("%u", ds.frames) : std::string());
        row("Errors",   d.connected ? fmt("RX %u, TX %u", ds.rx_errors, ds.tx_errors) : std::string(),
            "TX drop / recov.", d.connected ? fmt("%u / %u", ds.tx_dropped, ds.recoveries) : std::string());

        ImGui::EndTable();
    }

    // --- bit timing and mode of the selected adapter ---
    std::snprintf(title, sizeof(title), "Device %d bit timing and mode###devcfg", dev + 1);
    if (ImGui::CollapsingHeader(title, ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::BeginDisabled(d.channel_open || d.channel_opening);

        // A PEAK adapter runs its CAN cores from a fixed 80 MHz domain and has no internal loopback mode
        const bool fixed_clock = d.protocol == PROTOCOL_PEAK_PCAN_FD;
        int clk_mhz = static_cast<int>(d.bt_const.fclk_can / 1000000U);
        ImGui::BeginDisabled(fixed_clock);
        ImGui::PushItemWidth(80);
        if (ImGui::InputInt("CAN Clock (MHz)", &clk_mhz, 1, 10)) {
            d.bt_const.fclk_can = static_cast<uint32_t>(std::clamp(clk_mhz, 1, 400)) * 1000000U;
        }
        ImGui::PopItemWidth();
        ImGui::EndDisabled();
        if (fixed_clock && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Fixed by the adapter");
        ImGui::SameLine();
        ImGui::Checkbox("CAN FD", &d.enable_fd);
        ImGui::SameLine();
        ImGui::BeginDisabled(fixed_clock);
        ImGui::Checkbox("Loopback", &d.enable_loopback);
        ImGui::EndDisabled();
        if (fixed_clock && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Not available for this adapter");
        ImGui::SameLine();
        ImGui::Checkbox("Listen-Only", &d.enable_listen_only);

        ImGui::SeparatorText("Nominal Arbitration Timing");
        RenderBittimingEditor("Nom", d.protocol, d.bt_const.fclk_can, NominalLimitsOf(d), d.nom_bt, d.target_nom_kbps, d.target_nom_sp);

        if (d.enable_fd) {
            ImGui::SeparatorText("CAN FD Data Phase Timing");
            RenderBittimingEditor("Data", d.protocol, d.bt_const.fclk_can, DataLimitsOf(d), d.data_bt, d.target_data_kbps, d.target_data_sp);
        }

        ImGui::EndDisabled();
    }

    ImGui::End();
}

// ============================================================================
// Window 2: Receive Monitor Table Grouped by ID (Indirect Sorting via Pointers)
// ============================================================================
void AppUI::RenderIdTableWindow(double now_ms, float x, float y, float w, float h) {
    ImGuiCond cond = auto_tile_layout_ ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
    ImGui::SetNextWindowPos(ImVec2(x, y), cond);
    ImGui::SetNextWindowSize(ImVec2(w, h), cond);

    if (!ImGui::Begin("2. Receive Table by ID (PCAN-View Monitor)", &show_id_table_window_)) {
        ImGui::End();
        return;
    }

    ImGui::Text("Unique CAN IDs: %zu", id_table_.size());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click column headers to sort");
    ImGui::SameLine();
    ImGui::TextDisabled("Device:");
    ImGui::SameLine();
    DeviceCombo("##tablefilter", table_filter_dev_, "All devices", 190.0f);
    ImGui::SameLine(ImGui::GetWindowWidth() - 110);
    if (ImGui::SmallButton("Reset Table")) {
        id_table_.clear();
        WebUsbBridge::GetInstance().ClearIdTable();
    }

    const ImGuiTableFlags flags =
        ImGuiTableFlags_Borders |
        ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_Resizable |
        ImGuiTableFlags_Sortable |
        ImGuiTableFlags_SizingStretchProp;

    if (ImGui::BeginTable("IdMonitorTable", 7, flags)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Dev",             ImGuiTableColumnFlags_WidthFixed,                                     40.0f,  ID_COL_DEV);
        ImGui::TableSetupColumn("CAN ID",          ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthFixed, 90.0f,  ID_COL_CAN_ID);
        ImGui::TableSetupColumn("Type",            ImGuiTableColumnFlags_WidthFixed,                                     95.0f,  ID_COL_TYPE);
        ImGui::TableSetupColumn("Length",          ImGuiTableColumnFlags_WidthFixed,                                     65.0f,  ID_COL_LENGTH);
        ImGui::TableSetupColumn("Count",           ImGuiTableColumnFlags_WidthFixed,                                     65.0f,  ID_COL_COUNT);
        ImGui::TableSetupColumn("Cycle Time (ms)", ImGuiTableColumnFlags_WidthFixed,                                     110.0f, ID_COL_CYCLE);
        ImGui::TableSetupColumn("Data (HEX)",      ImGuiTableColumnFlags_WidthStretch,                                   0.0f,   ID_COL_DATA);
        ImGui::TableHeadersRow();

        // Build indirect pointer view over std::map entries to sort without mutating map keys or copying payloads
        std::vector<const IdTableEntry*> sorted_rows;
        sorted_rows.reserve(id_table_.size());
        for (const auto& kv : id_table_) {
            if (table_filter_dev_ >= 0 && kv.second.last_msg.dev != table_filter_dev_) continue;
            sorted_rows.push_back(&kv.second);
        }

        if (ImGuiTableSortSpecs* sort_specs = ImGui::TableGetSortSpecs()) {
            if (sort_specs->SpecsCount > 0 && sorted_rows.size() > 1) {
                const ImGuiTableColumnSortSpecs& spec = sort_specs->Specs[0];
                const bool ascending = (spec.SortDirection == ImGuiSortDirection_Ascending);

                std::sort(sorted_rows.begin(), sorted_rows.end(), [&](const IdTableEntry* a, const IdTableEntry* b) {
                    int cmp = 0;
                    switch (spec.ColumnUserID) {
                        case ID_COL_DEV:
                            cmp = ThreeWayCompare(a->last_msg.dev, b->last_msg.dev);
                            break;
                        case ID_COL_CAN_ID:
                            cmp = ThreeWayCompare(a->last_msg.id, b->last_msg.id);
                            break;
                        case ID_COL_TYPE:
                            cmp = ThreeWayCompare(GetFrameFormatRank(a->last_msg), GetFrameFormatRank(b->last_msg));
                            break;
                        case ID_COL_LENGTH:
                            cmp = ThreeWayCompare(a->last_msg.dlc, b->last_msg.dlc);
                            break;
                        case ID_COL_COUNT:
                            cmp = ThreeWayCompare(a->count, b->count);
                            break;
                        case ID_COL_CYCLE:
                            cmp = ThreeWayCompare(a->cycle_time_ms, b->cycle_time_ms);
                            break;
                        case ID_COL_DATA:
                            cmp = ComparePayloads(a->last_msg.payload, b->last_msg.payload);
                            break;
                        default:
                            break;
                    }
                    // Fallback tie-breakers: CAN ID, then device
                    if (cmp == 0) {
                        cmp = ThreeWayCompare(a->last_msg.id, b->last_msg.id);
                    }
                    if (cmp == 0) {
                        cmp = ThreeWayCompare(a->last_msg.dev, b->last_msg.dev);
                    }
                    return ascending ? (cmp < 0) : (cmp > 0);
                });
            }
            sort_specs->SpecsDirty = false;
        }

        for (const IdTableEntry* entry_ptr : sorted_rows) {
            const IdTableEntry& entry = *entry_ptr;
            const CanMessage& m = entry.last_msg;

            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(DeviceColor(m.dev), "%d", m.dev + 1);
            if (ImGui::IsItemHovered() && ValidDevice(m.dev) && devices_[m.dev].used) {
                ImGui::SetTooltip("%s", DeviceLabel(m.dev).c_str());
            }

            ImGui::TableSetColumnIndex(1);
            ImGui::Text(m.is_extended ? "0x%08X" : "0x%03X", m.id);

            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%s %s%s%s",
                        m.is_extended ? "EXT" : "STD",
                        m.is_fd ? "FD" : "2.0",
                        m.is_brs ? " BRS" : "",
                        m.is_rtr ? " RTR" : "");

            ImGui::TableSetColumnIndex(3);
            uint8_t byte_len = m.is_fd ? CanProtocolHandler::DlcToBytes(m.dlc) : std::min<uint8_t>(m.dlc, 8);
            ImGui::Text("%u (%uB)", m.dlc, byte_len);

            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%llu", static_cast<unsigned long long>(entry.count));

            ImGui::TableSetColumnIndex(5);
            if (entry.count > 1) {
                ImGui::Text("%.1f ms", entry.cycle_time_ms);
            } else {
                ImGui::TextDisabled("---");
            }

            ImGui::TableSetColumnIndex(6);
            if (m.is_rtr) {
                ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.3f, 1.0f), "REMOTE REQUEST");
            } else {
                bool recent_rx = (now_ms - entry.last_rx_time_ms) < 500.0;
                for (size_t i = 0; i < m.payload.size(); ++i) {
                    bool byte_changed = (entry.count > 1) &&
                                        (i < entry.prev_payload.size()) &&
                                        (m.payload[i] != entry.prev_payload[i]);

                    if (byte_changed && recent_rx) {
                        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%02X", m.payload[i]);
                    } else {
                        ImGui::Text("%02X", m.payload[i]);
                    }
                    if (i + 1 < m.payload.size()) {
                        ImGui::SameLine(0.0f, 4.0f);
                    }
                }
            }
        }
        ImGui::EndTable();
    }

    ImGui::End();
}

// ============================================================================
// Window 3: Chronological Receive Trace (Indirect Sorting via Pointers)
// ============================================================================
void AppUI::RenderTraceWindow(float x, float y, float w, float h) {
    ImGuiCond cond = auto_tile_layout_ ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
    ImGui::SetNextWindowPos(ImVec2(x, y), cond);
    ImGui::SetNextWindowSize(ImVec2(w, h), cond);

    if (!ImGui::Begin("3. Received Messages Trace (Chronological)", &show_trace_window_)) {
        ImGui::End();
        return;
    }

    if (ImGui::Checkbox("Pause Trace", &trace_paused_)) {
        WebUsbBridge::GetInstance().SetTraceEnabled(!trace_paused_);
    }
    ImGui::SameLine();
    ImGui::Checkbox("Auto-Scroll", &trace_autoscroll_);
    ImGui::SameLine(ImGui::GetWindowWidth() - 105);
    if (ImGui::SmallButton("Clear Trace")) {
        rx_trace_.clear();
        WebUsbBridge::GetInstance().ClearTrace();
    }

    ImGui::PushItemWidth(90);
    ImGui::InputInt("Filter ID (Hex, -1=All)", &trace_filter_id_, 1, 16);
    ImGui::PopItemWidth();
    ImGui::SameLine();
    ImGui::TextDisabled("Device:");
    ImGui::SameLine();
    DeviceCombo("##tracefilter", trace_filter_dev_, "All devices", 190.0f);

    const ImGuiTableFlags flags =
        ImGuiTableFlags_Borders |
        ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_Sortable |
        ImGuiTableFlags_SizingStretchProp;

    if (ImGui::BeginTable("TraceStreamTable", 7, flags)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Time (s)",   ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthFixed, 75.0f, TRACE_COL_TIME);
        ImGui::TableSetupColumn("Dev",        ImGuiTableColumnFlags_WidthFixed,                                     40.0f, TRACE_COL_DEV);
        ImGui::TableSetupColumn("Dir",        ImGuiTableColumnFlags_WidthFixed,                                     55.0f, TRACE_COL_DIR);
        ImGui::TableSetupColumn("CAN ID",     ImGuiTableColumnFlags_WidthFixed,                                     85.0f, TRACE_COL_CAN_ID);
        ImGui::TableSetupColumn("Format",     ImGuiTableColumnFlags_WidthFixed,                                     85.0f, TRACE_COL_FORMAT);
        ImGui::TableSetupColumn("DLC",        ImGuiTableColumnFlags_WidthFixed,                                     45.0f, TRACE_COL_DLC);
        ImGui::TableSetupColumn("Data (HEX)", ImGuiTableColumnFlags_WidthStretch,                                   0.0f,  TRACE_COL_DATA);
        ImGui::TableHeadersRow();

        // Filter and collect pointers into std::deque without disturbing chronological ring buffer order
        std::vector<const TraceEntry*> visible_trace;
        visible_trace.reserve(rx_trace_.size());
        for (const auto& item : rx_trace_) {
            if (trace_filter_id_ >= 0 && static_cast<int>(item.msg.id) != trace_filter_id_) {
                continue;
            }
            if (trace_filter_dev_ >= 0 && item.msg.dev != trace_filter_dev_) {
                continue;
            }
            visible_trace.push_back(&item);
        }

        if (ImGuiTableSortSpecs* sort_specs = ImGui::TableGetSortSpecs()) {
            if (sort_specs->SpecsCount > 0 && visible_trace.size() > 1) {
                const ImGuiTableColumnSortSpecs& spec = sort_specs->Specs[0];
                const bool ascending = (spec.SortDirection == ImGuiSortDirection_Ascending);

                // Optimization:rx_trace_ is naturally sorted by timestamp ascending;
                // only run std::sort if another column or descending order is requested
                bool is_default_time_asc = (spec.ColumnUserID == TRACE_COL_TIME && ascending);
                if (!is_default_time_asc) {
                    std::sort(visible_trace.begin(), visible_trace.end(), [&](const TraceEntry* a, const TraceEntry* b) {
                        int cmp = 0;
                        switch (spec.ColumnUserID) {
                            case TRACE_COL_TIME:
                                cmp = ThreeWayCompare(a->timestamp_sec, b->timestamp_sec);
                                break;
                            case TRACE_COL_DEV:
                                cmp = ThreeWayCompare(a->msg.dev, b->msg.dev);
                                break;
                            case TRACE_COL_DIR:
                                cmp = ThreeWayCompare(a->msg.is_tx_echo ? 1 : 0, b->msg.is_tx_echo ? 1 : 0);
                                break;
                            case TRACE_COL_CAN_ID:
                                cmp = ThreeWayCompare(a->msg.id, b->msg.id);
                                break;
                            case TRACE_COL_FORMAT:
                                cmp = ThreeWayCompare(GetFrameFormatRank(a->msg), GetFrameFormatRank(b->msg));
                                break;
                            case TRACE_COL_DLC:
                                cmp = ThreeWayCompare(a->msg.dlc, b->msg.dlc);
                                break;
                            case TRACE_COL_DATA:
                                cmp = ComparePayloads(a->msg.payload, b->msg.payload);
                                break;
                            default:
                                break;
                        }
                        if (cmp == 0) {
                            cmp = ThreeWayCompare(a->timestamp_sec, b->timestamp_sec);
                        }
                        return ascending ? (cmp < 0) : (cmp > 0);
                    });
                }
            }
            sort_specs->SpecsDirty = false;
        }

        for (const TraceEntry* item_ptr : visible_trace) {
            const TraceEntry& item = *item_ptr;
            const CanMessage& m = item.msg;

            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%.3f", item.timestamp_sec);

            ImGui::TableSetColumnIndex(1);
            ImGui::TextColored(DeviceColor(m.dev), "%d", m.dev + 1);
            if (ImGui::IsItemHovered() && ValidDevice(m.dev) && devices_[m.dev].used) {
                ImGui::SetTooltip("%s", DeviceLabel(m.dev).c_str());
            }

            ImGui::TableSetColumnIndex(2);
            if (m.is_tx_echo) {
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "TX");
            } else {
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 1.0f, 1.0f), "RX");
            }

            ImGui::TableSetColumnIndex(3);
            ImGui::Text(m.is_extended ? "0x%08X" : "0x%03X", m.id);

            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%s %s%s",
                        m.is_extended ? "EXT" : "STD",
                        m.is_fd ? "FD" : "2.0",
                        m.is_brs ? " BRS" : "");

            ImGui::TableSetColumnIndex(5);
            ImGui::Text("%u", m.dlc);

            ImGui::TableSetColumnIndex(6);
            if (m.is_rtr) {
                ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.3f, 1.0f), "RTR");
            } else {
                std::string hex = FormatHexPayload(m.payload);
                ImGui::TextUnformatted(hex.c_str());
            }
        }

        if (trace_autoscroll_ && !trace_paused_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
            ImGui::SetScrollHereY(1.0f);
        }

        ImGui::EndTable();
    }

    ImGui::End();
}

// ============================================================================
// Window 4: Dedicated Transmit Window & Active Periodic Jobs (Sortable)
// ============================================================================
void AppUI::RenderTransmitWindow(double now_ms, float x, float y, float w, float h) {
    ImGuiCond cond = auto_tile_layout_ ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
    ImGui::SetNextWindowPos(ImVec2(x, y), cond);
    ImGui::SetNextWindowSize(ImVec2(w, h), cond);

    if (!ImGui::Begin("4. Transmit Frame Composer & Periodic Jobs", &show_tx_window_)) {
        ImGui::End();
        return;
    }

    DeviceCombo("Send via##tx", tx_target_, "All devices with BUS ON", std::max(220.0f, w * 0.40f));
    if (!TargetHasOpenChannel(tx_target_)) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f),
                           tx_target_ == kTargetAllDevices ? "no device is BUS ON: frames are not sent"
                                                           : "this device is not BUS ON: frames are not sent");
    }

    ImGui::PushItemWidth(90);
    ImGui::InputInt("ID (Hex)", &tx_id_, 1, 16, ImGuiInputTextFlags_CharsHexadecimal);
    ImGui::PopItemWidth();
    ImGui::SameLine();
    ImGui::Checkbox("EXT", &tx_is_ext_);
    ImGui::SameLine();
    ImGui::Checkbox("FD", &tx_is_fd_);
    ImGui::SameLine();
    ImGui::BeginDisabled(!tx_is_fd_);
    ImGui::Checkbox("BRS", &tx_is_brs_);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(tx_is_fd_);
    ImGui::Checkbox("RTR", &tx_is_rtr_);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::PushItemWidth(60);
    int max_dlc = tx_is_fd_ ? 15 : 8;
    if (ImGui::SliderInt("DLC", &tx_dlc_, 0, max_dlc)) {
        tx_dlc_ = std::clamp(tx_dlc_, 0, max_dlc);
    }
    ImGui::PopItemWidth();

    uint8_t expected_bytes = tx_is_fd_ ? CanProtocolHandler::DlcToBytes(tx_dlc_) : std::min(tx_dlc_, 8);
    char payload_label[64];
    std::snprintf(payload_label, sizeof(payload_label), "Data (%u B)", expected_bytes);

    ImGui::PushItemWidth(w * 0.55f);
    ImGui::InputText(payload_label, tx_payload_, sizeof(tx_payload_));
    ImGui::PopItemWidth();
    ImGui::SameLine();
    ImGui::PushItemWidth(w * 0.20f);
    ImGui::InputText("Comment", tx_comment_, sizeof(tx_comment_));
    ImGui::PopItemWidth();

    ImGui::PushItemWidth(85);
    ImGui::InputInt("Cycle (ms)", &tx_cycle_ms_, 10, 100);
    tx_cycle_ms_ = std::max(0, tx_cycle_ms_);
    ImGui::SameLine();
    ImGui::InputInt("Burst N", &tx_burst_count_, 1, 10);
    tx_burst_count_ = std::clamp(tx_burst_count_, 1, 1000);
    ImGui::SameLine();
    ImGui::InputInt("Auto-Inc Byte", &tx_auto_inc_byte_, 1, 1);
    tx_auto_inc_byte_ = std::clamp(tx_auto_inc_byte_, -1, 63);
    ImGui::PopItemWidth();

    if (ImGui::Button("Single Send / Burst", ImVec2(165, 26))) {
        CanMessage msg = BuildComposerMessage();
        for (int i = 0; i < tx_burst_count_; ++i) {
            TransmitSingleMessage(msg, tx_target_, (i == 0), tx_cycle_ms_, tx_comment_);
            if (tx_auto_inc_byte_ >= 0 && tx_auto_inc_byte_ < static_cast<int>(msg.payload.size())) {
                msg.payload[tx_auto_inc_byte_]++;
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Add to Periodic Transmit List", ImVec2(215, 26))) {
        PeriodicTxJob job;
        job.enabled = true;
        job.msg = BuildComposerMessage();
        job.target = tx_target_;
        job.cycle_ms = (tx_cycle_ms_ > 0) ? tx_cycle_ms_ : 100;
        job.auto_inc_byte_index = tx_auto_inc_byte_;
        job.next_send_ms = now_ms + job.cycle_ms;
        job.tx_count = 0;
        job.comment = tx_comment_;
        periodic_jobs_.push_back(job);

        TransmitSingleMessage(job.msg, job.target, true, job.cycle_ms, job.comment);
    }

    ImGui::SeparatorText("Active Periodic Transmit Jobs");

    const ImGuiTableFlags flags =
        ImGuiTableFlags_Borders |
        ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_Sortable |
        ImGuiTableFlags_SizingStretchProp;

    if (ImGui::BeginTable("PeriodicJobsTable", 7, flags)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("On",         ImGuiTableColumnFlags_WidthFixed,                                     30.0f,  JOB_COL_ENABLED);
        ImGui::TableSetupColumn("Dev",        ImGuiTableColumnFlags_WidthFixed,                                     40.0f,  JOB_COL_DEV);
        ImGui::TableSetupColumn("ID",         ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthFixed, 75.0f,  JOB_COL_CAN_ID);
        ImGui::TableSetupColumn("Cycle",      ImGuiTableColumnFlags_WidthFixed,                                     65.0f,  JOB_COL_CYCLE);
        ImGui::TableSetupColumn("Sent",       ImGuiTableColumnFlags_WidthFixed,                                     55.0f,  JOB_COL_SENT);
        ImGui::TableSetupColumn("Data (HEX)", ImGuiTableColumnFlags_WidthStretch,                                   0.0f,   JOB_COL_DATA);
        ImGui::TableSetupColumn("Actions",    ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_WidthFixed,      105.0f, JOB_COL_ACTIONS);
        ImGui::TableHeadersRow();

        // Indirect index sorting over periodic_jobs_ so "Del" button indices remain valid
        std::vector<size_t> job_indices(periodic_jobs_.size());
        for (size_t i = 0; i < periodic_jobs_.size(); ++i) {
            job_indices[i] = i;
        }

        if (ImGuiTableSortSpecs* sort_specs = ImGui::TableGetSortSpecs()) {
            if (sort_specs->SpecsCount > 0 && job_indices.size() > 1) {
                const ImGuiTableColumnSortSpecs& spec = sort_specs->Specs[0];
                const bool ascending = (spec.SortDirection == ImGuiSortDirection_Ascending);

                std::sort(job_indices.begin(), job_indices.end(), [&](size_t ia, size_t ib) {
                    const PeriodicTxJob& a = periodic_jobs_[ia];
                    const PeriodicTxJob& b = periodic_jobs_[ib];
                    int cmp = 0;
                    switch (spec.ColumnUserID) {
                        case JOB_COL_ENABLED:
                            cmp = ThreeWayCompare(a.enabled ? 1 : 0, b.enabled ? 1 : 0);
                            break;
                        case JOB_COL_DEV:
                            cmp = ThreeWayCompare(a.target, b.target);
                            break;
                        case JOB_COL_CAN_ID:
                            cmp = ThreeWayCompare(a.msg.id, b.msg.id);
                            break;
                        case JOB_COL_CYCLE:
                            cmp = ThreeWayCompare(a.cycle_ms, b.cycle_ms);
                            break;
                        case JOB_COL_SENT:
                            cmp = ThreeWayCompare(a.tx_count, b.tx_count);
                            break;
                        case JOB_COL_DATA:
                            cmp = ComparePayloads(a.msg.payload, b.msg.payload);
                            break;
                        default:
                            break;
                    }
                    if (cmp == 0) cmp = ThreeWayCompare(ia, ib);
                    return ascending ? (cmp < 0) : (cmp > 0);
                });
            }
            sort_specs->SpecsDirty = false;
        }

        int delete_idx = -1;
        for (size_t real_idx : job_indices) {
            PeriodicTxJob& job = periodic_jobs_[real_idx];
            ImGui::PushID(static_cast<int>(real_idx));
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::Checkbox("##en", &job.enabled);

            ImGui::TableSetColumnIndex(1);
            if (job.target == kTargetAllDevices) {
                ImGui::TextDisabled("All");
            } else {
                ImGui::TextColored(DeviceColor(job.target), "%d", job.target + 1);
            }

            ImGui::TableSetColumnIndex(2);
            ImGui::Text(job.msg.is_extended ? "0x%08X" : "0x%03X", job.msg.id);

            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%d ms", job.cycle_ms);

            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%llu", static_cast<unsigned long long>(job.tx_count));

            ImGui::TableSetColumnIndex(5);
            std::string hex = FormatHexPayload(job.msg.payload);
            ImGui::TextUnformatted(hex.c_str());

            ImGui::TableSetColumnIndex(6);
            if (ImGui::SmallButton("Shot")) {
                TransmitSingleMessage(job.msg, job.target, false);
                job.tx_count++;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Del")) {
                delete_idx = static_cast<int>(real_idx);
            }

            ImGui::PopID();
        }

        if (delete_idx >= 0) {
            periodic_jobs_.erase(periodic_jobs_.begin() + delete_idx);
        }

        ImGui::EndTable();
    }

    ImGui::End();
}

// ============================================================================
// Window 5: Sent Messages History (Indirect Index Sorting)
// ============================================================================
void AppUI::RenderSentHistoryWindow(float x, float y, float w, float h) {
    ImGuiCond cond = auto_tile_layout_ ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
    ImGui::SetNextWindowPos(ImVec2(x, y), cond);
    ImGui::SetNextWindowSize(ImVec2(w, h), cond);

    if (!ImGui::Begin("5. Sent Messages History (Select & Resend)", &show_history_window_)) {
        ImGui::End();
        return;
    }

    ImGui::Text("History Entries: %zu (Click column headers to sort, 'Load' to edit, or 'Resend' to transmit)", tx_history_.size());
    ImGui::SameLine(ImGui::GetWindowWidth() - 120);
    if (ImGui::SmallButton("Clear History")) {
        tx_history_.clear();
        selected_history_index_ = -1;
    }

    const ImGuiTableFlags flags =
        ImGuiTableFlags_Borders |
        ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_Sortable |
        ImGuiTableFlags_SizingStretchProp;

    if (ImGui::BeginTable("SentHistoryTable", 8, flags)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Last Sent",            ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending | ImGuiTableColumnFlags_WidthFixed, 75.0f,  HIST_COL_LAST_SENT);
        ImGui::TableSetupColumn("Dev",                  ImGuiTableColumnFlags_WidthFixed,                                                                                  40.0f,  HIST_COL_DEV);
        ImGui::TableSetupColumn("CAN ID",               ImGuiTableColumnFlags_WidthFixed,                                                                                  85.0f,  HIST_COL_CAN_ID);
        ImGui::TableSetupColumn("Format",               ImGuiTableColumnFlags_WidthFixed,                                                                                  90.0f,  HIST_COL_FORMAT);
        ImGui::TableSetupColumn("DLC",                  ImGuiTableColumnFlags_WidthFixed,                                                                                  45.0f,  HIST_COL_DLC);
        ImGui::TableSetupColumn("Count",                ImGuiTableColumnFlags_WidthFixed,                                                                                  55.0f,  HIST_COL_COUNT);
        ImGui::TableSetupColumn("Data (HEX) & Comment", ImGuiTableColumnFlags_WidthStretch,                                                                                0.0f,   HIST_COL_DATA);
        ImGui::TableSetupColumn("Operations",           ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_WidthFixed,                                                   130.0f, HIST_COL_OPS);
        ImGui::TableHeadersRow();

        // Indirect index sorting over tx_history_
        std::vector<size_t> hist_indices(tx_history_.size());
        for (size_t i = 0; i < tx_history_.size(); ++i) {
            hist_indices[i] = i;
        }

        if (ImGuiTableSortSpecs* sort_specs = ImGui::TableGetSortSpecs()) {
            if (sort_specs->SpecsCount > 0 && hist_indices.size() > 1) {
                const ImGuiTableColumnSortSpecs& spec = sort_specs->Specs[0];
                const bool ascending = (spec.SortDirection == ImGuiSortDirection_Ascending);

                std::sort(hist_indices.begin(), hist_indices.end(), [&](size_t ia, size_t ib) {
                    const TxHistoryEntry& a = tx_history_[ia];
                    const TxHistoryEntry& b = tx_history_[ib];
                    int cmp = 0;
                    switch (spec.ColumnUserID) {
                        case HIST_COL_LAST_SENT:
                            cmp = ThreeWayCompare(a.last_sent_sec, b.last_sent_sec);
                            break;
                        case HIST_COL_DEV:
                            cmp = ThreeWayCompare(a.target, b.target);
                            break;
                        case HIST_COL_CAN_ID:
                            cmp = ThreeWayCompare(a.msg.id, b.msg.id);
                            break;
                        case HIST_COL_FORMAT:
                            cmp = ThreeWayCompare(GetFrameFormatRank(a.msg), GetFrameFormatRank(b.msg));
                            break;
                        case HIST_COL_DLC:
                            cmp = ThreeWayCompare(a.msg.dlc, b.msg.dlc);
                            break;
                        case HIST_COL_COUNT:
                            cmp = ThreeWayCompare(a.sent_count, b.sent_count);
                            break;
                        case HIST_COL_DATA:
                            cmp = ComparePayloads(a.msg.payload, b.msg.payload);
                            if (cmp == 0) cmp = ThreeWayCompare(a.comment, b.comment);
                            break;
                        default:
                            break;
                    }
                    if (cmp == 0) cmp = ThreeWayCompare(ia, ib);
                    return ascending ? (cmp < 0) : (cmp > 0);
                });
            }
            sort_specs->SpecsDirty = false;
        }

        for (size_t real_idx : hist_indices) {
            TxHistoryEntry& item = tx_history_[real_idx];
            const CanMessage& m = item.msg;
            ImGui::PushID(static_cast<int>(real_idx));

            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%.2f s", item.last_sent_sec);

            ImGui::TableSetColumnIndex(1);
            if (item.target == kTargetAllDevices) {
                ImGui::TextDisabled("All");
            } else {
                ImGui::TextColored(DeviceColor(item.target), "%d", item.target + 1);
            }

            ImGui::TableSetColumnIndex(2);
            ImGui::Text(m.is_extended ? "0x%08X" : "0x%03X", m.id);

            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%s %s%s",
                        m.is_extended ? "EXT" : "STD",
                        m.is_fd ? "FD" : "2.0",
                        m.is_brs ? " BRS" : "");

            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%u", m.dlc);

            ImGui::TableSetColumnIndex(5);
            ImGui::Text("%u", item.sent_count);

            ImGui::TableSetColumnIndex(6);
            std::string hex = FormatHexPayload(m.payload);
            if (!item.comment.empty()) {
                ImGui::Text("%s  [%s]", hex.c_str(), item.comment.c_str());
            } else {
                ImGui::TextUnformatted(hex.c_str());
            }

            ImGui::TableSetColumnIndex(7);
            if (ImGui::SmallButton("Resend")) {
                TransmitSingleMessage(item.msg, item.target, true, item.cycle_time_ms, item.comment);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Load")) {
                selected_history_index_ = static_cast<int>(real_idx);
                LoadMessageIntoComposer(item.msg, item.target, item.cycle_time_ms, item.comment);
            }

            ImGui::PopID();
        }

        ImGui::EndTable();
    }

    ImGui::End();
}

// ============================================================================
// Main Workspace Render Entry Point (Dynamic Full-Viewport Responsive Layout)
// ============================================================================
void AppUI::Render() {
    double now_ms = GetNowMs();

    // 1. Execute periodic CAN transmissions scheduled by user
    ProcessPeriodicJobs(now_ms);

    // 2. Render Top Menu Bar and get its height
    float menu_h = RenderMenuBar();

    // 3. Calculate responsive tiling coordinates from current browser viewport size
    ImVec2 display_size = ImGui::GetIO().DisplaySize;
    float pad = 4.0f;
    float usable_w = std::max(600.0f, display_size.x - pad * 3.0f);
    float usable_h = std::max(450.0f, display_size.y - menu_h - pad * 4.0f);

    float left_w  = std::floor(usable_w * 0.44f);
    float right_w = usable_w - left_w;

    float row1_h  = std::floor(usable_h * 0.38f);
    float row2_h  = std::floor(usable_h * 0.39f);
    float row3_h  = usable_h - row1_h - row2_h;

    float y1 = menu_h + pad;
    float y2 = y1 + row1_h + pad;
    float y3 = y2 + row2_h + pad;
    float x_left  = pad;
    float x_right = pad * 2.0f + left_w;

    // 4. Render Independent PCAN-View Style Windows
    if (show_hw_window_)       RenderHardwareSetupWindow(x_left, y1, left_w, row1_h);
    if (show_id_table_window_) RenderIdTableWindow(now_ms, x_right, y1, right_w, row1_h);
    if (show_tx_window_)       RenderTransmitWindow(now_ms, x_left, y2, left_w, row2_h);
    if (show_trace_window_)    RenderTraceWindow(x_right, y2, right_w, row2_h);
    if (show_history_window_)  RenderSentHistoryWindow(x_left, y3, usable_w + pad, row3_h);
}
