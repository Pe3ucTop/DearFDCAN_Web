#include "webusb_bridge.h"
#include <emscripten.h>
#include <emscripten/html5.h>

#include <cstring>

// All USB I/O and CAN processing live in usb_worker.js (dedicated Worker, served next to index.html)
// and its can_core wasm module. The page side forwards commands with postMessage and applies the
// snapshots (changed ID rows + new trace entries) it receives.
//
// Serial (SLCAN) adapters use the same worker for everything but the port I/O: that is done by a second
// worker, serial_worker.js, started on the first use. The page connects the two with a MessageChannel, so
// the received bytes go from the serial worker straight to the CAN worker. Only the choice of the port
// (a user gesture, window-only API) happens here; the page then hands the port over to the serial worker.
//
// Several adapters are handled at once. The page-side JS keeps a table of slots (device number ->
// USBDevice picked by the user); every command carries the device number, and every worker message
// carries the device number and the `epoch` of that device's connection. Messages of an older
// connection of the same slot are discarded here. Snapshots are the exception: they describe state
// that outlives connections and is shared by all adapters.

// Create the worker and route its messages into the exported wasm callbacks
EM_JS(void, js_webusb_init, (), {
    if (Module.usb) return;
    if (typeof Worker === 'undefined' || (!navigator.usb && !navigator.serial)) {
        console.error("WebUSB/Web Serial or Web Workers are not available in this browser");
        return;
    }

    const MAX_DEVICES = 8;      // kMaxDevices in can_protocol.h
    const usb = Module.usb = { worker: null, serialWorker: null, staging: 0, stagingCap: 0, slots: [], epochs: [] };
    for (let i = 0; i < MAX_DEVICES; i++) {
        // { transport: 'usb', device: USBDevice, state } or { transport: 'serial', key, info, baud, state },
        // state: 'connecting' | 'open' | 'closed'
        usb.slots.push(null);
        usb.epochs.push(0);     // bumped on every (re)connect and on removal, never reset
    }

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

    // Temporary C string for a callback argument (the caller frees it)
    usb.cstr = (s) => {
        s = String(s == null ? "" : s);
        const n = lengthBytesUTF8(s) + 1;
        const p = _malloc(n);
        stringToUTF8(s, p, n);
        return p;
    };

    // Short notice for the UI
    usb.notify = (text) => {
        const p = usb.cstr(text);
        _webusb_cb_message(p);
        _free(p);
    };

    // Tell the UI which adapter occupies a slot (what the browser knows about it)
    usb.announce = (dev) => {
        const slot = usb.slots[dev];
        let p;
        if (slot.transport === 'serial') {
            // A serial port tells only its USB ids, if it is a USB serial adapter
            const i = slot.info;
            p = ["", i.usbVendorId !== undefined ? "USB serial port" : "Serial port", "", "", ""].map(usb.cstr);
            _webusb_cb_device_added(dev, i.usbVendorId | 0, i.usbProductId | 0, p[0], p[1], p[2], p[3], p[4], 1);
        } else {
            const d = slot.device;
            const ver = (a, b, c) => a + "." + b + "." + c;
            p = [d.manufacturerName, d.productName, d.serialNumber,
                 ver(d.usbVersionMajor, d.usbVersionMinor, d.usbVersionSubminor),
                 ver(d.deviceVersionMajor, d.deviceVersionMinor, d.deviceVersionSubminor)].map(usb.cstr);
            _webusb_cb_device_added(dev, d.vendorId, d.productId, p[0], p[1], p[2], p[3], p[4], 0);
        }
        p.forEach(ptr => _free(ptr));
    };

    // (Re)connect the adapter of a slot: new epoch, the worker opens the permitted device
    usb.open = (dev) => {
        const slot = usb.slots[dev];
        slot.state = 'connecting';
        const epoch = ++usb.epochs[dev];
        usb.announce(dev);
        if (slot.transport === 'serial') {
            usb.worker.postMessage({ type: 'open', dev: dev, epoch: epoch, transport: 'serial', baud: slot.baud });
            return;
        }
        usb.worker.postMessage({
            type: 'open', dev: dev, epoch: epoch,
            vid: slot.device.vendorId, pid: slot.device.productId,
            serial: slot.device.serialNumber || null
        });
    };

    // Starts the serial worker (once) and connects it to the CAN worker with a MessageChannel
    usb.ensureSerial = () => {
        if (usb.serialWorker) return true;
        if (!navigator.serial) {
            usb.notify("Web Serial is not available in this browser");
            return false;
        }
        try {
            const src = Module.serialWorkerSource;
            const sw = src ? new Worker(URL.createObjectURL(new Blob([src], { type: 'text/javascript' })))
                           : new Worker('serial_worker.js');
            sw.onerror = (e) => console.error("serial_worker.js error:", e.message || e);
            const ch = new MessageChannel();
            usb.worker.postMessage({ type: 'serial_link', port: ch.port1 }, [ch.port1]);
            sw.postMessage({ type: 'link', port: ch.port2 }, [ch.port2]);
            usb.serialWorker = sw;
            return true;
        } catch (err) {
            console.error("Cannot start the serial worker:", err);
            usb.notify("Cannot start the serial worker: " + (err && err.message ? err.message : err));
            return false;
        }
    };

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

        const slot = usb.slots[m.dev];
        if (!slot || m.epoch !== usb.epochs[m.dev]) return;

        switch (m.type) {
        case 'opened': {
            slot.state = 'open';
            const eps = usb.cstr(m.eps);
            const info = usb.cstr(m.info);
            _webusb_cb_device_opened(m.dev, eps, m.protocol | 0, info);
            _free(eps);
            _free(info);
            break;
        }
        case 'closed': {
            slot.state = 'closed';
            if (m.reason) console.warn("WebUSB device " + (m.dev + 1) + " closed:", m.reason);
            const p = usb.cstr(m.reason);
            _webusb_cb_device_closed(m.dev, p);
            _free(p);
            break;
        }
        case 'ctrl_in': {
            const len = m.data.byteLength;
            const buf = _malloc(len);
            HEAPU8.set(new Uint8Array(m.data), buf);
            _webusb_cb_ctrl_in(m.dev, m.request, buf, len);
            _free(buf);
            break;
        }
        case 'ctrl_ack':
            _webusb_cb_ctrl_ack(m.dev, m.request, m.ok ? 1 : 0);
            break;
        case 'stats':
            _webusb_cb_stats(m.dev, m.rxPerSec, m.txDropped, m.recoveries, m.rxErrors, m.txErrors);
            break;
        case 'sync':
            _webusb_cb_sync(m.dev, m.s[0], m.s[1], m.s[2], m.s[3], m.s[4], m.s[5], m.s[6], m.s[7], m.s[8]);
            break;
        case 'events': {
            const text = usb.cstr(m.text);
            _webusb_cb_events(m.dev, text);
            _free(text);
            break;
        }
        }
    };
});

