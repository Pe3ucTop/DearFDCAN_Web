'use strict';
// usb_worker.js - the CAN worker: USB adapters, serial adapters and CAN processing for several adapters, in a dedicated Worker.
//
// The page (Emscripten/wasm + ImGui) talks to this worker only through postMessage; there is no SharedArrayBuffer/Atomics. Ownership of
// every ArrayBuffer is handed over with the transfer list, so two contexts never touch the same bytes. The messages are specified in
// WORKER_PROTOCOL.md.
//
// Every adapter is a context addressed by its device number `dev` (0..MAX_DEVICES-1, chosen by the page). A context owns its transport
// (a UsbPort of webusb_io.js, libs/webusb-io: open, receive slots, control/TX queues, recovery, watchdog; or a serial port of
// serial_worker.js), so a hung or unplugged adapter never delays the others. The classes here add what is specific to the CAN adapters:
//   Device        the part every USB adapter has in common: the messages of the page, the link to the core
//   GsUsbDevice   gs_usb (candleLight / CANable): vendor control requests configure the adapter, one frame per bulk transfer (Elmue:
//                 several messages); while a channel runs with adapter timestamps the adapter clock is read once a second
//   PcanDevice    PEAK PCAN-USB FD (uCAN): commands are bulk OUT transfers to a command endpoint, frames and reports are records on a
//                 shared bulk IN endpoint (a record can span two transfers); the open handshake reads the firmware info
//   SerialDevice  SLCAN on a Web Serial port: stages the bytes the serial worker delivers and turns the core's reply tokens into command
//                 acknowledgements
// The protocol of a USB adapter is chosen from its USB ids, or forced by `protocol` of `open`.
//
// Received transfers of all adapters are copied straight into the staging buffer of one shared wasm module (can_core.js/.wasm, built from
// libs/can-engine) which parses the frames (tagged with the device number), keeps the "Table by ID" aggregates and the trace queue, and
// publishes compact snapshots of what changed. The page only applies snapshots; it never sees individual frames of a busy bus.

// The libraries and the core are loaded once per worker. In a multi-file deployment they are fetched with importScripts(); the single-file
// bundle made by pack_worker.py already contains them in front of this script. If the core fails to load, open requests are answered
// with 'closed'.
if (typeof WebUsbIo === 'undefined') importScripts('webusb_io.js');
const { UsbPort, withTimeout, errText, sameDevice, describeEndpoints, inspectEndpoints } = WebUsbIo;

const MAX_DEVICES = 8;              // must match kMaxDevices in can_protocol_common.h
const IFACE = 0;
const GS_RX_SIZE = 2048;            // gs_usb receive transfers: room for an Elmue blob (ELM_MAX_BLOB_SIZE in can_protocol_elmue.h)
const RX_SLOTS = 6;                 // concurrent transferIn requests kept pending per adapter
const TX_INFLIGHT = 4;              // concurrent transferOut per adapter; set to 1 for strictly serialized TX
const TX_QUEUE_MAX = 4096;
const BATCH_CREDITS = 4;            // snapshots that may be unacknowledged by the page
const PUBLISH_MS = 16;              // snapshot period (about one display frame)
const STAGE_REC_HEADER = 12;        // u16 len | u8 dev | u8 ep_address | f64 host_time_ms
const CTRL_TIMEOUT_MS = 2000;
const WATCHDOG_MS = 1000;
const IDLE_PING_MS = 2000;          // RX silence after which an adapter is probed with a control IN
const PING_TIMEOUT_MS = 1500;
const PING_FAILS_MAX = 2;
const RX_ERRORS_BEFORE_RECOVER = 5;
const GS_USB_BREQ_BT_CONST = 4;     // harmless read used as liveness probe
const GS_USB_BREQ_TIMESTAMP = 6;    // the adapter's microsecond clock, read periodically to synchronise it with the page

// The USB transport of every adapter (see webusb_io.js); device matching skips the devices other contexts hold
const USB_OPTIONS = {
    iface: IFACE, rxSlots: RX_SLOTS, txInflight: TX_INFLIGHT, txQueueMax: TX_QUEUE_MAX, ctrlTimeoutMs: CTRL_TIMEOUT_MS,
    idlePingMs: IDLE_PING_MS, pingTimeoutMs: PING_TIMEOUT_MS, pingFailsMax: PING_FAILS_MAX, rxErrorsBeforeRecover: RX_ERRORS_BEFORE_RECOVER,
    tag: 'usb_worker', inUse: () => devices.filter(x => x && x.usb).map(x => x.usb)
};

// Clock synchronisation of gs_usb adapters: a burst of samples after the channel start for a quick first estimate, then
// one per second; the core fits a line (offset + drift) through the samples with the shortest round trips
const SYNC_FIRST_MS = 100;
const SYNC_BURST = 8;
const SYNC_BURST_MS = 150;
const SYNC_MS = 1000;
const SYNC_FAILS_MAX = 3;           // consecutive failures after which the adapter is not asked again

