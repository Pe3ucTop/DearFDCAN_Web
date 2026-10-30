#pragma once
#include "can_adapter.h"
#include "can_protocol.h"
#include "can_wire.h"
#include "webusb_bridge.h"
#include <array>
#include <deque>
#include <map>
#include <vector>
#include <string>
#include <cstdint>

// Transmit target meaning "every adapter whose CAN channel is open"
constexpr int kTargetAllDevices = -1;

// Sent History item for re-selection and quick re-transmission
struct TxHistoryEntry {
    CanMessage msg;
    int target = kTargetAllDevices;   // device number or kTargetAllDevices
    int cycle_time_ms = 0;
    uint32_t sent_count = 0;
    double last_sent_sec = 0.0;
    std::string comment;
};

// Periodic transmit job (PCAN-View Transmit List item)
struct PeriodicTxJob {
    bool enabled = true;
    CanMessage msg;
    int target = kTargetAllDevices;   // device number or kTargetAllDevices
    int cycle_ms = 100;
    int auto_inc_byte_index = -1; // -1 = disabled, 0..63 = payload byte index to increment
    double next_send_ms = 0.0;
    uint64_t tx_count = 0;
    std::string comment;
};

// The transfers of one adapter's driver, sent through the page's bridge to the worker
class BridgeTransport : public ITransport {
public:
    explicit BridgeTransport(int dev = 0) : dev_(dev) {}
    void ControlOut(uint8_t request, uint16_t value, uint16_t index, const uint8_t* data, int length, int id = -1) override;
    void ControlIn(uint8_t request, uint16_t value, uint16_t index, uint16_t length) override;
    void CommandOut(int id, const uint8_t* data, int length) override;
    void StartRx(int endpoint, int size, int flags) override;
    void StopRx() override;
private:
    int dev_;
};

// One adapter slot (every adapter has its own clock, bus settings and statistics). What the adapter is and does (AdapterState, written
// by its driver) and what the user wants from it (ChannelConfig) are the two base structures from can_adapter.h, so the interface can
// show and edit them directly; the slot adds what only the interface needs. `adapter` is the driver: it refers to the state and the
// configuration inside this slot, so a slot that has a driver must stay where it is (the slots live in a fixed array).
struct DeviceSlot : AdapterState, ChannelConfig {
    using HwFilter = AdapterHwFilter;

    bool used = false;            // slot is occupied (listed in the Devices window)
    bool connecting = false;      // the worker is opening the adapter
    bool connected = false;       // USB interface claimed by the worker
    UsbDeviceInfo info;
    std::string endpoints;        // data interface endpoints as read from the descriptor
    std::string status;           // reason of the last disconnect
    int serial_baud = 115200;     // serial adapters: matters for real UARTs only

    BridgeTransport transport;
    std::unique_ptr<CanAdapter> adapter;   // the driver, created when the adapter is added (serial) or opened (USB)
};

class AppUI : public IAdapterListener {
public:
    AppUI();

    // Render all workspace windows and process periodic transmit timers
    void Render();

    // Presets the nominal/data bit rate and sample point from the page URL query string
    // (baudrate, sample_point, data_baudrate, data_sample_point); formats are in url_params.h.
    // They become the defaults of every adapter that is added afterwards.
    void ApplyUrlParameters();

    // Device events from the bridge (dev = device number / slot)
    void OnDeviceAdded(int dev, const UsbDeviceInfo& info);
    void OnDeviceOpened(int dev, const DeviceOpenInfo& open);
    void OnDeviceClosed(int dev, const std::string& reason);
    void OnNotice(const std::string& text) override;
    // Applies a snapshot from the worker core: changed ID table rows and new trace entries
    void OnSnapshot(const SnapshotView& snapshot);
    void OnControlDataReceived(int dev, uint8_t request, const uint8_t* data, int length);
    void OnControlOutDone(int dev, int request, bool ok);
    // Elmue protocol: lines from the worker core ("U<percent>", "T<text>", "E<id>:<data>")
    void OnAdapterEvents(int dev, const std::string& lines);
    // A warning (level 1) or error (2) of the transport in the worker; goes to the event log
    void OnLog(int dev, int level, const std::string& text);

private:
    void SetTermination(int dev, bool on);
    void RenderHwFilters(DeviceSlot& d);
    void StartCanChannel(int dev);
    void StopCanChannel(int dev);
    void RemoveDevice(int dev);

