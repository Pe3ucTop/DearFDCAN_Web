#include "can_adapter.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace {

// Ids of the transfers of the start / stop sequences; the acknowledgements come back with them as `request`, which tells them apart
// from the request codes of plain control transfers (< 0x100)
constexpr int kPcanCmdIdStart = 0x100;
constexpr int kPcanCmdIdStop  = 0x200;
// SLCAN command ids are SLCAN_CMD_ID_START / SLCAN_CMD_ID_STOP / SLCAN_CMD_VERSION (can_protocol_slcan.h)
// The control transfers of the gs_usb Elmue sequences are acknowledged with ids of their own, because the same request code (MODE)
// is sent several times: a failure of a "required" one aborts the start, an "optional" one is ignored
constexpr int kGsIdRequired = 0x500;
constexpr int kGsIdOptional = 0x600;
constexpr int kGsIdQuiet    = 0x700;   // housekeeping outside of a start (mode reset at open, termination): the acknowledgement is not interesting

// Register limits of a gs_usb adapter that did not tell its own: as wide as the editor always allowed
const CanBitTimingLimits kGsLimits = {512, 128, 128, 1024};

CanBitTimingLimits GsLimitsOf(const ElmTimeLimits& t) {
    return CanBitTimingLimits{t.seg1_max, t.seg2_max, t.sjw_max, t.brp_max};
}

} // namespace

// Automatic calculation within the limits; if nothing fits gs_usb keeps the old search (it always produced some value), other
// protocols leave the previous values
void CalculateTiming(CanProtocolType protocol, uint32_t fclk_hz, uint32_t bps, float sample_point, const CanBitTimingLimits& limits,
                     GsDeviceBittiming& bt) {
    if (CanProtocolHandler::AutoCalculateBittiming(fclk_hz, bps, sample_point, limits, bt)) return;
    if (protocol == PROTOCOL_GS_USB_CANDLELIGHT) CanProtocolHandler::AutoCalculateBittiming(fclk_hz, bps, sample_point, bt);
}

// ============================================================================
// Shared helpers
// ============================================================================

CanBitTimingLimits NominalLimits(const AdapterState& st) {
    if (st.protocol == PROTOCOL_SLCAN) return st.slcan_info.valid && st.slcan_info.nominal.brp_max ? st.slcan_info.nominal : CanProtocolSlcan::DefaultNominalLimits();
    if (st.protocol == PROTOCOL_PEAK_PCAN_FD) return PcanNominalLimits();
    return st.has_bt_ext && st.bt_ext.nominal.brp_max ? GsLimitsOf(st.bt_ext.nominal) : kGsLimits;
}

CanBitTimingLimits DataLimits(const AdapterState& st) {
    if (st.protocol == PROTOCOL_SLCAN) return st.slcan_info.valid && st.slcan_info.data.brp_max ? st.slcan_info.data : CanProtocolSlcan::DefaultDataLimits();
    if (st.protocol == PROTOCOL_PEAK_PCAN_FD) return PcanDataLimits();
    return st.has_bt_ext && st.bt_ext.data.brp_max ? GsLimitsOf(st.bt_ext.data) : kGsLimits;
}

void RecalculateTimings(const AdapterState& st, ChannelConfig& cfg) {
    CalculateTiming(st.protocol, st.bt_const.fclk_can, cfg.target_nom_kbps * 1000U, cfg.target_nom_sp, NominalLimits(st), cfg.nom_bt);
    CalculateTiming(st.protocol, st.bt_const.fclk_can, cfg.target_data_kbps * 1000U, cfg.target_data_sp, DataLimits(st), cfg.data_bt);
}

void InitAdapterState(AdapterState& st) {
    // Placeholder limits until the adapter answers GS_USB_BREQ_BT_CONST
    std::memset(&st.bt_const, 0, sizeof(st.bt_const));
    st.bt_const.fclk_can  = 80000000U;   // default 80 MHz clock
    st.bt_const.tseg1_min = 1;
    st.bt_const.tseg1_max = 256;
    st.bt_const.tseg2_min = 1;
    st.bt_const.tseg2_max = 128;
    st.bt_const.sjw_max   = 128;
    st.bt_const.brp_min   = 1;
    st.bt_const.brp_max   = 512;
    st.has_bt_const = false;
    st.has_config = false;
    std::memset(&st.config, 0, sizeof(st.config));
}

