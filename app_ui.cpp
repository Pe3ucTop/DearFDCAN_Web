#include "app_ui.h"
#include "webusb_bridge.h"
#include "imgui.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

// Parse a space-separated hexadecimal string into a byte vector
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

AppUI::AppUI()
    : is_connected_(false),
      is_channel_open_(false),
      device_name_("No Device"),
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
      tx_is_brs_(true) {
    std::memset(&bt_const_, 0, sizeof(bt_const_));
    // Default fallback clock: 80 MHz (standard for STM32G4 Candlelight)
    bt_const_.fclk_can = 80000000U;
    bt_const_.tseg1_min = 1;
    bt_const_.tseg1_max = 256;
    bt_const_.tseg2_min = 1;
    bt_const_.tseg2_max = 128;
    bt_const_.sjw_max = 128;
    bt_const_.brp_min = 1;
    bt_const_.brp_max = 512;

    // Initialize default timings for 500 kbps (Nominal) and 2000 kbps (FD Data) @ 80 MHz
    CanProtocolHandler::AutoCalculateBittiming(bt_const_.fclk_can, target_nom_kbps_ * 1000U, target_nom_sp_, nom_bt_);
    CanProtocolHandler::AutoCalculateBittiming(bt_const_.fclk_can, target_data_kbps_ * 1000U, target_data_sp_, data_bt_);

    std::strncpy(tx_payload_, "01 02 03 04 05 06 07 08", sizeof(tx_payload_) - 1);
    tx_payload_[sizeof(tx_payload_) - 1] = '\0';
}

void AppUI::OnDeviceConnected(const std::string& name) {
    is_connected_ = true;
    is_channel_open_ = false;
    device_name_ = name.empty() ? "WebUSB CAN Adapter" : name;

    // Request hardware bit-timing constants (fclk_can, min/max tq, brp limits) over EP0
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
            // Recalculate timings with the actual hardware CAN clock frequency
            CanProtocolHandler::AutoCalculateBittiming(bt_const_.fclk_can, target_nom_kbps_ * 1000U, target_nom_sp_, nom_bt_);
            CanProtocolHandler::AutoCalculateBittiming(bt_const_.fclk_can, target_data_kbps_ * 1000U, target_data_sp_, data_bt_);
        }
    }
}

void AppUI::OnCanMessageReceived(const CanMessage& msg) {
    if (message_log_.size() >= 500) {
        message_log_.pop_front();
    }
    message_log_.push_back(msg);
}

