#include <iostream>
#include <vector>
#include <deque>
#include <string>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <cstdint>

#include <emscripten.h>
#include <emscripten/html5.h>
#include <SDL.h>
#include <SDL_opengles2.h>

#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_opengl3.h"

// ============================================================================
// 1. Candlelight (gs_usb) & ElmueSoft 2.5 Protocol Definitions (Packed)
// ============================================================================

#pragma pack(push, 1)

// USB Control Request Codes (bRequest for Vendor Interface 0)
enum eGsUsbBreq : uint8_t {
    GS_USB_BREQ_HOST_FORMAT     = 0,
    GS_USB_BREQ_BITTIMING       = 1,
    GS_USB_BREQ_MODE            = 2,
    GS_USB_BREQ_BERR            = 3,
    GS_USB_BREQ_BT_CONST        = 4,
    GS_USB_BREQ_DEVICE_CONFIG   = 5,
    GS_USB_BREQ_TIMESTAMP       = 6,
    GS_USB_BREQ_IDENTIFY        = 7,
    GS_USB_BREQ_GET_USER_ID     = 8,
    GS_USB_BREQ_SET_USER_ID     = 9,
    GS_USB_BREQ_DATA_BITTIMING  = 10,
    GS_USB_BREQ_BT_CONST_EXT    = 11,
    // ElmueSoft 2.5 specific control extensions
    ELMUE_USB_BREQ_FILTER       = 20,
    ELMUE_USB_BREQ_BUSLOAD      = 21,
    ELMUE_USB_BREQ_PIN_STATUS   = 22
};

// Feature Flags reported by the firmware in gs_device_bt_const::feature
enum eGsCanFeature : uint32_t {
    GS_CAN_FEATURE_LISTEN_ONLY      = (1 << 0),
    GS_CAN_FEATURE_LOOP_BACK        = (1 << 1),
    GS_CAN_FEATURE_TRIPLE_SAMPLE    = (1 << 2),
    GS_CAN_FEATURE_ONE_SHOT         = (1 << 3),
    GS_CAN_FEATURE_HW_TIMESTAMP     = (1 << 4),
    GS_CAN_FEATURE_IDENTIFY         = (1 << 5),
    GS_CAN_FEATURE_USER_ID          = (1 << 6),
    GS_CAN_FEATURE_PAD_PKTS_TO_MAX  = (1 << 7),
    GS_CAN_FEATURE_FD               = (1 << 8),
    GS_CAN_FEATURE_REQ_USB_QUIRK    = (1 << 9),
    GS_CAN_FEATURE_BT_CONST_EXT     = (1 << 10),
    GS_CAN_FEATURE_TERMINATION      = (1 << 11),
    GS_CAN_FEATURE_BERR_REPORTING   = (1 << 12),
    // ElmueSoft CANable 2.5 extended protocol flag
    ELMUE_CAN_FEATURE_PROTOCOL_25   = (1 << 16)
};

// CAN Channel Mode Commands (gs_device_mode::mode)
enum eGsCanMode : uint32_t {
    GS_CAN_MODE_RESET = 0,
    GS_CAN_MODE_START = 1
};

// CAN ID Flags (gs_host_frame::can_id)
constexpr uint32_t CAN_EFF_FLAG = 0x80000000U; // Extended Frame Format (29-bit)
constexpr uint32_t CAN_RTR_FLAG = 0x40000000U; // Remote Transmission Request
constexpr uint32_t CAN_ERR_FLAG = 0x20000000U; // Error Message Frame
constexpr uint32_t CAN_EFF_MASK = 0x1FFFFFFFU;
constexpr uint32_t CAN_SFF_MASK = 0x000007FFU;

// Frame Flags (gs_host_frame::flags)
constexpr uint8_t GS_CAN_FLAG_OVERFLOW = (1 << 0);
constexpr uint8_t GS_CAN_FLAG_FD       = (1 << 1); // CAN FD frame
constexpr uint8_t GS_CAN_FLAG_BRS      = (1 << 2); // Bit Rate Switch (FD fast data phase)
constexpr uint8_t GS_CAN_FLAG_ESI      = (1 << 3); // Error State Indicator

// Device Configuration Descriptor returned by GS_USB_BREQ_DEVICE_CONFIG
struct gs_device_config {
    uint8_t  reserved1;
    uint8_t  reserved2;
    uint8_t  reserved3;
    uint8_t  icount;     // Number of CAN channels minus 1 (0 = 1 channel)
    uint32_t sw_version; // Software version
    uint32_t hw_version; // Hardware version
};

// Classic Bit-timing capabilities returned by GS_USB_BREQ_BT_CONST
struct gs_device_bt_const {
    uint32_t feature;    // Bitmask of eGsCanFeature
    uint32_t fclk_can;   // CAN peripheral clock frequency in Hz (e.g., 80000000 or 160000000)
    uint32_t tseg1_min;
    uint32_t tseg1_max;
    uint32_t tseg2_min;
    uint32_t tseg2_max;
    uint32_t sjw_max;
    uint32_t brp_min;
    uint32_t brp_max;
    uint32_t brp_inc;
};

// Extended CAN FD Bit-timing capabilities returned by GS_USB_BREQ_BT_CONST_EXT
struct gs_device_bt_const_extended {
    uint32_t feature;
    uint32_t fclk_can;
    uint32_t tseg1_min;
    uint32_t tseg1_max;
    uint32_t tseg2_min;
    uint32_t tseg2_max;
    uint32_t sjw_max;
    uint32_t brp_min;
    uint32_t brp_max;
    uint32_t brp_inc;
    // Data phase ranges for CAN FD
    uint32_t dtseg1_min;
    uint32_t dtseg1_max;
    uint32_t dtseg2_min;
    uint32_t dtseg2_max;
    uint32_t dsjw_max;
    uint32_t dbrp_min;
    uint32_t dbrp_max;
    uint32_t dbrp_inc;
};