void ApplyProtocolDefaults(AdapterState& st, ChannelConfig& cfg, CanProtocolType protocol) {
    st.protocol = protocol;
    if (protocol == PROTOCOL_PEAK_PCAN_FD) {
        std::memset(&st.bt_const, 0, sizeof(st.bt_const));
        st.bt_const.feature = GS_CAN_FEATURE_FD | GS_CAN_FEATURE_HW_TIMESTAMP | GS_CAN_FEATURE_LISTEN_ONLY;
        st.bt_const.fclk_can = PCAN_CAN_CLOCK_HZ;
        st.has_bt_const = true;
        cfg.enable_loopback = false;   // no internal loopback mode in this protocol
    } else if (protocol == PROTOCOL_SLCAN) {
        // The clock and the limits come from the 'V' answer of a CANable adapter; until then the usual 160 MHz of the STM32G4
        // firmware and its default limits are assumed
        st.is_serial = true;
        st.bt_const.feature = GS_CAN_FEATURE_FD | GS_CAN_FEATURE_LISTEN_ONLY | GS_CAN_FEATURE_LOOP_BACK;
        if (st.slcan_info.valid && st.slcan_info.clock_mhz) st.bt_const.fclk_can = st.slcan_info.clock_mhz * 1000000U;
        else if (!st.has_bt_const) st.bt_const.fclk_can = 160000000U;      // first time: replaces the 80 MHz placeholder
        st.has_bt_const = true;
    }
    RecalculateTimings(st, cfg);
}

SlcanDialect EffectiveDialect(const AdapterState& st, const ChannelConfig& cfg) {
    if (cfg.slcan_dialect_choice == 1) return SLCAN_DIALECT_CANABLE;
    if (cfg.slcan_dialect_choice == 2) return SLCAN_DIALECT_LAWICEL;
    return st.slcan_probed ? st.slcan_dialect : SLCAN_DIALECT_LAWICEL;
}

// ============================================================================
// CanAdapter
// ============================================================================

CanAdapter::CanAdapter(int dev, AdapterState& state, ChannelConfig& config, ITransport& transport, IAdapterListener& listener)
    : dev_(dev), st_(state), cfg_(config), tr_(transport), listener_(listener) {}

void CanAdapter::Notice(const std::string& text) {
    listener_.OnNotice("Device " + std::to_string(dev_ + 1) + ": " + text);
}

void CanAdapter::ResetBusReport() {
    st_.bus_load = -1;
    st_.bus_state = ELM_BUS_ACTIVE;
    st_.bus_rx_errors = st_.bus_tx_errors = 0;
    st_.error_reports = 0;
    st_.last_error_text.clear();
    st_.adapter_text.clear();
}

void CanAdapter::Stop() {
    tr_.StopRx();
    SendStopCommands();
    st_.channel_opening = false;
    st_.channel_open = false;
    st_.elmue_active = false;
    st_.hw_timestamps_active = false;
    st_.feedback_labels.clear();
}

// Lines of the Elmue protocol and of PEAK that are not CAN frames: bus load, bus state, text of the firmware, error reports
void CanAdapter::OnEvents(const std::string& lines) {
    size_t pos = 0;
    while (pos < lines.size()) {
        size_t end = lines.find('\n', pos);
        if (end == std::string::npos) end = lines.size();
        const std::string line = lines.substr(pos, end - pos);
        pos = end + 1;
        if (line.empty()) continue;

        const std::string body = line.substr(1);
        if (line[0] == 'U') {
            st_.bus_load = std::clamp(std::atoi(body.c_str()), 0, 100);
        } else if (line[0] == 'S') {
            st_.bus_state = static_cast<ElmBusState>(std::clamp(std::atoi(body.c_str()), 0, 3));
        } else if (line[0] == 'T') {
            st_.adapter_text = body;
        } else if (line[0] == 'E') {
            uint32_t err_id = 0;
            uint8_t data[8] = {};
            if (!CanProtocolElmue::ParseErrorEvent(body, err_id, data)) continue;
            const ElmErrorInfo info = CanProtocolElmue::DecodeError(err_id, data);
            ++st_.error_reports;
            st_.bus_rx_errors = info.rx_errors;
            st_.bus_tx_errors = info.tx_errors;
            if (info.state_known) st_.bus_state = info.state;
            if (!info.text.empty()) st_.last_error_text = info.text;
        }
    }
}

