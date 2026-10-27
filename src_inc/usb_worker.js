'use strict';
// usb_worker.js - WebUSB transport and CAN processing for several adapters, running in a dedicated Worker.
//
// The page (Emscripten/wasm + ImGui) talks to this worker only through postMessage; there is no
// SharedArrayBuffer/Atomics. Ownership of every ArrayBuffer is handed over with the transfer list,
// so two contexts never touch the same bytes.
//
// Every adapter is a Device context addressed by its device number `dev` (0..MAX_DEVICES-1, chosen by
// the page). A context owns its USBDevice, receive slots, control/TX queues, watchdog and statistics, so a
// hung or unplugged adapter never delays the others.
//
// Received transfers of all adapters are copied straight into the staging buffer of one shared wasm
// module (can_core.js/.wasm, built from can_core.cpp) which parses the frames (tagged with the device
// number), keeps the "Table by ID" aggregates and the trace queue, and publishes compact snapshots of
// what changed. The page only applies snapshots; it never sees individual frames of a busy bus.
//
// Two protocols are supported (chosen per adapter from its USB ids, or forced by `protocol` of `open`):
//   0  gs_usb (candleLight / CANable): vendor control requests configure the adapter, one frame per bulk transfer
//   1  PEAK PCAN-USB FD (uCAN): commands are bulk OUT transfers to a command endpoint, frames are records on a
//      shared bulk IN endpoint (a record can span two transfers) and on one bulk OUT endpoint per CAN channel.
//      The open handshake reads the firmware info (vendor request, recipient "other") for the endpoint numbers.
//
// Main -> worker messages ("dev" selects the adapter)
//   init     {timeOrigin}                          performance.timeOrigin of the page (clock alignment)
//   open     {dev, epoch, vid, pid, serial, protocol?}  open the (already permitted) device, start RX slots
//   close    {dev}
//   ctrl_out {dev, request, value, index, data, reqType?, recipient?}   control OUT, executed strictly in order per device
//   ctrl_in  {dev, request, value, index, length, reqType?, recipient?} control IN, executed strictly in order per device
//   cmd_out  {dev, id, ep, data}                   bulk OUT command transfer (ep 0 = command endpoint), executed in
//                                                  the same order as the control transfers, acknowledged as ctrl_ack {request: id}
//   tx       {dev, ep, data}                       bulk OUT frame (bounded queue, limited in-flight); ep 0 = detected
//   rx       {dev, enable, ep, size}               gate processing of received frames (+ endpoint config)
//   core     {op, arg}                             op 1 = clear ID table, 2 = clear trace, 3 = trace on/off (all devices)
//   ret      {buf}                                 snapshot buffer handed back = one flow-control credit
//
// Worker -> main messages (`dev` + `epoch` = connection the message belongs to; snapshots are global and
// valid in any session because the table and trace live across connections)
//   opened   {dev, epoch, eps, rxEp, txEp, protocol, info}   info = "key=value;..." (PCAN: fw, bl, hw, mcu, ser, ch)
//   closed   {dev, epoch, reason}
//   snapshot {buf, used}                           SnapshotHeader + records, see can_wire.h
//   ctrl_in  {dev, epoch, request, data}
//   ctrl_ack {dev, epoch, request, ok}             completion of a control OUT
//   stats    {dev, epoch, rxPerSec, txDropped, recoveries, rxErrors, txErrors}

const MAX_DEVICES = 8;              // must match kMaxDevices in can_protocol.h
const IFACE = 0;
const DEFAULT_EP_IN = 1;            // endpoint numbers without direction bit, used if no descriptor is available
const DEFAULT_EP_OUT = 2;
const DEFAULT_RX_SIZE = 512;
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

// Protocols (CanProtocolType in can_protocol_common.h)
const PROTO_GS = 0;
const PROTO_PCAN = 1;

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
let openLock = Promise.resolve();   // device matching in getDevices() must not run concurrently

