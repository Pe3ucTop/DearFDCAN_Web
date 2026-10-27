#pragma once
#include "can_protocol.h"
#include <deque>
#include <string>

// Application state and UI rendering class
class AppUI {
public:
    AppUI();
    
    // Render the main ImGui window
    void Render();
    
    // Callbacks to update UI state from the hardware layer
    void OnDeviceConnected(const std::string& name);
    void OnDeviceDisconnected();
    void OnCanMessageReceived(const CanMessage& msg);

private:
    bool is_connected_;
    std::string device_name_;
    std::deque<CanMessage> message_log_;
    
    // UI input fields state
    int tx_id_;
    bool tx_is_ext_;
    bool tx_is_fd_;
    char tx_payload_[64];
};