// ============================================================================
// gs_usb (candleLight / CANable), legacy and ElmueSoft protocol
// ============================================================================

namespace {

class GsUsbAdapter : public CanAdapter {
public:
    using CanAdapter::CanAdapter;
    CanProtocolType Protocol() const override { return PROTOCOL_GS_USB_CANDLELIGHT; }

    void OnOpened(const DeviceOpenInfo&) override {
        ApplyProtocolDefaults(st_, cfg_, PROTOCOL_GS_USB_CANDLELIGHT);
        st_.elmue_supported = false;
        st_.elmue_active = false;
        st_.hw_timestamps_active = false;
        st_.has_bt_ext = false;
        st_.has_board = false;
        st_.feedback_labels.clear();
        ResetBusReport();
        // query bit-timing constants and the adapter description from EP0
        tr_.ControlIn(GS_USB_BREQ_BT_CONST, 0, 0, sizeof(GsDeviceBtConst));
        tr_.ControlIn(GS_USB_BREQ_DEVICE_CONFIG, 0, 0, sizeof(GsDeviceConfig));
    }

    void OnControlIn(uint8_t request, const uint8_t* data, int length) override {
        if (request == GS_USB_BREQ_BT_CONST && length >= static_cast<int>(sizeof(GsDeviceBtConst))) {
            std::memcpy(&st_.bt_const, data, sizeof(GsDeviceBtConst));
            st_.has_bt_const = true;
            if (st_.bt_const.fclk_can > 0) RecalculateTimings(st_, cfg_);

            // An Elmue (CANable 2.5) adapter: switch its extended requests on (they need the Elmue flag in the last MODE request, which
            // a reset with the flag provides) and read what only they can tell
            if ((st_.bt_const.feature & ELM_FEATURE_PROTOCOL) && !st_.elmue_supported) {
                st_.elmue_supported = true;
                const GsDeviceMode reset = {GS_CAN_MODE_RESET, ELM_FEATURE_PROTOCOL};
                tr_.ControlOut(GS_USB_BREQ_MODE, 0, 0, reinterpret_cast<const uint8_t*>(&reset), sizeof(reset), kGsIdQuiet);
                tr_.ControlIn(ELM_BREQ_GET_BOARD_INFO, 0, 0, sizeof(ElmBoardInfo));
                if (st_.bt_const.feature & GS_CAN_FEATURE_BT_CONST_EXT) tr_.ControlIn(GS_USB_BREQ_BT_CONST_EXT, 0, 0, sizeof(GsDeviceBtConstExt));
            }
        } else if (request == GS_USB_BREQ_DEVICE_CONFIG && length >= static_cast<int>(sizeof(GsDeviceConfig))) {
            std::memcpy(&st_.config, data, sizeof(GsDeviceConfig));
            st_.has_config = true;
        } else if (request == GS_USB_BREQ_BT_CONST_EXT && length >= static_cast<int>(sizeof(GsDeviceBtConstExt))) {
            std::memcpy(&st_.bt_ext, data, sizeof(GsDeviceBtConstExt));
            st_.has_bt_ext = true;
            RecalculateTimings(st_, cfg_);
        } else if (request == ELM_BREQ_GET_BOARD_INFO) {
            st_.has_board = CanProtocolElmue::ParseBoardInfo(data, length, st_.board);
        } else if (request == ELM_BREQ_GET_LAST_ERROR && length >= 1) {
            // The adapter's verdict on the request sent before (it cannot stall the data stage to say so)
            const std::string what = st_.feedback_labels.empty() ? std::string("a command") : st_.feedback_labels.front();
            if (!st_.feedback_labels.empty()) st_.feedback_labels.erase(st_.feedback_labels.begin());
            if (data[0] != 2 /* success */ && (st_.channel_opening || st_.channel_open)) {
                Notice("the adapter rejected " + what + ": " + CanProtocolElmue::FeedbackText(data[0]));
                Stop();
            }
        }
    }

