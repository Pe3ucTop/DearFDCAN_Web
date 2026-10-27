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

    static WebUsbBridge& GetInstance();

    // Initialize JavaScript bridge and setup internal state
    void Initialize();

    // Trigger browser prompt to request user permission for a USB device
    void RequestDevice();

    // Close the current USB connection
    void Disconnect();

    // Start a continuous polling loop for Bulk IN transfers on a specific endpoint
    void StartRxPump(int endpoint, int max_packet_size);

    // Stop the continuous polling loop
    void StopRxPump();

    // Send a Bulk OUT transfer
    void TransferOut(int endpoint, const uint8_t* data, int length);

    // Send a Control OUT transfer (EP0)
    void ControlTransferOut(uint8_t request_type, uint8_t request, uint16_t value, uint16_t index, const uint8_t* data, int length);

    // Callback setters for the upper layers
    void SetOnConnected(DeviceConnectedCallback cb) { on_connected_ = cb; }
    void SetOnDisconnected(DeviceDisconnectedCallback cb) { on_disconnected_ = cb; }
    void SetOnDataReceived(DataReceivedCallback cb) { on_data_received_ = cb; }

    // Internal methods invoked by extern "C" EMSCRIPTEN_KEEPALIVE functions
    void InternalOnConnected(uint16_t vid, uint16_t pid, const char* mfg, const char* prod);
    void InternalOnDisconnected();
    void InternalOnDataReceived(int ep, const uint8_t* data, int length);

private:
    WebUsbBridge() = default;
    
    DeviceConnectedCallback on_connected_;
    DeviceDisconnectedCallback on_disconnected_;
    DataReceivedCallback on_data_received_;
};