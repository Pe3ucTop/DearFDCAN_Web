#include <iostream>
#include <vector>
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
// C Callbacks Declarations (Invoked from JavaScript WebUSB Promises)
// ============================================================================
extern "C" {
    EMSCRIPTEN_KEEPALIVE void on_webusb_device_info(
        int vid, int pid,
        int usb_maj, int usb_min, int usb_sub,
        int dev_maj, int dev_min, int dev_sub,
        int dev_class, int dev_subclass, int dev_proto,
        int active_config,
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
    EMSCRIPTEN_KEEPALIVE void on_webusb_disconnected();
    EMSCRIPTEN_KEEPALIVE void on_webusb_error(const char* err_msg);
    EMSCRIPTEN_KEEPALIVE void on_webusb_tx_complete(int ep, int bytes_sent, int status);
    EMSCRIPTEN_KEEPALIVE void on_webusb_rx_complete(int ep, const uint8_t* data, int length, int status);
}

// ============================================================================
// Asynchronous JavaScript WebUSB Bridge (Zero Asyncify, Pure Event Callbacks)
// ============================================================================

EM_JS(void, js_webusb_init_bridge, (), {
    if (Module.webusbHelperInitialized) return;
    Module.webusbHelperInitialized = true;

    // Helper to allocate a temporary UTF-8 string in Wasm heap
    Module.allocStr = function(str) {
        const s = str || "";
        const len = lengthBytesUTF8(s) + 1;
        const ptr = _malloc(len);
        stringToUTF8(s, ptr, len);
        return ptr;
    };

    // Helper to send error messages to C++ log
    Module.sendUsbError = function(msg) {
        const ptr = Module.allocStr(msg);
        _on_webusb_error(ptr);
        _free(ptr);
    };

    // Helper to inspect and push the entire USB descriptor hierarchy to C++
    Module.syncDeviceTree = function(dev) {
        if (!dev) return;
        _on_webusb_clear_tree();

        const pMfg  = Module.allocStr(dev.manufacturerName);
        const pProd = Module.allocStr(dev.productName);
        const pSer  = Module.allocStr(dev.serialNumber);
        const activeCfg = dev.configuration ? dev.configuration.configurationValue : 0;

        _on_webusb_device_info(
            dev.vendorId, dev.productId,
            dev.usbVersionMajor, dev.usbVersionMinor, dev.usbVersionSubminor,
            dev.deviceVersionMajor, dev.deviceVersionMinor, dev.deviceVersionSubminor,
            dev.deviceClass, dev.deviceSubclass, dev.deviceProtocol,
            activeCfg, pMfg, pProd, pSer
        );

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
                            // Map endpoint type: 0 = bulk, 1 = interrupt, 2 = isochronous
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
});

EM_JS(void, js_webusb_request_and_open, (), {
    if (!navigator.usb) {
        Module.sendUsbError("WebUSB API is not supported in this browser (use Chromium/HTTPS).");
        return;
    }

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
            Module.syncDeviceTree(Module.activeUsbDevice);
        })
        .catch(err => {
            Module.sendUsbError("Open Error: " + (err.message || err.toString()));
        });
});

EM_JS(void, js_webusb_claim_interface, (int interface_num, int alt_setting), {
    if (!Module.activeUsbDevice) return;
    const dev = Module.activeUsbDevice;

    dev.claimInterface(interface_num)
        .then(() => {
            if (alt_setting > 0) {
                return dev.selectAlternateInterface(interface_num, alt_setting);
            }
        })
        .then(() => {
            _on_webusb_interface_claimed(interface_num, alt_setting, 1);
            Module.syncDeviceTree(dev);
        })
        .catch(err => {
            Module.sendUsbError("Claim Interface " + interface_num + " failed: " + (err.message || err.toString()));
        });
});

EM_JS(void, js_webusb_release_interface, (int interface_num), {
    if (!Module.activeUsbDevice) return;
    const dev = Module.activeUsbDevice;

    dev.releaseInterface(interface_num)
        .then(() => {
            _on_webusb_interface_claimed(interface_num, 0, 0);
            Module.syncDeviceTree(dev);
        })
        .catch(err => {
            Module.sendUsbError("Release Interface " + interface_num + " failed: " + (err.message || err.toString()));
        });
});

