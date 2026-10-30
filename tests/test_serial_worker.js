'use strict';
// The real serial_worker.js (the shell around webserial_io.js) in a Node vm with a mock link to the CAN worker and a mock navigator.serial:
// pick / sopen / srx / swrite / swack / sstats / sclose / forget, and a second session of the same slot.
// Usage: node tests/test_serial_worker.js [directory of serial_worker.js] [directory of webserial_io.js] [packed bundle of the single-file build]
const fs = require('fs');
const vm = require('vm');
const path = require('path');
const { sleep, createChecks } = require('./worker_harness');
const C = createChecks();
const check = C.check;

const appDir = process.argv[2] || path.join(__dirname, '..', 'apps', 'dearfdcan_web');
const libDir = process.argv[3] || path.join(__dirname, '..', 'libs', 'webserial-io');
const packedBundle = process.argv[4] || null;       // optional: build/tmp/single/usb_worker_bundle.js, run the packed serial worker instead

function mockPort() {
    const p = {
        written: [], closed: 0, ctl: null,
        getInfo: () => ({ usbVendorId: 0x1D50, usbProductId: 0x606F }),
        async open() {
            this.readable = new ReadableStream({ start: (c) => { this.ctl = c; } });
            this.writable = new WritableStream({ write: (chunk) => { this.written.push(Array.from(chunk)); } });
        },
        async setSignals() {}, async close() { this.closed++; },
    };
    return p;
}

(async () => {
    const port = mockPort();
    const link = { sent: [], postMessage(m, tr) { this.sent.push(m); } };
    const ctx = {
        console: { warn() {}, error() {}, log: console.log }, setTimeout, clearTimeout, setInterval, clearInterval, performance, Promise,
        ReadableStream, WritableStream, Uint8Array,
        navigator: { serial: { getPorts: async () => [port], addEventListener() {} } },
        importScripts: (f) => vm.runInContext(fs.readFileSync(path.join(libDir, f), 'utf8'), ctx, { filename: f }),
    };
    ctx.self = ctx;
    vm.createContext(ctx);
    let importScriptsUsed = false;
    const realImport = ctx.importScripts;
    ctx.importScripts = (f) => { importScriptsUsed = true; return realImport(f); };
    if (packedBundle) {
        const Module = {};
        new Function('Module', fs.readFileSync(packedBundle, 'utf8'))(Module);
        vm.runInContext(Module.serialWorkerSource, ctx, { filename: 'serialWorkerSource' });
        check(!importScriptsUsed && typeof ctx.WebSerialIo === 'object', 'the packed worker brings the library with it and does not load it');
    } else {
        vm.runInContext(fs.readFileSync(path.join(appDir, 'serial_worker.js'), 'utf8'), ctx, { filename: 'serial_worker.js' });
        check(importScriptsUsed && typeof ctx.WebSerialIo === 'object', 'the library is loaded with importScripts when it is missing');
    }
    const page = (m) => ctx.onmessage({ data: m });
    const fromCan = (m) => link.onmessage({ data: m });

    page({ type: 'link', port: link });
    page({ type: 'pick', dev: 2, vid: 0x1D50, pid: 0x606F, idx: 0 });

    fromCan({ type: 'sopen', dev: 2, epoch: 7, baud: 500000 });
    await sleep(20);
    const opened = link.sent.find(m => m.type === 'sopened');
    check(opened && opened.dev === 2 && opened.epoch === 7 && opened.eps === 'serial 500000 baud' && opened.info === 'vid=1D50;pid=606F', 'sopened ' + JSON.stringify(opened));

    port.ctl.enqueue(new Uint8Array([0x74, 0x0D]));
    await sleep(10);
    const rx = link.sent.find(m => m.type === 'srx');
    check(rx && rx.dev === 2 && rx.epoch === 7 && new Uint8Array(rx.buf).join() === '116,13' && rx.t > 1e12, 'srx carries the bytes and the time');

    fromCan({ type: 'swrite', dev: 2, epoch: 7, data: new Uint8Array([86, 13]).buffer, id: 5 });
    fromCan({ type: 'swrite', dev: 2, epoch: 7, data: new Uint8Array([1]).buffer, id: 0 });
    fromCan({ type: 'swrite', dev: 2, epoch: 6, data: new Uint8Array([2]).buffer, id: 8 });     // another epoch: refused
    await sleep(20);
    check(port.written.map(w => w.join('.')).join('|') === '86.13|1', 'writes reach the port in order, an old epoch is not written');
    check(link.sent.filter(m => m.type === 'swack').map(m => m.id + ':' + m.ok).join() === '8:false,5:true' || link.sent.filter(m => m.type === 'swack').map(m => m.id + ':' + m.ok).sort().join() === '5:true,8:false', 'swack for the acknowledged writes');

    await sleep(1100);
    const stats = link.sent.find(m => m.type === 'sstats');
    check(stats && stats.dev === 2 && stats.rx === 1 && stats.rxErrors === 0, 'sstats once a second ' + JSON.stringify(stats));

    // a second session of the same slot: the old one is gone before the port is opened again
    link.sent.length = 0;
    fromCan({ type: 'sopen', dev: 2, epoch: 8, baud: 115200 });
    await sleep(30);
    check(port.closed === 1 && link.sent.some(m => m.type === 'sopened' && m.epoch === 8), 'a new session closes the old one first and opens again');

    fromCan({ type: 'sclose', dev: 2 });
    await sleep(20);
    const closed = link.sent.find(m => m.type === 'sclosed');
    check(closed && closed.epoch === 8 && closed.reason === 'closed by host' && port.closed === 2, 'sclose ends the session');

    // forget: a removed slot has no pick any more
    page({ type: 'forget', dev: 2 });
    link.sent.length = 0;
    fromCan({ type: 'sopen', dev: 2, epoch: 9, baud: 115200 });
    await sleep(20);
    const failed = link.sent.find(m => m.type === 'sclosed');
    check(failed && /no serial port was chosen/.test(failed.reason), 'after forget the slot cannot be opened ' + JSON.stringify(failed));

    console.log(C.n + ' checks, ' + C.fails + ' failed');
    process.exit(C.fails ? 1 : 0);
})().catch(e => { process.stdout.write(String(e && e.stack || e) + '\n'); process.exit(2); });