    // Control OUT results arrive in submission order. The channel counts as open only after the MODE_START request was acknowledged;
    // any failure of the start sequence rolls it back.
    void OnControlOutDone(int request, bool ok) override {
        if (request == kGsIdQuiet) return;
        if (request >= kGsIdRequired && request < kGsIdQuiet) {
            // Elmue start sequence (see StartElmue): the last required transfer is MODE_START
            if (!st_.channel_opening) return;
            if (!ok) {
                if (request < kGsIdOptional) {
                    Notice("a start command was not accepted by the adapter");
                    Stop();
                }
            } else if (request == st_.gs_start_last_id) {
                st_.channel_opening = false;
                st_.channel_open = true;
            }
            return;
        }
        if (!st_.channel_opening) return;
        if (!ok) {
            // Firmware may legitimately reject HOST_FORMAT (it is optional); everything else is fatal
            if (request != GS_USB_BREQ_HOST_FORMAT) Stop();
            return;
        }
        if (request == GS_USB_BREQ_MODE) {
            st_.channel_opening = false;
            st_.channel_open = true;
        }
    }

    void Start() override {
        if (st_.elmue_supported && cfg_.use_elmue) {
            StartElmue();
            return;
        }
        const uint32_t host_format = 0x0000BEEFU;
        tr_.ControlOut(GS_USB_BREQ_HOST_FORMAT, 1, 0, reinterpret_cast<const uint8_t*>(&host_format), sizeof(host_format));
        tr_.ControlOut(GS_USB_BREQ_BITTIMING, 0, 0, reinterpret_cast<const uint8_t*>(&cfg_.nom_bt), sizeof(cfg_.nom_bt));
        if (cfg_.enable_fd) tr_.ControlOut(GS_USB_BREQ_DATA_BITTIMING, 0, 0, reinterpret_cast<const uint8_t*>(&cfg_.data_bt), sizeof(cfg_.data_bt));

        GsDeviceMode mode_cmd = {};
        mode_cmd.mode = GS_CAN_MODE_START;
        mode_cmd.flags = 0;
        if (cfg_.enable_fd)          mode_cmd.flags |= GS_CAN_FEATURE_FD;
        if (cfg_.enable_loopback)    mode_cmd.flags |= GS_CAN_FEATURE_LOOP_BACK;
        if (cfg_.enable_listen_only) mode_cmd.flags |= GS_CAN_FEATURE_LISTEN_ONLY;
        // Ask the adapter to append its microsecond timestamp to every frame: cycle times are then computed from the adapter clock
        // instead of the (jittery) USB/browser arrival time
        st_.hw_timestamps_active = cfg_.use_hw_timestamps && (st_.bt_const.feature & GS_CAN_FEATURE_HW_TIMESTAMP);
        if (st_.hw_timestamps_active) mode_cmd.flags |= GS_CAN_FEATURE_HW_TIMESTAMP;
        tr_.ControlOut(GS_USB_BREQ_MODE, 0, 0, reinterpret_cast<const uint8_t*>(&mode_cmd), sizeof(mode_cmd));

        // endpoint and transfer size as detected by the worker; with timestamps the worker also keeps the adapter clock synchronised
        tr_.StartRx(0, 0, st_.hw_timestamps_active ? kRxFlagTimestamps : 0);
        st_.channel_opening = true;     // channel_open is set when MODE_START is acknowledged
    }

    std::vector<uint8_t> BuildTx(const CanMessage& msg) const override {
        return st_.elmue_active ? CanProtocolElmue::BuildTxBuffer(msg) : CanProtocolGsUsb::BuildTxBuffer(msg, 0);
    }

