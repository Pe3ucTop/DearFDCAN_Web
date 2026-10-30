#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <string>

#include "can_adapter.h"      // DeviceOpenInfo, kRxFlag*
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
    bool is_serial = false;      // a Web Serial port (SLCAN adapter); vid/pid are 0 unless it is a USB serial adapter
};

// How well the adapter's clock is synchronised with the page clock (gs_usb: the worker reads the adapter's timestamp
// about once a second and the core fits a line through the samples with the shortest round trips)
struct ClockSyncStats {
    bool valid = false;            // at least one sample arrived since the channel was started
    bool pushed = false;           // the samples are records the adapter sends (PEAK): rtt_* is then the delay above the lowest, not a round trip
    uint32_t samples = 0;          // timestamp requests answered
    uint32_t accepted = 0;         // of the last 64, those with a short round trip (the ones the fit uses)
    double offset_ms = 0.0;        // adapter time -> page time at the newest sample
    double drift_ppm = 0.0;        // adapter clock rate against the page clock (+: the adapter runs fast)
    double jitter_us = 0.0;        // spread (rms) of the accepted samples around the fit
    double worst_us = 0.0;         // largest deviation of an accepted sample
    double rtt_min_ms = 0.0;       // shortest and latest round trip of a timestamp request
    double rtt_last_ms = 0.0;
};

// Transport counters of one adapter: worker stats (once per second) plus the frame count from the snapshots
struct UsbDeviceStats {
    ClockSyncStats sync;
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
    using DeviceOpenedCallback = std::function<void(int dev, const DeviceOpenInfo&)>;
    using DeviceClosedCallback = std::function<void(int dev, const std::string& reason)>;
    using MessageCallback = std::function<void(const std::string& text)>;               // user-facing notice
    using SnapshotCallback = std::function<void(const SnapshotView&)>;
    using ControlInCallback = std::function<void(int dev, uint8_t request, const uint8_t* data, int length)>;
    using ControlOutDoneCallback = std::function<void(int dev, int request, bool ok)>;
    using EventsCallback = std::function<void(int dev, const std::string& lines)>;   // Elmue bus load / text / error lines

    static WebUsbBridge& GetInstance();

    void Initialize();

    // Opens the browser's device chooser and connects the picked adapter in a free slot
    void RequestDevice();
    // Opens the browser's serial port chooser and connects the picked port in a free slot (an SLCAN adapter).
    // The baud rate matters for real UARTs; USB CDC ports ignore it.
    void RequestSerialPort(int baud);
    // Baud rate used the next time the serial port of a slot is connected
    void SetSerialBaud(int dev, int baud);
    // Reconnects the adapter of a known slot (no chooser)
    void ConnectDevice(int dev);
    // Closes the adapter but keeps its slot
    void DisconnectDevice(int dev);
    // Closes the adapter and releases its slot
    void RemoveDevice(int dev);

    // The worker keeps the IN transfers of a connected adapter pending for the whole connection.
    // Start/Stop only gate processing of received frames; endpoint 0 / size 0 keep the detected values.
    // flags (gs_usb): kRxElmue = the channel runs in Elmue mode, kRxTimestamps = it was started with adapter timestamps
    // (the worker then keeps reading the adapter clock to synchronise it)
    static constexpr int kRxElmue = kRxFlagElmue, kRxTimestamps = kRxFlagTimestamps;
    void StartRxPump(int dev, int endpoint, int max_packet_size, int flags = 0);
    void StopRxPump(int dev);

    // endpoint 0 = the bulk OUT endpoint detected from the descriptor
    void TransferOut(int dev, int endpoint, const uint8_t* data, int length);

    // Send a Control OUT transfer (Host -> Device on EP0). The acknowledgement carries `request`, or `id` if one is given
    // (>= 0): a start sequence that sends the same request several times tells the acknowledgements apart this way.
    void ControlTransferOut(int dev, uint8_t request, uint16_t value, uint16_t index, const uint8_t* data, int length, int id = -1);

    // Send a Control IN transfer (Device -> Host on EP0)
    void ControlTransferIn(int dev, uint8_t request, uint16_t value, uint16_t index, uint16_t length);

    // Send a command transfer to the adapter's command endpoint (PEAK uCAN) or a command line to a serial
    // adapter (SLCAN, '\r' terminated). It runs in the same order as the control transfers and is acknowledged
    // through the control-OUT-done callback with `id` as request. For SLCAN the acknowledge is the adapter's
    // own reply; id SLCAN_CMD_VERSION ('V') is answered with its text through the control-IN callback instead.
    void CommandOut(int dev, int id, const uint8_t* data, int length);

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
    void SetOnEvents(EventsCallback cb) { on_events_ = cb; }

    const UsbStats& GetStats() const { return stats_; }
    const UsbDeviceStats& GetDeviceStats(int dev) const;

    // Internal handlers invoked by extern "C" functions
    void InternalOnDeviceAdded(int dev, const UsbDeviceInfo& info);
    void InternalOnDeviceOpened(int dev, const char* endpoints, int protocol, const char* info);
    void InternalOnDeviceClosed(int dev, const char* reason);
    void InternalOnMessage(const char* text);
    void InternalOnSnapshot(const uint8_t* data, int length);
    void InternalOnControlIn(int dev, uint8_t request, const uint8_t* data, int length);
    void InternalOnControlOutDone(int dev, int request, bool ok);
    void InternalOnStats(int dev, uint32_t rx_per_sec, uint32_t tx_dropped, uint32_t recoveries,
                         uint32_t rx_errors, uint32_t tx_errors);
    void InternalOnClockSync(int dev, const ClockSyncStats& stats);
    void InternalOnEvents(int dev, const char* text);

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
    EventsCallback on_events_;

    UsbStats stats_;
    std::array<UsbDeviceStats, kMaxDevices> dev_stats_;
    bool have_snapshot_seq_ = false;
    uint32_t next_snapshot_seq_ = 0;
    uint32_t table_clear_sent_ = 0;
    uint32_t trace_clear_sent_ = 0;
};
