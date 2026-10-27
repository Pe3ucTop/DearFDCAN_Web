#pragma once
#include <cstdint>
#include <functional>
#include <string>

// Structure to hold basic USB device information
struct UsbDeviceInfo {
    uint16_t vid;
    uint16_t pid;
    std::string manufacturer;
    std::string product;
};

// WebUSB transport class abstracting browser-specific EM_JS calls
class WebUsbBridge {
public:
    using DeviceConnectedCallback = std::function<void(const UsbDeviceInfo&)>;
    using DeviceDisconnectedCallback = std::function<void()>;
    using DataReceivedCallback = std::function<void(int ep, const uint8_t* data, int length)>;
    using ControlInCallback = std::function<void(uint8_t request, const uint8_t* data, int length)>;

    static WebUsbBridge& GetInstance();

    void Initialize();
    void RequestDevice();
    void Disconnect();

    void StartRxPump(int endpoint, int max_packet_size);
    void StopRxPump();

    void TransferOut(int endpoint, const uint8_t* data, int length);

    // Send a Control OUT transfer (Host -> Device on EP0)
    void ControlTransferOut(uint8_t request, uint16_t value, uint16_t index, const uint8_t* data, int length);

    // Send a Control IN transfer (Device -> Host on EP0)
    void ControlTransferIn(uint8_t request, uint16_t value, uint16_t index, uint16_t length);

    void SetOnConnected(DeviceConnectedCallback cb) { on_connected_ = cb; }
    void SetOnDisconnected(DeviceDisconnectedCallback cb) { on_disconnected_ = cb; }
    void SetOnDataReceived(DataReceivedCallback cb) { on_data_received_ = cb; }
    void SetOnControlIn(ControlInCallback cb) { on_control_in_ = cb; }

    // Internal handlers invoked by extern "C" functions
    void InternalOnConnected(uint16_t vid, uint16_t pid, const char* mfg, const char* prod);
    void InternalOnDisconnected();
    void InternalOnDataReceived(int ep, const uint8_t* data, int length);
    void InternalOnControlIn(uint8_t request, const uint8_t* data, int length);

private:
    WebUsbBridge() = default;

    DeviceConnectedCallback on_connected_;
    DeviceDisconnectedCallback on_disconnected_;
    DataReceivedCallback on_data_received_;
    ControlInCallback on_control_in_;
};