    // Device slots
    static bool ValidDevice(int dev) { return dev >= 0 && dev < kMaxDevices; }
    void InitDeviceDefaults(DeviceSlot& d) const;
    static void RecalculateTimings(DeviceSlot& d);
    static void ApplyProtocol(DeviceSlot& d, CanProtocolType protocol);
    // Dialect an SLCAN adapter is driven in (the user's choice, else what the probe found, else classic)
    static SlcanDialect EffectiveDialect(const DeviceSlot& d) { return ::EffectiveDialect(d, d); }
    // Gives the slot the driver of a protocol (replacing the one it had)
    void CreateDriver(int dev, CanProtocolType protocol);
    bool IsBusOn(int dev) const;
    int ConnectedDeviceCount() const;
    int BusOnDeviceCount() const;
    // "2: CANable 2.0 (S/N 1234)"
    std::string DeviceLabel(int dev) const;
    // Combo box over "all" plus every occupied slot; value is a device number or kTargetAllDevices
    void DeviceCombo(const char* label, int& value, const char* all_label, float width);

    // Monotonic time in milliseconds since startup, on the same timeline the worker core uses for
    // frame times (performance.now() minus start_time_ms_)
    double GetNowMs() const;

    // Transmit helpers. A frame goes to one adapter (or to all open ones); returns how many received it.
    int SendFrame(const CanMessage& msg, int target);
    bool TargetHasOpenChannel(int target) const;
    void TransmitSingleMessage(const CanMessage& msg, int target, bool record_in_history, int associated_cycle_ms = 0, const std::string& comment = "");
    void ProcessPeriodicJobs(double now_ms);
    CanMessage BuildComposerMessage() const;
    void LoadMessageIntoComposer(const CanMessage& msg, int target, int cycle_ms, const std::string& comment);

    // Individual window rendering methods
    float RenderMenuBar();
    void RenderHardwareSetupWindow(float x, float y, float w, float h);
    void RenderTransmitWindow(double now_ms, float x, float y, float w, float h);
    void RenderSentHistoryWindow(float x, float y, float w, float h);
    void RenderIdTableWindow(double now_ms, float x, float y, float w, float h);
    void RenderTraceWindow(float x, float y, float w, float h);
    void RenderLogWindow();

    // Event log: connections, losses and their reasons, the transport's warnings, user notices. It is kept in the browser's
    // localStorage, so what happened before a reload (or a crash of the page) can still be read.
    struct LogLine { int level; std::string text; };   // level 0 info, 1 warning, 2 error
    void AddLog(int level, int dev, const std::string& text);
    void SaveLog();
    void LoadLog();                // the lines of the previous session, once, at the first frame
    std::deque<LogLine> log_;
    bool log_loaded_ = false;
    bool log_dirty_ = false;
    double log_saved_ms_ = 0.0;
    bool show_log_window_ = false;
    bool log_autoscroll_ = true;

    double start_time_ms_;

    // Adapters
    std::array<DeviceSlot, kMaxDevices> devices_;
    DeviceSlot defaults_;          // template for a newly added adapter (URL parameters applied here)
    int selected_dev_;             // adapter shown in the details / settings pane, -1 = none
    int new_serial_baud_ = 115200; // baud rate for the next "Add Serial Port"
    std::string notice_;           // last user-facing message from the bridge
    double notice_time_ms_;

    // Layout mode: when true, windows automatically tile and resize with the browser window
    bool auto_tile_layout_;

    // Window visibility toggles
    bool show_hw_window_;
    bool show_tx_window_;
    bool show_history_window_;
    bool show_id_table_window_;
    bool show_trace_window_;

    // Dedicated Transmit Window state (Composer + Extra Parameters)
    int tx_target_;                // device number or kTargetAllDevices
    int tx_id_;
    bool tx_is_ext_;
    bool tx_is_fd_;
    bool tx_is_brs_;
    bool tx_is_rtr_;
    int tx_dlc_;
    int tx_cycle_ms_;
    int tx_burst_count_;
    int tx_auto_inc_byte_;
    char tx_payload_[256];
    char tx_comment_[64];

    // Data containers for the 4 CAN views
    std::deque<TraceEntry> rx_trace_;
    bool trace_paused_;
    bool trace_autoscroll_;
    int trace_filter_id_;
    int trace_filter_dev_;         // device number or -1 = all
    int table_filter_dev_;         // device number or -1 = all

    std::map<uint64_t, IdTableEntry> id_table_;
    std::vector<TxHistoryEntry> tx_history_;
    int selected_history_index_;
    std::vector<PeriodicTxJob> periodic_jobs_;
};
