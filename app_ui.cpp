#include "app_ui.h"
#include "webusb_bridge.h"
#include "imgui.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Parse a space-separated hexadecimal string into a byte vector
static std::vector<uint8_t> ParseHexPayload(const char* hex_str, size_t max_len) {
    std::vector<uint8_t> result;
    const char* p = hex_str;

    while (*p && result.size() < max_len) {
        while (*p == ' ' || *p == ',' || *p == '\t') {
            p++;
        }
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
      device_name_("No Device"),
      tx_id_(0x123),
      tx_is_ext_(false),
      tx_is_fd_(true) {
    std::strncpy(tx_payload_, "01 02 03 04 05 06 07 08", sizeof(tx_payload_) - 1);
    tx_payload_[sizeof(tx_payload_) - 1] = '\0';
}

void AppUI::OnDeviceConnected(const std::string& name) {
    is_connected_ = true;
    device_name_ = name.empty() ? "Unknown WebUSB CAN Device" : name;

    // Start continuous Bulk IN polling on endpoint 0x81 with 512-byte buffer
    WebUsbBridge::GetInstance().StartRxPump(0x81, 512);
}

void AppUI::OnDeviceDisconnected() {
    is_connected_ = false;
    device_name_ = "Disconnected";
}

void AppUI::OnCanMessageReceived(const CanMessage& msg) {
    // Limit the ring buffer to 500 frames to keep UI rendering fast
    if (message_log_.size() >= 500) {
        message_log_.pop_front();
    }
    message_log_.push_back(msg);
}

void AppUI::Render() {
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(780, 580), ImGuiCond_FirstUseEver);

    ImGui::Begin("Modular WebUSB CAN Analyzer");

    // --- Connection Controls ---
    if (!is_connected_) {
        if (ImGui::Button("Request WebUSB Device", ImVec2(200, 30))) {
            WebUsbBridge::GetInstance().RequestDevice();
        }
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Status: Disconnected");
    } else {
        if (ImGui::Button("Disconnect", ImVec2(140, 30))) {
            WebUsbBridge::GetInstance().StopRxPump();
            WebUsbBridge::GetInstance().Disconnect();
        }
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.4f, 1.0f), "Connected: %s", device_name_.c_str());
    }

    ImGui::Separator();

    // --- Transmit Panel ---
    ImGui::BeginDisabled(!is_connected_);
    ImGui::Text("Transmit CAN / CAN FD Frame (Bulk OUT EP 0x02):");

    ImGui::PushItemWidth(110);
    ImGui::InputInt("CAN ID (Hex)", &tx_id_, 1, 16, ImGuiInputTextFlags_CharsHexadecimal);
    ImGui::PopItemWidth();

    ImGui::SameLine();
    ImGui::Checkbox("29-bit EXT", &tx_is_ext_);
    ImGui::SameLine();
    ImGui::Checkbox("CAN FD", &tx_is_fd_);

    ImGui::PushItemWidth(420);
    ImGui::InputText("Payload (Hex)", tx_payload_, sizeof(tx_payload_));
    ImGui::PopItemWidth();

    ImGui::SameLine();
    if (ImGui::Button("Send Frame", ImVec2(130, 26))) {
        CanMessage msg;
        msg.id = static_cast<uint32_t>(tx_id_);
        msg.is_extended = tx_is_ext_;
        msg.is_fd = tx_is_fd_;

        size_t max_bytes = tx_is_fd_ ? 64 : 8;
        msg.payload = ParseHexPayload(tx_payload_, max_bytes);
        msg.dlc = CanProtocolHandler::BytesToDlc(static_cast<uint8_t>(msg.payload.size()));

        CanProtocolHandler protocol;
        std::vector<uint8_t> raw_usb_packet = protocol.BuildTxBuffer(msg);

        // Send over Bulk OUT endpoint 0x02
        WebUsbBridge::GetInstance().TransferOut(0x02, raw_usb_packet.data(), static_cast<int>(raw_usb_packet.size()));
    }
    ImGui::EndDisabled();

    ImGui::Separator();

    // --- Receive Table ---
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

    if (ImGui::BeginTable("CanRxTable", 4, table_flags)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("CAN ID", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("DLC / Len", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Data (HEX)", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (const auto& frame : message_log_) {
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::Text(frame.is_extended ? "0x%08X" : "0x%03X", frame.id);

            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%s %s", frame.is_extended ? "EXT" : "STD", frame.is_fd ? "FD" : "2.0");

            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%u (%zu B)", frame.dlc, frame.payload.size());

            ImGui::TableSetColumnIndex(3);
            char hex_line[200] = {0};
            int offset = 0;
            for (size_t i = 0; i < frame.payload.size() && offset < 190; ++i) {
                offset += std::snprintf(hex_line + offset, sizeof(hex_line) - offset, "%02X ", frame.payload[i]);
            }
            ImGui::TextUnformatted(hex_line);
        }

        // Auto-scroll to the newest frame
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
            ImGui::SetScrollHereY(1.0f);
        }

        ImGui::EndTable();
    }

    ImGui::End();
}