// Bit-timing configuration sent via GS_USB_BREQ_BITTIMING and GS_USB_BREQ_DATA_BITTIMING
struct gs_device_bittiming {
    uint32_t prop_seg;
    uint32_t phase_seg1;
    uint32_t phase_seg2;
    uint32_t sjw;
    uint32_t brp;
};

// Mode configuration sent via GS_USB_BREQ_MODE
struct gs_device_mode {
    uint32_t mode;  // GS_CAN_MODE_RESET or GS_CAN_MODE_START
    uint32_t flags; // Feature flags to activate
};

// Legacy gs_usb frame structure (up to 80 bytes with FD + timestamp)
struct gs_host_frame {
    uint32_t echo_id;    // 0xFFFFFFFF for RX frames, >= 0 for TX echo frames
    uint32_t can_id;     // CAN ID + CAN_EFF_FLAG / CAN_RTR_FLAG / CAN_ERR_FLAG
    uint8_t  can_dlc;    // Data Length Code (0..15)
    uint8_t  channel;    // CAN channel index (0)
    uint8_t  flags;      // GS_CAN_FLAG_FD | GS_CAN_FLAG_BRS | ...
    uint8_t  reserved;
    uint8_t  data[64];   // Payload (8 bytes for Classic CAN, 64 bytes for CAN FD)
    uint32_t timestamp_us; // Optional hardware timestamp if GS_CAN_FEATURE_HW_TIMESTAMP is active
};

// ElmueSoft 2.5 Compact Variable-Length Packet Header (2 bytes)
// Allows packing multiple CAN / CAN FD frames into a single USB Bulk transfer
struct elmue_packet_header {
    uint8_t size;        // Total byte length of this single packet in the stream
    uint8_t packet_type; // 0 = CAN RX/TX Frame, 1 = Error Report, 2 = Bus Load, etc.
};

#pragma pack(pop)

// DLC to actual byte length lookup table for CAN FD
static const uint8_t DLC_TO_BYTES[16] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64
};

static uint8_t BytesToDlc(uint8_t len) {
    if (len <= 8) return len;
    if (len <= 12) return 9;
    if (len <= 16) return 10;
    if (len <= 20) return 11;
    if (len <= 24) return 12;
    if (len <= 32) return 13;
    if (len <= 48) return 14;
    return 15;
}

// ============================================================================
// 2. C Callbacks Declarations (Invoked from JavaScript WebUSB Promises)
// ============================================================================
extern "C" {
    EMSCRIPTEN_KEEPALIVE void on_webusb_device_info(
        int vid, int pid, int active_config,
        const char* mfg_name, const char* prod_name, const char* serial_num
    );
    EMSCRIPTEN_KEEPALIVE void on_webusb_clear_tree();
    EMSCRIPTEN_KEEPALIVE void on_webusb_add_interface(
        int config_val, int iface_num, int alt_setting,
        int iface_class, int iface_subclass, int iface_proto,
        int is_claimed, const char* iface_name
    );
    EMSCRIPTEN_KEEPALIVE void on_webusb_add_endpoint(
        int config_val, int iface_num, int alt_setting,
        int ep_num, int dir_in, int ep_type, int packet_size
    );
    EMSCRIPTEN_KEEPALIVE void on_webusb_interface_claimed(int iface_num, int alt_setting, int claimed_state);
    EMSCRIPTEN_KEEPALIVE void on_webusb_ctrl_in_complete(int bRequest, const uint8_t* data, int length, int status);
    EMSCRIPTEN_KEEPALIVE void on_webusb_ctrl_out_complete(int bRequest, int bytes_sent, int status);
    EMSCRIPTEN_KEEPALIVE void on_webusb_disconnected();
    EMSCRIPTEN_KEEPALIVE void on_webusb_error(const char* err_msg);
    EMSCRIPTEN_KEEPALIVE void on_webusb_tx_complete(int ep, int bytes_sent, int status);
    EMSCRIPTEN_KEEPALIVE void on_webusb_rx_complete(int ep, const uint8_t* data, int length, int status);
}

// ============================================================================
// 3. Asynchronous JavaScript WebUSB Bridge (Control + Bulk + Continuous RX)
// ============================================================================

EM_JS(void, js_webusb_init_bridge, (), {
    if (Module.webusbHelperInitialized) return;
    Module.webusbHelperInitialized = true;
    Module.rxPumpActive = false;

    Module.allocStr = function(str) {
        const s = str || "";
        const len = lengthBytesUTF8(s) + 1;
        const ptr = _malloc(len);
        stringToUTF8(s, ptr, len);
        return ptr;
    };

    Module.sendUsbError = function(msg) {
        const ptr = Module.allocStr(msg);
        _on_webusb_error(ptr);
        _free(ptr);
    };

    Module.syncDeviceTree = function(dev) {
        if (!dev) return;
        _on_webusb_clear_tree();

        const pMfg  = Module.allocStr(dev.manufacturerName);
        const pProd = Module.allocStr(dev.productName);
        const pSer  = Module.allocStr(dev.serialNumber);
        const activeCfg = dev.configuration ? dev.configuration.configurationValue : 0;

        _on_webusb_device_info(dev.vendorId, dev.productId, activeCfg, pMfg, pProd, pSer);
        _free(pMfg);
        _free(pProd);
        _free(pSer);

        if (!dev.configurations) return;

        for (const cfg of dev.configurations) {
            const cfgVal = cfg.configurationValue;
            for (const iface of cfg.interfaces) {
                const ifaceNum = iface.interfaceNumber;
                const claimed = iface.claimed ? 1 : 0;
                for (const alt of iface.alternates) {
                    const altNum = alt.alternateSetting;
                    const pName = Module.allocStr(alt.interfaceName);
                    _on_webusb_add_interface(
                        cfgVal, ifaceNum, altNum,
                        alt.interfaceClass, alt.interfaceSubclass, alt.interfaceProtocol,
                        claimed, pName
                    );
                    _free(pName);

                    if (alt.endpoints) {
                        for (const ep of alt.endpoints) {
                            const dirIn = (ep.direction === "in") ? 1 : 0;
                            let typeCode = 0;
                            if (ep.type === "interrupt") typeCode = 1;
                            else if (ep.type === "isochronous") typeCode = 2;
                            _on_webusb_add_endpoint(
                                cfgVal, ifaceNum, altNum,
                                ep.endpointNumber, dirIn, typeCode, ep.packetSize
                            );
                        }
                    }
                }
            }
        }
    };

    // Continuous non-blocking Bulk IN polling loop for high-speed CAN bus traffic
    Module.pumpBulkIn = function(epAddr, maxLen) {
        if (!Module.activeUsbDevice || !Module.rxPumpActive) return;
        const epNum = epAddr & 0x7F;

        Module.activeUsbDevice.transferIn(epNum, maxLen)
            .then(result => {
                const status = (result.status === "ok") ? 0 : 1;
                if (result.data && result.data.byteLength > 0) {
                    const byteLen = result.data.byteLength;
                    const wasmBuf = _malloc(byteLen);
                    const srcView = new Uint8Array(result.data.buffer, result.data.byteOffset, byteLen);
                    HEAPU8.set(srcView, wasmBuf);
                    _on_webusb_rx_complete(epAddr, wasmBuf, byteLen, status);
                    _free(wasmBuf);
                }
                // Immediately re-arm next transferIn if pump is still active
                if (Module.rxPumpActive && Module.activeUsbDevice) {
                    Module.pumpBulkIn(epAddr, maxLen);
                }
            })
            .catch(err => {
                if (Module.rxPumpActive) {
                    Module.sendUsbError("RX Pump stopped: " + (err.message || err.toString()));
                    Module.rxPumpActive = false;
                }
            });
    };
});

