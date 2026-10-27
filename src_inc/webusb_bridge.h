#pragma once
#include <cstdint>
#include <functional>
#include <string>

#include "can_wire.h"

// Structure to hold basic USB device information
struct UsbDeviceInfo {
    uint16_t vid;
    uint16_t pid;
    std::string manufacturer;
    std::string product;
};

// Transport and core counters: worker stats (once per second) plus values carried by the snapshots
struct UsbStats {
    uint32_t rx_per_sec = 0;       // USB transfers completed per second (worker side)
    uint32_t frames_total = 0;     // CAN frames parsed by the worker core
    uint32_t trace_dropped = 0;    // trace entries the core had to discard (page too slow), cumulative
    uint32_t table_overflow = 0;   // frames of new IDs ignored because the ID table is full, cumulative
    uint32_t tx_dropped = 0;       // frames dropped because the TX queue was full or no device, cumulative
    uint32_t recoveries = 0;       // interface re-claims performed by the worker watchdog, cumulative
    uint32_t snapshot_gaps = 0;    // snapshots lost between worker and page (sequence gaps)
    uint32_t bad_snapshots = 0;    // snapshots rejected (layout mismatch / truncated)
};

// WebUSB transport class abstracting browser-specific EM_JS calls.
// All USB I/O and CAN processing run in usb_worker.js (dedicated Worker) and its can_core wasm module;
// this class only exchanges postMessage traffic with it and hands validated snapshots to the UI.
class WebUsbBridge {
public:
    using DeviceConnectedCallback = std::function<void(const UsbDeviceInfo&)>;
    using DeviceDisconnectedCallback = std::function<void()>;
    using SnapshotCallback = std::function<void(const SnapshotView&)>;
    using ControlInCallback = std::function<void(uint8_t request, const uint8_t* data, int length)>;
    using ControlOutDoneCallback = std::function<void(uint8_t request, bool ok)>;

    static WebUsbBridge& GetInstance();

    void Initialize();
    void RequestDevice();
    void Disconnect();

    // The worker keeps its IN transfers pending for the whole connection. Start/Stop only gate
    // processing of received frames; endpoint/size are applied only if they differ from the current
    // configuration (which re-arms the pump).
    void StartRxPump(int endpoint, int max_packet_size);
    void StopRxPump();

    void TransferOut(int endpoint, const uint8_t* data, int length);

    // Send a Control OUT transfer (Host -> Device on EP0)
    void ControlTransferOut(uint8_t request, uint16_t value, uint16_t index, const uint8_t* data, int length);

    // Send a Control IN transfer (Device -> Host on EP0)
    void ControlTransferIn(uint8_t request, uint16_t value, uint16_t index, uint16_t length);

    // Core state commands. Snapshot parts produced before a clear are discarded by sequence number,
    // so cleared rows cannot reappear from snapshots that were already in flight.
    void ClearIdTable();
    void ClearTrace();
    void SetTraceEnabled(bool enable);

    void SetOnConnected(DeviceConnectedCallback cb) { on_connected_ = cb; }
    void SetOnDisconnected(DeviceDisconnectedCallback cb) { on_disconnected_ = cb; }
    void SetOnSnapshot(SnapshotCallback cb) { on_snapshot_ = cb; }
    void SetOnControlIn(ControlInCallback cb) { on_control_in_ = cb; }
    void SetOnControlOutDone(ControlOutDoneCallback cb) { on_control_out_done_ = cb; }

    const UsbStats& GetStats() const { return stats_; }

    // Internal handlers invoked by extern "C" functions
    void InternalOnConnected(uint16_t vid, uint16_t pid, const char* mfg, const char* prod);
    void InternalOnDisconnected();
    void InternalOnSnapshot(const uint8_t* data, int length);
    void InternalOnControlIn(uint8_t request, const uint8_t* data, int length);
    void InternalOnControlOutDone(uint8_t request, bool ok);
    void InternalOnStats(uint32_t rx_per_sec, uint32_t tx_dropped, uint32_t recoveries);

private:
    WebUsbBridge() = default;

    DeviceConnectedCallback on_connected_;
    DeviceDisconnectedCallback on_disconnected_;
    SnapshotCallback on_snapshot_;
    ControlInCallback on_control_in_;
    ControlOutDoneCallback on_control_out_done_;

    UsbStats stats_;
    bool have_snapshot_seq_ = false;
    uint32_t next_snapshot_seq_ = 0;
    uint32_t table_clear_sent_ = 0;
    uint32_t trace_clear_sent_ = 0;
};
