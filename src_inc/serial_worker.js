'use strict';
// serial_worker.js - WebSerial transport (I/O only), running in its own dedicated Worker.
//
// This worker owns the SerialPort objects and nothing else: it opens and closes ports, reads bytes and writes
// bytes. Parsing, the ID table, the trace and the snapshots stay in the CAN worker (usb_worker.js), which hosts
// the one shared can_core module. The two workers are connected by a MessageChannel: the page creates it and hands
// one end to each worker ('link'), so received bytes go from here straight to the CAN worker without a detour
// over the page. Buffers are moved with the transfer list, never copied.
//
// Page -> this worker (postMessage)
//   link  {port}                         MessagePort to the CAN worker
//   pick  {dev, port | vid, pid, idx}    the port the user chose for slot `dev`. If SerialPort objects can be transferred
//                                        `port` is the object itself; otherwise the port is found again with
//                                        navigator.serial.getPorts() as the idx-th permitted port with these USB ids.
//   forget {dev}                         drop the port of a removed slot
//
// CAN worker -> this worker (over the link)
//   sopen  {dev, epoch, baud}            open the picked port and start reading
//   sclose {dev}
//   swrite {dev, epoch, data, id}        write bytes in order; id != 0 is acknowledged with swack
//
// This worker -> CAN worker (over the link)
//   sopened {dev, epoch, eps, info}      eps = text for the information pane, info = "key=value;..."
//   sclosed {dev, epoch, reason}
//   srx     {dev, epoch, buf, t}         bytes read; t = performance.timeOrigin + performance.now() at completion
//   swack   {dev, epoch, id, ok}         the write with this id finished
//   sstats  {dev, epoch, rx, rxErrors, txErrors, txDropped}   about once a second

const MAX_DEVICES = 8;              // must match kMaxDevices in can_protocol.h
const TX_QUEUE_MAX = 4096;          // queued writes per port (a write is one command or one frame line)
const PORT_BUFFER = 1 << 16;        // browser side receive buffer of the port
const STATS_MS = 1000;
const OPEN_TIMEOUT_MS = 4000;

let link = null;
const picks = new Array(MAX_DEVICES).fill(null);     // { port, vid, pid, idx } chosen by the user
const conns = new Array(MAX_DEVICES).fill(null);
let statsTimer = null;

function errText(err) {
    return err && err.message ? err.message : String(err);
}

function withTimeout(promise, ms, what) {
    let timer;
    const timeout = new Promise((_, reject) => { timer = setTimeout(() => reject(new Error(what)), ms); });
    return Promise.race([promise, timeout]).finally(() => clearTimeout(timer));
}

function hex4(n) {
    return (n | 0).toString(16).toUpperCase().padStart(4, '0');
}

// One serial port in use
class Conn {
    constructor(dev, epoch) {
        this.dev = dev;
        this.epoch = epoch;
        this.port = null;
        this.active = false;            // between a successful open and close/loss
        this.opening = false;
        this.wantClose = false;         // close requested while opening
        this.reader = null;
        this.writer = null;
        this.txQueue = [];
        this.txHead = 0;
        this.txBusy = false;
        this.st = { rx: 0, rxPrev: 0, rxErrors: 0, txErrors: 0, txDropped: 0 };
    }

    post(type, fields, transfer) {
        const msg = fields || {};
        msg.type = type;
        msg.dev = this.dev;
        msg.epoch = this.epoch;
        link.postMessage(msg, transfer || []);
    }

    // The permitted port the user picked for this slot
    async resolvePort() {
        const pick = picks[this.dev];
        if (!pick) throw new Error('no serial port was chosen for this device');
        if (pick.port) return pick.port;
        if (typeof navigator === 'undefined' || !navigator.serial) throw new Error('Web Serial is not available in this worker');
        const all = await navigator.serial.getPorts();
        const same = all.filter(p => {
            const i = p.getInfo ? p.getInfo() : {};
            return (i.usbVendorId | 0) === (pick.vid | 0) && (i.usbProductId | 0) === (pick.pid | 0);
        });
        const port = same[pick.idx] || same[0];
        if (!port) throw new Error('the serial port is not among the permitted ports');
        return port;
    }