EM_JS(void, js_webusb_request_and_open, (), {
    if (!navigator.usb) {
        Module.sendUsbError("WebUSB API is not supported in this browser.");
        return;
    }

    // Request device (supports CANable 1D50:606F or any custom USB-CAN adapter)
    navigator.usb.requestDevice({ filters: [] })
        .then(device => {
            Module.activeUsbDevice = device;
            return device.open();
        })
        .then(() => {
            const dev = Module.activeUsbDevice;
            if (dev.configuration === null && dev.configurations.length > 0) {
                return dev.selectConfiguration(dev.configurations[0].configurationValue);
            }
        })
        .then(() => {
            // Automatically claim Interface 0 (Candlelight CAN channel)
            return Module.activeUsbDevice.claimInterface(0);
        })
        .then(() => {
            _on_webusb_interface_claimed(0, 0, 1);
            Module.syncDeviceTree(Module.activeUsbDevice);
        })
        .catch(err => {
            Module.sendUsbError("Open/Claim Error: " + (err.message || err.toString()));
        });
});

// Control Transfer IN (Device -> Host) on EP0 for reading gs_usb capabilities
EM_JS(void, js_webusb_control_in, (int req_type_enum, int bRequest, int wValue, int wIndex, int length), {
    if (!Module.activeUsbDevice) return;
    const reqTypes = ["standard", "class", "vendor"];
    const rType = reqTypes[req_type_enum] || "vendor";

    Module.activeUsbDevice.controlTransferIn({
        requestType: rType,
        recipient: 'interface',
        request: bRequest,
        value: wValue,
        index: wIndex
    }, length).then(result => {
        const status = (result.status === "ok") ? 0 : 1;
        if (result.data && result.data.byteLength > 0) {
            const byteLen = result.data.byteLength;
            const wasmBuf = _malloc(byteLen);
            const srcView = new Uint8Array(result.data.buffer, result.data.byteOffset, byteLen);
            HEAPU8.set(srcView, wasmBuf);
            _on_webusb_ctrl_in_complete(bRequest, wasmBuf, byteLen, status);
            _free(wasmBuf);
        } else {
            _on_webusb_ctrl_in_complete(bRequest, 0, 0, status);
        }
    }).catch(err => {
        Module.sendUsbError("Control IN (req " + bRequest + ") Error: " + (err.message || err.toString()));
        _on_webusb_ctrl_in_complete(bRequest, 0, 0, -1);
    });
});

// Control Transfer OUT (Host -> Device) on EP0 for configuring bitrates, mode, or DFU
EM_JS(void, js_webusb_control_out, (int req_type_enum, int bRequest, int wValue, int wIndex, const uint8_t* data_ptr, int length), {
    if (!Module.activeUsbDevice) return;
    const reqTypes = ["standard", "class", "vendor"];
    const rType = reqTypes[req_type_enum] || "vendor";
    const payload = (length > 0 && data_ptr) ? HEAPU8.slice(data_ptr, data_ptr + length) : new Uint8Array(0);

    Module.activeUsbDevice.controlTransferOut({
        requestType: rType,
        recipient: 'interface',
        request: bRequest,
        value: wValue,
        index: wIndex
    }, payload).then(result => {
        const status = (result.status === "ok") ? 0 : 1;
        _on_webusb_ctrl_out_complete(bRequest, result.bytesWritten, status);
    }).catch(err => {
        Module.sendUsbError("Control OUT (req " + bRequest + ") Error: " + (err.message || err.toString()));
        _on_webusb_ctrl_out_complete(bRequest, 0, -1);
    });
});

// Enter STM32 DFU Bootloader Mode via Interface 1 (as implemented in SampleApplication C++)
EM_JS(void, js_webusb_enter_dfu_mode, (), {
    if (!Module.activeUsbDevice) return;
    const dev = Module.activeUsbDevice;
    Module.rxPumpActive = false;

    // Claim Interface 1 (DFU Runtime Interface) and send DFU_DETACH (bRequest = 0x00)
    dev.claimInterface(1)
        .then(() => {
            return dev.controlTransferOut({
                requestType: 'class',
                recipient: 'interface',
                request: 0x00, // DFU_DETACH
                value: 1000,   // Timeout in ms
                index: 1       // Interface 1
            });
        })
        .then(() => {
            return dev.close();
        })
        .then(() => {
            Module.activeUsbDevice = null;
            _on_webusb_disconnected();
        })
        .catch(err => {
            Module.sendUsbError("Enter DFU Mode failed: " + (err.message || err.toString()));
        });
});

