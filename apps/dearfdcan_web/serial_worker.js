'use strict';
// serial_worker.js - WebSerial transport (I/O only), running in its own dedicated Worker.
//
// This worker owns the SerialPort objects and nothing else: it opens and closes ports, reads bytes and writes bytes (the work itself is done
// by webserial_io.js, libs/webserial-io). Parsing, the ID table, the trace and the snapshots stay in the CAN worker (usb_worker.js), which
// hosts the one shared can_core module. The two workers are connected by a MessageChannel: the page creates it and hands one end to each
// worker ('link'), so received bytes go from here straight to the CAN worker without a detour over the page. Buffers are moved with the
// transfer list, never copied. The messages are specified in WORKER_PROTOCOL.md.

// The library is loaded once per worker: the single-file bundle made by pack_worker.py already contains it in front of this script
if (typeof WebSerialIo === 'undefined') importScripts('webserial_io.js');

const MAX_DEVICES = 8;              // must match kMaxDevices in can_protocol_common.h
const STATS_MS = 1000;

let link = null;
const picks = new Array(MAX_DEVICES).fill(null);     // { port, vid, pid, idx } chosen by the user
const conns = new Array(MAX_DEVICES).fill(null);
let statsTimer = null;

// One serial port in use: a SerialConnection whose events go to the CAN worker over the link
function makeConn(dev, epoch) {
    const send = (type, fields, transfer) => {
        const msg = fields || {};
        msg.type = type;
        msg.dev = dev;
        msg.epoch = epoch;
        link.postMessage(msg, transfer || []);
    };
    const c = new WebSerialIo.SerialConnection('dev ' + dev, { tag: 'serial_worker' }, {
        resolvePort: () => WebSerialIo.findPermittedPort(picks[dev]),
        isPortBusy: (port) => conns.some(o => o && o !== c && o.port === port && o.active),
        onOpened: (baud, info) => { ensureStatsTimer(); send('sopened', { eps: 'serial ' + baud + ' baud', info: info }); },
        onClosed: (reason) => send('sclosed', { reason: reason }),
        onData: (buf, t) => send('srx', { buf: buf, t: t }, [buf]),
        onWriteDone: (id, ok) => send('swack', { id: id, ok: ok })
    });
    c.dev = dev;
    c.epoch = epoch;
    c.send = send;
    return c;
}

function ensureStatsTimer() {
    if (statsTimer === null) statsTimer = setInterval(() => {
        let any = false;
        for (const c of conns) {
            if (c && c.active) { c.send('sstats', c.takeStats()); any = true; }
        }
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
            const c = conns[n] = makeConn(n, m.epoch);
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