// Protocols (CanProtocolType in can_protocol_common.h)
const PROTO_GS = 0;
const PROTO_PCAN = 1;
const PROTO_SLCAN = 2;              // serial adapters (WebSerial), I/O is done by serial_worker.js

// SLCAN command acknowledgement: replies carry no id, they come in command order
const SLCAN_ID_VERSION = 0x56;      // the 'V' request, answered with a text line (SLCAN_CMD_VERSION in can_protocol_slcan.h)
const REPLY_TIMEOUT_MS = 1500;
let serialLink = null;              // MessagePort to serial_worker.js

// PEAK PCAN-USB FD (values from PEAK's Linux driver)
const PCAN_VID = 0x0C72;            // PIDs 0x0011 Pro FD, 0x0012 FD, 0x0013 Chip, 0x0014 X6
const PCAN_RX_SIZE = 2048;          // like the vendor driver: larger transfers split fewer records
const PCAN_EP_CMD_OUT = 1, PCAN_EP_CMD_IN = 1, PCAN_EP_MSG_IN = 2, PCAN_EP_MSG_OUT0 = 2;   // defaults, numbers without direction
const PCAN_VREQ_INFO = 0, PCAN_VREQ_FKT = 2;
const PCAN_VREQ_INFO_FIRMWARE = 1, PCAN_VREQ_FKT_DRIVER_LOADED = 5;
const PCAN_FWINFO_TYPE_EXT = 2;     // from this record type on the firmware info lists the endpoint numbers
const PCAN_FWINFO_SIZE = 84;

let mainOrigin = 0;
const devices = new Array(MAX_DEVICES).fill(null);

// CAN core (shared wasm module) and snapshot flow control
let core = null;
let stagePtr = 0, stageCap = 0, stageOff = 0, outPtr = 0, snapCap = 0;
let pubTimer = null;
let wdTimer = null;
let credits = BATCH_CREDITS;
let pool = [];
const stageHdr = new DataView(new ArrayBuffer(STAGE_REC_HEADER));
const stageHdrBytes = new Uint8Array(stageHdr.buffer);

// MODULARIZE'd Emscripten output defines the createCanCore factory
const coreReady = (async () => {
    if (typeof createCanCore === 'undefined') importScripts('can_core.js');
    const m = await createCanCore();
    core = m;
    stagePtr = m._core_stage_ptr();
    stageCap = m._core_stage_cap();
    outPtr = m._core_out_ptr();
    snapCap = m._core_out_cap();
})();
coreReady.catch(err => console.error('[usb_worker] cannot load can_core.js/.wasm:', err));

function now() {
    return performance.now() + performance.timeOrigin - mainOrigin;
}

// Same table as CanDetectProtocol() in can_protocol_common.h
function detectProtocol(vid, pid) {
    return (vid === PCAN_VID && pid >= 0x0011 && pid <= 0x0014) ? PROTO_PCAN : PROTO_GS;
}

// Firmware info record of a PCAN-USB FD adapter (layout of struct pcan_usbfd_fw_info; the fields after the
// first 28 bytes exist only in newer records, the record says how long it is)
function parsePcanFwInfo(dv) {
    const len = dv.byteLength;
    if (len < 28) return null;
    const u8 = (o) => dv.getUint8(o);
    const size = dv.getUint16(0, true);
    const fw = { size: size, type: dv.getUint16(2, true), hwType: u8(4), bl: [u8(5), u8(6), u8(7)], hwVersion: u8(8),
                 fwVersion: [u8(9), u8(10), u8(11)], devId: [dv.getUint32(12, true), dv.getUint32(16, true)],
                 serial: dv.getUint32(20, true), flags: dv.getUint32(24, true), endpoints: null, canCount: 0 };
    if (fw.type >= PCAN_FWINFO_TYPE_EXT && len >= 33 && size >= 33) {
        fw.endpoints = { cmdOut: u8(28) & 0x7F, cmdIn: u8(29) & 0x7F, msgOut: [u8(30) & 0x7F, u8(31) & 0x7F], msgIn: u8(32) & 0x7F };
    }
    if (len >= 34 && size >= 34) fw.canCount = u8(33);
    return fw;
}

