#include "webusb_bridge.h"
#include <emscripten.h>
#include <emscripten/html5.h>

// JavaScript implementation for requesting a device
EM_JS(void, js_webusb_request, (), {
    if (!navigator.usb) return;
    navigator.usb.requestDevice({ filters: [] })
        .then(device => {
            Module.usbDevice = device;
            return device.open();
        })
        .then(() => Module.usbDevice.claimInterface(0))
        .then(() => {
            const mfg = Module.usbDevice.manufacturerName || "";
            const prod = Module.usbDevice.productName || "";
            
            const mfg_ptr = _malloc(lengthBytesUTF8(mfg) + 1);
            stringToUTF8(mfg, mfg_ptr, lengthBytesUTF8(mfg) + 1);
            
            const prod_ptr = _malloc(lengthBytesUTF8(prod) + 1);
            stringToUTF8(prod, prod_ptr, lengthBytesUTF8(prod) + 1);

            _webusb_cb_connected(Module.usbDevice.vendorId, Module.usbDevice.productId, mfg_ptr, prod_ptr);
            
            _free(mfg_ptr);
            _free(prod_ptr);
        })
        .catch(err => {
            console.error("WebUSB Request Error:", err);
        });
});

// JavaScript implementation for Bulk OUT transfer
EM_JS(void, js_webusb_transfer_out, (int ep, const uint8_t* data, int len), {
    if (!Module.usbDevice) return;
    const payload = HEAPU8.slice(data, data + len);
    Module.usbDevice.transferOut(ep & 0x7F, payload).catch(console.error);
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
    // Initialization logic for the WebUSB Javascript module if needed
}

void WebUsbBridge::RequestDevice() {
    js_webusb_request();
}

void WebUsbBridge::TransferOut(int endpoint, const uint8_t* data, int length) {
    js_webusb_transfer_out(endpoint, data, length);
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