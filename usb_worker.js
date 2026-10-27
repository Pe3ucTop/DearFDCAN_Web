'use strict';
// usb_worker.js - WebUSB transport running in a dedicated Worker.
//
// The page (Emscripten/wasm + ImGui) talks to this worker only through postMessage; there is no
// SharedArrayBuffer/Atomics. Ownership of every ArrayBuffer is handed over with the transfer list,
// so two contexts never touch the same bytes.
//
// Main -> worker messages
//   init     {timeOrigin}                     performance.timeOrigin of the page (clock alignment)
//   open     {epoch, vid, pid, serial}        open the (already permitted) device, start RX slots
//   close    {}
//   ctrl_out {request, value, index, data}    vendor control OUT, executed strictly in order
//   ctrl_in  {request, value, index, length}  vendor control IN, executed strictly in order
//   tx       {ep, data}                       bulk OUT frame (bounded queue, limited in-flight)
//   rx       {enable, ep, size}               gate delivery of received frames (+ endpoint config)
//   ret      {buf}                            batch buffer handed back = one flow-control credit
//
// Worker -> main messages (all carry `epoch` of the session they belong to)
//   opened   {vid, pid, mfg, prod}
//   closed   {reason}
//   batch    {buf, used}                      see "Batch layout" below
//   ctrl_in  {request, data}
//   ctrl_ack {request, ok}                    completion of a control OUT
//   stats    {rxPerSec, rxDropped, txDropped, recoveries, rxErrors, txErrors}
//
// Batch layout (little endian), BATCH_CAP bytes, `used` valid:
//   u32 seq | u32 count | count x { u16 len | u16 ep_address | f64 host_time_ms | len bytes }
// host_time_ms is in the page's performance.now() timeline, taken when the transfer completed.

const IFACE = 0;
const DEFAULT_EP_IN = 1;            // endpoint number without direction bit
const DEFAULT_RX_SIZE = 512;
const RX_SLOTS = 6;                 // concurrent transferIn requests kept pending on the IN endpoint
const TX_INFLIGHT = 4;              // concurrent transferOut; set to 1 for strictly serialized TX
const TX_QUEUE_MAX = 4096;
const BATCH_CAP = 64 * 1024;
const BATCH_CREDITS = 4;            // batches that may be unacknowledged by the page
const BATCH_HEADER = 8;
const REC_HEADER = 12;
const FLUSH_MS = 2;                 // max extra latency added by batching at low rates
const FLUSH_PACKETS = 64;           // flush immediately at this many pending packets
const PEND_MAX_BYTES = 2 * 1024 * 1024; // backlog cap while the page is not acknowledging batches
const CTRL_TIMEOUT_MS = 2000;
const WATCHDOG_MS = 1000;
const IDLE_PING_MS = 2000;          // RX silence after which the device is probed with a control IN
const PING_TIMEOUT_MS = 1500;
const PING_FAILS_MAX = 2;
const RX_ERRORS_BEFORE_RECOVER = 5;
const GS_USB_BREQ_BT_CONST = 4;     // harmless read used as liveness probe

let dev = null;
let epoch = 0;
let mainOrigin = 0;
let cmdChain = Promise.resolve();   // serializes open/close/control commands

// RX state. rxGen is the generation token: every slot chain carries the generation it was started
// with and silently dies when it no longer matches (teardown/recover). A pending transferIn cannot
// be cancelled in WebUSB, so stale completions are simply ignored.
let rxGen = 0;
let rxEnabled = false;
let rxEp = DEFAULT_EP_IN;
let rxSize = DEFAULT_RX_SIZE;
const slotErr = new Array(RX_SLOTS).fill(0);
let recovering = false;

// Batching / flow control
let pend = [];
let pendBytes = 0;
let flushTimer = null;
let credits = BATCH_CREDITS;
let pool = [];
let batchSeq = 0;

// TX state
let txQueue = [];
let txHead = 0;
let txInflight = 0;

// Watchdog
let wdTimer = null;
let lastRxDone = 0;
let lastPingOk = 0;
let pingBusy = false;
let pingFails = 0;

const st = { rx: 0, rxPrev: 0, rxDropped: 0, rxErrors: 0, txDropped: 0, txErrors: 0, recoveries: 0 };

function now() {
    return performance.now() + performance.timeOrigin - mainOrigin;
}