// Open handshake of a PCAN-USB FD adapter: tell the module a driver is using it, read the firmware info
// and derive the endpoint layout. Both requests are best effort: without them the default layout is used.
async function pcanHandshake(usb) {
    const vendorOther = (request, value) => ({ requestType: 'vendor', recipient: 'other', request: request, value: value, index: 0 });
    try {
        await withTimeout(usb.controlTransferOut(vendorOther(PCAN_VREQ_FKT, PCAN_VREQ_FKT_DRIVER_LOADED), new Uint8Array([0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0])),
                          CTRL_TIMEOUT_MS, 'PCAN driver-loaded notification timeout');
    } catch (e) { console.warn('[usb_worker] PCAN driver-loaded notification failed:', errText(e)); }

    let fw = null;
    try {
        const r = await withTimeout(usb.controlTransferIn(vendorOther(PCAN_VREQ_INFO, PCAN_VREQ_INFO_FIRMWARE), PCAN_FWINFO_SIZE),
                                    CTRL_TIMEOUT_MS, 'PCAN firmware info timeout');
        if (r.status === 'ok' && r.data) fw = parsePcanFwInfo(r.data);
    } catch (e) { console.warn('[usb_worker] PCAN firmware info failed:', errText(e)); }

    const ep = fw && fw.endpoints;
    const layout = {
        cmdOut: ep ? ep.cmdOut : PCAN_EP_CMD_OUT, cmdIn: ep ? ep.cmdIn : PCAN_EP_CMD_IN,
        rxEp: ep ? ep.msgIn : PCAN_EP_MSG_IN, txEp: ep ? ep.msgOut[0] : PCAN_EP_MSG_OUT0
    };
    // "key=value;..." for the page
    let info = '';
    if (fw) {
        const ver = (a) => a[0] + '.' + a[1] + '.' + a[2];
        info = 'fw=' + ver(fw.fwVersion) + ';bl=' + ver(fw.bl) + ';hw=' + fw.hwVersion + ';mcu=' + fw.hwType + ';ser=' + fw.serial +
               ';ch=' + fw.canCount + ';fwtype=' + fw.type;
    }
    return { layout: layout, info: info };
}

// ---------------------------------------------------------------------------------------------
// One USB adapter: the transport is the UsbPort, this class connects it to the page and to the core
// ---------------------------------------------------------------------------------------------

class Device extends UsbPort {
    constructor(dev) {
        super('dev ' + dev, USB_OPTIONS);
        this.dev = dev;
        this.epoch = 0;
        this.rxEnabled = false;         // the page opened the gate: received frames are processed
        this.protocol = this.protocolId;
        this.syncTotal = 0;             // clock samples the core had when the statistics were last posted
    }

    post(type, fields, transfer) {
        const msg = fields || {};
        msg.type = type;
        msg.dev = this.dev;
        msg.epoch = this.epoch;
        self.postMessage(msg, transfer || []);
    }

    get active() { return this.isOpen; }

    // -----------------------------------------------------------------------------------------
    // What a protocol class (GsUsbDevice, PcanDevice below) provides; the rest of this class is the same for every USB adapter
    // -----------------------------------------------------------------------------------------

    get protocolId() { return PROTO_GS; }                   // CanProtocolType
    async handshake(usb) { throw new Error('not implemented'); }   // -> { layout: {rxEp, txEp, cmdEp, rxSize}, info, text }
    onRxEnable(m) {}                // the receive gate opens (the page started a channel); the core is told the protocol here
    onRxStarted(m) {}               // the gate is open and the adapter is connected
    onRxStopped() {}                // the gate closed
    onTeardown() {}                 // the session ends
    drainAux() {}                   // after the core processed a transfer: events and statistics that came with it
    // (probeRequest() and onTxQueued() are hooks of the UsbPort)

    // -----------------------------------------------------------------------------------------
    // Hooks of the UsbPort
    // -----------------------------------------------------------------------------------------

    onOpening() { core._core_set_protocol(this.dev, this.protocolId); }      // also starts a fresh receive stream
    onRecovered() { core._core_reset_stream(this.dev); }                     // the aborted transfers may have cut a record in two
    beforeTeardown() { publishTick(); }                                      // hand over the last aggregates (of all adapters)
    afterTeardown() {
        this.rxEnabled = false;
        this.onTeardown();
        updateTimers();
    }
    onLost(reason) { this.post('closed', { reason: reason }); }

    // The warnings and errors of the transport go to the page's event log (the console is gone when it was not open). At most a burst of
    // 10 lines and then 2 per second per adapter, so a failure that repeats cannot flood the page.
    onLog(level, text) {
        const t = performance.now();
        this.logBudget = Math.min(10, (this.logBudget === undefined ? 10 : this.logBudget) + (t - (this.logAt || t)) / 500);
        this.logAt = t;
        if (this.logBudget < 1) { this.logDropped = (this.logDropped || 0) + 1; return; }
        this.logBudget -= 1;
        const dropped = this.logDropped || 0;
        this.logDropped = 0;
        this.post('log', { level: level, text: dropped ? text + ' (' + dropped + ' lines before were left out)' : text });
    }

    // Copy one completed transfer into the core's staging buffer as [u16 len][u8 dev][u8 ep][f64 time][bytes]
    onRxData(dv) {
        if (!this.rxEnabled || !core) return;
        stageBytes(this.dev, 0x80 | this.rxEp, new Uint8Array(dv.buffer, dv.byteOffset, dv.byteLength), now());
    }

    // Statistics of the clock samples for the page: [samples, good, offset ms, drift ppm, spread µs, min rtt ms, last rtt ms, worst µs, pushed]
    postSync() {
        const ptr = core._core_clock_stats(this.dev);
        const s = Array.from(new Float64Array(core.HEAPU8.buffer, ptr, 9));
        this.syncTotal = s[0];
        this.post('sync', { s: s });
    }

