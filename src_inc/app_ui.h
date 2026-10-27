#pragma once
#include "can_protocol.h"
#include "can_wire.h"
#include <deque>
#include <map>
#include <vector>
#include <string>
#include <cstdint>

// Sent History item for re-selection and quick re-transmission
struct TxHistoryEntry {
    CanMessage msg;
    int cycle_time_ms = 0;
    uint32_t sent_count = 0;
    double last_sent_sec = 0.0;
    std::string comment;
};

// Periodic transmit job (PCAN-View Transmit List item)
struct PeriodicTxJob {
    bool enabled = true;
    CanMessage msg;
    int cycle_ms = 100;
    int auto_inc_byte_index = -1; // -1 = disabled, 0..63 = payload byte index to increment
    double next_send_ms = 0.0;
    uint64_t tx_count = 0;
    std::string comment;
};

class AppUI {
public:
    AppUI();

    // Render all workspace windows and process periodic transmit timers
    void Render();

    // Presets the nominal/data bit rate and sample point from the page URL query string
    // (baudrate, sample_point, data_baudrate, data_sample_point); formats are in url_params.h
    void ApplyUrlParameters();

    void OnDeviceConnected(const std::string& name);
    void OnDeviceDisconnected();
    // Applies a snapshot from the worker core: changed ID table rows and new trace entries
    void OnSnapshot(const SnapshotView& snapshot);
    void OnControlDataReceived(uint8_t request, const uint8_t* data, int length);
    void OnControlOutDone(uint8_t request, bool ok);

private:
    void StartCanChannel();
    void StopCanChannel();

    // Monotonic time in milliseconds since startup, on the same timeline the worker core uses for
    // frame times (performance.now() minus start_time_ms_)
    double GetNowMs() const;

    // Transmit helpers
    void TransmitSingleMessage(const CanMessage& msg, bool record_in_history, int associated_cycle_ms = 0, const std::string& comment = "");
    void ProcessPeriodicJobs(double now_ms);
    CanMessage BuildComposerMessage() const;
    void LoadMessageIntoComposer(const CanMessage& msg, int cycle_ms, const std::string& comment);

    // Individual window rendering methods
    float RenderMenuBar();
    void RenderHardwareSetupWindow(float x, float y, float w, float h);
    void RenderTransmitWindow(double now_ms, float x, float y, float w, float h);
    void RenderSentHistoryWindow(float x, float y, float w, float h);
    void RenderIdTableWindow(double now_ms, float x, float y, float w, float h);
    void RenderTraceWindow(float x, float y, float w, float h);

    // Connection and channel states
    bool is_connected_;
    bool is_channel_open_;
    bool channel_opening_;   // control sequence sent, waiting for the MODE_START acknowledgement
    std::string device_name_;
    double start_time_ms_;

    // Layout mode: when true, windows automatically tile and resize with the browser window
    bool auto_tile_layout_;

    // Window visibility toggles
    bool show_hw_window_;
    bool show_tx_window_;
    bool show_history_window_;
    bool show_id_table_window_;
    bool show_trace_window_;

    // Hardware clock & bit-timing state
    GsDeviceBtConst bt_const_;
    GsDeviceBittiming nom_bt_;
    int target_nom_kbps_;
    float target_nom_sp_;

    bool enable_fd_mode_;
    bool enable_loopback_;
    bool enable_listen_only_;
    GsDeviceBittiming data_bt_;
    int target_data_kbps_;
    float target_data_sp_;

    // Dedicated Transmit Window state (Composer + Extra Parameters)
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

    std::map<uint64_t, IdTableEntry> id_table_;
    std::vector<TxHistoryEntry> tx_history_;
    int selected_history_index_;
    std::vector<PeriodicTxJob> periodic_jobs_;
};