function post(type, fields, transfer) {
    const msg = fields || {};
    msg.type = type;
    msg.epoch = epoch;
    self.postMessage(msg, transfer || []);
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

// ---------------------------------------------------------------------------------------------
// Command dispatch
// ---------------------------------------------------------------------------------------------

function enqueue(fn) {
    cmdChain = cmdChain.then(fn).catch(err => console.error('[usb_worker] command failed:', err));
}

self.onmessage = (ev) => {
    const m = ev.data;
    switch (m.type) {
        case 'init':     mainOrigin = m.timeOrigin; break;
        case 'open':     enqueue(() => doOpen(m)); break;
        case 'close':    enqueue(() => doClose('closed by host', false)); break;
        case 'ctrl_out':
        case 'ctrl_in':  enqueue(() => doControl(m)); break;
        case 'tx':       enqueueTx(m.data, m.ep); break;
        case 'rx':       setRx(m); break;
        case 'ret':      returnBuffer(m.buf); break;
    }
};

if (typeof navigator !== 'undefined' && navigator.usb) {
    navigator.usb.addEventListener('disconnect', (ev) => {
        if (dev && sameDevice(ev.device, dev)) deviceLost('device disconnected');
    });
}

// ---------------------------------------------------------------------------------------------
// Open / close / loss
// ---------------------------------------------------------------------------------------------

async function doOpen(m) {
    if (dev) await doClose(null, true);
    epoch = m.epoch;

    let d = null;
    try {
        // requestDevice() is window-only; the page already obtained the permission for this origin.
        const list = await navigator.usb.getDevices();
        const cand = list.filter(x => x.vendorId === m.vid && x.productId === m.pid &&
                                      (m.serial == null || x.serialNumber === m.serial));
        d = cand.find(x => !x.opened) || cand[0];
        if (!d) throw new Error('device is not among the permitted devices');
        await d.open();
        if (d.configuration === null) await d.selectConfiguration(1);
        await d.claimInterface(IFACE);
    } catch (err) {
        console.error('[usb_worker] open failed:', err);
        if (d && d.opened) d.close().catch(() => {});
        post('closed', { reason: 'open failed: ' + errText(err) });
        return;
    }

    dev = d;
    rxEp = DEFAULT_EP_IN;
    rxSize = DEFAULT_RX_SIZE;
    startRx();
    startWatchdog();
    post('opened', {
        vid: d.vendorId, pid: d.productId,
        mfg: d.manufacturerName || '', prod: d.productName || ''
    });
}

async function doClose(reason, silent) {
    const d = dev;
    if (!d) {
        if (!silent) post('closed', { reason: reason || '' });
        return;
    }
    teardown();
    try { await d.close(); } catch (e) { /* already gone */ }
    if (!silent) post('closed', { reason: reason || '' });
}

function deviceLost(reason) {
    const d = dev;
    if (!d) return;
    console.warn('[usb_worker] device lost:', reason);
    teardown();
    d.close().catch(() => {});
    post('closed', { reason });
}

// Invalidates every chain of the current session. `dev` is cleared first so that the rejections
// produced by close()/releaseInterface() find a mismatching generation and are ignored.
function teardown() {
    dev = null;
    rxGen++;
    rxEnabled = false;
    stopWatchdog();
    if (flushTimer !== null) { clearTimeout(flushTimer); flushTimer = null; }
    pend = [];
    pendBytes = 0;
    txQueue = [];
    txHead = 0;
}

// ---------------------------------------------------------------------------------------------
// Control transfers (strictly ordered, each with its own timeout)
// ---------------------------------------------------------------------------------------------

async function doControl(m) {
    const d = dev;
    const isOut = m.type === 'ctrl_out';
    if (!d) {
        if (isOut) post('ctrl_ack', { request: m.request, ok: false });
        return;
    }
    const setup = {
        requestType: 'vendor', recipient: 'interface',
        request: m.request, value: m.value, index: m.index
    };
    try {
        if (isOut) {
            const r = await withTimeout(d.controlTransferOut(setup, m.data), CTRL_TIMEOUT_MS,
                                        'control OUT timeout (request ' + m.request + ')');
            post('ctrl_ack', { request: m.request, ok: r.status === 'ok' });
        } else {
            const r = await withTimeout(d.controlTransferIn(setup, m.length), CTRL_TIMEOUT_MS,
                                        'control IN timeout (request ' + m.request + ')');
            if (r.status === 'ok' && r.data && r.data.byteLength > 0) {
                const ab = r.data.buffer.slice(r.data.byteOffset, r.data.byteOffset + r.data.byteLength);
                post('ctrl_in', { request: m.request, data: ab }, [ab]);
            }
        }
    } catch (err) {
        console.error('[usb_worker] control transfer failed:', err);
        if (isOut) post('ctrl_ack', { request: m.request, ok: false });
        if (dev === d && isGone(err)) deviceLost(errText(err));
    }
}

// ---------------------------------------------------------------------------------------------
// RX: N concurrent transferIn slots, each re-armed from its own completion handler
// ---------------------------------------------------------------------------------------------

function startRx() {
    const gen = ++rxGen;
    slotErr.fill(0);
    lastRxDone = performance.now();
    for (let i = 0; i < RX_SLOTS; i++) submitRx(i, gen);
}

function submitRx(slot, gen) {
    const d = dev;
    if (gen !== rxGen || !d) return;
    let p;
    try {
        p = d.transferIn(rxEp, rxSize);
    } catch (err) {
        onRxError(slot, gen, err);
        return;
    }
    p.then(r => onRxDone(slot, gen, r), err => onRxError(slot, gen, err));
}

function onRxDone(slot, gen, r) {
    if (gen !== rxGen) return;          // completion of a cancelled/replaced session: ignore
    slotErr[slot] = 0;
    lastRxDone = performance.now();

    if (r.status === 'stall') {
        st.rxErrors++;
        const d = dev;
        d.clearHalt('in', rxEp).then(() => submitRx(slot, gen), err => onRxError(slot, gen, err));
        return;
    }

    // Re-arm the slot first, within the same microtask checkpoint as the completion, so no other
    // task (onmessage, timers) can run between completion and resubmission.
    submitRx(slot, gen);

    if (r.status === 'ok') {
        if (r.data && r.data.byteLength > 0) onPacket(r.data);
    } else {
        st.rxErrors++;                  // babble: partial data, drop it
    }
}

function onRxError(slot, gen, err) {
    if (gen !== rxGen) return;
    if (isGone(err)) { deviceLost(errText(err)); return; }
    st.rxErrors++;
    const n = ++slotErr[slot];
    if (n >= RX_ERRORS_BEFORE_RECOVER) {
        slotErr[slot] = 0;
        recover('RX errors: ' + errText(err));
        return;
    }
    setTimeout(() => submitRx(slot, gen), Math.min(20 * n * n, 500));
}

// The only way to abort pending transfers is to release the interface (or close the device).
// A new generation makes the aborted chains harmless; then the slots are rebuilt.
async function recover(reason) {
    const d = dev;
    if (recovering || !d) return;
    recovering = true;
    st.recoveries++;
    console.warn('[usb_worker] recovering:', reason);
    const gen = ++rxGen;
    try {
        await d.releaseInterface(IFACE);
        await d.claimInterface(IFACE);
        if (dev !== d) return;
        slotErr.fill(0);
        pingFails = 0;
        lastRxDone = performance.now();
        for (let i = 0; i < RX_SLOTS; i++) submitRx(i, gen);
    } catch (err) {
        if (dev === d) deviceLost('recovery failed: ' + errText(err));
    } finally {
        recovering = false;
    }
}

function setRx(m) {
    const enable = !!m.enable;
    if (!enable) {
        rxEnabled = false;
        pend = [];
        pendBytes = 0;
        return;
    }
    rxEnabled = true;
    if (!dev) return;
    const ep = (m.ep == null ? rxEp : m.ep) & 0x7F;
    const size = Math.max(64, Math.min(m.size | 0 || rxSize, 16384));
    if (ep !== rxEp || size !== rxSize) {
        rxEp = ep;
        rxSize = size;
        recover('RX endpoint/size changed');
    }
}

// ---------------------------------------------------------------------------------------------
// Batching with credit-based flow control
// ---------------------------------------------------------------------------------------------

function onPacket(dv) {
    st.rx++;
    if (!rxEnabled) return;
    const need = REC_HEADER + dv.byteLength;
    if (pendBytes + need > PEND_MAX_BYTES) { st.rxDropped++; return; }
    pend.push({ dv: dv, t: now() });
    pendBytes += need;
    if (pend.length >= FLUSH_PACKETS) flush();
    else if (flushTimer === null) flushTimer = setTimeout(flush, FLUSH_MS);
}

function flush() {
    if (flushTimer !== null) { clearTimeout(flushTimer); flushTimer = null; }
    while (pend.length > 0 && credits > 0) {
        const buf = pool.pop() || new ArrayBuffer(BATCH_CAP);
        const bytes = new Uint8Array(buf);
        const view = new DataView(buf);
        let off = BATCH_HEADER;
        let n = 0;
        while (n < pend.length) {
            const p = pend[n];
            const len = p.dv.byteLength;
            if (off + REC_HEADER + len > BATCH_CAP) break;
            view.setUint16(off, len, true);
            view.setUint16(off + 2, 0x80 | rxEp, true);
            view.setFloat64(off + 4, p.t, true);
            bytes.set(new Uint8Array(p.dv.buffer, p.dv.byteOffset, len), off + REC_HEADER);
            off += REC_HEADER + len;
            pendBytes -= REC_HEADER + len;
            n++;
        }
        if (n === 0) {                  // a single record larger than a batch: cannot be sent
            pendBytes -= REC_HEADER + pend[0].dv.byteLength;
            pend.shift();
            st.rxDropped++;
            pool.push(buf);
            continue;
        }
        view.setUint32(0, batchSeq >>> 0, true);
        view.setUint32(4, n, true);
        batchSeq = (batchSeq + 1) >>> 0;
        pend.splice(0, n);
        credits--;
        post('batch', { buf: buf, used: off }, [buf]);
    }
}

function returnBuffer(buf) {
    if (buf && buf.byteLength === BATCH_CAP && pool.length < BATCH_CREDITS) pool.push(buf);
    if (credits < BATCH_CREDITS) credits++;
    if (pend.length > 0) flush();
}

// ---------------------------------------------------------------------------------------------
// TX: bounded queue, limited number of transfers in flight
// ---------------------------------------------------------------------------------------------

function enqueueTx(data, ep) {
    if (!dev || txQueue.length - txHead >= TX_QUEUE_MAX) { st.txDropped++; return; }
    txQueue.push({ data: data, ep: ep & 0x7F });
    pumpTx();
}

function pumpTx() {
    if (txHead === txQueue.length) { txQueue.length = 0; txHead = 0; }
    const d = dev;
    while (d && txInflight < TX_INFLIGHT && txHead < txQueue.length) {
        const item = txQueue[txHead++];
        txInflight++;
        let p;
        try { p = d.transferOut(item.ep, item.data); } catch (e) { p = Promise.reject(e); }
        p.then(r => {
            if (r.status === 'stall') {
                st.txErrors++;
                return d.clearHalt('out', item.ep);
            }
            if (r.status !== 'ok') st.txErrors++;
        }).catch(err => {
            st.txErrors++;
            if (dev === d && isGone(err)) deviceLost(errText(err));
        }).finally(() => {
            txInflight--;
            pumpTx();
        });
    }
}

// ---------------------------------------------------------------------------------------------
// Watchdog: stats + liveness probe (a silent bus is not a failure, a dead control pipe is)
// ---------------------------------------------------------------------------------------------

function startWatchdog() {
    stopWatchdog();
    pingFails = 0;
    pingBusy = false;
    lastPingOk = performance.now();
    st.rxPrev = st.rx;
    wdTimer = setInterval(watchdogTick, WATCHDOG_MS);
}

function stopWatchdog() {
    if (wdTimer !== null) { clearInterval(wdTimer); wdTimer = null; }
}

function watchdogTick() {
    if (!dev) return;
    post('stats', {
        rxPerSec: st.rx - st.rxPrev, rxDropped: st.rxDropped, txDropped: st.txDropped,
        recoveries: st.recoveries, rxErrors: st.rxErrors, txErrors: st.txErrors
    });
    st.rxPrev = st.rx;
    const t = performance.now();
    if (!recovering && !pingBusy && t - Math.max(lastRxDone, lastPingOk) > IDLE_PING_MS) ping();
}

async function ping() {
    const d = dev;
    pingBusy = true;
    try {
        const r = await withTimeout(d.controlTransferIn({
            requestType: 'vendor', recipient: 'interface',
            request: GS_USB_BREQ_BT_CONST, value: 0, index: IFACE
        }, 40), PING_TIMEOUT_MS, 'liveness probe timeout');
        if (r.status !== 'ok') throw new Error('liveness probe status ' + r.status);
        pingFails = 0;
        lastPingOk = performance.now();
    } catch (err) {
        if (dev !== d) return;
        if (isGone(err)) { deviceLost(errText(err)); return; }
        if (++pingFails >= PING_FAILS_MAX) {
            pingFails = 0;
            await recover('watchdog: ' + errText(err));
        }
    } finally {
        pingBusy = false;
    }
}