    // Event lines of the Elmue protocol ("U<percent>", "T<text>", "E<id>:<data>"): bus load, firmware text, error reports
    drainEvents() {
        const n = core._core_take_events(this.dev);
        if (n <= 0) return;
        const ptr = core._core_reply_ptr();
        this.post('events', { text: utf8.decode(core.HEAPU8.subarray(ptr, ptr + n)) });
    }

    // -----------------------------------------------------------------------------------------
    // Open / close
    // -----------------------------------------------------------------------------------------

    async open(m) {
        if (this.usb) await this.close(null, true);
        this.epoch = m.epoch;

        try {
            await coreReady;
        } catch (err) {
            this.post('closed', { reason: 'CAN core module failed to load: ' + errText(err) });
            return;
        }

        // The endpoint layout (and, for PEAK, the firmware info) depends on the protocol of this class
        let hs;
        try {
            hs = await this.openDevice({ vid: m.vid, pid: m.pid, serial: m.serial }, usb => this.handshake(usb));
        } catch (err) {
            this.error('open failed:', err);
            this.post('closed', { reason: 'open failed: ' + errText(err) });
            return;
        }

        this.protocol = this.protocolId;
        this.st.rxPrev = this.st.rx;
        updateTimers();
        this.post('opened', { eps: hs.text, rxEp: hs.layout.rxEp, txEp: hs.layout.txEp, protocol: this.protocol, info: hs.info });
    }

    async close(reason, silent) {
        await this.closeDevice();
        if (!silent) this.post('closed', { reason: reason || '' });
    }

    // -----------------------------------------------------------------------------------------
    // Control and command transfers: executed strictly in order (the page's messages go through enqueue())
    // -----------------------------------------------------------------------------------------

    async control(m) {
        const isOut = m.type === 'ctrl_out';
        const ackId = m.id >= 0 ? m.id : m.request;     // the page may tag a control OUT with an id of its own
        if (!this.usb) {
            if (isOut) this.post('ctrl_ack', { request: ackId, ok: false });
            return;
        }
        const setup = {
            requestType: m.reqType || 'vendor', recipient: m.recipient || 'interface',
            request: m.request, value: m.value, index: m.index
        };
        const r = await this.controlTransfer(isOut, setup, isOut ? m.data : m.length);
        if (isOut) this.post('ctrl_ack', { request: ackId, ok: r.ok });
        else if (r.data) this.post('ctrl_in', { request: m.request, data: r.data }, [r.data]);
    }

    // Bulk OUT command transfer (PCAN): same queue and timeout as the control transfers, acknowledged
    // with the id the page gave it (reported as ctrl_ack {request: id})
    async cmdOut(m) {
        if (!this.usb) {
            this.post('ctrl_ack', { request: m.id, ok: false });
            return;
        }
        const r = await this.commandTransfer(m.ep ? (m.ep & 0x7F) : this.cmdEp, m.data, 'id ' + m.id);
        this.post('ctrl_ack', { request: m.id, ok: r.ok });
    }

    // -----------------------------------------------------------------------------------------
    // The receive gate of the page
    // -----------------------------------------------------------------------------------------

    setRx(m) {
        const enable = !!m.enable;
        if (!enable) {
            processStage();                 // everything received before the stop still counts
            this.rxEnabled = false;
            this.onRxStopped();
            return;
        }
        if (!this.rxEnabled && core) {
            core._core_reset_clock(this.dev);       // new channel start: adapter clock may restart
            this.syncTotal = 0;
            this.onRxEnable(m);                     // e.g. gs_usb: the channel runs in Elmue mode and with timestamps
            core._core_reset_stream(this.dev);      // and the record stream starts at a record boundary
        }
        this.rxEnabled = true;
        if (!this.usb) return;
        this.onRxStarted(m);
        this.reconfigureRx(m.ep, m.size);
    }

    // -----------------------------------------------------------------------------------------
    // Statistics for the page (once a second) and the watchdog of the transport
    // -----------------------------------------------------------------------------------------

    tick() {
        if (!this.usb) return;
        this.post('stats', {
            rxPerSec: this.st.rx - this.st.rxPrev, txDropped: this.st.txDropped,
            recoveries: this.st.recoveries, rxErrors: this.st.rxErrors, txErrors: this.st.txErrors
        });
        this.st.rxPrev = this.st.rx;
        this.watchdog();
    }
}

// ---------------------------------------------------------------------------------------------
// gs_usb (candleLight / CANable) in its legacy and in the ElmueSoft protocol: configured with vendor control requests, one frame per
// bulk transfer (Elmue: several messages). While a channel runs with adapter timestamps the worker reads the adapter's clock once a second.
// ---------------------------------------------------------------------------------------------