EM_JS(void, js_webusb_set_rx_pump, (int enable, int ep_addr, int max_len), {
    if (!enable) {
        Module.rxPumpActive = false;
        return;
    }
    if (!Module.rxPumpActive && Module.activeUsbDevice) {
        Module.rxPumpActive = true;
        // Launch 2 parallel in-flight requests to avoid USB microframe gaps
        Module.pumpBulkIn(ep_addr, max_len);
        Module.pumpBulkIn(ep_addr, max_len);
    }
});

EM_JS(void, js_webusb_transfer_out, (int endpoint_addr, const uint8_t* data_ptr, int length), {
    if (!Module.activeUsbDevice) return;
    const payload = HEAPU8.slice(data_ptr, data_ptr + length);
    const epNum = endpoint_addr & 0x7F;

    Module.activeUsbDevice.transferOut(epNum, payload)
        .then(result => {
            const status = (result.status === "ok") ? 0 : 1;
            _on_webusb_tx_complete(endpoint_addr, result.bytesWritten, status);
        })
        .catch(err => {
            Module.sendUsbError("Bulk TX Error: " + (err.message || err.toString()));
            _on_webusb_tx_complete(endpoint_addr, 0, -1);
        });
});

EM_JS(void, js_webusb_disconnect, (), {
    if (!Module.activeUsbDevice) return;
    Module.rxPumpActive = false;
    const dev = Module.activeUsbDevice;
    Module.activeUsbDevice = null;

    dev.close()
        .then(() => _on_webusb_disconnected())
        .catch(() => _on_webusb_disconnected());
});

// ============================================================================
// 4. Ported C++ Application Logic & Candlelight State Machine
// ============================================================================

struct CanFrameLogEntry {
    uint32_t timestamp_us;
    uint32_t can_id;
    bool     is_extended;
    bool     is_fd;
    bool     is_brs;
    bool     is_tx_echo;
    bool     is_error;
    uint8_t  dlc;
    uint8_t  data_len;
    uint8_t  data[64];
};

struct UsbEndpointInfo {
    int     endpoint_number;
    uint8_t address;
    bool    is_in;
    int     type;
    int     packet_size;
};

struct UsbInterfaceAltInfo {
    int config_value;
    int interface_number;
    int alternate_setting;
    int interface_class;
    int interface_subclass;
    int interface_protocol;
    bool is_claimed;
    std::string interface_name;
    std::vector<UsbEndpointInfo> endpoints;
};

struct AppState {
    SDL_Window* window = nullptr;
    SDL_GLContext gl_context = nullptr;

    bool is_connected = false;
    bool is_can_open  = false;
    bool use_elmue_25_proto = false;

    // USB Metadata
    int vid = 0;
    int pid = 0;
    std::string manufacturer;
    std::string product;
    std::string serial;
    std::vector<UsbInterfaceAltInfo> interfaces;

    // Candlelight Hardware Capabilities (read via EP0 Control IN)
    bool has_capabilities = false;
    gs_device_config dev_cfg = {};
    gs_device_bt_const_extended bt_caps = {};

    // Endpoints (Candlelight standard: 0x81 IN, 0x02 OUT)
    int ep_in  = 0x81;
    int ep_out = 0x02;

    // Bitrate Configuration UI
    int nominal_bitrate_kbps = 500;  // 500 kbps arbitration
    int data_bitrate_kbps    = 2000; // 2000 kbps CAN FD data phase
    bool enable_fd           = true;
    bool enable_hw_timestamp = true;
    bool enable_loopback     = false;
    bool enable_listen_only  = false;

    // TX Frame Builder UI
    int  tx_can_id      = 0x123;
    bool tx_extended_id = false;
    bool tx_fd_frame    = true;
    bool tx_brs         = true;
    char tx_hex_payload[196] = "01 02 03 04 05 06 07 08";

    // Captured CAN frames & diagnostic logs
    std::deque<CanFrameLogEntry> can_frames;
    std::vector<std::string> log_messages;

    void AddLog(const char* fmt, ...) {
        char buf[512];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        log_messages.push_back(std::string(buf));
    }

    // Calculate bit-timing segments for a target bitrate and sample point (~87.5% or ~75%)
    bool CalculateBittiming(uint32_t fclk_hz, int bitrate_kbps, bool is_data_phase, gs_device_bittiming& out_bt) {
        if (fclk_hz == 0 || bitrate_kbps <= 0) return false;
        const uint32_t target_bps = (uint32_t)bitrate_kbps * 1000U;

        // Typical total time quanta per bit: 16..20 for nominal, 8..10 for FD data phase
        const uint32_t preferred_tq = is_data_phase ? 10U : 16U;
        uint32_t total_tq = preferred_tq;
        uint32_t brp = fclk_hz / (target_bps * total_tq);

        if (brp == 0) {
            brp = 1;
            total_tq = fclk_hz / target_bps;
        }
        if (total_tq < 4) return false;

        // Place Sample Point at ~80% for Nominal, ~70% for FD Data Phase (Elmue recommendation for TDC/SSP)
        uint32_t tseg1 = (total_tq * (is_data_phase ? 70U : 80U)) / 100U - 1U;
        if (tseg1 < 1) tseg1 = 1;
        uint32_t tseg2 = total_tq - 1U - tseg1;
        if (tseg2 < 1) {
            tseg2 = 1;
            tseg1 = total_tq - 2U;
        }

        out_bt.prop_seg   = tseg1 / 2;
        out_bt.phase_seg1 = tseg1 - out_bt.prop_seg;
        out_bt.phase_seg2 = tseg2;
        out_bt.sjw        = (tseg2 > 4) ? 4 : tseg2;
        out_bt.brp        = brp;
        return true;
    }

