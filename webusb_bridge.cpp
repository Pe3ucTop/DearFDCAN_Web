#include "webusb_bridge.h"
#include <emscripten.h>
#include <emscripten/html5.h>

// Initialize internal JS helper for continuous Bulk IN transfers
EM_JS(void, js_webusb_init, (), {
    if (Module.webusbInitialized) return;
    Module.webusbInitialized = true;
    Module.rxPumpActive = false;

    Module.pollBulkIn = function(ep, maxLen) {
        if (!Module.usbDevice || !Module.rxPumpActive) return;

        Module.usbDevice.transferIn(ep & 0x7F, maxLen)
            .then(result => {
                if (result.status === "ok" && result.data && result.data.byteLength > 0) {
                    const len = result.data.byteLength;
                    const buf = _malloc(len);
                    const view = new Uint8Array(result.data.buffer, result.data.byteOffset, len);
                    HEAPU8.set(view, buf);
                    _webusb_cb_rx(ep, buf, len);
                    _free(buf);
                }
                if (Module.rxPumpActive && Module.usbDevice) {
                    Module.pollBulkIn(ep, maxLen);
                }
            })
            .catch(err => {
                console.error("Bulk IN Pump Error:", err);
                Module.rxPumpActive = false;
            });
    };
});

// Request WebUSB device and claim Interface 0
EM_JS(void, js_webusb_request, (), {
    if (!navigator.usb) return;
    navigator.usb.requestDevice({ filters: [] })
        .then(device => {
            Module.usbDevice = device;
            return device.open();
        })
        .then(() => {
            if (Module.usbDevice.configuration === null) {
                return Module.usbDevice.selectConfiguration(1);
            }
        })
        .then(() => Module.usbDevice.claimInterface(0))
        .then(() => {
            const mfg = Module.usbDevice.manufacturerName || "";
            const prod = Module.usbDevice.productName || "";

            const mfg_len = lengthBytesUTF8(mfg) + 1;
            const mfg_ptr = _malloc(mfg_len);
            stringToUTF8(mfg, mfg_ptr, mfg_len);

            const prod_len = lengthBytesUTF8(prod) + 1;
            const prod_ptr = _malloc(prod_len);
            stringToUTF8(prod, prod_ptr, prod_len);

            _webusb_cb_connected(Module.usbDevice.vendorId, Module.usbDevice.productId, mfg_ptr, prod_ptr);

            _free(mfg_ptr);
            _free(prod_ptr);
        })
        .catch(err => {
            console.error("WebUSB Request Error:", err);
        });
});

// Close active WebUSB device
EM_JS(void, js_webusb_disconnect, (), {
    Module.rxPumpActive = false;
    if (!Module.usbDevice) return;
    const dev = Module.usbDevice;
    Module.usbDevice = null;

    dev.close()
        .then(() => _webusb_cb_disconnected())
        .catch(() => _webusb_cb_disconnected());
});

// Start or stop continuous Bulk IN polling
EM_JS(void, js_webusb_set_rx_pump, (int enable, int ep, int max_len), {
    if (!enable) {
        Module.rxPumpActive = false;
        return;
    }
    if (!Module.rxPumpActive && Module.usbDevice) {
        Module.rxPumpActive = true;
        Module.pollBulkIn(ep, max_len);
    }
});

// Send Bulk OUT transfer
EM_JS(void, js_webusb_transfer_out, (int ep, const uint8_t* data, int len), {
    if (!Module.usbDevice) return;
    const payload = HEAPU8.slice(data, data + len);
    Module.usbDevice.transferOut(ep & 0x7F, payload).catch(console.error);
});

// Send Vendor Control OUT transfer on EP0
EM_JS(void, js_webusb_control_out, (int req_type, int request, int value, int index, const uint8_t* data, int len), {
    if (!Module.usbDevice) return;
    const payload = (len > 0 && data) ? HEAPU8.slice(data, data + len) : new Uint8Array(0);
    Module.usbDevice.controlTransferOut({
        requestType: 'vendor',
        recipient: 'interface',
        request: request,
        value: value,
        index: index
    }, payload).catch(console.error);
});

// Extern C callbacks invoked from JS
extern "C" {
    EMSCRIPTEN_KEEPALIVE void webusb_cb_connected(uint16_t vid, uint16_t pid, const char* mfg, const char* prod) {
        WebUsbBridge::GetInstance().InternalOnConnected(vid, pid, mfg, prod);
    }

    EMSCRIPTEN_KEEPALIVE void webusb_cb_disconnected() {
        WebUsbBridge::GetInstance().InternalOnDisconnected();
    }

    EMSCRIPTEN_KEEPALIVE void webusb_cb_rx(int ep, const uint8_t* data, int len) {
        WebUsbBridge::GetInstance().InternalOnDataReceived(ep, data, len);
    }
}

WebUsbBridge& WebUsbBridge::GetInstance() {
    static WebUsbBridge instance;
    return instance;
}

void WebUsbBridge::Initialize() {
    js_webusb_init();
}

void WebUsbBridge::RequestDevice() {
    js_webusb_request();
}

void WebUsbBridge::Disconnect() {
    js_webusb_disconnect();
}

void WebUsbBridge::StartRxPump(int endpoint, int max_packet_size) {
    js_webusb_set_rx_pump(1, endpoint, max_packet_size);
}

void WebUsbBridge::StopRxPump() {
    js_webusb_set_rx_pump(0, 0, 0);
}

void WebUsbBridge::TransferOut(int endpoint, const uint8_t* data, int length) {
    js_webusb_transfer_out(endpoint, data, length);
}

void WebUsbBridge::ControlTransferOut(uint8_t request_type, uint8_t request, uint16_t value, uint16_t index, const uint8_t* data, int length) {
    js_webusb_control_out(request_type, request, value, index, data, length);
}

void WebUsbBridge::InternalOnConnected(uint16_t vid, uint16_t pid, const char* mfg, const char* prod) {
    if (on_connected_) {
        UsbDeviceInfo info{vid, pid, mfg ? mfg : "", prod ? prod : ""};
        on_connected_(info);
    }
}

void WebUsbBridge::InternalOnDisconnected() {
    if (on_disconnected_) on_disconnected_();
}

void WebUsbBridge::InternalOnDataReceived(int ep, const uint8_t* data, int length) {
    if (on_data_received_) on_data_received_(ep, data, length);
}