    // 120 ohm termination resistor (GS_CAN_FEATURE_TERMINATION): switched at once, and again at every channel start
    void SetTermination(bool on) override {
        cfg_.termination_on = on;
        if (!(st_.bt_const.feature & GS_CAN_FEATURE_TERMINATION)) return;
        const uint32_t value = on ? 1 : 0;
        tr_.ControlOut(GS_USB_BREQ_SET_TERMINATION, 0, 0, reinterpret_cast<const uint8_t*>(&value), sizeof(value), kGsIdQuiet);
    }

protected:
    void SendStopCommands() override {
        // An Elmue adapter keeps its extended requests (filters, bus load, board info) available only if this MODE request has the flag
        const GsDeviceMode mode_cmd = {GS_CAN_MODE_RESET, st_.elmue_supported ? ELM_FEATURE_PROTOCOL : 0u};
        tr_.ControlOut(GS_USB_BREQ_MODE, 0, 0, reinterpret_cast<const uint8_t*>(&mode_cmd), sizeof(mode_cmd), st_.elmue_supported ? kGsIdQuiet : -1);
    }

private:
    // Start of a channel in Elmue mode. The adapter keeps the Elmue requests (id >= 20) switched on only while the last MODE request
    // carried the Elmue flag, so the sequence starts with a mode reset that has it. Each request is followed by a read of the adapter's
    // verdict (ELM_BREQ_GET_LAST_ERROR), because a firmware that rejects a parameter cannot stall the data stage to tell.
    void StartElmue() {
        st_.feedback_labels.clear();
        ResetBusReport();

        int required = 0, optional = 0;
        auto out = [&](uint8_t request, const void* data, int length, bool is_required, const char* label) {
            const int id = is_required ? kGsIdRequired + required++ : kGsIdOptional + optional++;
            tr_.ControlOut(request, 0, 0, static_cast<const uint8_t*>(data), length, id);
            if (is_required && label) {
                tr_.ControlIn(ELM_BREQ_GET_LAST_ERROR, 0, 0, 1);
                st_.feedback_labels.push_back(label);
            }
            return id;
        };

        const uint32_t host_format = 0x0000BEEFU;
        out(GS_USB_BREQ_HOST_FORMAT, &host_format, sizeof(host_format), false, nullptr);
        const GsDeviceMode reset = {GS_CAN_MODE_RESET, ELM_FEATURE_PROTOCOL};
        out(GS_USB_BREQ_MODE, &reset, sizeof(reset), true, "the mode reset");
        out(GS_USB_BREQ_BITTIMING, &cfg_.nom_bt, sizeof(cfg_.nom_bt), true, "the bit timing");
        if (cfg_.enable_fd) out(GS_USB_BREQ_DATA_BITTIMING, &cfg_.data_bt, sizeof(cfg_.data_bt), true, "the data bit timing");

        if (cfg_.use_filters) {
            bool any = false;
            for (const AdapterHwFilter& f : cfg_.filters) any = any || f.enabled;
            if (any) {
                const auto clear = CanProtocolElmue::BuildFilter(ELM_FIL_HOST_CLEAR);
                out(ELM_BREQ_SET_FILTER, clear.data(), static_cast<int>(clear.size()), true, "clearing the filters");
                for (const AdapterHwFilter& f : cfg_.filters) {
                    if (!f.enabled) continue;
                    const uint32_t id_mask = f.extended ? CAN_ID_EFF_MASK : CAN_ID_SFF_MASK;
                    const auto add = CanProtocolElmue::BuildFilter(f.extended ? ELM_FIL_HOST_PASS_29 : ELM_FIL_HOST_PASS_11, f.id & id_mask, f.mask & id_mask);
                    out(ELM_BREQ_SET_FILTER, add.data(), static_cast<int>(add.size()), true, "a filter");
                }
            }
        }

        const uint8_t busload = cfg_.busload_report ? 10 : 0;               // report interval in 100 ms steps
        out(ELM_BREQ_SET_BUSLOAD_REPORT, &busload, sizeof(busload), false, nullptr);
        if (cfg_.termination_on && (st_.bt_const.feature & GS_CAN_FEATURE_TERMINATION)) {
            const uint32_t on = 1;
            out(GS_USB_BREQ_SET_TERMINATION, &on, sizeof(on), false, nullptr);
        }

        st_.hw_timestamps_active = cfg_.use_hw_timestamps && (st_.bt_const.feature & GS_CAN_FEATURE_HW_TIMESTAMP);
        GsDeviceMode start = {GS_CAN_MODE_START, CanProtocolElmue::ModeFlags(st_.bt_const.feature, st_.hw_timestamps_active)};
        if (cfg_.enable_fd)          start.flags |= GS_CAN_FEATURE_FD;
        if (cfg_.enable_loopback)    start.flags |= GS_CAN_FEATURE_LOOP_BACK;
        if (cfg_.enable_listen_only) start.flags |= GS_CAN_FEATURE_LISTEN_ONLY;
        st_.gs_start_last_id = out(GS_USB_BREQ_MODE, &start, sizeof(start), true, "the start");

        st_.elmue_active = true;
        // Messages of up to 2048 bytes (blobs); the worker resolves tx echoes and, with timestamps, keeps the adapter clock synchronised
        tr_.StartRx(0, ELM_MAX_BLOB_SIZE, kRxFlagElmue | (st_.hw_timestamps_active ? kRxFlagTimestamps : 0));
        st_.channel_opening = true;
    }
};

// ============================================================================
// PEAK PCAN-USB FD
// ============================================================================

class PcanAdapter : public CanAdapter {
public:
    using CanAdapter::CanAdapter;
    CanProtocolType Protocol() const override { return PROTOCOL_PEAK_PCAN_FD; }

