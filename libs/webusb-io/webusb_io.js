// webusb_io.js - WebUSB transport for CAN (and other bulk) adapters: one UsbPort per adapter. No dependency on a CAN protocol, on
// WebAssembly or on postMessage; it runs in a window or in a worker (the permission for a device comes from navigator.usb.requestDevice()
// in a window, a worker finds the permitted device again with navigator.usb.getDevices()).
//
// A UsbPort owns one USBDevice and does what every adapter needs:
//   * open: finds the permitted device by vendor id / product id / serial number that no other port uses, opens it, selects the
//     configuration, claims the interface, lets the owner read the descriptor / do a handshake, then starts receiving
//   * receive: `rxSlots` transferIn requests are kept pending at all times and each is re-armed from its own completion, so no
//     transfer is ever missed. A pending request cannot be cancelled in WebUSB, so every request belongs to a generation (`rxGen`);
//     completions of an older generation are ignored. After repeated errors the interface is released and claimed again (recover)
//   * control and command transfers: executed strictly in order, each with its own timeout
//   * transmit: a bounded queue with a limited number of transfers in flight
//   * watchdog: an idle adapter is probed with a control read; a silent bus is not a failure, a dead control pipe is
//   * loss: an unplugged adapter ends the session at once
//
// The owner subclasses UsbPort and overrides the hooks (names start with `on`/`before`/`after`; they run synchronously):
//   onOpening(handshakeResult)   the device is claimed and the endpoints are set; the receive transfers are submitted right after
//   onRxData(dataView)           one completed receive transfer (status ok, not empty), in completion order
//   onTxQueued(data, ep)         a frame is about to be queued for sending (not called when it is dropped)
//   onRecovered()                the interface was claimed again after an error; the pending transfers were aborted
//   beforeTeardown() / afterTeardown()   around the end of a session (closed by the owner, or lost)
//   onLost(reason)               the session ended because of the adapter (unplugged, failed recovery, ...), after the teardown
//   probeRequest()               a harmless control read the adapter has to answer: [setup, length], or null for no probe
//
// Loaded as a classic script (importScripts or a <script>); the one global it defines is WebUsbIo.
var WebUsbIo = (function () {
'use strict';

const DEFAULTS = {
    iface: 0,
    defaultEpIn: 1,             // endpoint numbers (without the direction bit) used when the descriptor is not available
    defaultEpOut: 2,
    defaultRxSize: 512,
    rxSlots: 6,                 // concurrent transferIn requests per adapter
    txInflight: 4,              // concurrent transferOut per adapter; 1 = strictly serialized
    txQueueMax: 4096,
    ctrlTimeoutMs: 2000,
    idlePingMs: 2000,           // receive silence after which the adapter is probed
    pingTimeoutMs: 1500,
    pingFailsMax: 2,            // failed probes in a row that trigger a recovery
    rxErrorsBeforeRecover: 5,   // errors of one receive slot in a row
    inUse: null,                // () => array of the USBDevice objects other ports use (device matching skips them)
    tag: 'webusb-io'            // prefix of the log lines
};

function withTimeout(promise, ms, what) {
    let timer;
    const timeout = new Promise((_, reject) => { timer = setTimeout(() => reject(new Error(what)), ms); });
    return Promise.race([promise, timeout]).finally(() => clearTimeout(timer));
}

function errText(err) {
    return err && err.message ? err.message : String(err);
}

// The adapter was unplugged or its interface is not usable any more
function isGone(err) {
    return !!err && (err.name === 'NotFoundError' || err.name === 'InvalidStateError');
}

// Two USBDevice objects that are the same physical device (identical adapters without a serial number are not told apart)
function sameDevice(a, b) {
    return a === b || (a.vendorId === b.vendorId && a.productId === b.productId && a.serialNumber === b.serialNumber);
}

function endpointText(e) {
    return e.direction.toUpperCase() + ' ' + e.endpointNumber + ' ' + e.type + ' ' + e.packetSize + 'B';
}

// "IN 1 bulk 64B, OUT 2 bulk 64B" for the information pane; empty if the browser exposes no descriptor
function describeEndpoints(usb, iface) {
    try {
        const i = usb.configuration && usb.configuration.interfaces.find(x => x.interfaceNumber === (iface | 0));
        const alt = i && i.alternate;
        return alt ? alt.endpoints.map(endpointText).join(', ') : '';
    } catch (e) { return ''; }
}

// First bulk IN and OUT endpoint of the interface, from the descriptor if the browser exposes it
function inspectEndpoints(usb, options) {
    const o = Object.assign({}, DEFAULTS, options);
    const info = { rxEp: o.defaultEpIn, txEp: o.defaultEpOut, rxSize: o.defaultRxSize, text: '' };
    try {
        const i = usb.configuration && usb.configuration.interfaces.find(x => x.interfaceNumber === o.iface);
        const alt = i && i.alternate;
        if (!alt) return info;
        const parts = [];
        let haveIn = false, haveOut = false;
        for (const e of alt.endpoints) {
            parts.push(endpointText(e));
            if (e.type !== 'bulk') continue;
            if (e.direction === 'in' && !haveIn) {
                haveIn = true;
                info.rxEp = e.endpointNumber;
                info.rxSize = Math.max(o.defaultRxSize, e.packetSize | 0);
            } else if (e.direction === 'out' && !haveOut) {
                haveOut = true;
                info.txEp = e.endpointNumber;
            }
        }
        info.text = parts.join(', ');
    } catch (e) { /* keep defaults */ }
    return info;
}

// The matching of permitted devices must not interleave between ports: identical adapters without a serial number are told apart only
// by "not yet opened / not used by another port", so a port has to hold its device before the next one looks.
let openLock = Promise.resolve();

class UsbPort {
    // `label` appears in the log lines (e.g. 'dev 2')
    constructor(label, options) {
        this.label = label;
        this.opt = Object.assign({}, DEFAULTS, options);
        this.usb = null;                    // the USBDevice while the port is open
        this.chain = Promise.resolve();     // serializes the commands the owner enqueues (open, close, control transfers)

        // Receive state. rxGen is the generation token (see the top of the file).
        this.rxGen = 0;
        this.rxEp = this.opt.defaultEpIn;
        this.txEp = this.opt.defaultEpOut;
        this.cmdEp = 0;                     // command endpoint of protocols that have one (PEAK); others configure through control requests
        this.rxSize = this.opt.defaultRxSize;
        this.slotErr = new Array(this.opt.rxSlots).fill(0);
        this.recovering = false;

        this.txQueue = [];
        this.txHead = 0;
        this.txInflight = 0;

        this.lastRxDone = 0;
        this.lastPingOk = 0;
        this.pingBusy = false;
        this.pingFails = 0;

        this.st = { rx: 0, rxPrev: 0, rxErrors: 0, txDropped: 0, txErrors: 0, recoveries: 0 };
    }

    // ---- hooks (see the top of the file) ----
    onOpening(handshake) {}
    onRxData(dv) {}
    onTxQueued(data, ep) {}
    onRecovered() {}
    beforeTeardown() {}
    afterTeardown() {}
    onLost(reason) {}
    probeRequest() { return null; }

    get isOpen() { return !!this.usb; }

    warn(...args) { console.warn('[' + this.opt.tag + '] ' + this.label, ...args); }
    error(...args) { console.error('[' + this.opt.tag + '] ' + this.label, ...args); }

    // Runs `fn` after the commands enqueued before it; a failing command is logged and does not stop the chain
    enqueue(fn) {
        this.chain = this.chain.then(fn).catch(err => this.error('command failed:', err));
    }

    // -----------------------------------------------------------------------------------------
    // Open / close / loss
    // -----------------------------------------------------------------------------------------

    // Finds, opens and claims the permitted device {vid, pid, serial?}, calls `handshake(usb)` (async, returns
    // { layout: {rxEp, txEp, cmdEp, rxSize}, ... }) and starts receiving. Resolves with what the handshake returned; rejects if
    // anything failed (the device is closed again).
    openDevice(criteria, handshake) {
        const run = openLock.then(() => this._openDevice(criteria, handshake));
        openLock = run.catch(() => {});
        return run;
    }

    async _openDevice(c, handshake) {
        let d = null;
        try {
            // requestDevice() is window-only; the page already obtained the permission for this origin.
            const list = await navigator.usb.getDevices();
            const taken = new Set(this.opt.inUse ? this.opt.inUse() : []);
            const cand = list.filter(x => x.vendorId === c.vid && x.productId === c.pid &&
                                          (c.serial == null || x.serialNumber === c.serial) && !taken.has(x));
            d = cand.find(x => !x.opened) || cand[0];
            if (!d) throw new Error('device is not among the permitted devices');
            await d.open();
            if (d.configuration === null) await d.selectConfiguration(1);
            await d.claimInterface(this.opt.iface);
            const hs = await handshake(d);
            const layout = hs.layout;

            this.usb = d;
            this.rxEp = layout.rxEp;
            this.txEp = layout.txEp;
            this.cmdEp = layout.cmdEp | 0;
            this.rxSize = layout.rxSize;
            this.txQueue = [];
            this.txHead = 0;
            this.onOpening(hs);
            this.startRx();
            this.lastPingOk = performance.now();
            this.pingFails = 0;
            this.pingBusy = false;
            return hs;
        } catch (err) {
            if (this.usb === d) this.usb = null;
            if (d && d.opened) d.close().catch(() => {});
            throw err;
        }
    }

    // Ends the session at the owner's request. Resolves with false if the port was not open.
    async closeDevice() {
        const d = this.usb;
        if (!d) return false;
        this.teardown();
        try { await d.close(); } catch (e) { /* already gone */ }
        return true;
    }

    // The adapter went away or cannot be used any more: ends the session and tells the owner
    lost(reason) {
        const d = this.usb;
        if (!d) return;
        this.warn('lost:', reason);
        this.teardown();
        d.close().catch(() => {});
        this.onLost(reason);
    }

    // Invalidates every chain of the current session. `usb` is cleared first so that the rejections produced by
    // close()/releaseInterface() find a mismatching generation and are ignored.
    teardown() {
        this.beforeTeardown();
        this.usb = null;
        this.rxGen++;
        this.txQueue = [];
        this.txHead = 0;
        this.afterTeardown();
    }

    // -----------------------------------------------------------------------------------------
    // Control and command transfers (call them from enqueue() to keep the order; each has its own timeout)
    // -----------------------------------------------------------------------------------------

    // setup: {requestType, recipient, request, value, index}. OUT: `payload` is the data; IN: the number of bytes to read.
    // Resolves { ok, data }: data is an ArrayBuffer with the bytes of a successful IN (null for OUT, for no data and for a failure).
    // Never rejects; a lost adapter ends the session.
    async controlTransfer(isOut, setup, payload) {
        const d = this.usb;
        if (!d) return { ok: false, data: null };
        const ms = this.opt.ctrlTimeoutMs;
        try {
            if (isOut) {
                const r = await withTimeout(d.controlTransferOut(setup, payload), ms, 'control OUT timeout (request ' + setup.request + ')');
                return { ok: r.status === 'ok', data: null };
            }
            const r = await withTimeout(d.controlTransferIn(setup, payload), ms, 'control IN timeout (request ' + setup.request + ')');
            let data = null;
            if (r.status === 'ok' && r.data && r.data.byteLength > 0) data = r.data.buffer.slice(r.data.byteOffset, r.data.byteOffset + r.data.byteLength);
            return { ok: r.status === 'ok', data: data };
        } catch (err) {
            this.error('control transfer failed:', err);
            if (this.usb === d && isGone(err)) this.lost(errText(err));
            return { ok: false, data: null };
        }
    }

    // A bulk OUT transfer that is ordered with the control transfers (a command endpoint). `what` names it in messages.
    async commandTransfer(ep, data, what) {
        const d = this.usb;
        if (!d) return { ok: false };
        try {
            const r = await withTimeout(d.transferOut(ep, data), this.opt.ctrlTimeoutMs, 'command OUT timeout (' + what + ')');
            return { ok: r.status === 'ok' };
        } catch (err) {
            this.error('command transfer failed:', err);
            if (this.usb === d && isGone(err)) this.lost(errText(err));
            return { ok: false };
        }
    }

    // -----------------------------------------------------------------------------------------
    // Receive: N concurrent transferIn slots, each re-armed from its own completion handler
    // -----------------------------------------------------------------------------------------

    startRx() {
        const gen = ++this.rxGen;
        this.slotErr.fill(0);
        this.lastRxDone = performance.now();
        for (let i = 0; i < this.opt.rxSlots; i++) this.submitRx(i, gen);
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

        // Re-arm the slot first, within the same microtask checkpoint as the completion, so no other task (a message, a timer) can
        // run between completion and resubmission.
        this.submitRx(slot, gen);

        if (r.status === 'ok') {
            if (r.data && r.data.byteLength > 0) {
                this.st.rx++;
                this.onRxData(r.data);
            }
        } else {
            this.st.rxErrors++;             // babble: partial data, drop it
        }
    }

    onRxError(slot, gen, err) {
        if (gen !== this.rxGen) return;
        if (isGone(err)) { this.lost(errText(err)); return; }
        this.st.rxErrors++;
        const n = ++this.slotErr[slot];
        if (n >= this.opt.rxErrorsBeforeRecover) {
            this.slotErr[slot] = 0;
            this.recover('RX errors: ' + errText(err));
            return;
        }
        setTimeout(() => this.submitRx(slot, gen), Math.min(20 * n * n, 500));
    }

    // The only way to abort pending transfers is to release the interface (or close the device). A new generation makes the
    // aborted chains harmless; then the slots are rebuilt.
    async recover(reason) {
        const d = this.usb;
        if (this.recovering || !d) return;
        this.recovering = true;
        this.st.recoveries++;
        this.warn('recovering:', reason);
        const gen = ++this.rxGen;
        try {
            await d.releaseInterface(this.opt.iface);
            await d.claimInterface(this.opt.iface);
            if (this.usb !== d) return;
            this.slotErr.fill(0);
            this.pingFails = 0;
            this.lastRxDone = performance.now();
            this.onRecovered();
            for (let i = 0; i < this.opt.rxSlots; i++) this.submitRx(i, gen);
        } catch (err) {
            if (this.usb === d) this.lost('recovery failed: ' + errText(err));
        } finally {
            this.recovering = false;
        }
    }

    // Another endpoint / transfer size for the receive transfers (0 / 0 keep the current ones); the pending transfers are replaced
    reconfigureRx(ep, size) {
        const e = ep ? (ep & 0x7F) : this.rxEp;
        const s = Math.max(64, Math.min(size | 0 || this.rxSize, 16384));
        if (e !== this.rxEp || s !== this.rxSize) {
            this.rxEp = e;
            this.rxSize = s;
            this.recover('RX endpoint/size changed');
        }
    }

    // -----------------------------------------------------------------------------------------
    // Transmit: bounded queue, limited number of transfers in flight. Returns false if the frame was dropped.
    // -----------------------------------------------------------------------------------------

    enqueueTx(data, ep) {
        if (!this.usb || this.txQueue.length - this.txHead >= this.opt.txQueueMax) { this.st.txDropped++; return false; }
        this.onTxQueued(data, ep);
        this.txQueue.push({ data: data, ep: ep ? (ep & 0x7F) : this.txEp });
        this.pumpTx();
        return true;
    }

    pumpTx() {
        if (this.txHead === this.txQueue.length) { this.txQueue.length = 0; this.txHead = 0; }
        const d = this.usb;
        while (d && this.txInflight < this.opt.txInflight && this.txHead < this.txQueue.length) {
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
    // Watchdog: call watchdog() about once a second (a silent bus is not a failure, a dead control pipe is)
    // -----------------------------------------------------------------------------------------

    watchdog() {
        if (!this.usb) return;
        const t = performance.now();
        if (!this.recovering && !this.pingBusy && t - Math.max(this.lastRxDone, this.lastPingOk) > this.opt.idlePingMs) this.ping();
    }

    // Tells the watchdog that the control pipe works (the owner just completed a control transfer of its own)
    noteAlive() {
        this.lastPingOk = performance.now();
    }

    async ping() {
        const d = this.usb;
        const probe = this.probeRequest();
        if (!probe) return;
        this.pingBusy = true;
        try {
            const r = await withTimeout(d.controlTransferIn(probe[0], probe[1]), this.opt.pingTimeoutMs, 'liveness probe timeout');
            if (r.status !== 'ok') throw new Error('liveness probe status ' + r.status);
            this.pingFails = 0;
            this.lastPingOk = performance.now();
        } catch (err) {
            if (this.usb !== d) return;
            if (isGone(err)) { this.lost(errText(err)); return; }
            if (++this.pingFails >= this.opt.pingFailsMax) {
                this.pingFails = 0;
                await this.recover('watchdog: ' + errText(err));
            }
        } finally {
            this.pingBusy = false;
        }
    }
}

return { UsbPort, DEFAULTS, withTimeout, errText, isGone, sameDevice, describeEndpoints, inspectEndpoints };
})();