// Ask the user for an adapter (window-only API) and connect it in a free slot. Picking an adapter that
// already has a slot reconnects that slot (or reports that it is connected).
EM_JS(void, js_webusb_request, (), {
    const usb = Module.usb;
    if (!usb || !usb.worker) return;
    if (!navigator.usb) {
        usb.notify("WebUSB is not available in this browser");
        return;
    }
    navigator.usb.requestDevice({ filters: [] })
        .then(device => {
            let dev = usb.slots.findIndex(s => s && s.transport === 'usb' && s.device === device);
            if (dev < 0 && device.serialNumber) {
                dev = usb.slots.findIndex(s => s && s.transport === 'usb' && s.device.serialNumber === device.serialNumber &&
                                               s.device.vendorId === device.vendorId &&
                                               s.device.productId === device.productId);
            }
            if (dev >= 0) {
                if (usb.slots[dev].state !== 'closed') {
                    usb.notify("This adapter is already connected as device " + (dev + 1));
                    return;
                }
                usb.slots[dev].device = device;
                usb.open(dev);
                return;
            }
            dev = usb.slots.findIndex(s => !s);
            if (dev < 0) {
                usb.notify("All " + usb.slots.length + " device slots are in use: remove a device first");
                return;
            }
            usb.slots[dev] = { transport: 'usb', device: device, state: 'closed' };
            usb.open(dev);
        })
        .catch(err => {
            if (err && err.name === 'NotFoundError') return;   // the chooser was cancelled
            console.error("WebUSB Request Error:", err);
            usb.notify("Cannot open the device chooser: " + (err && err.message ? err.message : err));
        });
});