    void OnOpened(const DeviceOpenInfo& open) override {
        ApplyProtocolDefaults(st_, cfg_, PROTOCOL_PEAK_PCAN_FD);
        // The worker already read the firmware info while opening: no requests to send
        st_.pcan_fw = open.Get("fw");
        st_.pcan_bl = open.Get("bl");
        st_.pcan_hw = open.Get("hw");
        st_.pcan_mcu = open.Get("mcu");
        st_.pcan_serial = open.Get("ser");
        st_.pcan_channels = open.Get("ch");
    }

    void OnControlOutDone(int request, bool ok) override {
        if (!st_.channel_opening) return;
        // The start sequence is a few command transfers; the channel is open after the last one was written
        if (request < kPcanCmdIdStart) return;
        if (!ok) {
            Stop();
        } else if (request == st_.pcan_start_last_id) {
            st_.channel_opening = false;
            st_.channel_open = true;
        }
    }

    // Command transfers (clock, options, bit timing, acceptance filter, bus on), written in order; the channel is open when the last one is done
    void Start() override {
        ResetBusReport();
        const auto transfers = CanProtocolPcan::BuildStartCommands(0, cfg_.enable_fd, cfg_.enable_listen_only, cfg_.nom_bt, cfg_.data_bt);
        for (size_t i = 0; i < transfers.size(); ++i) {
            tr_.CommandOut(kPcanCmdIdStart + static_cast<int>(i), transfers[i].data(), static_cast<int>(transfers[i].size()));
        }
        st_.pcan_start_last_id = kPcanCmdIdStart + static_cast<int>(transfers.size()) - 1;
        tr_.StartRx(0, 0, 0);
        st_.channel_opening = true;
    }

    std::vector<uint8_t> BuildTx(const CanMessage& msg) const override { return CanProtocolPcan::BuildTxBuffer(msg, 0); }

protected:
    void SendStopCommands() override {
        const auto transfers = CanProtocolPcan::BuildStopCommands(0);
        for (size_t i = 0; i < transfers.size(); ++i) {
            tr_.CommandOut(kPcanCmdIdStop + static_cast<int>(i), transfers[i].data(), static_cast<int>(transfers[i].size()));
        }
    }
};

// ============================================================================
// SLCAN (serial)
// ============================================================================

class SlcanAdapter : public CanAdapter {
public:
    using CanAdapter::CanAdapter;
    CanProtocolType Protocol() const override { return PROTOCOL_SLCAN; }

    // Ask which firmware it runs. The answer sets the dialect, clock and bit-timing limits; if there is none (or it is not understood)
    // the adapter is treated as a classic Lawicel one.
    void OnOpened(const DeviceOpenInfo&) override {
        st_.protocol = PROTOCOL_SLCAN;
        st_.slcan_probed = false;
        st_.slcan_info = SlcanVersionInfo();
        static const uint8_t kVersionCmd[] = {'V', '\r'};
        tr_.CommandOut(SLCAN_CMD_VERSION, kVersionCmd, sizeof(kVersionCmd));
    }