EM_JS(void, js_webusb_disconnect, (), {
    if (!Module.activeUsbDevice) return;
    const dev = Module.activeUsbDevice;
    Module.activeUsbDevice = null;

    dev.close()
        .then(() => {
            _on_webusb_disconnected();
        })
        .catch(err => {
            Module.sendUsbError("Close Error: " + (err.message || err.toString()));
            _on_webusb_disconnected();
        });
});

EM_JS(void, js_webusb_transfer_out, (int endpoint_addr, const uint8_t* data_ptr, int length), {
    if (!Module.activeUsbDevice) return;

    const payload = HEAPU8.slice(data_ptr, data_ptr + length);
    const epNum = endpoint_addr & 0x7F; // Strip direction bit for WebUSB

    Module.activeUsbDevice.transferOut(epNum, payload)
        .then(result => {
            const status = (result.status === "ok") ? 0 : 1;
            _on_webusb_tx_complete(endpoint_addr, result.bytesWritten, status);
        })
        .catch(err => {
            Module.sendUsbError("TX Error (EP 0x" + endpoint_addr.toString(16) + "): " + (err.message || err.toString()));
            _on_webusb_tx_complete(endpoint_addr, 0, -1);
        });
});

EM_JS(void, js_webusb_transfer_in, (int endpoint_addr, int max_length), {
    if (!Module.activeUsbDevice) return;

    const epNum = endpoint_addr & 0x7F; // Strip 0x80 IN direction bit for WebUSB

    Module.activeUsbDevice.transferIn(epNum, max_length)
        .then(result => {
            const status = (result.status === "ok") ? 0 : 1;
            if (result.data && result.data.byteLength > 0) {
                const byteLen = result.data.byteLength;
                const wasmBuf = _malloc(byteLen);
                const srcView = new Uint8Array(result.data.buffer, result.data.byteOffset, byteLen);
                HEAPU8.set(srcView, wasmBuf);
                _on_webusb_rx_complete(endpoint_addr, wasmBuf, byteLen, status);
                _free(wasmBuf);
            } else {
                _on_webusb_rx_complete(endpoint_addr, 0, 0, status);
            }
        })
        .catch(err => {
            Module.sendUsbError("RX Error (EP 0x" + endpoint_addr.toString(16) + "): " + (err.message || err.toString()));
            _on_webusb_rx_complete(endpoint_addr, 0, 0, -1);
        });
});

// ============================================================================
// C++ Data Structures for Device Info, Interfaces, and Endpoints
// ============================================================================

struct UsbEndpointInfo {
    int endpoint_number; // 1..15
    uint8_t address;     // With 0x80 bit if IN
    bool is_in;          // true = IN (Device->Host), false = OUT (Host->Device)
    int type;            // 0 = Bulk, 1 = Interrupt, 2 = Isochronous
    int packet_size;     // wMaxPacketSize

    const char* TypeStr() const {
        switch (type) {
            case 0: return "Bulk";
            case 1: return "Interrupt";
            case 2: return "Isochronous";
            default: return "Unknown";
        }
    }
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

struct UsbDeviceMetadata {
    int vid = 0;
    int pid = 0;
    std::string usb_version;
    std::string device_version;
    int device_class = 0;
    int device_subclass = 0;
    int device_protocol = 0;
    int active_config = 0;
    std::string manufacturer;
    std::string product;
    std::string serial;
};

// Decode standard USB Class code to human-readable description
const char* get_usb_class_name(int class_code) {
    switch (class_code) {
        case 0x00: return "Per-Interface (0x00)";
        case 0x01: return "Audio (0x01)";
        case 0x02: return "CDC Control (0x02)";
        case 0x03: return "HID (0x03)";
        case 0x08: return "Mass Storage (0x08)";
        case 0x09: return "Hub (0x09)";
        case 0x0A: return "CDC Data (0x0A)";
        case 0x0E: return "Video (0x0E)";
        case 0xDC: return "Diagnostic (0xDC)";
        case 0xE0: return "Wireless (0xE0)";
        case 0xEF: return "Miscellaneous / IAD (0xEF)";
        case 0xFE: return "Application Specific (0xFE)";
        case 0xFF: return "Vendor Specific (0xFF)";
        default:   return "Other Class";
    }
}

struct AppState {
    SDL_Window* window = nullptr;
    SDL_GLContext gl_context = nullptr;