// Ask the user for a serial port (window-only API, needs the user gesture of the button press) and connect it
// in a free slot at the given baud rate. The port goes to the serial worker: as the object itself where the
// browser can transfer SerialPort objects, otherwise as "the idx-th permitted port with these USB ids".
EM_JS(void, js_serial_request, (int baud), {
    const usb = Module.usb;
    if (!usb || !usb.worker || !usb.ensureSerial()) return;
    navigator.serial.requestPort({})
        .then(async port => {
            let info = {};
            try { info = port.getInfo() || {}; } catch (e) { /* no info */ }
            let idx = 0;
            try {
                const all = await navigator.serial.getPorts();
                const same = all.filter(p => {
                    const i = p.getInfo ? p.getInfo() : {};
                    return i.usbVendorId === info.usbVendorId && i.usbProductId === info.usbProductId;
                });
                idx = Math.max(0, same.indexOf(port));
            } catch (e) { /* getPorts() failed: the first port with these ids is used */ }

            const key = (info.usbVendorId | 0) + ":" + (info.usbProductId | 0) + ":" + idx;
            let dev = usb.slots.findIndex(s => s && s.transport === 'serial' && s.key === key);
            if (dev >= 0) {
                if (usb.slots[dev].state !== 'closed') {
                    usb.notify("This port is already connected as device " + (dev + 1));
                    return;
                }
                usb.slots[dev].baud = baud;
                usb.open(dev);
                return;
            }
            dev = usb.slots.findIndex(s => !s);
            if (dev < 0) {
                usb.notify("All " + usb.slots.length + " device slots are in use: remove a device first");
                return;
            }
            const pick = { type: 'pick', dev: dev, vid: info.usbVendorId | 0, pid: info.usbProductId | 0, idx: idx };
            try {
                usb.serialWorker.postMessage(Object.assign({ port: port }, pick), [port]);
            } catch (e) {
                usb.serialWorker.postMessage(pick);          // SerialPort is not transferable here
            }
            usb.slots[dev] = { transport: 'serial', key: key, info: info, baud: baud, state: 'closed' };
            usb.open(dev);
        })
        .catch(err => {
            if (err && err.name === 'NotFoundError') return;   // the chooser was cancelled
            console.error("Web Serial Request Error:", err);
            usb.notify("Cannot open the serial port chooser: " + (err && err.message ? err.message : err));
        });
});

// Baud rate used the next time the serial port of a slot is connected
EM_JS(void, js_serial_set_baud, (int dev, int baud), {
    const usb = Module.usb;
    if (usb && usb.slots[dev] && usb.slots[dev].transport === 'serial') usb.slots[dev].baud = baud;
});

// Reconnect the adapter of a known, closed slot
EM_JS(void, js_webusb_connect, (int dev), {
    const usb = Module.usb;
    if (!usb || !usb.worker || !usb.slots[dev] || usb.slots[dev].state !== 'closed') return;
    usb.open(dev);
});

// Close the adapter of a slot (the worker answers with 'closed'); the slot stays
EM_JS(void, js_webusb_disconnect, (int dev), {
    const usb = Module.usb;
    if (!usb || !usb.worker || !usb.slots[dev]) return;
    usb.worker.postMessage({ type: 'close', dev: dev });
});

