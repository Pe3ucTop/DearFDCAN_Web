#include "webusb_bridge.h"
#include <emscripten.h>
#include <emscripten/html5.h>

#include <cstring>

// All USB I/O and CAN processing live in usb_worker.js (dedicated Worker, served next to index.html)
// and its can_core wasm module. The page side forwards commands with postMessage and applies the
// snapshots (changed ID rows + new trace entries) it receives.
// Worker messages carry the `epoch` of the connection they belong to; messages of an older connection
// are discarded here. Snapshots are the exception: they describe state that outlives connections.

// Create the worker and route its messages into the exported wasm callbacks
EM_JS(void, js_webusb_init, (), {
    if (Module.usb) return;
    if (typeof Worker === 'undefined' || !navigator.usb) {
        console.error("WebUSB or Web Workers are not available in this browser");
        return;
    }

    const usb = Module.usb = { worker: null, epoch: 0, staging: 0, stagingCap: 0 };

    // Single-file build: the worker source (can_core + usb_worker.js) is embedded in this page as
    // Module.usbWorkerSource (--pre-js made by pack_worker.py) and started from a Blob URL.
    // Multi-file build: the worker is loaded from usb_worker.js next to the page.
    let w;
    try {
        const src = Module.usbWorkerSource;
        w = src ? new Worker(URL.createObjectURL(new Blob([src], { type: 'text/javascript' })))
                : new Worker('usb_worker.js');
    } catch (err) {
        console.error("Cannot start the USB worker:", err);
        Module.usb = null;
        return;
    }
    usb.worker = w;
    w.postMessage({ type: 'init', timeOrigin: performance.timeOrigin });

    w.onerror = (e) => console.error("usb_worker.js error:", e.message || e);

    w.onmessage = (ev) => {
        const m = ev.data;

        if (m.type === 'snapshot') {
            // Snapshots carry the table/trace state, which lives across connections: apply them
            // whatever the session. The buffer always goes back (it is the flow-control credit).
            if (m.used > usb.stagingCap) {
                if (usb.staging) _free(usb.staging);
                usb.staging = _malloc(m.buf.byteLength);
                usb.stagingCap = m.buf.byteLength;
            }
            HEAPU8.set(new Uint8Array(m.buf, 0, m.used), usb.staging);
            _webusb_cb_snapshot(usb.staging, m.used);
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
            _webusb_cb_stats(m.rxPerSec, m.txDropped, m.recoveries);
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

// Commands for the CAN core in the worker: 1 = clear ID table, 2 = clear trace, 3 = trace on/off
EM_JS(void, js_webusb_core_cmd, (int op, int arg), {
    const usb = Module.usb;
    if (!usb || !usb.worker) return;
    usb.worker.postMessage({ type: 'core', op: op, arg: arg });
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

    EMSCRIPTEN_KEEPALIVE void webusb_cb_snapshot(const uint8_t* data, int len) {
        WebUsbBridge::GetInstance().InternalOnSnapshot(data, len);
    }

    EMSCRIPTEN_KEEPALIVE void webusb_cb_ctrl_in(int request, const uint8_t* data, int len) {
        WebUsbBridge::GetInstance().InternalOnControlIn(static_cast<uint8_t>(request), data, len);
    }

    EMSCRIPTEN_KEEPALIVE void webusb_cb_ctrl_ack(int request, int ok) {
        WebUsbBridge::GetInstance().InternalOnControlOutDone(static_cast<uint8_t>(request), ok != 0);
    }

    EMSCRIPTEN_KEEPALIVE void webusb_cb_stats(int rx_per_sec, int tx_dropped, int recoveries) {
        WebUsbBridge::GetInstance().InternalOnStats(static_cast<uint32_t>(rx_per_sec), static_cast<uint32_t>(tx_dropped),
                                                    static_cast<uint32_t>(recoveries));
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

void WebUsbBridge::ClearIdTable() {
    ++table_clear_sent_;
    js_webusb_core_cmd(1, 0);
}

void WebUsbBridge::ClearTrace() {
    ++trace_clear_sent_;
    js_webusb_core_cmd(2, 0);
}

void WebUsbBridge::SetTraceEnabled(bool enable) {
    js_webusb_core_cmd(3, enable ? 1 : 0);
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

// Snapshot layout: SnapshotHeader | n_table x WireTableRecord | n_trace x TraceEntry (see can_wire.h)
void WebUsbBridge::InternalOnSnapshot(const uint8_t* data, int length) {
    SnapshotView view;
    if (length < static_cast<int>(sizeof(SnapshotHeader))) { ++stats_.bad_snapshots; return; }
    std::memcpy(&view.header, data, sizeof(SnapshotHeader));
    const SnapshotHeader& h = view.header;

    const uint64_t need = sizeof(SnapshotHeader)
                        + static_cast<uint64_t>(h.n_table) * sizeof(WireTableRecord)
                        + static_cast<uint64_t>(h.n_trace) * sizeof(TraceEntry);
    if (h.magic != kSnapshotMagic || h.table_rec_size != sizeof(WireTableRecord) ||
        h.trace_rec_size != sizeof(TraceEntry) || need > static_cast<uint64_t>(length)) {
        ++stats_.bad_snapshots;
        return;
    }

    if (have_snapshot_seq_ && h.seq != next_snapshot_seq_) {
        stats_.snapshot_gaps += h.seq - next_snapshot_seq_;
    }
    have_snapshot_seq_ = true;
    next_snapshot_seq_ = h.seq + 1;

    stats_.frames_total = h.frames_total;
    stats_.trace_dropped = h.trace_dropped_total;
    stats_.table_overflow = h.table_overflow_total;

    // A part produced before the core processed our latest clear is stale: skip it
    const uint8_t* body = data + sizeof(SnapshotHeader);
    if (static_cast<int32_t>(h.table_clear_ack - table_clear_sent_) >= 0 && h.n_table > 0) {
        view.table = body;
        view.n_table = h.n_table;
    }
    if (static_cast<int32_t>(h.trace_clear_ack - trace_clear_sent_) >= 0 && h.n_trace > 0) {
        view.trace = body + static_cast<size_t>(h.n_table) * sizeof(WireTableRecord);
        view.n_trace = h.n_trace;
    }
    if (on_snapshot_) on_snapshot_(view);
}

void WebUsbBridge::InternalOnControlIn(uint8_t request, const uint8_t* data, int length) {
    if (on_control_in_) on_control_in_(request, data, length);
}

void WebUsbBridge::InternalOnControlOutDone(uint8_t request, bool ok) {
    if (on_control_out_done_) on_control_out_done_(request, ok);
}

void WebUsbBridge::InternalOnStats(uint32_t rx_per_sec, uint32_t tx_dropped, uint32_t recoveries) {
    stats_.rx_per_sec = rx_per_sec;
    stats_.tx_dropped = tx_dropped;
    stats_.recoveries = recoveries;
}