    bool is_connected = false;
    bool rx_pending = false;

    UsbDeviceMetadata dev_meta;
    std::vector<UsbInterfaceAltInfo> interfaces;

    // Active transfer parameters (can be selected directly from the Endpoints table)
    int ep_out = 0x01;
    int ep_in = 0x81;
    int rx_max_len = 64;

    char send_payload[256] = "Hello WebUSB!";
    std::vector<std::string> log_messages;

    void AddLog(const char* fmt, ...) {
        char buf[512];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        log_messages.push_back(std::string(buf));
    }
};

static AppState g_App;

// ============================================================================
// Implementation of C Callbacks
// ============================================================================
extern "C" {

EMSCRIPTEN_KEEPALIVE void on_webusb_clear_tree() {
    g_App.interfaces.clear();
}

EMSCRIPTEN_KEEPALIVE void on_webusb_device_info(
    int vid, int pid,
    int usb_maj, int usb_min, int usb_sub,
    int dev_maj, int dev_min, int dev_sub,
    int dev_class, int dev_subclass, int dev_proto,
    int active_config,
    const char* mfg_name, const char* prod_name, const char* serial_num)
{
    bool first_connect = !g_App.is_connected;
    g_App.is_connected = true;

    UsbDeviceMetadata& m = g_App.dev_meta;
    m.vid = vid;
    m.pid = pid;

    char ver_buf[32];
    snprintf(ver_buf, sizeof(ver_buf), "%d.%d.%d", usb_maj, usb_min, usb_sub);
    m.usb_version = ver_buf;

    snprintf(ver_buf, sizeof(ver_buf), "%d.%d.%d", dev_maj, dev_min, dev_sub);
    m.device_version = ver_buf;

    m.device_class = dev_class;
    m.device_subclass = dev_subclass;
    m.device_protocol = dev_proto;
    m.active_config = active_config;
    m.manufacturer = mfg_name ? mfg_name : "";
    m.product = prod_name ? prod_name : "";
    m.serial = serial_num ? serial_num : "";

    if (first_connect) {
        g_App.AddLog("[USB] Opened device VID: 0x%04X, PID: 0x%04X (%s - %s)",
                     vid, pid, m.manufacturer.c_str(), m.product.c_str());
    }
}

EMSCRIPTEN_KEEPALIVE void on_webusb_add_interface(
    int config_val, int iface_num, int alt_setting,
    int iface_class, int iface_subclass, int iface_proto,
    int is_claimed, const char* iface_name)
{
    UsbInterfaceAltInfo info;
    info.config_value = config_val;
    info.interface_number = iface_num;
    info.alternate_setting = alt_setting;
    info.interface_class = iface_class;
    info.interface_subclass = iface_subclass;
    info.interface_protocol = iface_proto;
    info.is_claimed = (is_claimed != 0);
    info.interface_name = (iface_name && strlen(iface_name) > 0) ? iface_name : "Unnamed Interface";
    g_App.interfaces.push_back(info);
}

EMSCRIPTEN_KEEPALIVE void on_webusb_add_endpoint(
    int config_val, int iface_num, int alt_setting,
    int ep_num, int dir_in, int ep_type, int packet_size)
{
    // Attach endpoint to the corresponding interface/alternate entry
    for (auto& iface : g_App.interfaces) {
        if (iface.config_value == config_val &&
            iface.interface_number == iface_num &&
            iface.alternate_setting == alt_setting)
        {
            UsbEndpointInfo ep;
            ep.endpoint_number = ep_num;
            ep.is_in = (dir_in != 0);
            ep.address = ep.is_in ? (uint8_t)(ep_num | 0x80) : (uint8_t)(ep_num & 0x7F);
            ep.type = ep_type;
            ep.packet_size = packet_size;
            iface.endpoints.push_back(ep);
            break;
        }
    }
}

EMSCRIPTEN_KEEPALIVE void on_webusb_interface_claimed(int iface_num, int alt_setting, int claimed_state) {
    if (claimed_state) {
        g_App.AddLog("[USB] Claimed Interface #%d (Alt %d).", iface_num, alt_setting);
        // Auto-populate default IN/OUT endpoints from the newly claimed interface
        for (const auto& iface : g_App.interfaces) {
            if (iface.interface_number == iface_num && iface.alternate_setting == alt_setting) {
                for (const auto& ep : iface.endpoints) {
                    if (ep.is_in) {
                        g_App.ep_in = ep.address;
                        g_App.rx_max_len = ep.packet_size > 0 ? ep.packet_size : 64;
                    } else {
                        g_App.ep_out = ep.address;
                    }
                }
            }
        }
    } else {
        g_App.AddLog("[USB] Released Interface #%d.", iface_num);
    }
}

EMSCRIPTEN_KEEPALIVE void on_webusb_disconnected() {
    g_App.is_connected = false;
    g_App.rx_pending = false;
    g_App.interfaces.clear();
    g_App.AddLog("[USB] Device disconnected.");
}

EMSCRIPTEN_KEEPALIVE void on_webusb_error(const char* err_msg) {
    g_App.AddLog("[ERR] %s", err_msg);
}

EMSCRIPTEN_KEEPALIVE void on_webusb_tx_complete(int ep, int bytes_sent, int status) {
    if (status == 0) {
        g_App.AddLog("[TX -> EP 0x%02X] Sent %d bytes.", ep, bytes_sent);
    } else {
        g_App.AddLog("[TX -> EP 0x%02X] Transfer failed (status %d).", ep, status);
    }
}

EMSCRIPTEN_KEEPALIVE void on_webusb_rx_complete(int ep, const uint8_t* data, int length, int status) {
    g_App.rx_pending = false;
    if (status == 0 && length > 0 && data != nullptr) {
        std::string hex_str;
        std::string ascii_str;
        char tmp[8];

        for (int i = 0; i < length; ++i) {
            uint8_t b = data[i];
            snprintf(tmp, sizeof(tmp), "%02X ", b);
            hex_str += tmp;
            ascii_str += (b >= 32 && b <= 126) ? (char)b : '.';
        }
        g_App.AddLog("[RX <- EP 0x%02X] (%d B) HEX: %s| ASCII: %s",
                     ep, length, hex_str.c_str(), ascii_str.c_str());
    } else if (status == 0) {
        g_App.AddLog("[RX <- EP 0x%02X] Empty packet (0 bytes).", ep);
    }
}

} // extern "C"

// ============================================================================
// Dear ImGui User Interface
// ============================================================================

void render_device_inspector() {
    if (!g_App.is_connected) {
        ImGui::TextDisabled("Connect a WebUSB device to inspect its descriptors, interfaces, and endpoints.");
        return;
    }

    const UsbDeviceMetadata& m = g_App.dev_meta;

    // 1. General Device Descriptor Summary
    if (ImGui::CollapsingHeader("Device Descriptor & Metadata", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::BeginTable("DevMetaTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextDisabled("Vendor / Product ID:");
            ImGui::TableSetColumnIndex(1); ImGui::Text("0x%04X : 0x%04X", m.vid, m.pid);
            ImGui::TableSetColumnIndex(2); ImGui::TextDisabled("Manufacturer:");
            ImGui::TableSetColumnIndex(3); ImGui::Text("%s", m.manufacturer.empty() ? "N/A" : m.manufacturer.c_str());

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextDisabled("Product Name:");
            ImGui::TableSetColumnIndex(1); ImGui::Text("%s", m.product.empty() ? "N/A" : m.product.c_str());
            ImGui::TableSetColumnIndex(2); ImGui::TextDisabled("Serial Number:");
            ImGui::TableSetColumnIndex(3); ImGui::Text("%s", m.serial.empty() ? "N/A" : m.serial.c_str());

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextDisabled("USB / Dev Version:");
            ImGui::TableSetColumnIndex(1); ImGui::Text("USB %s (Rev %s)", m.usb_version.c_str(), m.device_version.c_str());
            ImGui::TableSetColumnIndex(2); ImGui::TextDisabled("Class / Sub / Proto:");
            ImGui::TableSetColumnIndex(3); ImGui::Text("%s (Sub: 0x%02X, Proto: 0x%02X)",
                                                       get_usb_class_name(m.device_class), m.device_subclass, m.device_protocol);

            ImGui::EndTable();
        }
    }

    // 2. Interfaces & Endpoints Hierarchy
    if (ImGui::CollapsingHeader("Interfaces & Available Endpoints", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (size_t i = 0; i < g_App.interfaces.size(); ++i) {
            const auto& iface = g_App.interfaces[i];
            ImGui::PushID((int)i);

            // Interface Header Line
            ImGui::Separator();
            if (iface.is_claimed) {
                ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.4f, 1.0f), "[CLAIMED]");
            } else {
                ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.3f, 1.0f), "[UNCLAIMED]");
            }
            ImGui::SameLine();
            ImGui::Text("Cfg #%d | Interface #%d (Alt %d): %s  —  %s (Sub: 0x%02X, Proto: 0x%02X)",
                        iface.config_value, iface.interface_number, iface.alternate_setting,
                        iface.interface_name.c_str(),
                        get_usb_class_name(iface.interface_class),
                        iface.interface_subclass, iface.interface_protocol);

            ImGui::SameLine();
            if (!iface.is_claimed) {
                if (ImGui::SmallButton("Claim Interface")) {
                    js_webusb_claim_interface(iface.interface_number, iface.alternate_setting);
                }
            } else {
                if (ImGui::SmallButton("Release")) {
                    js_webusb_release_interface(iface.interface_number);
                }
            }

            // Endpoints Table for this Interface
            if (iface.endpoints.empty()) {
                ImGui::Indent();
                ImGui::TextDisabled("No dedicated endpoints (uses Control EP0 only).");
                ImGui::Unindent();
            } else {
                if (ImGui::BeginTable("EpTable", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                    ImGui::TableSetupColumn("EP Address", ImGuiTableColumnFlags_WidthFixed, 90.0f);
                    ImGui::TableSetupColumn("EP Number", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                    ImGui::TableSetupColumn("Direction", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                    ImGui::TableSetupColumn("Transfer Type", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                    ImGui::TableSetupColumn("Max Packet", ImGuiTableColumnFlags_WidthFixed, 90.0f);
                    ImGui::TableSetupColumn("Quick Select", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableHeadersRow();

                    for (size_t e = 0; e < iface.endpoints.size(); ++e) {
                        const auto& ep = iface.endpoints[e];
                        ImGui::PushID((int)e);
                        ImGui::TableNextRow();

                        ImGui::TableSetColumnIndex(0);
                        ImGui::Text("0x%02X", ep.address);

                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text("EP %d", ep.endpoint_number);

                        ImGui::TableSetColumnIndex(2);
                        if (ep.is_in) {
                            ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "IN (Dev->Host)");
                        } else {
                            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "OUT (Host->Dev)");
                        }

                        ImGui::TableSetColumnIndex(3);
                        ImGui::TextUnformatted(ep.TypeStr());

                        ImGui::TableSetColumnIndex(4);
                        ImGui::Text("%d bytes", ep.packet_size);

                        ImGui::TableSetColumnIndex(5);
                        if (ep.is_in) {
                            bool is_selected = (g_App.ep_in == ep.address);
                            if (ImGui::SmallButton(is_selected ? "[Active IN EP]" : "Use as IN EP")) {
                                g_App.ep_in = ep.address;
                                g_App.rx_max_len = ep.packet_size > 0 ? ep.packet_size : 64;
                                g_App.AddLog("[UI] Selected IN Endpoint 0x%02X (MaxPacketSize: %d B)", ep.address, ep.packet_size);
                            }
                        } else {
                            bool is_selected = (g_App.ep_out == ep.address);
                            if (ImGui::SmallButton(is_selected ? "[Active OUT EP]" : "Use as OUT EP")) {
                                g_App.ep_out = ep.address;
                                g_App.AddLog("[UI] Selected OUT Endpoint 0x%02X (MaxPacketSize: %d B)", ep.address, ep.packet_size);
                            }
                        }

                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
            }

            ImGui::PopID();
        }
    }
}

void main_loop() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        ImGui_ImplSDL2_ProcessEvent(&event);
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(860, 660), ImGuiCond_FirstUseEver);

    ImGui::Begin("WebUSB Device Inspector & Endpoint Terminal (Event-Driven)");

    // --- 1. Top Connection Bar ---
    if (!g_App.is_connected) {
        if (ImGui::Button("Request & Open WebUSB Device", ImVec2(260, 30))) {
            g_App.AddLog("[SYS] Opening browser WebUSB chooser...");
            js_webusb_request_and_open();
        }
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Status: Disconnected");
    } else {
        if (ImGui::Button("Disconnect Device", ImVec2(180, 30))) {
            js_webusb_disconnect();
        }
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "Status: Connected (VID: 0x%04X, PID: 0x%04X)",
                           g_App.dev_meta.vid, g_App.dev_meta.pid);
    }

    ImGui::Separator();

    // --- 2. Device Descriptors & Endpoints Inspector ---
    render_device_inspector();

    ImGui::Separator();

    // --- 3. Packet Transfer Controls ---
    ImGui::BeginDisabled(!g_App.is_connected);

    ImGui::Text("Active Transfer Endpoints (click 'Use as ...' in the table above or edit manually):");
    ImGui::PushItemWidth(100);
    ImGui::InputInt("OUT EP (Hex)", &g_App.ep_out, 1, 16, ImGuiInputTextFlags_CharsHexadecimal);
    ImGui::SameLine();
    ImGui::InputInt("IN EP (Hex)", &g_App.ep_in, 1, 16, ImGuiInputTextFlags_CharsHexadecimal);
    ImGui::SameLine();
    ImGui::InputInt("IN Max Bytes", &g_App.rx_max_len);
    ImGui::PopItemWidth();

    ImGui::InputText("TX Payload (ASCII)", g_App.send_payload, sizeof(g_App.send_payload));

    if (ImGui::Button("Send Packet (Async OUT)", ImVec2(210, 30))) {
        size_t len = strlen(g_App.send_payload);
        if (len > 0) {
            js_webusb_transfer_out(g_App.ep_out, (const uint8_t*)g_App.send_payload, (int)len);
        }
    }
    ImGui::SameLine();

    ImGui::BeginDisabled(g_App.rx_pending);
    if (ImGui::Button(g_App.rx_pending ? "Waiting for IN data..." : "Read Endpoint (Async IN)", ImVec2(210, 30))) {
        g_App.rx_pending = true;
        js_webusb_transfer_in(g_App.ep_in, g_App.rx_max_len);
    }
    ImGui::EndDisabled();

    ImGui::EndDisabled();
    ImGui::Separator();

    // --- 4. Transfer & Event Log ---
    ImGui::Text("Endpoint Transfer & Event Log:");
    ImGui::SameLine(ImGui::GetWindowWidth() - 90);
    if (ImGui::SmallButton("Clear Log")) {
        g_App.log_messages.clear();
    }

    ImGui::BeginChild("LogRegion", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
    for (const auto& line : g_App.log_messages) {
        ImGui::TextUnformatted(line.c_str());
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();

    ImGui::End();

    // Render OpenGL frame
    ImGui::Render();
    ImGuiIO& io = ImGui::GetIO();
    glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
    glClearColor(0.12f, 0.12f, 0.14f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow(g_App.window);
}

int main(int, char**) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        return -1;
    }

    const char* glsl_version = "#version 300 es";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);

    g_App.window = SDL_CreateWindow(
        "WebUSB Device & Endpoint Inspector",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        900, 700,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI
    );
    g_App.gl_context = SDL_GL_CreateContext(g_App.window);
    SDL_GL_MakeCurrent(g_App.window, g_App.gl_context);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForOpenGL(g_App.window, g_App.gl_context);
    ImGui_ImplOpenGL3_Init(glsl_version);

    // Initialize JavaScript helper functions on the Module object
    js_webusb_init_bridge();
    g_App.AddLog("[SYS] Ready. Click 'Request & Open WebUSB Device' to enumerate endpoints.");

    emscripten_set_main_loop(main_loop, 0, true);
    return 0;
}