// Close the adapter and release its slot; late messages of the removed connection are dropped
EM_JS(void, js_webusb_remove, (int dev), {
    const usb = Module.usb;
    if (!usb || !usb.worker || !usb.slots[dev]) return;
    usb.worker.postMessage({ type: 'close', dev: dev });
    if (usb.slots[dev].transport === 'serial' && usb.serialWorker) usb.serialWorker.postMessage({ type: 'forget', dev: dev });
    usb.slots[dev] = null;
    usb.epochs[dev]++;
});

// Gate processing of received frames of an adapter and (re)configure its IN endpoint
EM_JS(void, js_webusb_set_rx_pump, (int dev, int enable, int ep, int max_len, int flags), {
    const usb = Module.usb;
    if (!usb || !usb.worker) return;
    usb.worker.postMessage({ type: 'rx', dev: dev, enable: !!enable, ep: ep & 0x7F, size: max_len,
                             elmue: !!(flags & 1), ts: !!(flags & 2) });
});

// Commands for the CAN core in the worker: 1 = clear ID table, 2 = clear trace, 3 = trace on/off
EM_JS(void, js_webusb_core_cmd, (int op, int arg), {
    const usb = Module.usb;
    if (!usb || !usb.worker) return;
    usb.worker.postMessage({ type: 'core', op: op, arg: arg });
});

// Queue a Bulk OUT transfer in the worker (buffer ownership is transferred, no shared memory)
EM_JS(void, js_webusb_transfer_out, (int dev, int ep, const uint8_t* data, int len), {
    const usb = Module.usb;
    if (!usb || !usb.worker) return;
    const ab = HEAPU8.slice(data, data + len).buffer;
    usb.worker.postMessage({ type: 'tx', dev: dev, ep: ep & 0x7F, data: ab }, [ab]);
});

// Queue a Vendor Control OUT transfer on EP0 (executed in order, acknowledged with 'ctrl_ack')
EM_JS(void, js_webusb_control_out, (int dev, int request, int value, int index, const uint8_t* data, int len, int id), {
    const usb = Module.usb;
    if (!usb || !usb.worker) return;
    const ab = (len > 0 && data) ? HEAPU8.slice(data, data + len).buffer : new ArrayBuffer(0);
    usb.worker.postMessage({ type: 'ctrl_out', dev: dev, request: request, value: value, index: index, data: ab, id: id }, [ab]);
});

// Queue a bulk OUT transfer to the command endpoint (PEAK uCAN); acknowledged as 'ctrl_ack' with `id`
EM_JS(void, js_webusb_cmd_out, (int dev, int id, const uint8_t* data, int len), {
    const usb = Module.usb;
    if (!usb || !usb.worker) return;
    const ab = HEAPU8.slice(data, data + len).buffer;
    usb.worker.postMessage({ type: 'cmd_out', dev: dev, id: id, ep: 0, data: ab }, [ab]);
});

// Queue a Vendor Control IN transfer on EP0 (result arrives as 'ctrl_in')
EM_JS(void, js_webusb_control_in, (int dev, int request, int value, int index, int length), {
    const usb = Module.usb;
    if (!usb || !usb.worker) return;
    usb.worker.postMessage({ type: 'ctrl_in', dev: dev, request: request, value: value, index: index, length: length });
});