    // Answer to 'V'. CANable firmware: clock and limits for the bit-timing calculation; classic: just a version.
    void OnControlIn(uint8_t request, const uint8_t* data, int length) override {
        if (request != SLCAN_CMD_VERSION || length <= 0) return;
        SlcanVersionInfo v;
        if (!CanProtocolSlcan::ParseVersionLine(std::string(reinterpret_cast<const char*>(data), static_cast<size_t>(length)), v)) {
            st_.slcan_probed = true;
            st_.slcan_dialect = SLCAN_DIALECT_LAWICEL;
            return;
        }
        st_.slcan_info = v;
        st_.slcan_dialect = v.dialect;
        st_.slcan_probed = true;
        if (v.dialect == SLCAN_DIALECT_CANABLE) {
            st_.bt_const.fclk_can = v.clock_mhz * 1000000U;
            st_.has_bt_const = true;
            RecalculateTimings(st_, cfg_);
        }
    }

    void OnControlOutDone(int request, bool ok) override {
        if (request == SLCAN_CMD_VERSION) {
            if (!ok) {              // no answer to 'V': not a CANable firmware
                st_.slcan_probed = true;
                st_.slcan_dialect = SLCAN_DIALECT_LAWICEL;
            }
            return;
        }
        if (!st_.channel_opening) return;
        // The start sequence is a few command lines; every one is answered by the adapter
        if (request < SLCAN_CMD_ID_START) return;
        if (!ok) {
            Notice("the adapter rejected a start command");
            Stop();
        } else if (request == st_.slcan_start_last_id) {
            st_.channel_opening = false;
            st_.channel_open = true;
        }
    }

    // Command lines (reset, flags, bit timing, open), each answered by the adapter; the channel is open when the last one was acknowledged
    void Start() override {
        const SlcanDialect dialect = EffectiveDialect(st_, cfg_);
        const bool fd = cfg_.enable_fd && dialect == SLCAN_DIALECT_CANABLE;
        const auto cmds = CanProtocolSlcan::BuildStartCommands(dialect, fd, cfg_.enable_listen_only, cfg_.enable_loopback,
                                                               cfg_.nom_bt, cfg_.data_bt, static_cast<uint32_t>(cfg_.target_nom_kbps) * 1000U);
        if (cmds.empty()) {
            Notice("this adapter supports only the bit rates 10, 20, 50, 100, 125, 250, 500, 800 and 1000 kbps");
            return;
        }
        for (size_t i = 0; i < cmds.size(); ++i) {
            tr_.CommandOut(SLCAN_CMD_ID_START + static_cast<int>(i), reinterpret_cast<const uint8_t*>(cmds[i].data()), static_cast<int>(cmds[i].size()));
        }
        st_.slcan_start_last_id = SLCAN_CMD_ID_START + static_cast<int>(cmds.size()) - 1;
        tr_.StartRx(0, 0, 0);
        st_.channel_opening = true;
    }

    // The line depends on the dialect of the adapter; a classic one cannot send CAN FD frames (empty result)
    std::vector<uint8_t> BuildTx(const CanMessage& msg) const override {
        return CanProtocolSlcan::BuildTxBuffer(msg, EffectiveDialect(st_, cfg_));
    }

protected:
    void SendStopCommands() override {
        const auto cmds = CanProtocolSlcan::BuildStopCommands();
        for (size_t i = 0; i < cmds.size(); ++i) {
            tr_.CommandOut(SLCAN_CMD_ID_STOP + static_cast<int>(i), reinterpret_cast<const uint8_t*>(cmds[i].data()), static_cast<int>(cmds[i].size()));
        }
    }
};

} // namespace

std::unique_ptr<CanAdapter> CreateAdapter(CanProtocolType protocol, int dev, AdapterState& state, ChannelConfig& config,
                                          ITransport& transport, IAdapterListener& listener) {
    switch (protocol) {
        case PROTOCOL_PEAK_PCAN_FD: return std::unique_ptr<CanAdapter>(new PcanAdapter(dev, state, config, transport, listener));
        case PROTOCOL_SLCAN:        return std::unique_ptr<CanAdapter>(new SlcanAdapter(dev, state, config, transport, listener));
        default:                    return std::unique_ptr<CanAdapter>(new GsUsbAdapter(dev, state, config, transport, listener));
    }
}
