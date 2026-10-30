'use strict';
// Runs the real usb_worker.js and the real can_core.wasm (multi-file build) in a Node vm, with a mock USBDevice.
// Usage from a test: const h = await createHarness(buildDir, srcDir, mockDevice);  h.send({...}); h.posted; h.pushRx([...])
const fs = require('fs'), vm = require('vm'), path = require('path');

const sleep = ms => new Promise(r => setTimeout(r, ms));

function createChecks() {
    const c = { n: 0, fails: 0 };
    c.check = (cond, msg) => { c.n++; if (!cond) { c.fails++; console.log('FAIL:', msg); } };
    return c;
}

// A mock USBDevice. `controlIn(setup, len)` answers control IN requests (returns a DataView or null).
function createMockDevice(vendorId, productId, serialNumber, controlIn) {
    const state = { rxWaiters: [], ctrlLog: [], txOut: [], t0: performance.now() };
    const device = {
        vendorId, productId, serialNumber, opened: false,
        configuration: { interfaces: [{ interfaceNumber: 0, alternate: { endpoints: [
            { direction: 'in', endpointNumber: 1, type: 'bulk', packetSize: 64 }, { direction: 'out', endpointNumber: 2, type: 'bulk', packetSize: 64 }] } }] },
        async open() { this.opened = true; }, async close() { this.opened = false; },
        async selectConfiguration() {}, async claimInterface() {}, async clearHalt() {},
        async releaseInterface() { state.rxWaiters = []; },          // aborts the pending transfers
        async controlTransferIn(setup, len) {
            state.ctrlLog.push({ request: setup.request, value: setup.value, recipient: setup.recipient, t: performance.now() - state.t0 });
            await sleep(1 + Math.random() * 1.5);
            const dv = controlIn ? controlIn(setup, len, state) : null;
            return { status: 'ok', data: dv || new DataView(new ArrayBuffer(len)) };
        },
        async controlTransferOut(setup, data) { state.ctrlLog.push({ request: setup.request, value: setup.value, out: true }); return { status: 'ok', bytesWritten: data.byteLength }; },
        transferIn(ep, len) { return new Promise(res => state.rxWaiters.push({ ep, len, res })); },
        async transferOut(ep, data) {
            state.txOut.push({ ep, data: new Uint8Array(data.buffer ? data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength) : data) });
            return { status: 'ok', bytesWritten: data.byteLength };
        },
    };
    state.device = device;
    state.pushRx = (bytes) => {
        const w = state.rxWaiters.shift();
        if (!w) throw new Error('no pending transferIn');
        w.res({ status: 'ok', data: new DataView(new Uint8Array(bytes).buffer) });
    };
    return state;
}

function createHarness(buildDir, srcDir, mock) {
    const posted = [];
    const ctx = {
        console, setTimeout, clearTimeout, setInterval, clearInterval, performance, TextDecoder, TextEncoder, WebAssembly, Promise,
        navigator: { usb: { getDevices: async () => [mock.device], addEventListener() {} } },
        postMessage: (m) => posted.push(m),
        importScripts: (f) => vm.runInContext(fs.readFileSync(path.join(buildDir, f), 'utf8'), ctx, { filename: f }),
        // the Emscripten loader of a worker reads the wasm file with a synchronous XHR
        XMLHttpRequest: class {
            open(m, u) { this.u = u; }
            send() { const b = fs.readFileSync(path.join(buildDir, path.basename(this.u))); this.response = b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength); this.status = 200; }
        },
        location: { href: 'file:///t/' },
    };
    ctx.self = ctx;
    vm.createContext(ctx);
    vm.runInContext(fs.readFileSync(path.join(buildDir, 'can_core.js'), 'utf8'), ctx, { filename: 'can_core.js' });
    vm.runInContext(fs.readFileSync(path.join(srcDir, 'usb_worker.js'), 'utf8'), ctx, { filename: 'usb_worker.js' });
    const send = (m) => ctx.onmessage({ data: m });
    const waitFor = async (pred, ms, what, checks) => {
        const end = Date.now() + ms;
        while (Date.now() < end) { const f = posted.find(pred); if (f) return f; await sleep(10); }
        if (checks) checks.check(false, 'timeout waiting for ' + what);
        return null;
    };
    return { posted, send, waitFor, ctx };
}

const le32 = (v, x) => { for (let i = 0; i < 4; i++) v.push((x >>> (8 * i)) & 255); };

module.exports = { sleep, createChecks, createMockDevice, createHarness, le32 };