    // Request adapter capabilities via EP0 (ported from Candlelight::Connect in SampleApplication C++)
    void QueryCandlelightCapabilities() {
        AddLog("[GS_USB] Querying device configuration and bit-timing constants over EP0...");
        // 2 = 'vendor' requestType
        js_webusb_control_in(2, GS_USB_BREQ_DEVICE_CONFIG, 0, 0, sizeof(gs_device_config));
        js_webusb_control_in(2, GS_USB_BREQ_BT_CONST, 0, 0, sizeof(gs_device_bt_const));
    }

    // Start CAN channel (ported from Candlelight::Start in SampleApplication C++)
    void StartCanChannel() {
        uint32_t clk = bt_caps.fclk_can ? bt_caps.fclk_can : 80000000U; // Fallback 80 MHz if not read yet

        gs_device_bittiming nom_bt = {};
        if (!CalculateBittiming(clk, nominal_bitrate_kbps, false, nom_bt)) {
            AddLog("[ERR] Invalid Nominal Bitrate (%d kbps) for clock %u Hz", nominal_bitrate_kbps, clk);
            return;
        }

        // 1. Send Host Format (Little Endian 0x0000beef)
        uint32_t host_fmt = 0x0000BEEFU;
        js_webusb_control_out(2, GS_USB_BREQ_HOST_FORMAT, 1, 0, (const uint8_t*)&host_fmt, sizeof(host_fmt));

        // 2. Send Nominal Bit-timing
        js_webusb_control_out(2, GS_USB_BREQ_BITTIMING, 0, 0, (const uint8_t*)&nom_bt, sizeof(nom_bt));
        AddLog("[CAN] Nominal Timing (%d kbps): BRP=%u, Prop=%u, Seg1=%u, Seg2=%u, SJW=%u",
               nominal_bitrate_kbps, nom_bt.brp, nom_bt.prop_seg, nom_bt.phase_seg1, nom_bt.phase_seg2, nom_bt.sjw);

        // 3. Send Data Phase Bit-timing if CAN FD is enabled and supported
        if (enable_fd && (bt_caps.feature & GS_CAN_FEATURE_FD)) {
            gs_device_bittiming data_bt = {};
            if (CalculateBittiming(clk, data_bitrate_kbps, true, data_bt)) {
                js_webusb_control_out(2, GS_USB_BREQ_DATA_BITTIMING, 0, 0, (const uint8_t*)&data_bt, sizeof(data_bt));
                AddLog("[CAN] FD Data Timing (%d kbps): BRP=%u, Prop=%u, Seg1=%u, Seg2=%u, SJW=%u",
                       data_bitrate_kbps, data_bt.brp, data_bt.prop_seg, data_bt.phase_seg1, data_bt.phase_seg2, data_bt.sjw);
            }
        }

        // 4. Build Mode Flags and Start Channel
        gs_device_mode mode_cmd = {};
        mode_cmd.mode  = GS_CAN_MODE_START;
        mode_cmd.flags = 0;

        if (enable_hw_timestamp && (bt_caps.feature & GS_CAN_FEATURE_HW_TIMESTAMP))
            mode_cmd.flags |= GS_CAN_FEATURE_HW_TIMESTAMP;
        if (enable_fd && (bt_caps.feature & GS_CAN_FEATURE_FD))
            mode_cmd.flags |= GS_CAN_FEATURE_FD;
        if (enable_loopback)
            mode_cmd.flags |= GS_CAN_FEATURE_LOOP_BACK;
        if (enable_listen_only)
            mode_cmd.flags |= GS_CAN_FEATURE_LISTEN_ONLY;
        if (use_elmue_25_proto && (bt_caps.feature & ELMUE_CAN_FEATURE_PROTOCOL_25))
            mode_cmd.flags |= ELMUE_CAN_FEATURE_PROTOCOL_25;

        js_webusb_control_out(2, GS_USB_BREQ_MODE, 0, 0, (const uint8_t*)&mode_cmd, sizeof(mode_cmd));

        // 5. Start non-blocking Bulk IN RX pump
        js_webusb_set_rx_pump(1, ep_in, 512);
        is_can_open = true;
        AddLog("[CAN] Channel started (Flags: 0x%08X). Continuous RX Pump active on EP 0x%02X.", mode_cmd.flags, ep_in);
    }

    // Stop CAN channel
    void StopCanChannel() {
        js_webusb_set_rx_pump(0, ep_in, 512);
        gs_device_mode mode_cmd = { GS_CAN_MODE_RESET, 0 };
        js_webusb_control_out(2, GS_USB_BREQ_MODE, 0, 0, (const uint8_t*)&mode_cmd, sizeof(mode_cmd));
        is_can_open = false;
        AddLog("[CAN] Channel reset and RX Pump stopped.");
    }

    // Parse hexadecimal string into byte buffer
    int ParseHexString(const char* hex_str, uint8_t* out_bytes, int max_bytes) {
        int count = 0;
        const char* p = hex_str;
        while (*p && count < max_bytes) {
            while (*p == ' ' || *p == ',' || *p == '\t') p++;
            if (!*p) break;
            char* endptr = nullptr;
            unsigned long val = strtoul(p, &endptr, 16);
            if (endptr == p) break;
            out_bytes[count++] = (uint8_t)(val & 0xFF);
            p = endptr;
        }
        return count;
    }

    // Send a CAN / CAN FD frame over Bulk OUT
    void SendCanFrame() {
        if (!is_connected || !is_can_open) return;

        uint8_t raw_payload[64] = {0};
        int payload_len = ParseHexString(tx_hex_payload, raw_payload, tx_fd_frame ? 64 : 8);

        gs_host_frame frame = {};
        frame.echo_id = 0x00000000U; // Request TX Echo from firmware upon bus ACK
        frame.can_id  = (uint32_t)tx_can_id & (tx_extended_id ? CAN_EFF_MASK : CAN_SFF_MASK);
        if (tx_extended_id) frame.can_id |= CAN_EFF_FLAG;

        frame.channel = 0;
        frame.flags   = 0;
        if (tx_fd_frame) {
            frame.flags |= GS_CAN_FLAG_FD;
            if (tx_brs) frame.flags |= GS_CAN_FLAG_BRS;
        }

        frame.can_dlc = BytesToDlc((uint8_t)payload_len);
        memcpy(frame.data, raw_payload, payload_len);

        // Determine transfer size based on active protocol mode (Classic 2.0 vs FD)
        size_t transfer_size = tx_fd_frame ? sizeof(gs_host_frame) - sizeof(uint32_t) : 20U;
        js_webusb_transfer_out(ep_out, (const uint8_t*)&frame, (int)transfer_size);
    }