    async open(baud) {
        this.opening = true;
        try {
            const port = await this.resolvePort();
            for (const c of conns) if (c && c !== this && c.port === port && c.active) throw new Error('the port is already in use');
            await withTimeout(port.open({ baudRate: baud > 0 ? baud : 115200, bufferSize: PORT_BUFFER }), OPEN_TIMEOUT_MS, 'open timeout');
            this.port = port;
            // CDC-ACM firmware often waits for DTR before it talks
            try { await port.setSignals({ dataTerminalReady: true, requestToSend: true }); } catch (e) { /* not supported */ }
            if (!port.readable || !port.writable) throw new Error('the port has no readable/writable stream');
            this.writer = port.writable.getWriter();
            this.active = true;
            this.opening = false;
            if (this.wantClose) {           // closed while it was still opening
                await this.cleanup();
                this.post('sclosed', { reason: '' });
                return;
            }
            ensureStatsTimer();
            let info = '';
            try {
                const i = port.getInfo();
                if (i.usbVendorId !== undefined) info = 'vid=' + hex4(i.usbVendorId) + ';pid=' + hex4(i.usbProductId);
            } catch (e) { /* no info */ }
            this.post('sopened', { eps: 'serial ' + (baud > 0 ? baud : 115200) + ' baud', info: info });
            this.readLoop(port);
        } catch (err) {
            this.opening = false;
            await this.cleanup();
            this.post('sclosed', { reason: this.wantClose ? '' : 'open failed: ' + errText(err) });
        }
    }

    // Reads until the port is closed. A recoverable error (framing, parity, buffer overrun) replaces
    // port.readable with a new stream, so the outer loop gets a new reader; a fatal one ends the loop.
    async readLoop(port) {
        let reason = '';
        while (this.active && port.readable) {
            const reader = this.reader = port.readable.getReader();
            try {
                for (;;) {
                    const r = await reader.read();
                    if (r.done) break;
                    const v = r.value;
                    if (v && v.byteLength > 0 && this.active) {
                        this.st.rx++;
                        // Own the bytes exactly: a view into a larger buffer cannot be moved as it is
                        const buf = (v.byteOffset === 0 && v.byteLength === v.buffer.byteLength) ? v.buffer : v.slice().buffer;
                        this.post('srx', { buf: buf, t: performance.timeOrigin + performance.now() }, [buf]);
                    }
                }
            } catch (err) {
                this.st.rxErrors++;
                reason = errText(err);
                if (err && (err.name === 'NetworkError' || err.name === 'NotFoundError')) { this.active && this.lost(reason); return; }
            } finally {
                try { reader.releaseLock(); } catch (e) { /* already released */ }
                if (this.reader === reader) this.reader = null;
            }
            if (!this.active) return;
        }
        if (this.active) this.lost(reason || 'the port was closed');
    }

    write(data, id) {
        if (!this.active || this.txQueue.length - this.txHead >= TX_QUEUE_MAX) {
            this.st.txDropped++;
            if (id) this.post('swack', { id: id, ok: false });
            return;
        }
        this.txQueue.push({ data: new Uint8Array(data), id: id | 0 });
        this.pump();
    }

    async pump() {
        if (this.txBusy) return;
        this.txBusy = true;
        try {
            while (this.active && this.txHead < this.txQueue.length) {
                const item = this.txQueue[this.txHead++];
                let ok = true;
                try {
                    await this.writer.write(item.data);
                } catch (err) {
                    ok = false;
                    this.st.txErrors++;
                    if (err && (err.name === 'NetworkError' || err.name === 'NotFoundError')) { this.lost(errText(err)); }
                }
                if (item.id) this.post('swack', { id: item.id, ok: ok });
            }
        } finally {
            if (this.txHead >= this.txQueue.length) { this.txQueue.length = 0; this.txHead = 0; }
            this.txBusy = false;
        }
    }

