// webserial_io.js - Web Serial transport for adapters on a serial port: one SerialConnection per port. No dependency on a CAN protocol, on
// WebAssembly or on postMessage; it runs in a window or in a dedicated worker.
//
// A SerialConnection opens a SerialPort (DTR and RTS asserted: CDC-ACM firmware often waits for DTR), reads until the port is closed
// or lost, and writes in order from a bounded queue. Everything it has to tell the owner goes through the `hooks` object:
//   resolvePort()              async, returns the SerialPort this connection is for (see findPermittedPort)
//   isPortBusy(port)           true if another open connection uses this port (optional)
//   onOpened(baud, info)       the port is open and reading started; info = "vid=1D50;pid=606F" for USB serial ports, else ''
//   onClosed(reason)           the connection ended (closed by the owner: ''; lost, or failed to open: the reason)
//   onData(buffer, t)          bytes read (an ArrayBuffer the owner may keep or transfer); t = performance.timeOrigin + performance.now()
//   onWriteDone(id, ok)        a write that was given a non-zero id finished (or was dropped / never ran)
//
// Loaded as a classic script (importScripts or a <script>); the one global it defines is WebSerialIo.
var WebSerialIo = (function () {
'use strict';

const DEFAULTS = {
    txQueueMax: 4096,           // queued writes per port (a write is one command or one frame line)
    portBuffer: 1 << 16,        // browser side receive buffer of the port
    openTimeoutMs: 4000,
    defaultBaud: 115200,
    tag: 'webserial-io'         // prefix of the log lines
};

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

// The permitted port the user picked. If SerialPort objects can be transferred to this context, `pick.port` is the object itself;
// otherwise the port is found again with navigator.serial.getPorts() as the idx-th permitted port with these USB ids.
async function findPermittedPort(pick) {
    if (!pick) throw new Error('no serial port was chosen for this device');
    if (pick.port) return pick.port;
    if (typeof navigator === 'undefined' || !navigator.serial) throw new Error('Web Serial is not available in this context');
    const all = await navigator.serial.getPorts();
    const same = all.filter(p => {
        const i = p.getInfo ? p.getInfo() : {};
        return (i.usbVendorId | 0) === (pick.vid | 0) && (i.usbProductId | 0) === (pick.pid | 0);
    });
    const port = same[pick.idx] || same[0];
    if (!port) throw new Error('the serial port is not among the permitted ports');
    return port;
}

class SerialConnection {
    constructor(label, options, hooks) {
        this.label = label;
        this.opt = Object.assign({}, DEFAULTS, options);
        this.hooks = hooks;
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

    warn(...args) { console.warn('[' + this.opt.tag + '] ' + this.label, ...args); }

    // Opens the port and starts reading. Always ends in onOpened or onClosed.
    async open(baud) {
        const rate = baud > 0 ? baud : this.opt.defaultBaud;
        this.opening = true;
        try {
            const port = await this.hooks.resolvePort();
            if (this.hooks.isPortBusy && this.hooks.isPortBusy(port)) throw new Error('the port is already in use');
            await withTimeout(port.open({ baudRate: rate, bufferSize: this.opt.portBuffer }), this.opt.openTimeoutMs, 'open timeout');
            this.port = port;
            // CDC-ACM firmware often waits for DTR before it talks
            try { await port.setSignals({ dataTerminalReady: true, requestToSend: true }); } catch (e) { /* not supported */ }
            if (!port.readable || !port.writable) throw new Error('the port has no readable/writable stream');
            this.writer = port.writable.getWriter();
            this.active = true;
            this.opening = false;
            if (this.wantClose) {           // closed while it was still opening
                await this.cleanup();
                this.hooks.onClosed('');
                return;
            }
            let info = '';
            try {
                const i = port.getInfo();
                if (i.usbVendorId !== undefined) info = 'vid=' + hex4(i.usbVendorId) + ';pid=' + hex4(i.usbProductId);
            } catch (e) { /* no info */ }
            this.hooks.onOpened(rate, info);
            this.readLoop(port);
        } catch (err) {
            this.opening = false;
            await this.cleanup();
            this.hooks.onClosed(this.wantClose ? '' : 'open failed: ' + errText(err));
        }
    }

    // Reads until the port is closed. A recoverable error (framing, parity, buffer overrun) replaces port.readable with a new
    // stream, so the outer loop gets a new reader; a fatal one ends the loop.
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
                        this.hooks.onData(buf, performance.timeOrigin + performance.now());
                    }
                }
            } catch (err) {
                this.st.rxErrors++;
                reason = errText(err);
                if (err && (err.name === 'NetworkError' || err.name === 'NotFoundError')) { if (this.active) this.lost(reason); return; }
            } finally {
                try { reader.releaseLock(); } catch (e) { /* already released */ }
                if (this.reader === reader) this.reader = null;
            }
            if (!this.active) return;
        }
        if (this.active) this.lost(reason || 'the port was closed');
    }

    // Queues bytes for writing; a non-zero `id` is reported through onWriteDone
    write(data, id) {
        if (!this.active || this.txQueue.length - this.txHead >= this.opt.txQueueMax) {
            this.st.txDropped++;
            if (id) this.hooks.onWriteDone(id, false);
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
                    if (err && (err.name === 'NetworkError' || err.name === 'NotFoundError')) this.lost(errText(err));
                }
                if (item.id) this.hooks.onWriteDone(item.id, ok);
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
        for (const item of rest) if (item.id) { try { this.hooks.onWriteDone(item.id, false); } catch (e) { /* owner gone */ } }
        if (reader) { try { await reader.cancel(); } catch (e) { /* stream already errored */ } }
        if (writer) { try { writer.releaseLock(); } catch (e) { /* write pending */ } }
        if (port) { try { await port.close(); } catch (e) { /* already closed or lost */ } }
    }

    // Closes at the owner's request (reason is passed on to onClosed). A close during open() finishes when the open does.
    async close(reason) {
        if (this.opening) { this.wantClose = true; return; }
        if (!this.active && !this.port) { this.hooks.onClosed(reason || ''); return; }
        await this.cleanup();
        this.hooks.onClosed(reason || '');
    }

    // The port went away or failed
    lost(reason) {
        if (!this.active) return;
        this.warn('lost:', reason);
        this.cleanup().then(() => this.hooks.onClosed(reason));
    }

    // Counters since the previous call (rx = reads) and the totals of the errors
    takeStats() {
        const s = { rx: this.st.rx - this.st.rxPrev, rxErrors: this.st.rxErrors, txErrors: this.st.txErrors, txDropped: this.st.txDropped };
        this.st.rxPrev = this.st.rx;
        return s;
    }
}

return { SerialConnection, DEFAULTS, findPermittedPort, errText, withTimeout, hex4 };
})();