void AppUI::StartCanChannel() {
    auto& usb = WebUsbBridge::GetInstance();

    // 1. Set Host byte order format (little-endian 0x0000BEEF)
    uint32_t host_format = 0x0000BEEFU;
    usb.ControlTransferOut(GS_USB_BREQ_HOST_FORMAT, 1, 0, reinterpret_cast<const uint8_t*>(&host_format), sizeof(host_format));

    // 2. Send Nominal Bit-Timing (Prescaler, Prop_Seg, Phase_Seg1, Phase_Seg2, SJW)
    usb.ControlTransferOut(GS_USB_BREQ_BITTIMING, 0, 0, reinterpret_cast<const uint8_t*>(&nom_bt_), sizeof(nom_bt_));

    // 3. Send CAN FD Data Phase Bit-Timing if enabled
    if (enable_fd_mode_) {
        usb.ControlTransferOut(GS_USB_BREQ_DATA_BITTIMING, 0, 0, reinterpret_cast<const uint8_t*>(&data_bt_), sizeof(data_bt_));
    }

    // 4. Configure mode flags and start CAN controller
    GsDeviceMode mode_cmd = {};
    mode_cmd.mode = GS_CAN_MODE_START;
    mode_cmd.flags = 0;
    if (enable_fd_mode_)     mode_cmd.flags |= GS_CAN_FEATURE_FD;
    if (enable_loopback_)    mode_cmd.flags |= GS_CAN_FEATURE_LOOP_BACK;
    if (enable_listen_only_) mode_cmd.flags |= GS_CAN_FEATURE_LISTEN_ONLY;

    usb.ControlTransferOut(GS_USB_BREQ_MODE, 0, 0, reinterpret_cast<const uint8_t*>(&mode_cmd), sizeof(mode_cmd));

    // 5. Start Bulk IN polling on endpoint 0x81
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

// Helper to render a single Bit-Timing editor block (Nominal or Data Phase)
static void RenderBittimingEditor(
    const char* label_prefix,
    uint32_t fclk_hz,
    GsDeviceBittiming& bt,
    int& target_kbps,
    float& target_sp		)
{
	bool recalc_val = false;
	
    ImGui::PushID(label_prefix);

    // Quick Auto-Calculator Row
    ImGui::PushItemWidth(100);
    ImGui::InputInt("Target (kbps)", &target_kbps, 50, 1000);
    ImGui::SameLine();
    ImGui::InputFloat("Target SP (%)", &target_sp, 50.0f, 95.0f, "%.1f%%");
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Button("Auto-Calc TQ & Prescaler")) {
		target_kbps = std::max(10, target_kbps);
        CanProtocolHandler::AutoCalculateBittiming(
				fclk_hz, 
				static_cast<uint32_t>(target_kbps) * 1000U,
				target_sp,
				bt											);
    }

    // Manual TQ, Prescaler, and SJW Sliders/Inputs
    int prop = static_cast<int>(bt.prop_seg);
    int brp  = static_cast<int>(bt.brp);
    int seg1 = static_cast<int>(bt.phase_seg1);
    int seg2 = static_cast<int>(bt.phase_seg2);
    int sjw  = static_cast<int>(bt.sjw);

    ImGui::PushItemWidth(95);
    if (ImGui::InputInt("Prescaler (BRP)", &brp, 1, 10)) 	recalc_val = true;
    //ImGui::SameLine();
    //if (ImGui::InputInt("Prop_Seg (tq)", &prop, 1, 4))   	recalc_val = true;
    ImGui::SameLine();
    if (ImGui::InputInt("Phase_Seg1 (tq)", &seg1, 1, 4)) 	recalc_val = true;
    ImGui::SameLine();
    if (ImGui::InputInt("Phase_Seg2 (tq)", &seg2, 1, 4)) 	recalc_val = true;
    ImGui::SameLine();
    if (ImGui::InputInt("SJW (tq)", &sjw, 1, 2))         	recalc_val = true;
    ImGui::PopItemWidth();

    if(recalc_val)
    {
		bt.brp 			= std::clamp(brp, 1, 1024);
		bt.prop_seg		= 0; //std::clamp(prop, 0, 256);
		bt.phase_seg1	= std::clamp(seg1, 1, 256);
		bt.phase_seg2	= std::clamp(seg2, 1, 128);
		bt.sjw			= std::clamp(sjw, 1, 128);
	}

    // Live Computed Timing Summary
    CanTimingMetrics metrics = CanProtocolHandler::ComputeTimingMetrics(fclk_hz, bt);
    ImGui::TextColored(
        ImVec4(0.4f, 0.9f, 1.0f, 1.0f),
        "Result -> 1 tq: %.2f ns | Total TQ: %u (TSEG1: %u, TSEG2: %u) | Actual Bitrate: %.2f kbps | Sample Point: %.1f%%",
        metrics.tq_ns,
        metrics.total_tq,
        metrics.tseg1_total,
        bt.phase_seg2,
        metrics.actual_bitrate / 1000.0,
        metrics.sample_point_pct
    );

    ImGui::PopID();
    
}

