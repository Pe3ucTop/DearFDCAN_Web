#pragma once
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

// One adapter slot: what the browser and the adapter report, its connection/channel state and its own
// bit-timing configuration (every adapter has its own clock and bus settings)
struct DeviceSlot {
    bool used = false;            // slot is occupied (listed in the Devices window)
    bool connecting = false;      // the worker is opening the adapter
    bool connected = false;       // USB interface claimed by the worker
    bool channel_opening = false; // control sequence sent, waiting for the MODE_START acknowledgement
    bool channel_open = false;    // BUS ON
    UsbDeviceInfo info;
    CanProtocolType protocol = PROTOCOL_GS_USB_CANDLELIGHT;   // known once the worker opened the adapter
    std::string endpoints;        // data interface endpoints as read from the descriptor
    std::string status;           // reason of the last disconnect

    // PEAK adapters report these in their firmware info (empty for gs_usb)
    std::string pcan_fw, pcan_bl, pcan_hw, pcan_mcu, pcan_serial, pcan_channels;
    int pcan_start_last_id = 0;   // id of the last command transfer of a pending start sequence

    // Serial (SLCAN) adapters: the port settings and what the 'V' request told about the adapter
    bool is_serial = false;       // a Web Serial port instead of a USB device
    int serial_baud = 115200;     // matters for real UARTs only
    int slcan_dialect_choice = 0; // 0 = detect with 'V', 1 = CANable firmware (CAN FD), 2 = Lawicel (classic CAN)
    bool slcan_probed = false;    // the 'V' request was answered (or failed) since the port was opened
    SlcanDialect slcan_dialect = SLCAN_DIALECT_LAWICEL;   // dialect found by the probe (used for "detect")
    SlcanVersionInfo slcan_info;
    int slcan_start_last_id = 0;  // id of the last command of a pending start sequence

    bool has_bt_const = false;    // GS_USB_BREQ_BT_CONST answered
    bool has_config = false;      // GS_USB_BREQ_DEVICE_CONFIG answered
    GsDeviceBtConst bt_const = {};
    GsDeviceConfig config = {};

    // gs_usb adapter with the ElmueSoft CANable 2.5 extensions (reported by the BT_CONST feature bits)
    struct HwFilter {             // one host acceptance filter of the adapter: passes the ids with (id & mask) == (filter & mask)
        bool enabled = false;
        bool extended = false;    // 29 bit ids; an 11 bit and a 29 bit filter cover different frames
        uint32_t id = 0;
        uint32_t mask = 0x7FF;
    };
    bool elmue_supported = false; // the adapter offers the Elmue protocol
    bool use_elmue = true;        // user's choice: use it when offered (compact messages, blobs, error reports)
    bool elmue_active = false;    // the channel was started in Elmue mode
    bool use_hw_timestamps = true;   // gs_usb: ask for adapter timestamps (and synchronise the adapter clock with the page)
    bool hw_timestamps_active = false;
    bool has_bt_ext = false;      // GS_USB_BREQ_BT_CONST_EXT answered: separate limits of the nominal and the data phase
    GsDeviceBtConstExt bt_ext = {};
    bool has_board = false;       // ELM_BREQ_GET_BOARD_INFO answered
    ElmBoardInfo board = {};
    bool termination_on = false;  // 120 ohm resistor of the adapter (only if GS_CAN_FEATURE_TERMINATION)
    bool busload_report = true;   // ask the adapter to report the bus load
    bool use_filters = false;     // program the host filters below into the adapter at the channel start
    HwFilter filters[ELM_MAX_HOST_FILTERS];
    int gs_start_last_id = 0;     // id of the last control transfer of a pending Elmue start sequence
    std::vector<std::string> feedback_labels;   // what each pending ELM_BREQ_GET_LAST_ERROR answer is about
    // What the adapter reports while the channel runs (Elmue protocol)
    int bus_load = -1;            // percent, -1 = not reported
    ElmBusState bus_state = ELM_BUS_ACTIVE;
    uint32_t bus_rx_errors = 0, bus_tx_errors = 0;
    uint32_t error_reports = 0;   // error messages received
    std::string last_error_text;  // decoded last error message
    std::string adapter_text;     // last text message of the firmware

    GsDeviceBittiming nom_bt = {};
    int target_nom_kbps = 500;
    float target_nom_sp = 87.5f;

    bool enable_fd = true;
    bool enable_loopback = false;
    bool enable_listen_only = false;
    GsDeviceBittiming data_bt = {};
    int target_data_kbps = 2000;
    float target_data_sp = 75.0f;
};

class AppUI {
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
    void OnNotice(const std::string& text);
    // Applies a snapshot from the worker core: changed ID table rows and new trace entries
    void OnSnapshot(const SnapshotView& snapshot);
    void OnControlDataReceived(int dev, uint8_t request, const uint8_t* data, int length);
    void OnControlOutDone(int dev, int request, bool ok);
    // Elmue protocol: lines from the worker core ("U<percent>", "T<text>", "E<id>:<data>")
    void OnAdapterEvents(int dev, const std::string& lines);

private:
    void StartGsUsbElmue(int dev);
    void SetTermination(int dev, bool on);
    void RenderHwFilters(DeviceSlot& d);
    void ResetBusReport(DeviceSlot& d);
    void StartCanChannel(int dev);
    void StopCanChannel(int dev);
    void RemoveDevice(int dev);

    // Device slots
    static bool ValidDevice(int dev) { return dev >= 0 && dev < kMaxDevices; }
    void InitDeviceDefaults(DeviceSlot& d) const;
    static void RecalculateTimings(DeviceSlot& d);
    static void ApplyProtocol(DeviceSlot& d, CanProtocolType protocol);
    // Dialect an SLCAN adapter is driven in (the user's choice, else what the probe found, else classic)
    static SlcanDialect EffectiveDialect(const DeviceSlot& d);
    // Protocol of the frames an adapter exchanges right now (gs_usb in Elmue mode is a protocol of its own)
    static CanProtocolType EffectiveProtocol(const DeviceSlot& d) { return d.elmue_active ? PROTOCOL_GS_USB_ELMUE : d.protocol; }
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
