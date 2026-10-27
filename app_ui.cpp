#include "app_ui.h"
#include "webusb_bridge.h"
#include "imgui.h"
#include <GLFW/glfw3.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <cmath>

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

// Helper: Format byte vector into a space-separated hexadecimal string
static std::string FormatHexPayload(const std::vector<uint8_t>& data) {
    char buf[256] = {0};
    int offset = 0;
    for (size_t i = 0; i < data.size() && offset < 245; ++i) {
        offset += std::snprintf(buf + offset, sizeof(buf) - offset, "%02X%s",
                                data[i], (i + 1 < data.size()) ? " " : "");
    }
    return std::string(buf);
}

// Helper: Build unique 64-bit lookup key for ID Table (combines 29-bit/11-bit ID and EXT flag)
static uint64_t MakeCanIdKey(uint32_t id, bool is_extended) {
    return (static_cast<uint64_t>(is_extended ? 1 : 0) << 32) | static_cast<uint64_t>(id);
}

AppUI::AppUI()
    : is_connected_(false),
      is_channel_open_(false),
      device_name_("No Device"),
      start_time_ms_(glfwGetTime() * 1000.0),
      auto_tile_layout_(true),
      show_hw_window_(true),
      show_tx_window_(true),
      show_history_window_(true),
      show_id_table_window_(true),
      show_trace_window_(true),
      target_nom_kbps_(500),
      target_nom_sp_(87.5f),
      enable_fd_mode_(true),
      enable_loopback_(false),
      enable_listen_only_(false),
      target_data_kbps_(2000),
      target_data_sp_(75.0f),
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
      selected_history_index_(-1)
{
    std::memset(&bt_const_, 0, sizeof(bt_const_));
    bt_const_.fclk_can  = 80000000U; // Default 80 MHz clock
    bt_const_.tseg1_min = 1;
    bt_const_.tseg1_max = 256;
    bt_const_.tseg2_min = 1;
    bt_const_.tseg2_max = 128;
    bt_const_.sjw_max   = 128;
    bt_const_.brp_min   = 1;
    bt_const_.brp_max   = 512;

    CanProtocolHandler::AutoCalculateBittiming(bt_const_.fclk_can, target_nom_kbps_ * 1000U, target_nom_sp_, nom_bt_);
    CanProtocolHandler::AutoCalculateBittiming(bt_const_.fclk_can, target_data_kbps_ * 1000U, target_data_sp_, data_bt_);

    std::strncpy(tx_payload_, "01 02 03 04 05 06 07 08", sizeof(tx_payload_) - 1);
    tx_payload_[sizeof(tx_payload_) - 1] = '\0';

    std::strncpy(tx_comment_, "Engine Status", sizeof(tx_comment_) - 1);
    tx_comment_[sizeof(tx_comment_) - 1] = '\0';
}

double AppUI::GetNowMs() const {
    return (glfwGetTime() * 1000.0) - start_time_ms_;
}

void AppUI::OnDeviceConnected(const std::string& name) {
    is_connected_ = true;
    is_channel_open_ = false;
    device_name_ = name.empty() ? "WebUSB CAN Adapter" : name;

    // Query bit-timing constants from EP0
    WebUsbBridge::GetInstance().ControlTransferIn(GS_USB_BREQ_BT_CONST, 0, 0, sizeof(GsDeviceBtConst));
}

void AppUI::OnDeviceDisconnected() {
    is_connected_ = false;
    is_channel_open_ = false;
    device_name_ = "Disconnected";
}

void AppUI::OnControlDataReceived(uint8_t request, const uint8_t* data, int length) {
    if (request == GS_USB_BREQ_BT_CONST && length >= static_cast<int>(sizeof(GsDeviceBtConst))) {
        std::memcpy(&bt_const_, data, sizeof(GsDeviceBtConst));
        if (bt_const_.fclk_can > 0) {
            CanProtocolHandler::AutoCalculateBittiming(bt_const_.fclk_can, target_nom_kbps_ * 1000U, target_nom_sp_, nom_bt_);
            CanProtocolHandler::AutoCalculateBittiming(bt_const_.fclk_can, target_data_kbps_ * 1000U, target_data_sp_, data_bt_);
        }
    }
}