// CAN core (shared wasm module) and snapshot flow control
let core = null;
let stagePtr = 0, stageCap = 0, stageOff = 0, outPtr = 0, snapCap = 0;
let pubTimer = null;
let wdTimer = null;
let credits = BATCH_CREDITS;
let pool = [];
const stageHdr = new DataView(new ArrayBuffer(STAGE_REC_HEADER));
const stageHdrBytes = new Uint8Array(stageHdr.buffer);

// Loaded once per worker. MODULARIZE'd Emscripten output defines the createCanCore factory. In a
// multi-file deployment it is fetched with importScripts(); the single-file bundle made by
// pack_worker.py already contains it in front of this script. If loading fails, open requests are
// answered with 'closed'.
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

function withTimeout(promise, ms, what) {
    let timer;
    const timeout = new Promise((_, reject) => {
        timer = setTimeout(() => reject(new Error(what)), ms);
    });
    return Promise.race([promise, timeout]).finally(() => clearTimeout(timer));
}

function errText(err) {
    return err && err.message ? err.message : String(err);
}

function isGone(err) {
    return !!err && (err.name === 'NotFoundError' || err.name === 'InvalidStateError');
}

function sameDevice(a, b) {
    return a === b || (a.vendorId === b.vendorId && a.productId === b.productId &&
                       a.serialNumber === b.serialNumber);
}

// Same table as CanDetectProtocol() in can_protocol_common.h
function detectProtocol(vid, pid) {
    return (vid === PCAN_VID && pid >= 0x0011 && pid <= 0x0014) ? PROTO_PCAN : PROTO_GS;
}