    // Gives up the streams and closes the port; every step is best effort
    async cleanup() {
        const port = this.port;
        this.active = false;
        const reader = this.reader, writer = this.writer;
        this.reader = null;
        this.writer = null;
        this.port = null;
        // queued writes will never run: answer the ones that wait for an acknowledgement
        const rest = this.txQueue.slice(this.txHead);
        this.txQueue = [];
        this.txHead = 0;
        for (const item of rest) if (item.id) { try { this.post('swack', { id: item.id, ok: false }); } catch (e) { /* link gone */ } }
        if (reader) { try { await reader.cancel(); } catch (e) { /* stream already errored */ } }
        if (writer) { try { writer.releaseLock(); } catch (e) { /* write pending */ } }
        if (port) { try { await port.close(); } catch (e) { /* already closed or lost */ } }
    }

    async close(reason) {
        if (this.opening) { this.wantClose = true; return; }     // open() finishes the job
        if (!this.active && !this.port) { this.post('sclosed', { reason: reason || '' }); return; }
        await this.cleanup();
        this.post('sclosed', { reason: reason || '' });
    }

    lost(reason) {
        if (!this.active) return;
        console.warn('[serial_worker] dev ' + this.dev + ' lost:', reason);
        this.cleanup().then(() => this.post('sclosed', { reason: reason }));
    }

    tick() {
        if (!this.active) return;
        this.post('sstats', { rx: this.st.rx - this.st.rxPrev, rxErrors: this.st.rxErrors, txErrors: this.st.txErrors, txDropped: this.st.txDropped });
        this.st.rxPrev = this.st.rx;
    }
}

function ensureStatsTimer() {
    if (statsTimer === null) statsTimer = setInterval(() => {
        let any = false;
        for (const c of conns) if (c && c.active) { c.tick(); any = true; }
        if (!any) { clearInterval(statsTimer); statsTimer = null; }
    }, STATS_MS);
}

// ---------------------------------------------------------------------------------------------
// Messages from the CAN worker
// ---------------------------------------------------------------------------------------------

function onLinkMessage(ev) {
    const m = ev.data;
    const n = m.dev;
    if (!Number.isInteger(n) || n < 0 || n >= MAX_DEVICES) return;
    switch (m.type) {
        case 'sopen': {
            const old = conns[n];
            const c = conns[n] = new Conn(n, m.epoch);
            // The previous session of the slot (if any) must be gone before its port is opened again
            const wait = old && (old.active || old.port) ? old.cleanup() : Promise.resolve();
            wait.then(() => c.open(m.baud | 0));
            break;
        }
        case 'sclose': {
            const c = conns[n];
            if (c) c.close('closed by host');
            break;
        }
        case 'swrite': {
            const c = conns[n];
            if (c && c.epoch === m.epoch) c.write(m.data, m.id);
            else if (m.id) link.postMessage({ type: 'swack', dev: n, epoch: m.epoch, id: m.id, ok: false });
            break;
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Messages from the page
// ---------------------------------------------------------------------------------------------

self.onmessage = (ev) => {
    const m = ev.data;
    switch (m.type) {
        case 'link':
            link = m.port;
            link.onmessage = onLinkMessage;
            return;
        case 'pick':
            if (Number.isInteger(m.dev) && m.dev >= 0 && m.dev < MAX_DEVICES) {
                picks[m.dev] = { port: m.port || null, vid: m.vid | 0, pid: m.pid | 0, idx: m.idx | 0 };
            }
            return;
        case 'forget':
            if (Number.isInteger(m.dev) && m.dev >= 0 && m.dev < MAX_DEVICES) {
                const c = conns[m.dev];
                if (c && (c.active || c.port)) c.cleanup();
                conns[m.dev] = null;
                picks[m.dev] = null;
            }
            return;
    }
};

if (typeof navigator !== 'undefined' && navigator.serial) {
    navigator.serial.addEventListener('disconnect', (ev) => {
        const hit = conns.find(c => c && c.active && c.port === ev.target);
        if (hit) hit.lost('device disconnected');
    });
}