void AppUI::OnCanMessageReceived(const CanMessage& msg) {
    double now_ms = GetNowMs();

    // 1. Update Linear Receive Trace (if not paused)
    if (!trace_paused_) {
        if (rx_trace_.size() >= 2000) {
            rx_trace_.pop_front();
        }
        TraceEntry trace_item;
        trace_item.timestamp_sec = now_ms / 1000.0;
        trace_item.msg = msg;
        rx_trace_.push_back(trace_item);
    }

    // 2. Update Grouped ID Table (PCAN-View style)
    uint64_t key = MakeCanIdKey(msg.id, msg.is_extended);
    auto it = id_table_.find(key);
    if (it == id_table_.end()) {
        IdTableEntry entry;
        entry.last_msg = msg;
        entry.prev_payload = msg.payload;
        entry.count = 1;
        entry.last_rx_time_ms = now_ms;
        entry.cycle_time_ms = 0.0;
        entry.min_cycle_ms = 0.0;
        entry.max_cycle_ms = 0.0;
        id_table_[key] = entry;
    } else {
        IdTableEntry& entry = it->second;
        double dt = now_ms - entry.last_rx_time_ms;
        if (dt >= 0.0) {
            entry.cycle_time_ms = dt;
            if (entry.count == 1 || dt < entry.min_cycle_ms) entry.min_cycle_ms = dt;
            if (entry.count == 1 || dt > entry.max_cycle_ms) entry.max_cycle_ms = dt;
        }
        entry.prev_payload = entry.last_msg.payload;
        entry.last_msg = msg;
        entry.last_rx_time_ms = now_ms;
        entry.count++;
    }
}

void AppUI::StartCanChannel() {
    auto& usb = WebUsbBridge::GetInstance();

    uint32_t host_format = 0x0000BEEFU;
    usb.ControlTransferOut(GS_USB_BREQ_HOST_FORMAT, 1, 0, reinterpret_cast<const uint8_t*>(&host_format), sizeof(host_format));
    usb.ControlTransferOut(GS_USB_BREQ_BITTIMING, 0, 0, reinterpret_cast<const uint8_t*>(&nom_bt_), sizeof(nom_bt_));

    if (enable_fd_mode_) {
        usb.ControlTransferOut(GS_USB_BREQ_DATA_BITTIMING, 0, 0, reinterpret_cast<const uint8_t*>(&data_bt_), sizeof(data_bt_));
    }

    GsDeviceMode mode_cmd = {};
    mode_cmd.mode = GS_CAN_MODE_START;
    mode_cmd.flags = 0;
    if (enable_fd_mode_)     mode_cmd.flags |= GS_CAN_FEATURE_FD;
    if (enable_loopback_)    mode_cmd.flags |= GS_CAN_FEATURE_LOOP_BACK;
    if (enable_listen_only_) mode_cmd.flags |= GS_CAN_FEATURE_LISTEN_ONLY;

    usb.ControlTransferOut(GS_USB_BREQ_MODE, 0, 0, reinterpret_cast<const uint8_t*>(&mode_cmd), sizeof(mode_cmd));

    usb.StartRxPump(0x81, 512);
    is_channel_open_ = true;
}

void AppUI::StopCanChannel() {
    auto& usb = WebUsbBridge::GetInstance();
    usb.StopRxPump();

    GsDeviceMode mode_cmd = {GS_CAN_MODE_RESET, 0};
    usb.ControlTransferOut(GS_USB_BREQ_MODE, 0, 0, reinterpret_cast<const uint8_t*>(&mode_cmd), sizeof(mode_cmd));
    is_channel_open_ = false;
}

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

void AppUI::LoadMessageIntoComposer(const CanMessage& msg, int cycle_ms, const std::string& comment) {
    tx_id_ = static_cast<int>(msg.id);
    tx_is_ext_ = msg.is_extended;
    tx_is_fd_ = msg.is_fd;
    tx_is_brs_ = msg.is_brs;
    tx_is_rtr_ = msg.is_rtr;
    tx_dlc_ = msg.dlc;
    tx_cycle_ms_ = cycle_ms;

    std::string hex = FormatHexPayload(msg.payload);
    std::strncpy(tx_payload_, hex.c_str(), sizeof(tx_payload_) - 1);
    tx_payload_[sizeof(tx_payload_) - 1] = '\0';

    std::strncpy(tx_comment_, comment.c_str(), sizeof(tx_comment_) - 1);
    tx_comment_[sizeof(tx_comment_) - 1] = '\0';

    show_tx_window_ = true;
}