// Extern C callbacks invoked from JS
extern "C" {
    EMSCRIPTEN_KEEPALIVE void webusb_cb_device_added(int dev, int vid, int pid, const char* mfg, const char* prod,
                                                     const char* serial, const char* usb_ver, const char* dev_ver,
                                                     int is_serial) {
        UsbDeviceInfo info;
        info.is_serial = is_serial != 0;
        info.vid = static_cast<uint16_t>(vid);
        info.pid = static_cast<uint16_t>(pid);
        info.manufacturer = mfg ? mfg : "";
        info.product = prod ? prod : "";
        info.serial = serial ? serial : "";
        info.usb_version = usb_ver ? usb_ver : "";
        info.device_version = dev_ver ? dev_ver : "";
        WebUsbBridge::GetInstance().InternalOnDeviceAdded(dev, info);
    }

    EMSCRIPTEN_KEEPALIVE void webusb_cb_device_opened(int dev, const char* endpoints, int protocol, const char* info) {
        WebUsbBridge::GetInstance().InternalOnDeviceOpened(dev, endpoints, protocol, info);
    }

    EMSCRIPTEN_KEEPALIVE void webusb_cb_device_closed(int dev, const char* reason) {
        WebUsbBridge::GetInstance().InternalOnDeviceClosed(dev, reason);
    }

    EMSCRIPTEN_KEEPALIVE void webusb_cb_message(const char* text) {
        WebUsbBridge::GetInstance().InternalOnMessage(text);
    }

    EMSCRIPTEN_KEEPALIVE void webusb_cb_snapshot(const uint8_t* data, int len) {
        WebUsbBridge::GetInstance().InternalOnSnapshot(data, len);
    }

    EMSCRIPTEN_KEEPALIVE void webusb_cb_ctrl_in(int dev, int request, const uint8_t* data, int len) {
        WebUsbBridge::GetInstance().InternalOnControlIn(dev, static_cast<uint8_t>(request), data, len);
    }

    EMSCRIPTEN_KEEPALIVE void webusb_cb_ctrl_ack(int dev, int request, int ok) {
        WebUsbBridge::GetInstance().InternalOnControlOutDone(dev, request, ok != 0);
    }

    EMSCRIPTEN_KEEPALIVE void webusb_cb_sync(int dev, double samples, double accepted, double offset_ms, double drift_ppm,
                                             double jitter_us, double rtt_min_ms, double rtt_last_ms, double worst_us, double pushed) {
        ClockSyncStats s;
        s.valid = true;
        s.pushed = pushed != 0.0;
        s.samples = static_cast<uint32_t>(samples);
        s.accepted = static_cast<uint32_t>(accepted);
        s.offset_ms = offset_ms;
        s.drift_ppm = drift_ppm;
        s.jitter_us = jitter_us;
        s.rtt_min_ms = rtt_min_ms;
        s.rtt_last_ms = rtt_last_ms;
        s.worst_us = worst_us;
        WebUsbBridge::GetInstance().InternalOnClockSync(dev, s);
    }

    EMSCRIPTEN_KEEPALIVE void webusb_cb_events(int dev, const char* text) {
        WebUsbBridge::GetInstance().InternalOnEvents(dev, text);
    }

    EMSCRIPTEN_KEEPALIVE void webusb_cb_stats(int dev, int rx_per_sec, int tx_dropped, int recoveries,
                                              int rx_errors, int tx_errors) {
        WebUsbBridge::GetInstance().InternalOnStats(dev, static_cast<uint32_t>(rx_per_sec), static_cast<uint32_t>(tx_dropped),
                                                    static_cast<uint32_t>(recoveries), static_cast<uint32_t>(rx_errors),
                                                    static_cast<uint32_t>(tx_errors));
    }
}

WebUsbBridge& WebUsbBridge::GetInstance() {
    static WebUsbBridge instance;
    return instance;
}

const UsbDeviceStats& WebUsbBridge::GetDeviceStats(int dev) const {
    static const UsbDeviceStats empty;
    return ValidDevice(dev) ? dev_stats_[dev] : empty;
}

void WebUsbBridge::Initialize() {
    js_webusb_init();
}

void WebUsbBridge::RequestDevice() {
    js_webusb_request();
}

void WebUsbBridge::RequestSerialPort(int baud) {
    js_serial_request(baud);
}

void WebUsbBridge::SetSerialBaud(int dev, int baud) {
    if (ValidDevice(dev)) js_serial_set_baud(dev, baud);
}

void WebUsbBridge::ConnectDevice(int dev) {
    if (ValidDevice(dev)) js_webusb_connect(dev);
}

void WebUsbBridge::DisconnectDevice(int dev) {
    if (ValidDevice(dev)) js_webusb_disconnect(dev);
}

void WebUsbBridge::RemoveDevice(int dev) {
    if (!ValidDevice(dev)) return;
    js_webusb_remove(dev);
    dev_stats_[dev] = UsbDeviceStats();
}

