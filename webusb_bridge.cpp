#include "webusb_bridge.h"
#include <emscripten.h>
#include <emscripten/html5.h>

#include <cstring>

// All USB I/O lives in usb_worker.js (dedicated Worker, served next to index.html).
// The page side only forwards commands with postMessage and feeds batched RX data into wasm.
// Every worker message carries the `epoch` of the connection it belongs to; messages of an older
// connection are discarded here (batch buffers are still handed back to keep the credit flow).

// Create the worker and route its messages into the exported wasm callbacks
EM_JS(void, js_webusb_init, (), {
    if (Module.usb) return;
    if (typeof Worker === 'undefined' || !navigator.usb) {
        console.error("WebUSB or Web Workers are not available in this browser");
        return;
    }

    const BATCH_CAP = 64 * 1024;          // must match BATCH_CAP in usb_worker.js
    const usb = Module.usb = { worker: null, epoch: 0, staging: 0 };

    let w;
    try {
        w = new Worker('usb_worker.js');
    } catch (err) {
        console.error("Cannot start usb_worker.js:", err);
        Module.usb = null;
        return;
    }
    usb.worker = w;
    w.postMessage({ type: 'init', timeOrigin: performance.timeOrigin });

    w.onerror = (e) => console.error("usb_worker.js error:", e.message || e);

    w.onmessage = (ev) => {
        const m = ev.data;

        if (m.type === 'batch') {
            if (m.epoch === usb.epoch && m.used <= BATCH_CAP) {
                if (!usb.staging) usb.staging = _malloc(BATCH_CAP);
                HEAPU8.set(new Uint8Array(m.buf, 0, m.used), usb.staging);
                _webusb_cb_rx_batch(usb.staging, m.used);
            }
            // Handing the buffer back is the flow-control credit; do it for stale batches too
            w.postMessage({ type: 'ret', buf: m.buf }, [m.buf]);
            return;
        }

        if (m.epoch !== usb.epoch) return;

        switch (m.type) {
        case 'opened': {
            const mfg_len = lengthBytesUTF8(m.mfg) + 1;
            const mfg_ptr = _malloc(mfg_len);
            stringToUTF8(m.mfg, mfg_ptr, mfg_len);

            const prod_len = lengthBytesUTF8(m.prod) + 1;
            const prod_ptr = _malloc(prod_len);
            stringToUTF8(m.prod, prod_ptr, prod_len);

            _webusb_cb_connected(m.vid, m.pid, mfg_ptr, prod_ptr);

            _free(mfg_ptr);
            _free(prod_ptr);
            break;
        }
        case 'closed':
            if (m.reason) console.warn("WebUSB closed:", m.reason);
            _webusb_cb_disconnected();
            break;
        case 'ctrl_in': {
            const len = m.data.byteLength;
            const buf = _malloc(len);
            HEAPU8.set(new Uint8Array(m.data), buf);
            _webusb_cb_ctrl_in(m.request, buf, len);
            _free(buf);
            break;
        }
        case 'ctrl_ack':
            _webusb_cb_ctrl_ack(m.request, m.ok ? 1 : 0);
            break;
        case 'stats':
            _webusb_cb_stats(m.rxPerSec, m.rxDropped, m.txDropped, m.recoveries);
            break;
        }
    };
});

// Ask the user for a device (window-only API); the worker opens the permitted device by VID/PID/serial
EM_JS(void, js_webusb_request, (), {
    const usb = Module.usb;
    if (!usb || !usb.worker) return;
    navigator.usb.requestDevice({ filters: [] })
        .then(device => {
            usb.epoch++;
            usb.worker.postMessage({
                type: 'open',
                epoch: usb.epoch,
                vid: device.vendorId,
                pid: device.productId,
                serial: device.serialNumber || null
            });
        })
        .catch(err => {
            console.error("WebUSB Request Error:", err);
        });
});

// Close the active WebUSB device (the worker answers with 'closed')
EM_JS(void, js_webusb_disconnect, (), {
    const usb = Module.usb;
    if (!usb || !usb.worker) return;
    usb.worker.postMessage({ type: 'close' });
});

// Gate delivery of received frames and (re)configure the IN endpoint
EM_JS(void, js_webusb_set_rx_pump, (int enable, int ep, int max_len), {
    const usb = Module.usb;
    if (!usb || !usb.worker) return;
    usb.worker.postMessage({ type: 'rx', enable: !!enable, ep: ep & 0x7F, size: max_len });
});

// Queue a Bulk OUT transfer in the worker (buffer ownership is transferred, no shared memory)
EM_JS(void, js_webusb_transfer_out, (int ep, const uint8_t* data, int len), {
    const usb = Module.usb;
    if (!usb || !usb.worker) return;
    const ab = HEAPU8.slice(data, data + len).buffer;
    usb.worker.postMessage({ type: 'tx', ep: ep & 0x7F, data: ab }, [ab]);
});

// Queue a Vendor Control OUT transfer on EP0 (executed in order, acknowledged with 'ctrl_ack')
EM_JS(void, js_webusb_control_out, (int request, int value, int index, const uint8_t* data, int len), {
    const usb = Module.usb;
    if (!usb || !usb.worker) return;
    const ab = (len > 0 && data) ? HEAPU8.slice(data, data + len).buffer : new ArrayBuffer(0);
    usb.worker.postMessage({ type: 'ctrl_out', request: request, value: value, index: index, data: ab }, [ab]);
});