void AppUI::TransmitSingleMessage(const CanMessage& msg, bool record_in_history, int associated_cycle_ms, const std::string& comment) {
    if (is_connected_ && is_channel_open_) {
        CanProtocolHandler protocol;
        std::vector<uint8_t> usb_bytes = protocol.BuildTxBuffer(msg);
        WebUsbBridge::GetInstance().TransferOut(0x02, usb_bytes.data(), static_cast<int>(usb_bytes.size()));
    }

    if (!record_in_history) return;

    double now_sec = GetNowMs() / 1000.0;

    // Update existing entry if identical message is already in history
    for (auto& item : tx_history_) {
        if (item.msg.id == msg.id &&
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
    new_entry.cycle_time_ms = associated_cycle_ms;
    new_entry.sent_count = 1;
    new_entry.last_sent_sec = now_sec;
    new_entry.comment = comment;
    tx_history_.push_back(new_entry);
}

void AppUI::ProcessPeriodicJobs(double now_ms) {
    if (!is_connected_ || !is_channel_open_) return;

    for (auto& job : periodic_jobs_) {
        if (!job.enabled || job.cycle_ms <= 0) continue;

        if (now_ms >= job.next_send_ms) {
            TransmitSingleMessage(job.msg, false);
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
            ImGui::MenuItem("1. Hardware & Bit-Timing Setup", nullptr, &show_hw_window_);
            ImGui::MenuItem("2. Receive Table by ID (Grouped Monitor)", nullptr, &show_id_table_window_);
            ImGui::MenuItem("3. Receive Trace (Chronological Stream)", nullptr, &show_trace_window_);
            ImGui::MenuItem("4. Transmit Frame & Periodic Jobs", nullptr, &show_tx_window_);
            ImGui::MenuItem("5. Sent Messages History", nullptr, &show_history_window_);
            ImGui::EndMenu();
        }

        ImGui::Separator();
        ImGui::Checkbox("Auto-Tile Resize", &auto_tile_layout_);
        ImGui::Separator();

        if (is_connected_) {
            ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.4f, 1.0f), "USB: %s (%s)",
                               device_name_.c_str(), is_channel_open_ ? "BUS ON" : "BUS OFF");
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "USB: Disconnected");
        }

        ImGui::EndMainMenuBar();
    }
    return bar_height;
}

// Helper for Bit-Timing configuration UI
static void RenderBittimingEditor(
    const char* label_prefix,
    uint32_t fclk_hz,
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
        CanProtocolHandler::AutoCalculateBittiming(fclk_hz, static_cast<uint32_t>(target_kbps) * 1000U, target_sp, bt);
    }

    int brp  = static_cast<int>(bt.brp);
    int prop = static_cast<int>(bt.prop_seg);
    int seg1 = static_cast<int>(bt.phase_seg1);
    int seg2 = static_cast<int>(bt.phase_seg2);
    int sjw  = static_cast<int>(bt.sjw);

    ImGui::PushItemWidth(70);
    if (ImGui::InputInt("BRP", &brp, 1, 5))   bt.brp = std::clamp(brp, 1, 1024);
    ImGui::SameLine();
    if (ImGui::InputInt("Prop", &prop, 1, 2)) bt.prop_seg = std::clamp(prop, 0, 256);
    ImGui::SameLine();
    if (ImGui::InputInt("Seg1", &seg1, 1, 2)) bt.phase_seg1 = std::clamp(seg1, 1, 256);
    ImGui::SameLine();
    if (ImGui::InputInt("Seg2", &seg2, 1, 2)) bt.phase_seg2 = std::clamp(seg2, 1, 128);
    ImGui::SameLine();
    if (ImGui::InputInt("SJW", &sjw, 1, 1))   bt.sjw = std::clamp(sjw, 1, 128);
    ImGui::PopItemWidth();

    CanTimingMetrics m = CanProtocolHandler::ComputeTimingMetrics(fclk_hz, bt);
    ImGui::TextColored(ImVec4(0.4f, 0.9f, 1.0f, 1.0f),
        "1 tq: %.1f ns | Total: %u tq (TSEG1:%u, TSEG2:%u) | Actual: %.1f kbps | SP: %.1f%%",
        m.tq_ns, m.total_tq, m.tseg1_total, bt.phase_seg2, m.actual_bitrate / 1000.0, m.sample_point_pct);

    ImGui::PopID();
}