void WebUsbBridge::StartRxPump(int dev, int endpoint, int max_packet_size, int flags) {
    if (!ValidDevice(dev)) return;
    dev_stats_[dev].sync = ClockSyncStats();         // the estimates of the previous run do not apply
    js_webusb_set_rx_pump(dev, 1, endpoint, max_packet_size, flags);
}

void WebUsbBridge::StopRxPump(int dev) {
    if (ValidDevice(dev)) js_webusb_set_rx_pump(dev, 0, 0, 0, 0);
}

void WebUsbBridge::TransferOut(int dev, int endpoint, const uint8_t* data, int length) {
    if (ValidDevice(dev)) js_webusb_transfer_out(dev, endpoint, data, length);
}

void WebUsbBridge::ControlTransferOut(int dev, uint8_t request, uint16_t value, uint16_t index, const uint8_t* data, int length, int id) {
    if (ValidDevice(dev)) js_webusb_control_out(dev, request, value, index, data, length, id);
}

void WebUsbBridge::ControlTransferIn(int dev, uint8_t request, uint16_t value, uint16_t index, uint16_t length) {
    if (ValidDevice(dev)) js_webusb_control_in(dev, request, value, index, length);
}

void WebUsbBridge::CommandOut(int dev, int id, const uint8_t* data, int length) {
    if (ValidDevice(dev)) js_webusb_cmd_out(dev, id, data, length);
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

void WebUsbBridge::InternalOnDeviceAdded(int dev, const UsbDeviceInfo& info) {
    if (!ValidDevice(dev)) return;
    if (on_device_added_) on_device_added_(dev, info);
}

void WebUsbBridge::InternalOnDeviceOpened(int dev, const char* endpoints, int protocol, const char* info) {
    if (!ValidDevice(dev)) return;
    DeviceOpenInfo open;
    open.endpoints = endpoints ? endpoints : "";
    open.protocol = (protocol == PROTOCOL_PEAK_PCAN_FD || protocol == PROTOCOL_SLCAN)
                        ? static_cast<CanProtocolType>(protocol) : PROTOCOL_GS_USB_CANDLELIGHT;
    open.info = info ? info : "";
    if (on_device_opened_) on_device_opened_(dev, open);
}

void WebUsbBridge::InternalOnDeviceClosed(int dev, const char* reason) {
    if (!ValidDevice(dev)) return;
    if (on_device_closed_) on_device_closed_(dev, reason ? reason : "");
}

void WebUsbBridge::InternalOnMessage(const char* text) {
    if (on_message_) on_message_(text ? text : "");
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
    for (int i = 0; i < kMaxDevices; ++i) dev_stats_[i].frames = h.dev_frames[i];

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

void WebUsbBridge::InternalOnControlIn(int dev, uint8_t request, const uint8_t* data, int length) {
    if (!ValidDevice(dev)) return;
    if (on_control_in_) on_control_in_(dev, request, data, length);
}

void WebUsbBridge::InternalOnControlOutDone(int dev, int request, bool ok) {
    if (!ValidDevice(dev)) return;
    if (on_control_out_done_) on_control_out_done_(dev, request, ok);
}

void WebUsbBridge::InternalOnClockSync(int dev, const ClockSyncStats& stats) {
    if (!ValidDevice(dev)) return;
    dev_stats_[dev].sync = stats;
}

void WebUsbBridge::InternalOnEvents(int dev, const char* text) {
    if (!ValidDevice(dev) || !text) return;
    if (on_events_) on_events_(dev, std::string(text));
}

void WebUsbBridge::InternalOnStats(int dev, uint32_t rx_per_sec, uint32_t tx_dropped, uint32_t recoveries,
                                   uint32_t rx_errors, uint32_t tx_errors) {
    if (!ValidDevice(dev)) return;
    UsbDeviceStats& s = dev_stats_[dev];
    s.rx_per_sec = rx_per_sec;
    s.tx_dropped = tx_dropped;
    s.recoveries = recoveries;
    s.rx_errors = rx_errors;
    s.tx_errors = tx_errors;
}