    // Process incoming Bulk IN buffer (handles both Legacy gs_host_frame and ElmueSoft multi-packet stream)
    void ProcessBulkRxData(const uint8_t* data, int length) {
        if (length <= 0 || !data) return;

        // If ElmueSoft 2.5 variable-length multi-packet protocol is active
        if (use_elmue_25_proto) {
            int offset = 0;
            while (offset + (int)sizeof(elmue_packet_header) <= length) {
                const auto* hdr = reinterpret_cast<const elmue_packet_header*>(data + offset);
                if (hdr->size < sizeof(elmue_packet_header) || offset + hdr->size > length) break;

                // Packet type 0: Compact CAN frame
                if (hdr->packet_type == 0 && hdr->size >= 8) {
                    CanFrameLogEntry entry = {};
                    const uint8_t* p = data + offset + 2;
                    memcpy(&entry.can_id, p, 4);
                    entry.is_extended = (entry.can_id & CAN_EFF_FLAG) != 0;
                    entry.is_error    = (entry.can_id & CAN_ERR_FLAG) != 0;
                    uint8_t flags     = p[4];
                    entry.dlc         = p[5] & 0x0F;
                    entry.is_fd       = (flags & GS_CAN_FLAG_FD) != 0;
                    entry.is_brs      = (flags & GS_CAN_FLAG_BRS) != 0;
                    entry.data_len    = DLC_TO_BYTES[entry.dlc];

                    int avail_data = hdr->size - 8;
                    if (avail_data > 0) {
                        int copy_len = (avail_data < entry.data_len) ? avail_data : entry.data_len;
                        memcpy(entry.data, p + 6, copy_len);
                    }
                    PushFrameLog(entry);
                } else {
                    AddLog("[ELMUE 2.5] Event Packet Type %u (%u bytes)", hdr->packet_type, hdr->size);
                }
                offset += hdr->size;
            }
            return;
        }

        // Legacy gs_usb frame decoding (20/24 bytes for Classic CAN, 76/80 bytes for CAN FD)
        if (length >= 20) {
            const auto* f = reinterpret_cast<const gs_host_frame*>(data);
            CanFrameLogEntry entry = {};
            entry.is_tx_echo  = (f->echo_id != 0xFFFFFFFFU);
            entry.can_id      = f->can_id;
            entry.is_extended = (f->can_id & CAN_EFF_FLAG) != 0;
            entry.is_error    = (f->can_id & CAN_ERR_FLAG) != 0;
            entry.is_fd       = (f->flags & GS_CAN_FLAG_FD) != 0;
            entry.is_brs      = (f->flags & GS_CAN_FLAG_BRS) != 0;
            entry.dlc         = f->can_dlc & 0x0F;
            entry.data_len    = entry.is_fd ? DLC_TO_BYTES[entry.dlc] : ((entry.dlc > 8) ? 8 : entry.dlc);
            memcpy(entry.data, f->data, entry.data_len);

            // Extract hardware timestamp if present at the end of the frame
            if (length == 24) {
                memcpy(&entry.timestamp_us, data + 20, sizeof(uint32_t));
            } else if (length >= 80) {
                memcpy(&entry.timestamp_us, data + 76, sizeof(uint32_t));
            }
            PushFrameLog(entry);
        }
    }

    void PushFrameLog(const CanFrameLogEntry& entry) {
        if (can_frames.size() >= 500) {
            can_frames.pop_front();
        }
        can_frames.push_back(entry);
    }
};

static AppState g_App;