// Text list of the bulk/interrupt endpoints of the data interface (for the information pane)
function describeEndpoints(usb) {
    try {
        const iface = usb.configuration && usb.configuration.interfaces.find(i => i.interfaceNumber === IFACE);
        const alt = iface && iface.alternate;
        if (!alt) return '';
        return alt.endpoints.map(e => e.direction.toUpperCase() + ' ' + e.endpointNumber + ' ' + e.type + ' ' + e.packetSize + 'B').join(', ');
    } catch (e) { return ''; }
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

// Bulk endpoints of the data interface, from the descriptor if the browser exposes it (gs_usb)
function inspectEndpoints(usb) {
    const info = { rxEp: DEFAULT_EP_IN, txEp: DEFAULT_EP_OUT, rxSize: DEFAULT_RX_SIZE, text: '' };
    try {
        const iface = usb.configuration && usb.configuration.interfaces.find(i => i.interfaceNumber === IFACE);
        const alt = iface && iface.alternate;
        if (!alt) return info;
        const parts = [];
        let haveIn = false, haveOut = false;
        for (const e of alt.endpoints) {
            parts.push(e.direction.toUpperCase() + ' ' + e.endpointNumber + ' ' + e.type + ' ' + e.packetSize + 'B');
            if (e.type !== 'bulk') continue;
            if (e.direction === 'in' && !haveIn) {
                haveIn = true;
                info.rxEp = e.endpointNumber;
                info.rxSize = Math.max(DEFAULT_RX_SIZE, e.packetSize | 0);
            } else if (e.direction === 'out' && !haveOut) {
                haveOut = true;
                info.txEp = e.endpointNumber;
            }
        }
        info.text = parts.join(', ');
    } catch (e) { /* keep defaults */ }
    return info;
}

// ---------------------------------------------------------------------------------------------
// One adapter
// ---------------------------------------------------------------------------------------------

class Device {
    constructor(dev) {
        this.dev = dev;
        this.usb = null;                // USBDevice while open
        this.epoch = 0;
        this.chain = Promise.resolve(); // serializes open/close/control commands of this adapter

        // RX state. rxGen is the generation token: every slot chain carries the generation it was started
        // with and silently dies when it no longer matches (teardown/recover). A pending transferIn cannot
        // be cancelled in WebUSB, so stale completions are simply ignored.
        this.rxGen = 0;
        this.rxEnabled = false;
        this.rxEp = DEFAULT_EP_IN;
        this.txEp = DEFAULT_EP_OUT;
        this.cmdEp = 0;                 // command endpoint (PCAN); gs_usb configures through control requests
        this.rxSize = DEFAULT_RX_SIZE;
        this.protocol = PROTO_GS;
        this.slotErr = new Array(RX_SLOTS).fill(0);
        this.recovering = false;

        // TX state
        this.txQueue = [];
        this.txHead = 0;
        this.txInflight = 0;

        // Watchdog
        this.lastRxDone = 0;
        this.lastPingOk = 0;
        this.pingBusy = false;
        this.pingFails = 0;

        this.st = { rx: 0, rxPrev: 0, rxErrors: 0, txDropped: 0, txErrors: 0, recoveries: 0 };
    }

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

    // -----------------------------------------------------------------------------------------
    // Open / close / loss
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

        // The matching of permitted devices must not interleave between adapters (identical adapters
        // without serial number are told apart only by "not yet opened / not taken by another context")
        const run = openLock.then(() => this.openUsb(m));
        openLock = run.catch(() => {});
        return run;
    }

    async openUsb(m) {
        let d = null;
        try {
            // requestDevice() is window-only; the page already obtained the permission for this origin.
            const list = await navigator.usb.getDevices();
            const taken = new Set(devices.filter(x => x && x.usb).map(x => x.usb));
            const cand = list.filter(x => x.vendorId === m.vid && x.productId === m.pid &&
                                          (m.serial == null || x.serialNumber === m.serial) && !taken.has(x));
            d = cand.find(x => !x.opened) || cand[0];
            if (!d) throw new Error('device is not among the permitted devices');
            await d.open();
            if (d.configuration === null) await d.selectConfiguration(1);
            await d.claimInterface(IFACE);
        } catch (err) {
            console.error('[usb_worker] dev ' + this.dev + ' open failed:', err);
            if (d && d.opened) d.close().catch(() => {});
            this.post('closed', { reason: 'open failed: ' + errText(err) });
            return;
        }

        // Protocol: forced by the page or chosen from the USB ids; the endpoint layout depends on it
        const protocol = (m.protocol === PROTO_GS || m.protocol === PROTO_PCAN) ? m.protocol : detectProtocol(d.vendorId, d.productId);
        let layout, info = '', text;
        if (protocol === PROTO_PCAN) {
            const hs = await pcanHandshake(d);
            layout = { rxEp: hs.layout.rxEp, txEp: hs.layout.txEp, cmdEp: hs.layout.cmdOut, rxSize: PCAN_RX_SIZE };
            info = hs.info;
            text = describeEndpoints(d) || ('CMD OUT ' + hs.layout.cmdOut + ', CMD IN ' + hs.layout.cmdIn + ', MSG IN ' + hs.layout.rxEp + ', MSG OUT ' + hs.layout.txEp);
        } else {
            const ep = inspectEndpoints(d);
            layout = { rxEp: ep.rxEp, txEp: ep.txEp, cmdEp: 0, rxSize: ep.rxSize };
            text = ep.text;
        }

        this.protocol = protocol;
        this.usb = d;
        this.rxEp = layout.rxEp;
        this.txEp = layout.txEp;
        this.cmdEp = layout.cmdEp;
        this.rxSize = layout.rxSize;
        this.txQueue = [];
        this.txHead = 0;
        core._core_set_protocol(this.dev, protocol);     // also starts a fresh receive stream
        this.startRx();
        this.lastPingOk = performance.now();
        this.pingFails = 0;
        this.pingBusy = false;
        this.st.rxPrev = this.st.rx;
        updateTimers();
        this.post('opened', { eps: text, rxEp: layout.rxEp, txEp: layout.txEp, protocol: protocol, info: info });
    }

    async close(reason, silent) {
        const d = this.usb;
        if (!d) {
            if (!silent) this.post('closed', { reason: reason || '' });
            return;
        }
        this.teardown();
        try { await d.close(); } catch (e) { /* already gone */ }
        if (!silent) this.post('closed', { reason: reason || '' });
    }

    lost(reason) {
        const d = this.usb;
        if (!d) return;
        console.warn('[usb_worker] dev ' + this.dev + ' lost:', reason);
        this.teardown();
        d.close().catch(() => {});
        this.post('closed', { reason: reason });
    }

    // Invalidates every chain of the current session. `usb` is cleared first so that the rejections
    // produced by close()/releaseInterface() find a mismatching generation and are ignored.
    teardown() {
        publishTick();                  // hand over the last aggregates (of all adapters)
        this.usb = null;
        this.rxGen++;
        this.rxEnabled = false;
        this.txQueue = [];
        this.txHead = 0;
        updateTimers();
    }

    // -----------------------------------------------------------------------------------------
    // Control transfers (strictly ordered per adapter, each with its own timeout)
    // -----------------------------------------------------------------------------------------

    async control(m) {
        const d = this.usb;
        const isOut = m.type === 'ctrl_out';
        if (!d) {
            if (isOut) this.post('ctrl_ack', { request: m.request, ok: false });
            return;
        }
        const setup = {
            requestType: m.reqType || 'vendor', recipient: m.recipient || 'interface',
            request: m.request, value: m.value, index: m.index
        };
        try {
            if (isOut) {
                const r = await withTimeout(d.controlTransferOut(setup, m.data), CTRL_TIMEOUT_MS,
                                            'control OUT timeout (request ' + m.request + ')');
                this.post('ctrl_ack', { request: m.request, ok: r.status === 'ok' });
            } else {
                const r = await withTimeout(d.controlTransferIn(setup, m.length), CTRL_TIMEOUT_MS,
                                            'control IN timeout (request ' + m.request + ')');
                if (r.status === 'ok' && r.data && r.data.byteLength > 0) {
                    const ab = r.data.buffer.slice(r.data.byteOffset, r.data.byteOffset + r.data.byteLength);
                    this.post('ctrl_in', { request: m.request, data: ab }, [ab]);
                }
            }
        } catch (err) {
            console.error('[usb_worker] dev ' + this.dev + ' control transfer failed:', err);
            if (isOut) this.post('ctrl_ack', { request: m.request, ok: false });
            if (this.usb === d && isGone(err)) this.lost(errText(err));
        }
    }

    // Bulk OUT command transfer (PCAN): same queue and timeout as the control transfers, acknowledged
    // with the id the page gave it (reported as ctrl_ack {request: id})
    async cmdOut(m) {
        const d = this.usb;
        if (!d) {
            this.post('ctrl_ack', { request: m.id, ok: false });
            return;
        }
        const ep = m.ep ? (m.ep & 0x7F) : this.cmdEp;
        try {
            const r = await withTimeout(d.transferOut(ep, m.data), CTRL_TIMEOUT_MS, 'command OUT timeout (id ' + m.id + ')');
            this.post('ctrl_ack', { request: m.id, ok: r.status === 'ok' });
        } catch (err) {
            console.error('[usb_worker] dev ' + this.dev + ' command transfer failed:', err);
            this.post('ctrl_ack', { request: m.id, ok: false });
            if (this.usb === d && isGone(err)) this.lost(errText(err));
        }
    }

    // -----------------------------------------------------------------------------------------
    // RX: N concurrent transferIn slots, each re-armed from its own completion handler
    // -----------------------------------------------------------------------------------------

    startRx() {
        const gen = ++this.rxGen;
        this.slotErr.fill(0);
        this.lastRxDone = performance.now();
        for (let i = 0; i < RX_SLOTS; i++) this.submitRx(i, gen);
    }

    submitRx(slot, gen) {
        const d = this.usb;
        if (gen !== this.rxGen || !d) return;
        let p;
        try {
            p = d.transferIn(this.rxEp, this.rxSize);
        } catch (err) {
            this.onRxError(slot, gen, err);
            return;
        }
        p.then(r => this.onRxDone(slot, gen, r), err => this.onRxError(slot, gen, err));
    }

    onRxDone(slot, gen, r) {
        if (gen !== this.rxGen) return;     // completion of a cancelled/replaced session: ignore
        this.slotErr[slot] = 0;
        this.lastRxDone = performance.now();

        if (r.status === 'stall') {
            this.st.rxErrors++;
            this.usb.clearHalt('in', this.rxEp).then(() => this.submitRx(slot, gen), err => this.onRxError(slot, gen, err));
            return;
        }

        // Re-arm the slot first, within the same microtask checkpoint as the completion, so no other
        // task (onmessage, timers) can run between completion and resubmission.
        this.submitRx(slot, gen);

        if (r.status === 'ok') {
            if (r.data && r.data.byteLength > 0) this.onPacket(r.data);
        } else {
            this.st.rxErrors++;             // babble: partial data, drop it
        }
    }

    onRxError(slot, gen, err) {
        if (gen !== this.rxGen) return;
        if (isGone(err)) { this.lost(errText(err)); return; }
        this.st.rxErrors++;
        const n = ++this.slotErr[slot];
        if (n >= RX_ERRORS_BEFORE_RECOVER) {
            this.slotErr[slot] = 0;
            this.recover('RX errors: ' + errText(err));
            return;
        }
        setTimeout(() => this.submitRx(slot, gen), Math.min(20 * n * n, 500));
    }

    // The only way to abort pending transfers is to release the interface (or close the device).
    // A new generation makes the aborted chains harmless; then the slots are rebuilt.
    async recover(reason) {
        const d = this.usb;
        if (this.recovering || !d) return;
        this.recovering = true;
        this.st.recoveries++;
        console.warn('[usb_worker] dev ' + this.dev + ' recovering:', reason);
        const gen = ++this.rxGen;
        try {
            await d.releaseInterface(IFACE);
            await d.claimInterface(IFACE);
            if (this.usb !== d) return;
            this.slotErr.fill(0);
            this.pingFails = 0;
            this.lastRxDone = performance.now();
            core._core_reset_stream(this.dev);      // the aborted transfers may have cut a record in two
            for (let i = 0; i < RX_SLOTS; i++) this.submitRx(i, gen);
        } catch (err) {
            if (this.usb === d) this.lost('recovery failed: ' + errText(err));
        } finally {
            this.recovering = false;
        }
    }

    setRx(m) {
        const enable = !!m.enable;
        if (!enable) {
            processStage();                 // everything received before the stop still counts
            this.rxEnabled = false;
            return;
        }
        if (!this.rxEnabled && core) {
            core._core_reset_clock(this.dev);       // new channel start: adapter clock may restart
            core._core_reset_stream(this.dev);      // and the record stream starts at a record boundary
        }
        this.rxEnabled = true;
        if (!this.usb) return;
        const ep = m.ep ? (m.ep & 0x7F) : this.rxEp;
        const size = Math.max(64, Math.min(m.size | 0 || this.rxSize, 16384));
        if (ep !== this.rxEp || size !== this.rxSize) {
            this.rxEp = ep;
            this.rxSize = size;
            this.recover('RX endpoint/size changed');
        }
    }

    // Copy one completed transfer into the core's staging buffer as [u16 len][u8 dev][u8 ep][f64 time][bytes]
    onPacket(dv) {
        this.st.rx++;
        if (!this.rxEnabled || !core) return;
        const len = dv.byteLength;
        if (len > 0xFFFF) return;
        if (stageOff + STAGE_REC_HEADER + len > stageCap) processStage();

        stageHdr.setUint16(0, len, true);
        stageHdr.setUint8(2, this.dev);
        stageHdr.setUint8(3, 0x80 | this.rxEp);
        stageHdr.setFloat64(4, now(), true);

        const heap = core.HEAPU8;           // re-read on every use: the heap may be replaced on growth
        heap.set(stageHdrBytes, stagePtr + stageOff);
        heap.set(new Uint8Array(dv.buffer, dv.byteOffset, len), stagePtr + stageOff + STAGE_REC_HEADER);
        stageOff += STAGE_REC_HEADER + len;
    }

    // -----------------------------------------------------------------------------------------
    // TX: bounded queue, limited number of transfers in flight
    // -----------------------------------------------------------------------------------------

    enqueueTx(data, ep) {
        if (!this.usb || this.txQueue.length - this.txHead >= TX_QUEUE_MAX) { this.st.txDropped++; return; }
        this.txQueue.push({ data: data, ep: ep ? (ep & 0x7F) : this.txEp });
        this.pumpTx();
    }

    pumpTx() {
        if (this.txHead === this.txQueue.length) { this.txQueue.length = 0; this.txHead = 0; }
        const d = this.usb;
        while (d && this.txInflight < TX_INFLIGHT && this.txHead < this.txQueue.length) {
            const item = this.txQueue[this.txHead++];
            this.txInflight++;
            let p;
            try { p = d.transferOut(item.ep, item.data); } catch (e) { p = Promise.reject(e); }
            p.then(r => {
                if (r.status === 'stall') {
                    this.st.txErrors++;
                    return d.clearHalt('out', item.ep);
                }
                if (r.status !== 'ok') this.st.txErrors++;
            }).catch(err => {
                this.st.txErrors++;
                if (this.usb === d && isGone(err)) this.lost(errText(err));
            }).finally(() => {
                this.txInflight--;
                this.pumpTx();
            });
        }
    }

    // -----------------------------------------------------------------------------------------
    // Watchdog: stats + liveness probe (a silent bus is not a failure, a dead control pipe is)
    // -----------------------------------------------------------------------------------------

    tick() {
        if (!this.usb) return;
        this.post('stats', {
            rxPerSec: this.st.rx - this.st.rxPrev, txDropped: this.st.txDropped,
            recoveries: this.st.recoveries, rxErrors: this.st.rxErrors, txErrors: this.st.txErrors
        });
        this.st.rxPrev = this.st.rx;
        const t = performance.now();
        if (!this.recovering && !this.pingBusy && t - Math.max(this.lastRxDone, this.lastPingOk) > IDLE_PING_MS) this.ping();
    }

    async ping() {
        const d = this.usb;
        this.pingBusy = true;
        try {
            // A harmless read the adapter has to answer: gs_usb bit-timing constants, PCAN firmware info
            const probe = this.protocol === PROTO_PCAN
                ? [{ requestType: 'vendor', recipient: 'other', request: PCAN_VREQ_INFO, value: PCAN_VREQ_INFO_FIRMWARE, index: 0 }, 36]
                : [{ requestType: 'vendor', recipient: 'interface', request: GS_USB_BREQ_BT_CONST, value: 0, index: IFACE }, 40];
            const r = await withTimeout(d.controlTransferIn(probe[0], probe[1]), PING_TIMEOUT_MS, 'liveness probe timeout');
            if (r.status !== 'ok') throw new Error('liveness probe status ' + r.status);
            this.pingFails = 0;
            this.lastPingOk = performance.now();
        } catch (err) {
            if (this.usb !== d) return;
            if (isGone(err)) { this.lost(errText(err)); return; }
            if (++this.pingFails >= PING_FAILS_MAX) {
                this.pingFails = 0;
                await this.recover('watchdog: ' + errText(err));
            }
        } finally {
            this.pingBusy = false;
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Shared timers: snapshot publishing and watchdog run while at least one adapter is open
// ---------------------------------------------------------------------------------------------

function updateTimers() {
    const open = devices.some(x => x && x.usb);
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

function processStage() {
    if (stageOff > 0 && core) {
        core._core_process(stageOff);
        stageOff = 0;
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
    }

    const n = m.dev;
    if (!Number.isInteger(n) || n < 0 || n >= MAX_DEVICES) return;
    let d = devices[n];

    if (m.type === 'open') {
        if (!d) d = devices[n] = new Device(n);
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
