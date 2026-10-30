#pragma once
// Adapter drivers: everything that is knowledge of an adapter's protocol and not of the user interface. A driver learns what the
// adapter is (capabilities), brings its CAN channel on the bus and off, builds transmit frames and digests the adapter's reports.
//
// A driver talks to the hardware only through ITransport (control and command transfers, the receive pump) and reports to its
// owner through IAdapterListener. It works on two plain structures that its owner holds:
//   AdapterState   what the adapter and the channel are (written by the driver, read by the owner)
//   ChannelConfig  what the user wants (written by the owner, read by the driver)
// so a user interface can show and edit them directly. This header does not depend on Dear ImGui or on Emscripten.
#include "can_protocol.h"

#include <memory>
#include <string>
#include <vector>

// Flags of ITransport::StartRx (gs_usb): the channel runs in Elmue mode / was started with adapter timestamps. With timestamps the
// transport keeps the adapter clock synchronised with the page.
constexpr int kRxFlagElmue = 1;
constexpr int kRxFlagTimestamps = 2;

// What the worker learned when it opened an adapter
struct DeviceOpenInfo {
    std::string endpoints;                                   // endpoint list of the data interface
    CanProtocolType protocol = PROTOCOL_GS_USB_CANDLELIGHT;  // detected from the USB ids
    std::string info;                                        // protocol specific details as "key=value;key=value"

    // Value of a key of `info`, empty if it is not there (PCAN: fw, bl, hw, mcu, ser, ch, fwtype)
    std::string Get(const std::string& key) const {
        size_t pos = 0;
        while (pos < info.size()) {
            size_t end = info.find(';', pos);
            if (end == std::string::npos) end = info.size();
            const size_t eq = info.find('=', pos);
            if (eq != std::string::npos && eq < end && info.compare(pos, eq - pos, key) == 0) return info.substr(eq + 1, end - eq - 1);
            pos = end + 1;
        }
        return std::string();
    }
};

// Transfers to one adapter. Acknowledgements and answers come back through CanAdapter::OnControlOutDone / OnControlIn, in the order
// the transfers were submitted.
struct ITransport {
    virtual ~ITransport() = default;
    // Vendor control OUT. The acknowledgement carries `id` when it is >= 0, else `request`.
    virtual void ControlOut(uint8_t request, uint16_t value, uint16_t index, const uint8_t* data, int length, int id = -1) = 0;
    virtual void ControlIn(uint8_t request, uint16_t value, uint16_t index, uint16_t length) = 0;
    // Bulk command transfer (PEAK) or command line (SLCAN); acknowledged with `id`
    virtual void CommandOut(int id, const uint8_t* data, int length) = 0;
    // Starts / stops the processing of received frames (endpoint 0 / size 0: as detected by the worker); `flags`: kRxFlag*
    virtual void StartRx(int endpoint, int size, int flags) = 0;
    virtual void StopRx() = 0;
};

struct IAdapterListener {
    virtual ~IAdapterListener() = default;
    virtual void OnNotice(const std::string& text) = 0;      // a message for the user
};

// One host acceptance filter of an Elmue adapter: passes the ids with (id & mask) == (filter & mask)
struct AdapterHwFilter {
    bool enabled = false;
    bool extended = false;        // 29 bit ids; an 11 bit and a 29 bit filter cover different frames
    uint32_t id = 0;
    uint32_t mask = 0x7FF;
};

// What the user wants from an adapter and its channel
struct ChannelConfig {
    bool enable_fd = true;
    bool enable_loopback = false;
    bool enable_listen_only = false;
    GsDeviceBittiming nom_bt = {};
    int target_nom_kbps = 500;
    float target_nom_sp = 87.5f;
    GsDeviceBittiming data_bt = {};
    int target_data_kbps = 2000;
    float target_data_sp = 75.0f;

    int slcan_dialect_choice = 0;    // SLCAN: 0 = detect with 'V', 1 = CANable firmware (CAN FD), 2 = Lawicel (classic CAN)
    bool use_elmue = true;           // gs_usb: use the Elmue protocol when the adapter offers it (compact messages, blobs, error reports)
    bool use_hw_timestamps = true;   // gs_usb: ask for adapter timestamps (and synchronise the adapter clock with the page)
    bool termination_on = false;     // 120 ohm resistor of the adapter (only if GS_CAN_FEATURE_TERMINATION)
    bool busload_report = true;      // Elmue: ask the adapter to report the bus load
    bool use_filters = false;        // Elmue: program the host filters below into the adapter at the channel start
    AdapterHwFilter filters[ELM_MAX_HOST_FILTERS];
};

// What the adapter and its channel are
struct AdapterState {
    CanProtocolType protocol = PROTOCOL_GS_USB_CANDLELIGHT;
    bool is_serial = false;          // a Web Serial port instead of a USB device
    bool channel_opening = false;    // start sequence sent, waiting for the acknowledgements
    bool channel_open = false;       // BUS ON