// ============================================================================
// 5. Implementation of C Callbacks from WebUSB
// ============================================================================
extern "C" {

EMSCRIPTEN_KEEPALIVE void on_webusb_clear_tree() {
    g_App.interfaces.clear();
}

EMSCRIPTEN_KEEPALIVE void on_webusb_device_info(
    int vid, int pid, int active_config,
    const char* mfg_name, const char* prod_name, const char* serial_num)
{
    bool first_connect = !g_App.is_connected;
    g_App.is_connected = true;
    g_App.vid = vid;
    g_App.pid = pid;
    g_App.manufacturer = mfg_name ? mfg_name : "";
    g_App.product      = prod_name ? prod_name : "";
    g_App.serial       = serial_num ? serial_num : "";

    if (first_connect) {
        g_App.AddLog("[USB] Connected to %s %s (VID: 0x%04X, PID: 0x%04X, S/N: %s)",
                     g_App.manufacturer.c_str(), g_App.product.c_str(), vid, pid, g_App.serial.c_str());
        // Immediately query Candlelight descriptors on EP0
        g_App.QueryCandlelightCapabilities();
    }
}

EMSCRIPTEN_KEEPALIVE void on_webusb_add_interface(
    int config_val, int iface_num, int alt_setting,
    int iface_class, int iface_subclass, int iface_proto,
    int is_claimed, const char* iface_name)
{
    UsbInterfaceAltInfo info;
    info.config_value       = config_val;
    info.interface_number   = iface_num;
    info.alternate_setting  = alt_setting;
    info.interface_class    = iface_class;
    info.interface_subclass = iface_subclass;
    info.interface_protocol = iface_proto;
    info.is_claimed         = (is_claimed != 0);
    info.interface_name     = (iface_name && strlen(iface_name) > 0) ? iface_name : "Interface";
    g_App.interfaces.push_back(info);
}

EMSCRIPTEN_KEEPALIVE void on_webusb_add_endpoint(
    int config_val, int iface_num, int alt_setting,
    int ep_num, int dir_in, int ep_type, int packet_size)
{
    for (auto& iface : g_App.interfaces) {
        if (iface.config_value == config_val &&
            iface.interface_number == iface_num &&
            iface.alternate_setting == alt_setting)
        {
            UsbEndpointInfo ep;
            ep.endpoint_number = ep_num;
            ep.is_in           = (dir_in != 0);
            ep.address         = ep.is_in ? (uint8_t)(ep_num | 0x80) : (uint8_t)(ep_num & 0x7F);
            ep.type            = ep_type;
            ep.packet_size     = packet_size;
            iface.endpoints.push_back(ep);

            if (iface_num == 0) {
                if (ep.is_in) g_App.ep_in = ep.address;
                else          g_App.ep_out = ep.address;
            }
            break;
        }
    }
}

EMSCRIPTEN_KEEPALIVE void on_webusb_interface_claimed(int iface_num, int alt_setting, int claimed_state) {
    g_App.AddLog("[USB] Interface #%d (Alt %d) %s.",
                 iface_num, alt_setting, claimed_state ? "claimed" : "released");
}

EMSCRIPTEN_KEEPALIVE void on_webusb_ctrl_in_complete(int bRequest, const uint8_t* data, int length, int status) {
    if (status != 0 || !data || length <= 0) return;

    if (bRequest == GS_USB_BREQ_DEVICE_CONFIG && length >= (int)sizeof(gs_device_config)) {
        memcpy(&g_App.dev_cfg, data, sizeof(gs_device_config));
        g_App.AddLog("[GS_USB] Channels: %u, SW Version: %u, HW Version: %u",
                     g_App.dev_cfg.icount + 1, g_App.dev_cfg.sw_version, g_App.dev_cfg.hw_version);
    }
    else if (bRequest == GS_USB_BREQ_BT_CONST && length >= (int)sizeof(gs_device_bt_const)) {
        memcpy(&g_App.bt_caps, data, sizeof(gs_device_bt_const));
        g_App.has_capabilities = true;
        g_App.AddLog("[GS_USB] CAN Clock: %u MHz, Feature Mask: 0x%08X (FD: %s, Elmue2.5: %s)",
                     g_App.bt_caps.fclk_can / 1000000U,
                     g_App.bt_caps.feature,
                     (g_App.bt_caps.feature & GS_CAN_FEATURE_FD) ? "Yes" : "No",
                     (g_App.bt_caps.feature & ELMUE_CAN_FEATURE_PROTOCOL_25) ? "Yes" : "No");

        // If device supports extended CAN FD timing constants, query them automatically
        if (g_App.bt_caps.feature & GS_CAN_FEATURE_BT_CONST_EXT) {
            js_webusb_control_in(2, GS_USB_BREQ_BT_CONST_EXT, 0, 0, sizeof(gs_device_bt_const_extended));
        }
    }
    else if (bRequest == GS_USB_BREQ_BT_CONST_EXT && length >= (int)sizeof(gs_device_bt_const_extended)) {
        memcpy(&g_App.bt_caps, data, sizeof(gs_device_bt_const_extended));
        g_App.AddLog("[GS_USB] Extended CAN FD Bit-Timing ranges received.");
    }
}

EMSCRIPTEN_KEEPALIVE void on_webusb_ctrl_out_complete(int bRequest, int bytes_sent, int status) {
    if (status != 0) {
        g_App.AddLog("[ERR] Control OUT bRequest %d failed.", bRequest);
    }
}

EMSCRIPTEN_KEEPALIVE void on_webusb_disconnected() {
    g_App.is_connected     = false;
    g_App.is_can_open      = false;
    g_App.has_capabilities = false;
    g_App.interfaces.clear();
    g_App.AddLog("[USB] Device disconnected.");
}

EMSCRIPTEN_KEEPALIVE void on_webusb_error(const char* err_msg) {
    g_App.AddLog("[ERR] %s", err_msg);
}

EMSCRIPTEN_KEEPALIVE void on_webusb_tx_complete(int ep, int bytes_sent, int status) {
    if (status != 0) {
        g_App.AddLog("[ERR] Bulk TX on EP 0x%02X failed.", ep);
    }
}

EMSCRIPTEN_KEEPALIVE void on_webusb_rx_complete(int ep, const uint8_t* data, int length, int status) {
    if (status == 0 && length > 0 && data != nullptr) {
        g_App.ProcessBulkRxData(data, length);
    }
}

} // extern "C"

// ============================================================================
// 6. Dear ImGui Interface Rendering
// ============================================================================