class GsUsbDevice extends Device {
    constructor(dev) {
        super(dev);
        this.elmue = false;             // the channel runs in Elmue mode: the core needs the transmitted frames to resolve the tx echoes

        // Clock synchronisation (GS_USB_BREQ_TIMESTAMP)
        this.syncOn = false;
        this.syncTimer = 0;
        this.syncFails = 0;
        this.syncCount = 0;
    }

    get protocolId() { return PROTO_GS; }

    async handshake(usb) {
        const ep = inspectEndpoints(usb, USB_OPTIONS);
        // 2048 bytes: the biggest transfer of the Elmue protocol (a blob of several frames); a legacy adapter simply
        // ends its transfers early. The page asks for the same size when it starts an Elmue channel, so no re-claim is needed.
        return { layout: { rxEp: ep.rxEp, txEp: ep.txEp, cmdEp: 0, rxSize: Math.max(GS_RX_SIZE, ep.rxSize) }, info: '', text: ep.text };
    }

    probeRequest() {
        return [{ requestType: 'vendor', recipient: 'interface', request: GS_USB_BREQ_BT_CONST, value: 0, index: IFACE }, 40];
    }

    // The page says whether the channel runs in Elmue mode (compact messages) and with timestamps
    onRxEnable(m) {
        this.elmue = !!m.elmue;
        if (this.elmue) core._core_set_elmue(this.dev, m.ts ? 1 : 0);
        else core._core_set_protocol(this.dev, PROTO_GS);
    }

    onRxStarted(m) { if (m.ts) this.startSync(); }
    onRxStopped() { this.stopSync(); }
    onTeardown() { this.elmue = false; this.stopSync(); }

    // Elmue echoes only carry a marker: the core keeps the frame until its echo arrives
    onTxQueued(data, ep) {
        if (this.elmue && this.rxEnabled && core) stageBytes(this.dev, ep ? (ep & 0x7F) : this.txEp, new Uint8Array(data), now());
    }

    drainAux() { if (this.elmue) this.drainEvents(); }

    // -----------------------------------------------------------------------------------------
    // Clock synchronisation: the adapter's timestamp is read with a control transfer; the round trip brackets the moment
    // the adapter latched it, so the middle of it is the page time of the sample (the core filters by round trip)
    // -----------------------------------------------------------------------------------------

    startSync() {
        if (this.syncOn) return;
        this.syncOn = true;
        this.syncFails = 0;
        this.syncCount = 0;
        this.scheduleSync(SYNC_FIRST_MS);
    }

    stopSync() {
        this.syncOn = false;
        clearTimeout(this.syncTimer);
        this.syncTimer = 0;
    }

    scheduleSync(ms) {
        clearTimeout(this.syncTimer);
        // queued behind the control transfers that are still waiting (the channel start), like every control transfer
        this.syncTimer = setTimeout(() => { if (this.syncOn && this.usb) this.enqueue(() => this.syncOnce()); }, ms);
    }

    async syncOnce() {
        const d = this.usb;
        if (!this.syncOn || !d || !core) return;
        let ok = false;
        const setup = { requestType: 'vendor', recipient: 'interface', request: GS_USB_BREQ_TIMESTAMP, value: 0, index: IFACE };
        const t0 = now();
        const r = await this.controlTransfer(false, setup, 4);
        const t1 = now();
        if (this.usb !== d) return;                 // the adapter is gone (controlTransfer ended the session)
        if (r.ok && r.data && r.data.byteLength >= 4 && this.syncOn) {
            processStage();                         // frames received so far are mapped with the estimate they were received under
            core._core_clock_sample(this.dev, new DataView(r.data).getUint32(0, true), (t0 + t1) / 2, t1 - t0);
            this.noteAlive();                       // the control pipe answered: no liveness probe needed
            this.postSync();
            ok = true;
        }
        if (!this.syncOn) return;
        if (ok) {
            this.syncFails = 0;
        } else if (++this.syncFails >= SYNC_FAILS_MAX) {
            this.warn('does not answer the clock request, synchronisation stopped');
            this.syncOn = false;
            return;
        }
        this.syncCount++;
        this.scheduleSync(this.syncCount < SYNC_BURST ? SYNC_BURST_MS : SYNC_MS);
    }
}

// ---------------------------------------------------------------------------------------------
// PEAK PCAN-USB FD (uCAN): commands are bulk OUT transfers, frames and reports are records on a shared bulk IN endpoint. The adapter
// pushes calibration records (its clock), whose statistics are posted when a new one arrived.
// ---------------------------------------------------------------------------------------------

class PcanDevice extends Device {
    get protocolId() { return PROTO_PCAN; }

    async handshake(usb) {
        const hs = await pcanHandshake(usb);
        return {
            layout: { rxEp: hs.layout.rxEp, txEp: hs.layout.txEp, cmdEp: hs.layout.cmdOut, rxSize: PCAN_RX_SIZE },
            info: hs.info,
            text: describeEndpoints(usb, IFACE) || ('CMD OUT ' + hs.layout.cmdOut + ', CMD IN ' + hs.layout.cmdIn + ', MSG IN ' + hs.layout.rxEp + ', MSG OUT ' + hs.layout.txEp)
        };
    }