// ============================================================================
// Window 1: Hardware Connection & Bit-Timing Setup
// ============================================================================
void AppUI::RenderHardwareSetupWindow(float x, float y, float w, float h) {
    ImGuiCond cond = auto_tile_layout_ ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
    ImGui::SetNextWindowPos(ImVec2(x, y), cond);
    ImGui::SetNextWindowSize(ImVec2(w, h), cond);

    if (!ImGui::Begin("1. Hardware & Bit-Timing Setup", &show_hw_window_)) {
        ImGui::End();
        return;
    }

    if (!is_connected_) {
        if (ImGui::Button("Request WebUSB CAN Device", ImVec2(210, 26))) {
            WebUsbBridge::GetInstance().RequestDevice();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Select your CANable / Candlelight adapter");
    } else {
        if (ImGui::Button("Disconnect USB", ImVec2(130, 26))) {
            if (is_channel_open_) StopCanChannel();
            WebUsbBridge::GetInstance().Disconnect();
        }
        ImGui::SameLine();
        if (!is_channel_open_) {
            if (ImGui::Button("Start CAN Channel", ImVec2(150, 26))) {
                StartCanChannel();
            }
        } else {
            if (ImGui::Button("Stop CAN Channel", ImVec2(150, 26))) {
                StopCanChannel();
            }
        }
        ImGui::SameLine();
        ImGui::Text("Clock: %.1f MHz", bt_const_.fclk_can / 1e6);
    }

    ImGui::Separator();
    ImGui::BeginDisabled(is_channel_open_);

    int clk_mhz = static_cast<int>(bt_const_.fclk_can / 1000000U);
    ImGui::PushItemWidth(80);
    if (ImGui::InputInt("CAN Clock (MHz)", &clk_mhz, 1, 10)) {
        bt_const_.fclk_can = static_cast<uint32_t>(std::clamp(clk_mhz, 1, 400)) * 1000000U;
    }
    ImGui::PopItemWidth();
    ImGui::SameLine();
    ImGui::Checkbox("CAN FD", &enable_fd_mode_);
    ImGui::SameLine();
    ImGui::Checkbox("Loopback", &enable_loopback_);
    ImGui::SameLine();
    ImGui::Checkbox("Listen-Only", &enable_listen_only_);

    ImGui::SeparatorText("Nominal Arbitration Timing");
    RenderBittimingEditor("Nom", bt_const_.fclk_can, nom_bt_, target_nom_kbps_, target_nom_sp_);

    if (enable_fd_mode_) {
        ImGui::SeparatorText("CAN FD Data Phase Timing");
        RenderBittimingEditor("Data", bt_const_.fclk_can, data_bt_, target_data_kbps_, target_data_sp_);
    }

    ImGui::EndDisabled();
    ImGui::End();
}

// ============================================================================
// Window 2: Receive Monitor Table Grouped by ID (PCAN-View Receive Table)
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
    ImGui::SameLine(ImGui::GetWindowWidth() - 110);
    if (ImGui::SmallButton("Reset Table")) {
        id_table_.clear();
    }

    const ImGuiTableFlags flags =
        ImGuiTableFlags_Borders |
        ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_Resizable |
        ImGuiTableFlags_SizingStretchProp;

    if (ImGui::BeginTable("IdMonitorTable", 6, flags)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("CAN ID", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 95.0f);
        ImGui::TableSetupColumn("Length", ImGuiTableColumnFlags_WidthFixed, 65.0f);
        ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed, 65.0f);
        ImGui::TableSetupColumn("Cycle Time (ms)", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Data (Changed bytes highlighted)", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (const auto& kv : id_table_) {
            const IdTableEntry& entry = kv.second;
            const CanMessage& m = entry.last_msg;

            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::Text(m.is_extended ? "0x%08X" : "0x%03X", m.id);

            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%s %s%s%s",
                        m.is_extended ? "EXT" : "STD",
                        m.is_fd ? "FD" : "2.0",
                        m.is_brs ? " BRS" : "",
                        m.is_rtr ? " RTR" : "");

            ImGui::TableSetColumnIndex(2);
            uint8_t byte_len = m.is_fd ? CanProtocolHandler::DlcToBytes(m.dlc) : std::min<uint8_t>(m.dlc, 8);
            ImGui::Text("%u (%uB)", m.dlc, byte_len);

            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%llu", static_cast<unsigned long long>(entry.count));

            ImGui::TableSetColumnIndex(4);
            if (entry.count > 1) {
                ImGui::Text("%.1f ms", entry.cycle_time_ms);
            } else {
                ImGui::TextDisabled("---");
            }

            ImGui::TableSetColumnIndex(5);
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
// Window 3: Chronological Receive Trace (Linear Stream)
// ============================================================================
void AppUI::RenderTraceWindow(float x, float y, float w, float h) {
    ImGuiCond cond = auto_tile_layout_ ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
    ImGui::SetNextWindowPos(ImVec2(x, y), cond);
    ImGui::SetNextWindowSize(ImVec2(w, h), cond);

    if (!ImGui::Begin("3. Received Messages Trace (Chronological)", &show_trace_window_)) {
        ImGui::End();
        return;
    }

    ImGui::Checkbox("Pause Trace", &trace_paused_);
    ImGui::SameLine();
    ImGui::Checkbox("Auto-Scroll", &trace_autoscroll_);
    ImGui::SameLine();
    ImGui::PushItemWidth(90);
    ImGui::InputInt("Filter ID (Hex, -1=All)", &trace_filter_id_, 1, 16);
    ImGui::PopItemWidth();
    ImGui::SameLine(ImGui::GetWindowWidth() - 105);
    if (ImGui::SmallButton("Clear Trace")) {
        rx_trace_.clear();
    }

    const ImGuiTableFlags flags =
        ImGuiTableFlags_Borders |
        ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_SizingStretchProp;

    if (ImGui::BeginTable("TraceStreamTable", 6, flags)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Time (s)", ImGuiTableColumnFlags_WidthFixed, 75.0f);
        ImGui::TableSetupColumn("Dir", ImGuiTableColumnFlags_WidthFixed, 55.0f);
        ImGui::TableSetupColumn("CAN ID", ImGuiTableColumnFlags_WidthFixed, 85.0f);
        ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthFixed, 85.0f);
        ImGui::TableSetupColumn("DLC", ImGuiTableColumnFlags_WidthFixed, 45.0f);
        ImGui::TableSetupColumn("Data (HEX)", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (const auto& item : rx_trace_) {
            const CanMessage& m = item.msg;
            if (trace_filter_id_ >= 0 && static_cast<int>(m.id) != trace_filter_id_) {
                continue;
            }

            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%.3f", item.timestamp_sec);

            ImGui::TableSetColumnIndex(1);
            if (m.is_tx_echo) {
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "TX");
            } else {
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 1.0f, 1.0f), "RX");
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
// Window 4: Dedicated Transmit Window & Active Periodic Jobs
// ============================================================================
void AppUI::RenderTransmitWindow(double now_ms, float x, float y, float w, float h) {
    ImGuiCond cond = auto_tile_layout_ ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
    ImGui::SetNextWindowPos(ImVec2(x, y), cond);
    ImGui::SetNextWindowSize(ImVec2(w, h), cond);

    if (!ImGui::Begin("4. Transmit Frame Composer & Periodic Jobs", &show_tx_window_)) {
        ImGui::End();
        return;
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
            TransmitSingleMessage(msg, (i == 0), tx_cycle_ms_, tx_comment_);
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
        job.cycle_ms = (tx_cycle_ms_ > 0) ? tx_cycle_ms_ : 100;
        job.auto_inc_byte_index = tx_auto_inc_byte_;
        job.next_send_ms = now_ms + job.cycle_ms;
        job.tx_count = 0;
        job.comment = tx_comment_;
        periodic_jobs_.push_back(job);

        TransmitSingleMessage(job.msg, true, job.cycle_ms, job.comment);
    }

    ImGui::SeparatorText("Active Periodic Transmit Jobs");

    const ImGuiTableFlags flags =
        ImGuiTableFlags_Borders |
        ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_SizingStretchProp;

    if (ImGui::BeginTable("PeriodicJobsTable", 6, flags)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, 30.0f);
        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 75.0f);
        ImGui::TableSetupColumn("Cycle", ImGuiTableColumnFlags_WidthFixed, 65.0f);
        ImGui::TableSetupColumn("Sent", ImGuiTableColumnFlags_WidthFixed, 55.0f);
        ImGui::TableSetupColumn("Data (HEX)", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 105.0f);
        ImGui::TableHeadersRow();

        int delete_idx = -1;
        for (size_t i = 0; i < periodic_jobs_.size(); ++i) {
            PeriodicTxJob& job = periodic_jobs_[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::Checkbox("##en", &job.enabled);

            ImGui::TableSetColumnIndex(1);
            ImGui::Text(job.msg.is_extended ? "0x%08X" : "0x%03X", job.msg.id);

            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%d ms", job.cycle_ms);

            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%llu", static_cast<unsigned long long>(job.tx_count));

            ImGui::TableSetColumnIndex(4);
            std::string hex = FormatHexPayload(job.msg.payload);
            ImGui::TextUnformatted(hex.c_str());

            ImGui::TableSetColumnIndex(5);
            if (ImGui::SmallButton("Shot")) {
                TransmitSingleMessage(job.msg, false);
                job.tx_count++;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Del")) {
                delete_idx = static_cast<int>(i);
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
// Window 5: Sent Messages History (Select, Load to Composer, or Quick Resend)
// ============================================================================
void AppUI::RenderSentHistoryWindow(float x, float y, float w, float h) {
    ImGuiCond cond = auto_tile_layout_ ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
    ImGui::SetNextWindowPos(ImVec2(x, y), cond);
    ImGui::SetNextWindowSize(ImVec2(w, h), cond);

    if (!ImGui::Begin("5. Sent Messages History (Select & Resend)", &show_history_window_)) {
        ImGui::End();
        return;
    }

    ImGui::Text("History Entries: %zu (Click 'Load' to edit in Composer, or 'Resend' for immediate transmission)", tx_history_.size());
    ImGui::SameLine(ImGui::GetWindowWidth() - 120);
    if (ImGui::SmallButton("Clear History")) {
        tx_history_.clear();
        selected_history_index_ = -1;
    }

    const ImGuiTableFlags flags =
        ImGuiTableFlags_Borders |
        ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_SizingStretchProp;

    if (ImGui::BeginTable("SentHistoryTable", 7, flags)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Last Sent", ImGuiTableColumnFlags_WidthFixed, 75.0f);
        ImGui::TableSetupColumn("CAN ID", ImGuiTableColumnFlags_WidthFixed, 85.0f);
        ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("DLC", ImGuiTableColumnFlags_WidthFixed, 45.0f);
        ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed, 55.0f);
        ImGui::TableSetupColumn("Data (HEX) & Comment", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Operations", ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < tx_history_.size(); ++i) {
            TxHistoryEntry& item = tx_history_[i];
            const CanMessage& m = item.msg;
            ImGui::PushID(static_cast<int>(i));

            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%.2f s", item.last_sent_sec);

            ImGui::TableSetColumnIndex(1);
            ImGui::Text(m.is_extended ? "0x%08X" : "0x%03X", m.id);

            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%s %s%s",
                        m.is_extended ? "EXT" : "STD",
                        m.is_fd ? "FD" : "2.0",
                        m.is_brs ? " BRS" : "");

            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%u", m.dlc);

            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%u", item.sent_count);

            ImGui::TableSetColumnIndex(5);
            std::string hex = FormatHexPayload(m.payload);
            if (!item.comment.empty()) {
                ImGui::Text("%s  [%s]", hex.c_str(), item.comment.c_str());
            } else {
                ImGui::TextUnformatted(hex.c_str());
            }

            ImGui::TableSetColumnIndex(6);
            if (ImGui::SmallButton("Resend")) {
                TransmitSingleMessage(item.msg, true, item.cycle_time_ms, item.comment);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Load")) {
                selected_history_index_ = static_cast<int>(i);
                LoadMessageIntoComposer(item.msg, item.cycle_time_ms, item.comment);
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