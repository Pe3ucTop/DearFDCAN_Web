#pragma once
#include "can_protocol.h"
#include <deque>
#include <string>

class AppUI {
public:
    AppUI();

    void Render();

    void OnDeviceConnected(const std::string& name);
    void OnDeviceDisconnected();
    void OnCanMessageReceived(const CanMessage& msg);
    void OnControlDataReceived(uint8_t request, const uint8_t* data, int length);

private:
    void StartCanChannel();
    void StopCanChannel();

    bool is_connected_;
    bool is_channel_open_;
    std::string device_name_;
    std::deque<CanMessage> message_log_;

    // Hardware clock & capabilities read from device
    GsDeviceBtConst bt_const_;

    // Nominal (Arbitration) Bit-Timing parameters (Prescaler, TQ segments, SJW)
    GsDeviceBittiming nom_bt_;
    int target_nom_kbps_;
    float target_nom_sp_;

    // CAN FD Data Phase Bit-Timing parameters
    bool enable_fd_mode_;
    bool enable_loopback_;
    bool enable_listen_only_;
    GsDeviceBittiming data_bt_;
    int target_data_kbps_;
    float target_data_sp_;

    // TX Frame fields
    int tx_id_;
    bool tx_is_ext_;
    bool tx_is_fd_;
    bool tx_is_brs_;
    char tx_payload_[196];
};