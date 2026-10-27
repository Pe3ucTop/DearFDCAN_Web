#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <string>

#include "can_wire.h"

// What the browser reports about an adapter (available as soon as the user has picked it)
struct UsbDeviceInfo {
    uint16_t vid = 0;
    uint16_t pid = 0;
    std::string manufacturer;
    std::string product;
    std::string serial;
    std::string usb_version;     // bcdUSB, e.g. "2.0.0"
    std::string device_version;  // bcdDevice, e.g. "1.0.0"
};

// Transport counters of one adapter: worker stats (once per second) plus the frame count from the snapshots
struct UsbDeviceStats {
    uint32_t rx_per_sec = 0;       // USB transfers completed per second
    uint32_t rx_errors = 0;        // failed or partial receive transfers, cumulative
    uint32_t tx_errors = 0;        // failed transmit transfers, cumulative
    uint32_t tx_dropped = 0;       // frames dropped because the TX queue was full or the adapter was closed, cumulative
    uint32_t recoveries = 0;       // interface re-claims performed by the worker watchdog, cumulative
    uint32_t frames = 0;           // CAN frames parsed for this adapter by the worker core
};

// Counters that are not tied to one adapter (carried by the snapshots)
struct UsbStats {
    uint32_t frames_total = 0;     // CAN frames parsed by the worker core, all adapters
    uint32_t trace_dropped = 0;    // trace entries the core had to discard (page too slow), cumulative
    uint32_t table_overflow = 0;   // frames of new IDs ignored because the ID table is full, cumulative
    uint32_t snapshot_gaps = 0;    // snapshots lost between worker and page (sequence gaps)
    uint32_t bad_snapshots = 0;    // snapshots rejected (layout mismatch / truncated)
};

// WebUSB transport class abstracting browser-specific EM_JS calls.
// All USB I/O and CAN processing run in usb_worker.js (dedicated Worker) and its can_core wasm module;
// this class only exchanges postMessage traffic with it and hands validated snapshots to the UI.
//
// Several adapters can be connected at once. Each one lives in a slot (device number 0..kMaxDevices-1)
// that is assigned when the user picks the adapter and stays reserved until RemoveDevice(), so a
// disconnected adapter can be reconnected without the browser chooser.
class WebUsbBridge {
public:
    using DeviceAddedCallback = std::function<void(int dev, const UsbDeviceInfo&)>;     // picked / reconnecting
    using DeviceOpenedCallback = std::function<void(int dev, const std::string& endpoints)>;
    using DeviceClosedCallback = std::function<void(int dev, const std::string& reason)>;
    using MessageCallback = std::function<void(const std::string& text)>;               // user-facing notice
    using SnapshotCallback = std::function<void(const SnapshotView&)>;
    using ControlInCallback = std::function<void(int dev, uint8_t request, const uint8_t* data, int length)>;
    using ControlOutDoneCallback = std::function<void(int dev, uint8_t request, bool ok)>;

    static WebUsbBridge& GetInstance();

    void Initialize();

    // Opens the browser's device chooser and connects the picked adapter in a free slot
    void RequestDevice();
    // Reconnects the adapter of a known slot (no chooser)
    void ConnectDevice(int dev);
    // Closes the adapter but keeps its slot
    void DisconnectDevice(int dev);
    // Closes the adapter and releases its slot
    void RemoveDevice(int dev);

    // The worker keeps the IN transfers of a connected adapter pending for the whole connection.
    // Start/Stop only gate processing of received frames; endpoint 0 / size 0 keep the detected values.
    void StartRxPump(int dev, int endpoint, int max_packet_size);
    void StopRxPump(int dev);

    // endpoint 0 = the bulk OUT endpoint detected from the descriptor
    void TransferOut(int dev, int endpoint, const uint8_t* data, int length);

    // Send a Control OUT transfer (Host -> Device on EP0)
    void ControlTransferOut(int dev, uint8_t request, uint16_t value, uint16_t index, const uint8_t* data, int length);

    // Send a Control IN transfer (Device -> Host on EP0)
    void ControlTransferIn(int dev, uint8_t request, uint16_t value, uint16_t index, uint16_t length);

    // Core state commands (all adapters). Snapshot parts produced before a clear are discarded by
    // sequence number, so cleared rows cannot reappear from snapshots that were already in flight.
    void ClearIdTable();
    void ClearTrace();
    void SetTraceEnabled(bool enable);

    void SetOnDeviceAdded(DeviceAddedCallback cb) { on_device_added_ = cb; }
    void SetOnDeviceOpened(DeviceOpenedCallback cb) { on_device_opened_ = cb; }
    void SetOnDeviceClosed(DeviceClosedCallback cb) { on_device_closed_ = cb; }
    void SetOnMessage(MessageCallback cb) { on_message_ = cb; }
    void SetOnSnapshot(SnapshotCallback cb) { on_snapshot_ = cb; }
    void SetOnControlIn(ControlInCallback cb) { on_control_in_ = cb; }
    void SetOnControlOutDone(ControlOutDoneCallback cb) { on_control_out_done_ = cb; }

    const UsbStats& GetStats() const { return stats_; }
    const UsbDeviceStats& GetDeviceStats(int dev) const;

    // Internal handlers invoked by extern "C" functions
    void InternalOnDeviceAdded(int dev, const UsbDeviceInfo& info);
    void InternalOnDeviceOpened(int dev, const char* endpoints);
    void InternalOnDeviceClosed(int dev, const char* reason);
    void InternalOnMessage(const char* text);
    void InternalOnSnapshot(const uint8_t* data, int length);
    void InternalOnControlIn(int dev, uint8_t request, const uint8_t* data, int length);
    void InternalOnControlOutDone(int dev, uint8_t request, bool ok);
    void InternalOnStats(int dev, uint32_t rx_per_sec, uint32_t tx_dropped, uint32_t recoveries,
                         uint32_t rx_errors, uint32_t tx_errors);

private:
    WebUsbBridge() = default;

    static bool ValidDevice(int dev) { return dev >= 0 && dev < kMaxDevices; }

    DeviceAddedCallback on_device_added_;
    DeviceOpenedCallback on_device_opened_;
    DeviceClosedCallback on_device_closed_;
    MessageCallback on_message_;
    SnapshotCallback on_snapshot_;
    ControlInCallback on_control_in_;
    ControlOutDoneCallback on_control_out_done_;

    UsbStats stats_;
    std::array<UsbDeviceStats, kMaxDevices> dev_stats_;
    bool have_snapshot_seq_ = false;
    uint32_t next_snapshot_seq_ = 0;
    uint32_t table_clear_sent_ = 0;
    uint32_t trace_clear_sent_ = 0;
};