void AppUI::Render() {
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(940, 700), ImGuiCond_FirstUseEver);

    ImGui::Begin("Modular WebUSB CAN Analyzer (With TQ / SJW / Prescaler Configuration)");

    // --- 1. USB Device Connection Bar ---
    if (!is_connected_) {
        if (ImGui::Button("Request WebUSB Device", ImVec2(200, 30))) {
            WebUsbBridge::GetInstance().RequestDevice();
        }
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Status: Disconnected");
    } else {
        if (ImGui::Button("Disconnect USB", ImVec2(140, 30))) {
            if (is_channel_open_) StopCanChannel();
            WebUsbBridge::GetInstance().Disconnect();
        }
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.4f, 1.0f), "USB Connected: %s | CAN Clock (fclk_can): %.2f MHz",
                           device_name_.c_str(), bt_const_.fclk_can / 1e6);
    }

    ImGui::Separator();

    // --- 2. CAN Timing Configuration (Prescaler, TQ, SJW) ---
    if (ImGui::CollapsingHeader("CAN Bit-Timing Setup (Prescaler, TQ Segments, SJW)", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::BeginDisabled(is_channel_open_);

        // Allow manual clock override if offline or testing custom MCU clock
        int clk_mhz = static_cast<int>(bt_const_.fclk_can / 1000000U);
        ImGui::PushItemWidth(100);
        if (ImGui::InputInt("CAN Clock (MHz)", &clk_mhz, 1, 10)) {
            clk_mhz = std::clamp(clk_mhz, 1, 400);
            bt_const_.fclk_can = static_cast<uint32_t>(clk_mhz) * 1000000U;
        }
        ImGui::PopItemWidth();
        ImGui::SameLine();
        ImGui::Checkbox("Enable CAN FD", &enable_fd_mode_);
        ImGui::SameLine();
        ImGui::Checkbox("Loopback Mode", &enable_loopback_);
        ImGui::SameLine();
        ImGui::Checkbox("Listen-Only Mode", &enable_listen_only_);

        ImGui::Spacing();
        ImGui::SeparatorText("Nominal (Arbitration Phase) Bit-Timing");
        RenderBittimingEditor("Nomina", bt_const_.fclk_can, nom_bt_, target_nom_kbps_, target_nom_sp_);

        if (enable_fd_mode_) {
            ImGui::Spacing();
            ImGui::SeparatorText("CAN FD (Data Phase) Bit-Timing");
            RenderBittimingEditor("FDData", bt_const_.fclk_can, data_bt_, target_data_kbps_, target_data_sp_);
        }

        ImGui::EndDisabled();

        ImGui::Spacing();
        ImGui::BeginDisabled(!is_connected_);
        if (!is_channel_open_) {
            if (ImGui::Button("Apply Timings & Start CAN Channel", ImVec2(280, 30))) {
                StartCanChannel();
            }
        } else {
            if (ImGui::Button("Stop CAN Channel", ImVec2(280, 30))) {
                StopCanChannel();
            }
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.4f, 1.0f), "[CAN Channel Active]");
        }
        ImGui::EndDisabled();
    }

    ImGui::Separator();

    // --- 3. Transmit Panel ---
    ImGui::BeginDisabled(!is_connected_ || !is_channel_open_);
    ImGui::Text("Transmit CAN / CAN FD Frame (Bulk OUT EP 0x02):");

    ImGui::PushItemWidth(110);
    ImGui::InputInt("CAN ID (Hex)", &tx_id_, 1, 16, ImGuiInputTextFlags_CharsHexadecimal);
    ImGui::PopItemWidth();

    ImGui::SameLine();
    ImGui::Checkbox("29-bit EXT", &tx_is_ext_);
    ImGui::SameLine();
    ImGui::Checkbox("FD Frame", &tx_is_fd_);
    ImGui::SameLine();
    ImGui::BeginDisabled(!tx_is_fd_);
    ImGui::Checkbox("BRS (Bit Rate Switch)", &tx_is_brs_);
    ImGui::EndDisabled();

    ImGui::PushItemWidth(460);
    ImGui::InputText("Payload (Hex)", tx_payload_, sizeof(tx_payload_));
    ImGui::PopItemWidth();

    ImGui::SameLine();
    if (ImGui::Button("Send Frame", ImVec2(140, 26))) {
        CanMessage msg;
        msg.id = static_cast<uint32_t>(tx_id_);
        msg.is_extended = tx_is_ext_;
        msg.is_fd = tx_is_fd_;
        msg.is_brs = tx_is_fd_ && tx_is_brs_;
        msg.is_tx_echo = false;

        size_t max_bytes = tx_is_fd_ ? 64 : 8;
        msg.payload = ParseHexPayload(tx_payload_, max_bytes);
        msg.dlc = CanProtocolHandler::BytesToDlc(static_cast<uint8_t>(msg.payload.size()));

        CanProtocolHandler protocol;
        std::vector<uint8_t> raw_usb_packet = protocol.BuildTxBuffer(msg);

        WebUsbBridge::GetInstance().TransferOut(0x02, raw_usb_packet.data(), static_cast<int>(raw_usb_packet.size()));
    }
    ImGui::EndDisabled();

    ImGui::Separator();

    // --- 4. Receive Table ---
    ImGui::Text("Captured Frames (%zu):", message_log_.size());
    ImGui::SameLine(ImGui::GetWindowWidth() - 100);
    if (ImGui::SmallButton("Clear Log")) {
        message_log_.clear();
    }

    const ImGuiTableFlags table_flags =
        ImGuiTableFlags_Borders |
        ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_SizingStretchProp;

    if (ImGui::BeginTable("CanRxTable", 5, table_flags)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Dir", ImGuiTableColumnFlags_WidthFixed, 65.0f);
        ImGui::TableSetupColumn("CAN ID", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("DLC / Len", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Data (HEX)", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (const auto& frame : message_log_) {
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            if (frame.is_tx_echo) {
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "TX-ACK");
            } else {
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 1.0f, 1.0f), "RX");
            }

            ImGui::TableSetColumnIndex(1);
            ImGui::Text(frame.is_extended ? "0x%08X" : "0x%03X", frame.id);

            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%s %s%s",
                        frame.is_extended ? "EXT" : "STD",
                        frame.is_fd ? "FD" : "2.0",
                        frame.is_brs ? " BRS" : "");

            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%u (%zu B)", frame.dlc, frame.payload.size());

            ImGui::TableSetColumnIndex(4);
            char hex_line[200] = {0};
            int offset = 0;
            for (size_t i = 0; i < frame.payload.size() && offset < 190; ++i) {
                offset += std::snprintf(hex_line + offset, sizeof(hex_line) - offset, "%02X ", frame.payload[i]);
            }
            ImGui::TextUnformatted(hex_line);
        }

        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
            ImGui::SetScrollHereY(1.0f);
        }

        ImGui::EndTable();
    }

    ImGui::End();
}
