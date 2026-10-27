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

// Transport counters reported by usb_worker.js once per second plus local batch-sequence checks
struct UsbStats {
    uint32_t rx_per_sec = 0;      // transfers completed per second (worker side)
    uint32_t rx_dropped = 0;      // transfers dropped by the worker (backlog overflow), cumulative
    uint32_t tx_dropped = 0;      // frames dropped because the TX queue was full or no device, cumulative
    uint32_t recoveries = 0;      // interface re-claims performed by the worker watchdog, cumulative
    uint32_t batch_gaps = 0;      // batches missing between worker and page (sequence gaps), per session
};

// WebUSB transport class abstracting browser-specific EM_JS calls.
// All USB I/O runs in usb_worker.js (dedicated Worker); this class only exchanges postMessage
// traffic with it and delivers batched receive data to the wasm side.
class WebUsbBridge {
public:
    using DeviceConnectedCallback = std::function<void(const UsbDeviceInfo&)>;
    using DeviceDisconnectedCallback = std::function<void()>;
    // host_time_ms is the completion time in the page's performance.now() timeline (emscripten_get_now())
    using DataReceivedCallback = std::function<void(int ep, const uint8_t* data, int length, double host_time_ms)>;
    using ControlInCallback = std::function<void(uint8_t request, const uint8_t* data, int length)>;
    using ControlOutDoneCallback = std::function<void(uint8_t request, bool ok)>;

    static WebUsbBridge& GetInstance();

    void Initialize();
    void RequestDevice();
    void Disconnect();

    // The worker keeps its IN transfers pending for the whole connection. Start/Stop only gate
    // delivery of received frames; endpoint/size are applied only if they differ from the current
    // configuration (which re-arms the pump).
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
    void SetOnControlOutDone(ControlOutDoneCallback cb) { on_control_out_done_ = cb; }

    const UsbStats& GetStats() const { return stats_; }

    // Internal handlers invoked by extern "C" functions
    void InternalOnConnected(uint16_t vid, uint16_t pid, const char* mfg, const char* prod);
    void InternalOnDisconnected();
    void InternalOnRxBatch(const uint8_t* data, int length);
    void InternalOnControlIn(uint8_t request, const uint8_t* data, int length);
    void InternalOnControlOutDone(uint8_t request, bool ok);
    void InternalOnStats(uint32_t rx_per_sec, uint32_t rx_dropped, uint32_t tx_dropped, uint32_t recoveries);

private:
    WebUsbBridge() = default;

    DeviceConnectedCallback on_connected_;
    DeviceDisconnectedCallback on_disconnected_;
    DataReceivedCallback on_data_received_;
    ControlInCallback on_control_in_;
    ControlOutDoneCallback on_control_out_done_;

    UsbStats stats_;
    bool have_batch_seq_ = false;
    uint32_t next_batch_seq_ = 0;
};