    probeRequest() {
        return [{ requestType: 'vendor', recipient: 'other', request: PCAN_VREQ_INFO, value: PCAN_VREQ_INFO_FIRMWARE, index: 0 }, 36];
    }

    drainAux() {
        this.drainEvents();
        if (this.rxEnabled) {
            const ptr = core._core_clock_stats(this.dev);
            if (new Float64Array(core.HEAPU8.buffer, ptr, 1)[0] !== this.syncTotal) this.postSync();
        }
    }
}

// ---------------------------------------------------------------------------------------------
// One serial (SLCAN) adapter. The port itself is owned by serial_worker.js; this context stages the bytes it
// delivers into the shared core and turns the core's reply tokens into command acknowledgements.
// ---------------------------------------------------------------------------------------------

const utf8 = new TextDecoder();

class SerialDevice {
    constructor(dev) {
        this.dev = dev;
        this.epoch = 0;
        this.isOpen = false;
        this.opening = false;
        this.rxEnabled = false;
        this.chain = Promise.resolve();
        // Commands that wait for a reply, oldest first. SLCAN replies carry no id, they come in command order.
        this.pending = [];
        this.st = { rx: 0, rxPrev: 0, rxErrors: 0, txDropped: 0, txErrors: 0, recoveries: 0 };
        this.port = { rxErrors: 0, txErrors: 0, txDropped: 0 };
    }

    get active() { return this.isOpen; }

    post(type, fields, transfer) {
        const msg = fields || {};
        msg.type = type;
        msg.dev = this.dev;
        msg.epoch = this.epoch;
        self.postMessage(msg, transfer || []);
    }

    enqueue(fn) {
        this.chain = this.chain.then(fn).catch(err => console.error('[usb_worker] dev ' + this.dev + ' command failed:', err));
    }

    async open(m) {
        if (this.isOpen || this.opening) await this.close(null, true);
        this.epoch = m.epoch;
        try {
            await coreReady;
        } catch (err) {
            this.post('closed', { reason: 'CAN core module failed to load: ' + errText(err) });
            return;
        }
        if (!serialLink) {
            this.post('closed', { reason: 'the serial worker is not available' });
            return;
        }
        this.opening = true;
        core._core_set_protocol(this.dev, PROTO_SLCAN);       // fresh stream, gate open until the page says otherwise
        serialLink.postMessage({ type: 'sopen', dev: this.dev, epoch: this.epoch, baud: m.baud | 0 });
    }

    async close(reason, silent) {
        if (!this.isOpen && !this.opening) {
            if (!silent) this.post('closed', { reason: reason || '' });
            return;
        }
        this.teardown();
        if (serialLink) serialLink.postMessage({ type: 'sclose', dev: this.dev });
        if (!silent) this.post('closed', { reason: reason || '' });
    }

    // Everything the serial worker reports about this port
    onLink(m) {
        if (m.epoch !== this.epoch) return;
        switch (m.type) {
            case 'sopened':
                if (!this.opening) return;
                this.opening = false;
                this.isOpen = true;
                this.st.rxPrev = this.st.rx;
                updateTimers();
                this.post('opened', { eps: m.eps, rxEp: 0, txEp: 0, protocol: PROTO_SLCAN, info: m.info || '' });
                break;
            case 'sclosed':
                if (this.isOpen || this.opening) {
                    this.teardown();
                    this.post('closed', { reason: m.reason || '' });
                }
                break;
            case 'srx':
                if (!this.isOpen || !core) return;
                this.st.rx++;
                stageBytes(this.dev, 0x80, new Uint8Array(m.buf), m.t - mainOrigin);
                break;
            case 'swack': {
                // The write finished. A reply may already have been matched (it can overtake this message).
                const i = this.pending.findIndex(p => p.id === m.id);
                if (i < 0) break;
                const p = this.pending[i];
                if (!m.ok || !p.expectReply) {
                    this.pending.splice(i, 1);
                    clearTimeout(p.timer);
                    this.post('ctrl_ack', { request: p.id, ok: !!m.ok });
                }
                break;
            }
            case 'sstats':
                this.port = { rxErrors: m.rxErrors, txErrors: m.txErrors, txDropped: m.txDropped };    // counted by the serial worker
                break;
        }
    }

    replyTimeout(p) {
        const i = this.pending.indexOf(p);
        if (i < 0) return;
        this.pending.splice(i, 1);
        console.warn('[usb_worker] dev ' + this.dev + ' no reply to command ' + p.id);
        this.post('ctrl_ack', { request: p.id, ok: false });
    }