// Queue a Vendor Control IN transfer on EP0 (result arrives as 'ctrl_in')
EM_JS(void, js_webusb_control_in, (int request, int value, int index, int length), {
    const usb = Module.usb;
    if (!usb || !usb.worker) return;
    usb.worker.postMessage({ type: 'ctrl_in', request: request, value: value, index: index, length: length });
});

// Extern C callbacks invoked from JS
extern "C" {
    EMSCRIPTEN_KEEPALIVE void webusb_cb_connected(uint16_t vid, uint16_t pid, const char* mfg, const char* prod) {
        WebUsbBridge::GetInstance().InternalOnConnected(vid, pid, mfg, prod);
    }

    EMSCRIPTEN_KEEPALIVE void webusb_cb_disconnected() {
        WebUsbBridge::GetInstance().InternalOnDisconnected();
    }

    EMSCRIPTEN_KEEPALIVE void webusb_cb_rx_batch(const uint8_t* data, int len) {
        WebUsbBridge::GetInstance().InternalOnRxBatch(data, len);
    }

    EMSCRIPTEN_KEEPALIVE void webusb_cb_ctrl_in(int request, const uint8_t* data, int len) {
        WebUsbBridge::GetInstance().InternalOnControlIn(static_cast<uint8_t>(request), data, len);
    }

    EMSCRIPTEN_KEEPALIVE void webusb_cb_ctrl_ack(int request, int ok) {
        WebUsbBridge::GetInstance().InternalOnControlOutDone(static_cast<uint8_t>(request), ok != 0);
    }

    EMSCRIPTEN_KEEPALIVE void webusb_cb_stats(int rx_per_sec, int rx_dropped, int tx_dropped, int recoveries) {
        WebUsbBridge::GetInstance().InternalOnStats(static_cast<uint32_t>(rx_per_sec), static_cast<uint32_t>(rx_dropped),
                                                    static_cast<uint32_t>(tx_dropped), static_cast<uint32_t>(recoveries));
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

void WebUsbBridge::ControlTransferOut(uint8_t request, uint16_t value, uint16_t index, const uint8_t* data, int length) {
    js_webusb_control_out(request, value, index, data, length);
}

void WebUsbBridge::ControlTransferIn(uint8_t request, uint16_t value, uint16_t index, uint16_t length) {
    js_webusb_control_in(request, value, index, length);
}

void WebUsbBridge::InternalOnConnected(uint16_t vid, uint16_t pid, const char* mfg, const char* prod) {
    // Batch sequence numbers continue across connections inside the worker: restart the gap check
    have_batch_seq_ = false;
    stats_.batch_gaps = 0;
    if (on_connected_) {
        UsbDeviceInfo info{vid, pid, mfg ? mfg : "", prod ? prod : ""};
        on_connected_(info);
    }
}

void WebUsbBridge::InternalOnDisconnected() {
    if (on_disconnected_) on_disconnected_();
}

// Batch layout (little endian), produced by usb_worker.js:
//   u32 seq | u32 count | count x { u16 len | u16 ep_address | f64 host_time_ms | len bytes }
void WebUsbBridge::InternalOnRxBatch(const uint8_t* data, int length) {
    constexpr int kBatchHeader = 8;
    constexpr int kRecordHeader = 12;
    if (length < kBatchHeader) return;

    uint32_t seq = 0, count = 0;
    std::memcpy(&seq, data, sizeof(seq));
    std::memcpy(&count, data + 4, sizeof(count));

    if (have_batch_seq_ && seq != next_batch_seq_) {
        stats_.batch_gaps += seq - next_batch_seq_;
    }
    have_batch_seq_ = true;
    next_batch_seq_ = seq + 1;

    int off = kBatchHeader;
    for (uint32_t i = 0; i < count && off + kRecordHeader <= length; ++i) {
        uint16_t len = 0, ep = 0;
        double host_time_ms = 0.0;
        std::memcpy(&len, data + off, sizeof(len));
        std::memcpy(&ep, data + off + 2, sizeof(ep));
        std::memcpy(&host_time_ms, data + off + 4, sizeof(host_time_ms));
        off += kRecordHeader;
        if (off + len > length) break;
        if (on_data_received_) on_data_received_(ep, data + off, len, host_time_ms);
        off += len;
    }
}

void WebUsbBridge::InternalOnControlIn(uint8_t request, const uint8_t* data, int length) {
    if (on_control_in_) on_control_in_(request, data, length);
}

void WebUsbBridge::InternalOnControlOutDone(uint8_t request, bool ok) {
    if (on_control_out_done_) on_control_out_done_(request, ok);
}

void WebUsbBridge::InternalOnStats(uint32_t rx_per_sec, uint32_t rx_dropped, uint32_t tx_dropped, uint32_t recoveries) {
    stats_.rx_per_sec = rx_per_sec;
    stats_.rx_dropped = rx_dropped;
    stats_.tx_dropped = tx_dropped;
    stats_.recoveries = recoveries;
}