    // gs_usb capabilities (PEAK and SLCAN fill the parts that apply: clock, features)
    bool has_bt_const = false;       // GS_USB_BREQ_BT_CONST answered
    bool has_config = false;         // GS_USB_BREQ_DEVICE_CONFIG answered
    GsDeviceBtConst bt_const = {};
    GsDeviceConfig config = {};
    bool has_bt_ext = false;         // GS_USB_BREQ_BT_CONST_EXT answered: separate limits of the nominal and the data phase
    GsDeviceBtConstExt bt_ext = {};
    bool has_board = false;          // ELM_BREQ_GET_BOARD_INFO answered
    ElmBoardInfo board = {};
    bool elmue_supported = false;    // the adapter offers the Elmue protocol
    bool elmue_active = false;       // the channel was started in Elmue mode
    bool hw_timestamps_active = false;

    // PEAK adapters report these in their firmware info
    std::string pcan_fw, pcan_bl, pcan_hw, pcan_mcu, pcan_serial, pcan_channels;

    // Serial (SLCAN) adapters: what the 'V' request told about the adapter
    bool slcan_probed = false;       // the 'V' request was answered (or failed) since the port was opened
    SlcanDialect slcan_dialect = SLCAN_DIALECT_LAWICEL;   // dialect found by the probe (used for "detect")
    SlcanVersionInfo slcan_info;

    // What the adapter reports while the channel runs (Elmue protocol, PEAK)
    int bus_load = -1;               // percent, -1 = not reported
    ElmBusState bus_state = ELM_BUS_ACTIVE;
    uint32_t bus_rx_errors = 0, bus_tx_errors = 0;
    uint32_t error_reports = 0;      // error messages received
    std::string last_error_text;     // decoded last error message
    std::string adapter_text;        // last text message of the firmware

    // bookkeeping of a pending start sequence
    int pcan_start_last_id = 0;      // id of the last command transfer
    int slcan_start_last_id = 0;
    int gs_start_last_id = 0;
    std::vector<std::string> feedback_labels;   // gs_usb Elmue: what each pending ELM_BREQ_GET_LAST_ERROR answer is about
};

// Bit-timing limits of an adapter: what an editor may enter and the automatic calculation may use
CanBitTimingLimits NominalLimits(const AdapterState& st);
CanBitTimingLimits DataLimits(const AdapterState& st);
// Automatic bit timing for a target bit rate and sample point within the limits; if nothing fits, gs_usb keeps the old unrestricted
// search (it always produced some value) and other protocols leave `bt` as it was
void CalculateTiming(CanProtocolType protocol, uint32_t fclk_hz, uint32_t bps, float sample_point, const CanBitTimingLimits& limits,
                     GsDeviceBittiming& bt);
// Calculates both bit timings of `cfg` from the targets, the adapter's clock and limits
void RecalculateTimings(const AdapterState& st, ChannelConfig& cfg);
// Placeholders for an adapter that was not asked yet (80 MHz clock, wide limits)
void InitAdapterState(AdapterState& st);
// Fixed properties of a protocol (a PEAK adapter has no constants request: 80 MHz, FD, timestamps, listen-only; a serial adapter
// assumes the CANable defaults until the 'V' answer); recalculates the timings
void ApplyProtocolDefaults(AdapterState& st, ChannelConfig& cfg, CanProtocolType protocol);
// Dialect an SLCAN adapter is driven in (the user's choice, else what the probe found, else classic)
SlcanDialect EffectiveDialect(const AdapterState& st, const ChannelConfig& cfg);

class CanAdapter {
public:
    // `dev` only appears in the user messages
    CanAdapter(int dev, AdapterState& state, ChannelConfig& config, ITransport& transport, IAdapterListener& listener);
    virtual ~CanAdapter() = default;

    virtual CanProtocolType Protocol() const = 0;

    // The worker opened the adapter: asks it what it is
    virtual void OnOpened(const DeviceOpenInfo& open) = 0;
    // Answer of a control IN (gs_usb) or of the 'V' command (SLCAN)
    virtual void OnControlIn(uint8_t request, const uint8_t* data, int length) { (void)request; (void)data; (void)length; }
    // Acknowledgement of a control OUT or command transfer, with the request or id it was sent with
    virtual void OnControlOutDone(int request, bool ok) { (void)request; (void)ok; }
    // Bus load / error / state / text lines of the worker core
    void OnEvents(const std::string& lines);

    // Brings the channel on the bus: sends the sequence and starts the receive pump; the channel is open when the last step was
    // acknowledged. The owner checks that no start is pending. Stop() also rolls a start back.
    virtual void Start() = 0;
    void Stop();

    // The bulk OUT transfer of one frame; empty if the adapter cannot send it (an FD frame through a classic SLCAN adapter)
    virtual std::vector<uint8_t> BuildTx(const CanMessage& msg) const = 0;

    // gs_usb with the termination feature: switches the resistor (the owner decides whether the adapter is connected)
    virtual void SetTermination(bool on) { cfg_.termination_on = on; }

protected:
    virtual void SendStopCommands() = 0;
    void Notice(const std::string& text);
    void ResetBusReport();

    int dev_;
    AdapterState& st_;
    ChannelConfig& cfg_;
    ITransport& tr_;
    IAdapterListener& listener_;
};

std::unique_ptr<CanAdapter> CreateAdapter(CanProtocolType protocol, int dev, AdapterState& state, ChannelConfig& config,
                                          ITransport& transport, IAdapterListener& listener);