    // Reply tokens of the core: "K" ok, "E<c>" error, "L<text>" any other line, one per '\n'
    drainReplies() {
        const n = core._core_take_replies(this.dev);
        if (n <= 0) return;
        const ptr = core._core_reply_ptr();
        const text = utf8.decode(core.HEAPU8.subarray(ptr, ptr + n));
        for (const tok of text.split('\n')) {
            if (!tok) continue;
            const kind = tok[0];
            const head = this.pending.find(p => p.expectReply);     // replies come in command order
            if (kind === 'L') {
                if (head && head.wantLine) {
                    this.pending.splice(this.pending.indexOf(head), 1);
                    clearTimeout(head.timer);
                    const ab = new TextEncoder().encode(tok.substring(1)).buffer;
                    this.post('ctrl_in', { request: head.id, data: ab }, [ab]);
                }
                continue;       // other text (debug output, TX markers): nothing waits for it
            }
            if (!head) {
                if (kind === 'E') this.st.txErrors++;                // error reply to a transmitted frame
                continue;
            }
            this.pending.splice(this.pending.indexOf(head), 1);
            clearTimeout(head.timer);
            // a line was expected but the adapter answered with a plain ack/error: the command failed
            this.post('ctrl_ack', { request: head.id, ok: kind === 'K' && !head.wantLine });
        }
    }

    // Text command (one line, '\r' terminated). id is given back in the acknowledgement; the version request
    // (id SLCAN_ID_VERSION) is answered with a text line instead.
    cmdOut(m) {
        if (!this.isOpen || !serialLink) {
            this.post('ctrl_ack', { request: m.id, ok: false });
            return;
        }
        const bytes = new Uint8Array(m.data);
        const noReply = bytes.length === 2 && bytes[0] === 0x43 && bytes[1] === 0x0D;    // "C\r" is never answered
        const p = { id: m.id, expectReply: !noReply, wantLine: m.id === SLCAN_ID_VERSION, timer: 0 };
        if (p.expectReply) p.timer = setTimeout(() => this.replyTimeout(p), REPLY_TIMEOUT_MS);   // covers the write and the reply
        this.pending.push(p);
        serialLink.postMessage({ type: 'swrite', dev: this.dev, epoch: this.epoch, data: m.data, id: m.id || 1 }, [m.data]);
    }

    control(m) {
        if (m.type === 'ctrl_out') this.post('ctrl_ack', { request: m.request, ok: false });    // serial adapters have no control pipe
    }

    enqueueTx(data) {
        if (!this.isOpen || !serialLink) { this.st.txDropped++; return; }
        serialLink.postMessage({ type: 'swrite', dev: this.dev, epoch: this.epoch, data: data, id: 0 }, [data]);
    }

    setRx(m) {
        const enable = !!m.enable;
        if (!core) return;
        if (!enable) {
            processStage();                 // everything received before the stop still counts
            this.rxEnabled = false;
            core._core_set_rx_enabled(this.dev, 0);
            return;
        }
        if (!this.rxEnabled) {
            processStage();
            core._core_reset_clock(this.dev);
            core._core_reset_stream(this.dev);
            core._core_set_rx_enabled(this.dev, 1);
        }
        this.rxEnabled = true;
    }

    teardown() {
        if (this.isOpen || this.opening) publishTick();
        this.isOpen = false;
        this.opening = false;
        this.rxEnabled = false;
        if (core) core._core_set_rx_enabled(this.dev, 1);
        for (const p of this.pending) {
            clearTimeout(p.timer);
            this.post('ctrl_ack', { request: p.id, ok: false });
        }
        this.pending = [];
        updateTimers();
    }

    tick() {
        if (!this.isOpen) return;
        // own counters: frames the adapter refused (error reply) and writes dropped here; the port's: stream errors
        const p = this.port;
        this.post('stats', {
            rxPerSec: this.st.rx - this.st.rxPrev, txDropped: this.st.txDropped + p.txDropped,
            recoveries: 0, rxErrors: this.st.rxErrors + p.rxErrors, txErrors: this.st.txErrors + p.txErrors
        });
        this.st.rxPrev = this.st.rx;
    }
}

// ---------------------------------------------------------------------------------------------
// Shared timers: snapshot publishing and watchdog run while at least one adapter is open
// ---------------------------------------------------------------------------------------------

function updateTimers() {
    const open = devices.some(x => x && x.active);
    if (open) {
        if (pubTimer === null) pubTimer = setInterval(publishTick, PUBLISH_MS);
        if (wdTimer === null) wdTimer = setInterval(watchdogTick, WATCHDOG_MS);
    } else {
        if (pubTimer !== null) { clearInterval(pubTimer); pubTimer = null; }
        if (wdTimer !== null) { clearInterval(wdTimer); wdTimer = null; }
    }
}

function watchdogTick() {
    for (const d of devices) if (d) d.tick();
}

// ---------------------------------------------------------------------------------------------
// CAN core: staging, periodic snapshots with credit-based flow control
// ---------------------------------------------------------------------------------------------