void main_loop() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        ImGui_ImplSDL2_ProcessEvent(&event);
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(940, 720), ImGuiCond_FirstUseEver);

    ImGui::Begin("CANable 2.5 (Candlelight / ElmueSoft) WebUSB Analyzer");

    // --- 1. Connection & DFU Bar ---
    if (!g_App.is_connected) {
        if (ImGui::Button("Connect CANable (WebUSB)", ImVec2(220, 30))) {
            js_webusb_request_and_open();
        }
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Disconnected");
    } else {
        if (ImGui::Button("Disconnect", ImVec2(120, 30))) {
            if (g_App.is_can_open) g_App.StopCanChannel();
            js_webusb_disconnect();
        }
        ImGui::SameLine();
        if (ImGui::Button("Enter STM32 DFU Mode (Interface 1)", ImVec2(250, 30))) {
            g_App.AddLog("[DFU] Sending DFU_DETACH to Interface 1...");
            js_webusb_enter_dfu_mode();
        }
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.4f, 1.0f), "Connected: %s (0x%04X:0x%04X) | CAN Clock: %u MHz",
                           g_App.product.c_str(), g_App.vid, g_App.pid,
                           g_App.bt_caps.fclk_can / 1000000U);
    }

    ImGui::Separator();

    // --- 2. CAN / CAN FD Channel Configuration ---
    ImGui::BeginDisabled(!g_App.is_connected);
    if (ImGui::CollapsingHeader("CAN / CAN FD Channel Setup (EP0 Control)", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::BeginDisabled(g_App.is_can_open);

        ImGui::PushItemWidth(110);
        ImGui::InputInt("Nominal (kbps)", &g_App.nominal_bitrate_kbps, 50, 250);
        ImGui::SameLine();
        ImGui::InputInt("FD Data (kbps)", &g_App.data_bitrate_kbps, 500, 1000);
        ImGui::PopItemWidth();

        ImGui::Checkbox("CAN FD Mode", &g_App.enable_fd);
        ImGui::SameLine();
        ImGui::Checkbox("HW Timestamp", &g_App.enable_hw_timestamp);
        ImGui::SameLine();
        ImGui::Checkbox("Loopback", &g_App.enable_loopback);
        ImGui::SameLine();
        ImGui::Checkbox("Listen Only", &g_App.enable_listen_only);
        ImGui::SameLine();
        ImGui::Checkbox("ElmueSoft 2.5 Compact Protocol", &g_App.use_elmue_25_proto);

        ImGui::EndDisabled();

        if (!g_App.is_can_open) {
            if (ImGui::Button("Open CAN Channel & Start RX Pump", ImVec2(260, 28))) {
                g_App.StartCanChannel();
            }
        } else {
            if (ImGui::Button("Stop CAN Channel", ImVec2(260, 28))) {
                g_App.StopCanChannel();
            }
        }
    }

    // --- 3. Transmit CAN / CAN FD Frame ---
    if (ImGui::CollapsingHeader("Transmit Frame (Bulk OUT EP 0x02)", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::BeginDisabled(!g_App.is_can_open);

        ImGui::PushItemWidth(100);
        ImGui::InputInt("CAN ID (Hex)", &g_App.tx_can_id, 1, 16, ImGuiInputTextFlags_CharsHexadecimal);
        ImGui::PopItemWidth();
        ImGui::SameLine();
        ImGui::Checkbox("29-bit EXT", &g_App.tx_extended_id);
        ImGui::SameLine();
        ImGui::Checkbox("FD", &g_App.tx_fd_frame);
        ImGui::SameLine();
        ImGui::Checkbox("BRS", &g_App.tx_brs);

        ImGui::PushItemWidth(460);
        ImGui::InputText("Data (Hex)", g_App.tx_hex_payload, sizeof(g_App.tx_hex_payload));
        ImGui::PopItemWidth();
        ImGui::SameLine();
        if (ImGui::Button("Send CAN Frame", ImVec2(150, 26))) {
            g_App.SendCanFrame();
        }

        ImGui::EndDisabled();
    }
    ImGui::EndDisabled();

    ImGui::Separator();

    // --- 4. Real-Time CAN Bus Traffic Table ---
    ImGui::Text("CAN Traffic Monitor (%zu frames):", g_App.can_frames.size());
    ImGui::SameLine(ImGui::GetWindowWidth() - 110);
    if (ImGui::SmallButton("Clear Frames")) {
        g_App.can_frames.clear();
    }

    if (ImGui::BeginTable("CanTrafficTable", 6,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
        ImVec2(0, 240)))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Time (us)", ImGuiTableColumnFlags_WidthFixed, 95.0f);
        ImGui::TableSetupColumn("Dir", ImGuiTableColumnFlags_WidthFixed, 55.0f);
        ImGui::TableSetupColumn("CAN ID", ImGuiTableColumnFlags_WidthFixed, 95.0f);
        ImGui::TableSetupColumn("Flags", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Len", ImGuiTableColumnFlags_WidthFixed, 45.0f);
        ImGui::TableSetupColumn("Payload (HEX)", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (const auto& f : g_App.can_frames) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%u", f.timestamp_us);

            ImGui::TableSetColumnIndex(1);
            if (f.is_error) {
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "ERR");
            } else if (f.is_tx_echo) {
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "TX-ACK");
            } else {
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 1.0f, 1.0f), "RX");
            }

            ImGui::TableSetColumnIndex(2);
            uint32_t clean_id = f.can_id & (f.is_extended ? CAN_EFF_MASK : CAN_SFF_MASK);
            ImGui::Text(f.is_extended ? "0x%08X" : "0x%03X", clean_id);

            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%s%s%s",
                        f.is_extended ? "EXT " : "STD ",
                        f.is_fd ? "FD " : "",
                        f.is_brs ? "BRS" : "");

            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%u", f.data_len);

            ImGui::TableSetColumnIndex(5);
            char hex_buf[200] = {0};
            int pos = 0;
            for (int i = 0; i < f.data_len && pos < 190; ++i) {
                pos += snprintf(hex_buf + pos, sizeof(hex_buf) - pos, "%02X ", f.data[i]);
            }
            ImGui::TextUnformatted(hex_buf);
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
            ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndTable();
    }

    // --- 5. System & USB Diagnostic Log ---
    ImGui::Text("USB & Control Pipe Log:");
    ImGui::BeginChild("SysLogChild", ImVec2(0, 0), true);
    for (const auto& line : g_App.log_messages) {
        ImGui::TextUnformatted(line.c_str());
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();

    ImGui::End();

    ImGui::Render();
    ImGuiIO& io = ImGui::GetIO();
    glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
    glClearColor(0.12f, 0.12f, 0.14f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow(g_App.window);
}

int main(int, char**) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) return -1;

    const char* glsl_version = "#version 300 es";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);

    g_App.window = SDL_CreateWindow(
        "CANable 2.5 WebUSB Analyzer",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        960, 740,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI
    );
    g_App.gl_context = SDL_GL_CreateContext(g_App.window);
    SDL_GL_MakeCurrent(g_App.window, g_App.gl_context);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForOpenGL(g_App.window, g_App.gl_context);
    ImGui_ImplOpenGL3_Init(glsl_version);

    js_webusb_init_bridge();
    g_App.AddLog("[SYS] Ready. Connect a CANable 2.5 (Candlelight) adapter to begin.");

    emscripten_set_main_loop(main_loop, 0, true);
    return 0;
}