// Copies one received chunk into the core's staging buffer as [u16 len][u8 dev][u8 ep][f64 time][bytes];
// host_ms is on the page's performance.now() timeline
function stageBytes(dev, ep, bytes, hostMs) {
    const len = bytes.byteLength;
    if (len > 0xFFFF || len === 0) return;
    if (stageOff + STAGE_REC_HEADER + len > stageCap) processStage();

    stageHdr.setUint16(0, len, true);
    stageHdr.setUint8(2, dev);
    stageHdr.setUint8(3, ep);
    stageHdr.setFloat64(4, hostMs, true);

    const heap = core.HEAPU8;           // re-read on every use: the heap may be replaced on growth
    heap.set(stageHdrBytes, stagePtr + stageOff);
    heap.set(bytes, stagePtr + stageOff + STAGE_REC_HEADER);
    stageOff += STAGE_REC_HEADER + len;
}

function processStage() {
    if (stageOff > 0 && core) {
        core._core_process(stageOff);
        stageOff = 0;
        // serial adapters: the lines that were not frames are answers to commands
        for (const d of devices) {
            if (d instanceof SerialDevice) { if (d.isOpen) d.drainReplies(); }
            else if (d && d.usb) d.drainAux();
        }
    }
}

// Publishes at most one snapshot per tick. Without a credit nothing is published and the core keeps
// aggregating: updates of one ID coalesce, the trace queue is bounded, so a slow page costs
// resolution but never memory.
function publishTick() {
    if (!core) return;
    processStage();
    if (credits <= 0) return;
    const n = core._core_publish();
    if (n <= 0) return;
    const buf = pool.pop() || new ArrayBuffer(snapCap);
    new Uint8Array(buf, 0, n).set(core.HEAPU8.subarray(outPtr, outPtr + n));
    credits--;
    self.postMessage({ type: 'snapshot', buf: buf, used: n }, [buf]);
}

function returnBuffer(buf) {
    if (buf && buf.byteLength === snapCap && pool.length < BATCH_CREDITS) pool.push(buf);
    if (credits < BATCH_CREDITS) credits++;
}

// Commands from the page that act on the core state. They run after the already received frames
// have been processed, so a "clear" also removes everything that arrived before it.
function coreCommand(m) {
    coreReady.then(() => {
        processStage();
        switch (m.op) {
            case 1: core._core_clear_table(); break;
            case 2: core._core_clear_trace(); break;
            case 3: core._core_set_trace_enabled(m.arg ? 1 : 0); break;
        }
    }, () => {});
}

// ---------------------------------------------------------------------------------------------
// Command dispatch
// ---------------------------------------------------------------------------------------------

self.onmessage = (ev) => {
    const m = ev.data;
    switch (m.type) {
        case 'init': mainOrigin = m.timeOrigin; return;
        case 'ret':  returnBuffer(m.buf); return;
        case 'core': coreCommand(m); return;
        case 'serial_link':
            serialLink = m.port;
            serialLink.onmessage = onSerialMessage;
            return;
    }

    const n = m.dev;
    if (!Number.isInteger(n) || n < 0 || n >= MAX_DEVICES) return;
    let d = devices[n];

    if (m.type === 'open') {
        // A slot is a PEAK adapter, another USB adapter (gs_usb) or a serial port, each with a context class of its own; reusing
        // the slot for another kind replaces the context. The protocol is forced by the page or chosen from the USB ids.
        const Kind = m.transport === 'serial' ? SerialDevice
            : ((m.protocol === PROTO_PCAN || (m.protocol !== PROTO_GS && detectProtocol(m.vid, m.pid) === PROTO_PCAN)) ? PcanDevice : GsUsbDevice);
        if (d && d.constructor !== Kind) {
            d.close(null, true);
            d = devices[n] = null;
        }
        if (!d) d = devices[n] = new Kind(n);
        d.enqueue(() => d.open(m));
        return;
    }
    if (!d) return;                         // command for a device that was never opened

    switch (m.type) {
        case 'close':    d.enqueue(() => d.close('closed by host', false)); break;
        case 'ctrl_out':
        case 'ctrl_in':  d.enqueue(() => d.control(m)); break;
        case 'cmd_out':  d.enqueue(() => d.cmdOut(m)); break;
        case 'tx':       d.enqueueTx(m.data, m.ep); break;
        case 'rx':       d.setRx(m); break;
    }
};

// Messages from serial_worker.js (received bytes, open/close results, write acknowledgements)
function onSerialMessage(ev) {
    const m = ev.data;
    const d = Number.isInteger(m.dev) ? devices[m.dev] : null;
    if (d instanceof SerialDevice) d.onLink(m);
}

if (typeof navigator !== 'undefined' && navigator.usb) {
    navigator.usb.addEventListener('disconnect', (ev) => {
        // Prefer object identity: identical adapters without serial number look the same otherwise
        const open = devices.filter(x => x && x.usb);
        let hit = open.find(x => x.usb === ev.device);
        if (!hit) {
            const same = open.filter(x => sameDevice(ev.device, x.usb));
            if (same.length === 1) hit = same[0];
        }
        if (hit) hit.lost('device disconnected');
    });
}
