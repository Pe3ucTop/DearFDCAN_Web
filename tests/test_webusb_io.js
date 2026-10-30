'use strict';
// Tests of libs/webusb-io (UsbPort) against a mock USBDevice: device matching, the open sequence and its hooks, the receive slots and
// their generations, error handling and recovery, loss, ordered control transfers with timeouts, the transmit queue, the watchdog.
// Usage: node tests/test_webusb_io.js [path of webusb_io.js]
const fs = require('fs');
const path = require('path');
const { sleep, createChecks } = require('./worker_harness');
const C = createChecks();
const check = C.check;

const src = fs.readFileSync(process.argv[2] || path.join(__dirname, '..', 'libs', 'webusb-io', 'webusb_io.js'), 'utf8');
const WebUsbIo = new Function(src + '\n;return WebUsbIo;')();
const { UsbPort, isGone, sameDevice, inspectEndpoints, describeEndpoints } = WebUsbIo;

// The warnings and errors the library logs for the failures the tests provoke are not shown
const logged = [];
console.warn = console.error = (...args) => logged.push(args.join(' '));

const goneError = () => Object.assign(new Error('device lost'), { name: 'NotFoundError' });

// A USBDevice whose transfers are controlled by the test. Pending transferIn calls wait in `rxWaiters` until the test completes them.
function mockUsb(vendorId = 0x1D50, productId = 0x606F, serialNumber = 'S1') {
    const u = {
        vendorId, productId, serialNumber, opened: false, claimed: 0, released: 0, closed: 0, halts: [],
        rxWaiters: [], outs: [], outPending: [], ctrl: [], ctrlBehavior: null, inflightOut: 0, maxInflightOut: 0, outBehavior: null,
        configuration: { interfaces: [{ interfaceNumber: 0, alternate: { endpoints: [
            { direction: 'in', endpointNumber: 3, type: 'bulk', packetSize: 64 }, { direction: 'out', endpointNumber: 4, type: 'bulk', packetSize: 64 },
            { direction: 'in', endpointNumber: 5, type: 'interrupt', packetSize: 8 }] } }] },
        async open() { this.opened = true; },
        async close() { this.opened = false; this.closed++; },
        async selectConfiguration() {},
        async claimInterface() { this.claimed++; },
        async releaseInterface() { this.released++; this.rxWaiters = []; },     // releasing aborts the pending transfers
        async clearHalt(dir, ep) { this.halts.push(dir + ep); },
        transferIn(ep, len) { return new Promise((resolve, reject) => this.rxWaiters.push({ ep, len, resolve, reject })); },
        async transferOut(ep, data) {
            this.outs.push({ ep, n: data.byteLength });
            this.inflightOut++; this.maxInflightOut = Math.max(this.maxInflightOut, this.inflightOut);
            try {
                if (this.outBehavior) return await this.outBehavior(ep, data);
                return { status: 'ok' };
            } finally { this.inflightOut--; }
        },
        async controlTransferIn(setup, len) { this.ctrl.push(['in', setup.request]); return this.ctrlBehavior ? this.ctrlBehavior('in', setup, len) : { status: 'ok', data: new DataView(new ArrayBuffer(len)) }; },
        async controlTransferOut(setup, data) { this.ctrl.push(['out', setup.request]); return this.ctrlBehavior ? this.ctrlBehavior('out', setup, data) : { status: 'ok' }; },
    };
    return u;
}
const completeRx = (u, bytes, status = 'ok') => { const w = u.rxWaiters.shift(); w.resolve({ status, data: bytes ? new DataView(new Uint8Array(bytes).buffer) : undefined }); return w; };

function installUsb(list) {
    Object.defineProperty(globalThis, 'navigator', { value: { usb: { getDevices: async () => list } }, configurable: true, writable: true });
}

// A port that records its hooks
class TestPort extends UsbPort {
    constructor(label, options) {
        super(label, Object.assign({ tag: 'test', rxSlots: 3, ctrlTimeoutMs: 60, idlePingMs: 40, pingTimeoutMs: 40 }, options));
        this.events = [];
        this.data = [];
        this.probe = null;
    }
    onOpening(hs) { this.events.push('opening'); this.openingSawRx = this.usb && this.usb.rxWaiters.length; }
    onRxData(dv) { this.data.push(Array.from(new Uint8Array(dv.buffer, dv.byteOffset, dv.byteLength))); }
    onTxQueued(data, ep) { this.events.push('txq'); }
    onRecovered() { this.events.push('recovered'); }
    beforeTeardown() { this.events.push('before'); }
    afterTeardown() { this.events.push('after'); }
    onLost(reason) { this.events.push('lost:' + reason); }
    probeRequest() { return this.probe; }
}
const layout = { layout: { rxEp: 3, txEp: 4, cmdEp: 0, rxSize: 512 } };
const handshake = async () => layout;

async function testMatchingAndOpen() {
    // two identical adapters without a serial number, opened at the same moment, must end up with different devices
    const a = mockUsb(0x1D50, 0x606F, undefined), b = mockUsb(0x1D50, 0x606F, undefined), other = mockUsb(0x0C72, 0x0012, 'X');
    installUsb([a, b, other]);
    const ports = [];
    const inUse = () => ports.map(p => p.usb).filter(Boolean);
    const p1 = new TestPort('p1', { inUse }), p2 = new TestPort('p2', { inUse }), p3 = new TestPort('p3', { inUse });
    ports.push(p1, p2, p3);
    await Promise.all([p1.openDevice({ vid: 0x1D50, pid: 0x606F }, handshake), p2.openDevice({ vid: 0x1D50, pid: 0x606F }, handshake)]);
    check(p1.usb && p2.usb && p1.usb !== p2.usb, 'two ports opened at once get two different devices');
    check(a.opened && b.opened && a.claimed === 1 && b.claimed === 1 && !other.opened, 'both opened and claimed, the other adapter untouched');
    let err = null;
    try { await p3.openDevice({ vid: 0x1D50, pid: 0x606F }, handshake); } catch (e) { err = e; }
    check(err && /not among the permitted/.test(err.message) && !p3.usb, 'a third port finds no free device');

    // serial number match, hook order, receive slots started with the endpoints of the layout
    const c = mockUsb(0x1D50, 0x606F, 'WANT');
    installUsb([a, c]);
    const p4 = new TestPort('p4', { inUse: () => [a] });
    const hs = await p4.openDevice({ vid: 0x1D50, pid: 0x606F, serial: 'WANT' }, handshake);
    check(hs === layout && p4.usb === c, 'matches by serial number and returns the handshake result');
    check(p4.events[0] === 'opening' && p4.openingSawRx === 0, 'onOpening runs before the first transfer is submitted');
    check(c.rxWaiters.length === 3 && c.rxWaiters.every(w => w.ep === 3 && w.len === 512), 'rx slots pending on the endpoint of the layout');

    // a failing handshake closes the device again and rejects
    const d = mockUsb();
    installUsb([d]);
    const p5 = new TestPort('p5');
    err = null;
    try { await p5.openDevice({ vid: 0x1D50, pid: 0x606F }, async () => { throw new Error('no firmware'); }); } catch (e) { err = e; }
    check(err && err.message === 'no firmware' && !p5.usb && d.closed === 1 && !d.opened, 'a failed handshake rejects and closes the device');

    // descriptor helpers
    const ep = inspectEndpoints(c);
    check(ep.rxEp === 3 && ep.txEp === 4 && ep.rxSize === 512 && /IN 3 bulk 64B/.test(ep.text) && /interrupt/.test(ep.text), 'inspectEndpoints picks the first bulk IN and OUT');
    check(describeEndpoints(c, 0).includes('OUT 4 bulk 64B') && describeEndpoints(c, 7) === '', 'describeEndpoints');
    check(sameDevice(c, mockUsb(0x1D50, 0x606F, 'WANT')) && !sameDevice(c, a) && !isGone(new Error('x')) && isGone(goneError()), 'sameDevice / isGone');
    await p4.closeDevice();
}

async function testReceive() {
    const u = mockUsb(); installUsb([u]);
    const p = new TestPort('rx', { rxErrorsBeforeRecover: 2 });
    await p.openDevice({ vid: u.vendorId, pid: u.productId }, handshake);

    completeRx(u, [1, 2, 3]);
    await sleep(1);
    check(u.rxWaiters.length === 3 && p.data.length === 1 && p.data[0].join() === '1,2,3' && p.st.rx === 1, 'a completed transfer is delivered and its slot re-armed at once');
    completeRx(u, [], 'ok'); completeRx(u, null, 'ok');
    await sleep(1);
    check(p.data.length === 1 && u.rxWaiters.length === 3, 'empty transfers are not delivered');
    const w = completeRx(u, [9], 'stall');
    await sleep(2);
    check(u.halts.join() === 'in3' && u.rxWaiters.length === 3 && p.st.rxErrors === 1 && p.data.length === 1, 'a stall clears the halt and re-arms (' + u.halts + ')');
    completeRx(u, [7], 'babble');
    await sleep(1);
    check(p.st.rxErrors === 2 && p.data.length === 1 && u.rxWaiters.length === 3, 'babble is counted and dropped');

    // errors of one slot in a row (two here, five by default): the interface is released and claimed again, the generation changes
    const stale = u.rxWaiters[1];                       // still pending when the recovery aborts everything
    const gen0 = p.rxGen;
    u.rxWaiters.shift().reject(new Error('io'));        // the slot retries after 20 ms ...
    await sleep(35);
    check(u.rxWaiters.length === 3 && p.st.recoveries === 0, 'a failed slot is re-armed after a pause');
    u.rxWaiters.pop().reject(new Error('io'));          // ... the retry is the last one, and fails again
    await sleep(100);
    check(u.released === 1 && u.claimed === 2 && p.events.includes('recovered') && p.st.recoveries === 1 && p.rxGen > gen0, 'repeated errors of a slot trigger a recovery');
    check(u.rxWaiters.length === 3, 'the slots are rebuilt after the recovery (' + u.rxWaiters.length + ')');
    stale.resolve({ status: 'ok', data: new DataView(new Uint8Array([0xEE]).buffer) });
    await sleep(2);
    check(!p.data.some(x => x[0] === 0xEE), 'the completion of an older generation is ignored');

    // another endpoint / size: recovery; the same: nothing
    const before = u.released;
    p.reconfigureRx(0, 0); p.reconfigureRx(3, 512);
    await sleep(5);
    check(u.released === before, 'the same endpoint and size change nothing');
    p.reconfigureRx(6, 4096);
    await sleep(20);
    check(u.released === before + 1 && p.rxEp === 6 && p.rxSize === 4096 && u.rxWaiters.length === 3 && u.rxWaiters[0].ep === 6 && u.rxWaiters[0].len === 4096, 'a new endpoint / size replaces the pending transfers');
    p.reconfigureRx(0, 10);
    await sleep(20);
    check(p.rxSize === 64, 'the size is limited to 64..16384');

    // loss
    p.events.length = 0;
    u.rxWaiters[0].reject(goneError());
    await sleep(5);
    check(!p.usb && p.events.join() === 'before,after,lost:device lost' && u.closed === 1, 'an unplugged adapter ends the session: teardown, close, onLost (' + p.events.join() + ')');
    p.events.length = 0;
    p.lost('again');
    check(p.events.length === 0, 'losing a closed port does nothing');
}

async function testControl() {
    const u = mockUsb(); installUsb([u]);
    const p = new TestPort('ctl');
    await p.openDevice({ vid: u.vendorId, pid: u.productId }, handshake);
    const setup = (request) => ({ requestType: 'vendor', recipient: 'interface', request, value: 0, index: 0 });

    // in order, whatever each transfer takes
    const order = [];
    u.ctrlBehavior = async (dir, s) => { await sleep(s.request === 1 ? 20 : s.request === 2 ? 10 : 1); order.push(s.request); return dir === 'in' ? { status: 'ok', data: new DataView(new Uint8Array([s.request, 9]).buffer, 1, 1) } : { status: 'ok' }; };
    const results = [];
    p.enqueue(async () => results.push(await p.controlTransfer(true, setup(1), new Uint8Array(2))));
    p.enqueue(async () => results.push(await p.controlTransfer(false, setup(2), 2)));
    p.enqueue(async () => results.push(await p.controlTransfer(true, setup(3), new Uint8Array(0))));
    await p.chain;
    check(order.join() === '1,2,3', 'control transfers run strictly in the order they were enqueued (' + order + ')');
    check(results[0].ok && results[0].data === null && results[1].ok && results[1].data instanceof ArrayBuffer && results[1].data.byteLength === 1 && results[1].data.byteLength === 1 && new Uint8Array(results[1].data)[0] === 9, 'results: OUT has no data, IN gives exactly the bytes of the view');

    // empty IN data, a failing status, a timeout
    u.ctrlBehavior = async (dir) => ({ status: 'ok', data: new DataView(new ArrayBuffer(0)) });
    let r = await p.controlTransfer(false, setup(4), 4);
    check(r.ok && r.data === null, 'an empty IN answer has no data');
    u.ctrlBehavior = async () => ({ status: 'stall' });
    r = await p.controlTransfer(true, setup(5), new Uint8Array(1));
    check(!r.ok, 'a stalled control OUT is not ok');
    u.ctrlBehavior = () => new Promise(() => {});
    const t0 = Date.now();
    r = await p.controlTransfer(false, setup(6), 4);
    check(!r.ok && Date.now() - t0 >= 50 && Date.now() - t0 < 500 && p.usb, 'a control transfer that never completes times out and keeps the session');

    // commands on a bulk endpoint share the timeout
    r = await p.commandTransfer(1, new Uint8Array(8), 'id 1');
    check(r.ok && u.outs.length === 1 && u.outs[0].ep === 1, 'commandTransfer');
    u.outBehavior = () => new Promise(() => {});
    r = await p.commandTransfer(1, new Uint8Array(8), 'id 2');
    check(!r.ok, 'a command that never completes times out');
    u.outBehavior = null;

    // an unplugged adapter during a control transfer ends the session
    u.ctrlBehavior = async () => { throw goneError(); };
    p.events.length = 0;
    r = await p.controlTransfer(true, setup(7), new Uint8Array(1));
    check(!r.ok && !p.usb && p.events.some(x => x.startsWith('lost:')), 'a lost adapter ends the session from a control transfer');
    r = await p.controlTransfer(true, setup(8), new Uint8Array(1));
    check(!r.ok, 'no transfers on a closed port');
}

async function testTransmit() {
    const u = mockUsb(); installUsb([u]);
    const p = new TestPort('tx', { txInflight: 2, txQueueMax: 5 });
    await p.openDevice({ vid: u.vendorId, pid: u.productId }, handshake);
    p.events.length = 0;

    // at most two transfers in flight, all of them sent, in order; a full queue drops
    const release = [];
    u.outBehavior = (ep, data) => new Promise(res => release.push(() => res({ status: 'ok' })));
    const accepted = [];
    for (let i = 0; i < 9; i++) accepted.push(p.enqueueTx(new Uint8Array([i]).buffer, i === 8 ? 0x84 : 0));
    await sleep(2);
    check(u.maxInflightOut === 2 && u.outs.length === 2, 'two transfers in flight (' + u.maxInflightOut + ')');
    check(accepted.filter(Boolean).length === 7 && p.st.txDropped === 2 && p.events.filter(x => x === 'txq').length === 7, 'the queue holds 5 behind the 2 in flight, the rest is dropped without onTxQueued');
    while (release.length) { release.shift()(); await sleep(1); }
    await sleep(5);
    check(u.outs.length === 7 && u.maxInflightOut === 2 && u.outs[0].ep === 4 && u.outs[6].ep === 4, 'everything queued was sent on the default endpoint (' + u.outs.length + ')');

    // a stalled endpoint is cleared, errors are counted
    u.outBehavior = async () => ({ status: 'stall' });
    p.enqueueTx(new Uint8Array(1).buffer, 9);
    await sleep(5);
    check(u.halts.includes('out9') && p.st.txErrors === 1, 'a stall on OUT clears the halt and counts an error');
    u.outBehavior = async () => ({ status: 'babble' });
    p.enqueueTx(new Uint8Array(1).buffer);
    await sleep(5);
    check(p.st.txErrors === 2, 'a failed transfer counts an error');
    u.outBehavior = async () => { throw goneError(); };
    p.events.length = 0;
    p.enqueueTx(new Uint8Array(1).buffer);
    await sleep(5);
    check(!p.usb && p.events.some(x => x.startsWith('lost:')), 'an unplugged adapter ends the session from a transmit');
    check(p.enqueueTx(new Uint8Array(1).buffer) === false, 'no transmit on a closed port');
}

async function testWatchdogAndClose() {
    const u = mockUsb(); installUsb([u]);
    const p = new TestPort('wd', { idlePingMs: 30, pingTimeoutMs: 30, pingFailsMax: 2 });
    p.probe = [{ requestType: 'vendor', recipient: 'interface', request: 4, value: 0, index: 0 }, 40];
    await p.openDevice({ vid: u.vendorId, pid: u.productId }, handshake);

    p.watchdog();
    await sleep(5);
    check(u.ctrl.length === 0, 'no probe while the port is not idle');
    await sleep(40);
    p.watchdog(); await sleep(10);
    check(u.ctrl.length === 1 && u.ctrl[0][1] === 4, 'an idle port is probed with the control request of the owner');
    await sleep(40);
    p.noteAlive(); p.watchdog(); await sleep(5);
    check(u.ctrl.length === 1, 'a control transfer of the owner counts as alive');

    // the probe fails twice in a row: recovery
    u.ctrlBehavior = async () => ({ status: 'stall' });
    await sleep(40); p.watchdog(); await sleep(15);
    await sleep(40); p.watchdog(); await sleep(60);
    check(u.released === 1 && p.events.includes('recovered'), 'two failed probes in a row trigger a recovery');
    u.ctrlBehavior = null;

    // no probe request: no probe
    const q = new TestPort('nop');
    const v = mockUsb(); installUsb([v]);
    await q.openDevice({ vid: v.vendorId, pid: v.productId }, handshake);
    await sleep(50); q.watchdog(); await sleep(5);
    check(v.ctrl.length === 0, 'nothing to probe with');

    // close: hooks in order, completions after it are ignored, a second close says so
    p.events.length = 0;
    const stale = u.rxWaiters[0];
    const closed = await p.closeDevice();
    check(closed === true && p.events.join() === 'before,after' && !p.usb && !u.opened, 'closeDevice: teardown hooks around it, device closed');
    const n = p.data.length;
    stale.resolve({ status: 'ok', data: new DataView(new Uint8Array([1]).buffer) });
    await sleep(2);
    check(p.data.length === n, 'completions after the close are ignored');
    check((await p.closeDevice()) === false, 'a second close reports that the port was not open');
    await q.closeDevice();
}

// A device the owner already holds ({device}), receiving on demand (autoReceive false) and completion callbacks of transmits:
// what a library for other programs (libs/emscripten-webusb) needs
async function testDirectAndOnDemand() {
    const u = mockUsb(); u.configuration = null;
    let selected = null;
    u.selectConfiguration = async (v) => { selected = v; u.configuration = mockUsb().configuration; };
    installUsb([]);                                         // getDevices() would find nothing: the device object is used as it is
    const p = new TestPort('direct', { autoReceive: false, txInflight: 1 });
    await p.openDevice({ device: u, configuration: 2 }, handshake);
    check(p.usb === u && u.opened && selected === 2 && u.claimed === 1, 'a given device is opened, the requested configuration selected and the interface claimed');
    check(u.rxWaiters.length === 0 && !p.receiving, 'with autoReceive false no transfer is pending after the open');

    p.reconfigureRx(7, 256);
    await sleep(5);
    check(u.released === 0 && p.rxEp === 7 && p.rxSize === 256 && u.rxWaiters.length === 0, 'reconfiguring while not receiving only remembers the values');
    p.beginRx(3, 1024);
    check(p.receiving && u.rxWaiters.length === 3 && u.rxWaiters[0].ep === 3 && u.rxWaiters[0].len === 1024, 'beginRx starts the receive slots on the endpoint and size given');
    completeRx(u, [1]);
    await sleep(2);
    check(p.data.length === 1, 'data arrives after beginRx');
    p.stopRx();
    completeRx(u, [2]);
    await sleep(2);
    check(!p.receiving && p.data.length === 1 && u.rxWaiters.length === 2, 'after stopRx what the pending transfers bring is dropped and no slot is re-armed');
    p.beginRx(0, 0);
    check(p.receiving && u.rxWaiters.length === 2 + 3, 'beginRx can start again');

    // completion callbacks: after the transfer, with false for a failed one and for one that never ran
    const done = [];
    const gate = [];
    u.outBehavior = (ep, data) => new Promise(res => gate.push(() => res(data.byteLength === 2 ? { status: 'babble' } : { status: 'ok' })));
    p.enqueueTx(new Uint8Array(1).buffer, 0, ok => done.push('a' + ok));
    p.enqueueTx(new Uint8Array(2).buffer, 0, ok => done.push('b' + ok));
    p.enqueueTx(new Uint8Array(3).buffer, 0, ok => done.push('c' + ok));
    p.enqueueTx(new Uint8Array(4).buffer, 0, ok => done.push('d' + ok));
    await sleep(2);
    gate.shift()(); await sleep(2);
    gate.shift()(); await sleep(2);
    check(done.join() === 'atrue,bfalse' && u.outs.length === 3, 'a callback per transfer with its result, in order (' + done + ')');
    await p.closeDevice();
    check(done.join() === 'atrue,bfalse,dfalse' && u.outs.length === 3, 'a transfer that never ran is reported as failed when the session ends (' + done + ')');
    gate.shift()(); await sleep(2);
    check(done.join() === 'atrue,bfalse,dfalse,ctrue', 'the one in flight reports when the browser completes it (' + done + ')');
}

// While the interface is released and claimed again the browser refuses every transfer with InvalidStateError, which looks like an
// unplugged adapter. Transfers started in that window must wait for its end and must not end the session.
async function testRecoveryWindow() {
    const invalid = () => Object.assign(new Error('An operation that changes interface state is in progress.'), { name: 'InvalidStateError' });
    const u = mockUsb(); installUsb([u]);
    let busy = false;
    u.releaseInterface = async () => { busy = true; u.released++; u.rxWaiters = []; await sleep(25); };
    const claim = u.claimInterface.bind(u);
    u.claimInterface = async (n) => { await sleep(25); await claim(n); busy = false; };
    for (const name of ['transferOut', 'controlTransferIn', 'controlTransferOut']) {
        const real = u[name].bind(u);
        u[name] = async (...a) => { if (busy) throw invalid(); return real(...a); };
    }
    const realIn = u.transferIn.bind(u);
    u.transferIn = (ep, len) => (busy ? Promise.reject(invalid()) : realIn(ep, len));

    const p = new TestPort('window', { txInflight: 2 });
    await p.openDevice({ vid: u.vendorId, pid: u.productId }, handshake);
    p.events.length = 0;

    p.reconfigureRx(6, 512);                                    // starts the recovery
    check(p.recovering, 'the recovery is running');
    const accepted = [1, 2, 3].map(i => p.enqueueTx(new Uint8Array([i]).buffer));
    const ctl = p.controlTransfer(false, { requestType: 'vendor', recipient: 'interface', request: 6, value: 0, index: 0 }, 4);
    const cmd = p.commandTransfer(1, new Uint8Array(2), 'id 9');
    await sleep(15);
    check(u.outs.length === 0 && u.ctrl.length === 0, 'nothing is sent while the interface is being re-claimed');
    const [rc, rm] = await Promise.all([ctl, cmd]);
    await sleep(20);
    check(p.usb && !p.events.some(x => x.startsWith('lost:')), 'the session survives transfers started during the recovery (' + p.events.join() + ')');
    check(rc.ok && rm.ok && u.ctrl.length === 1, 'a control transfer and a command that were started meanwhile ran afterwards');
    check(accepted.every(Boolean) && u.outs.length === 3 + 1 && p.st.recoveries === 1 && u.rxWaiters.length === 3 && u.rxWaiters[0].ep === 6, 'the queued transmits went out afterwards, receiving continues on the new endpoint');

    // a refusal that does arrive during the recovery (a transfer already on its way) is an ordinary failure
    p.reconfigureRx(3, 512);
    const early = p.controlTransfer(true, { requestType: 'vendor', recipient: 'interface', request: 1, value: 0, index: 0 }, new Uint8Array(1));
    await sleep(60);
    check((await early).ok && p.usb, 'a control transfer queued behind a second recovery succeeds too');

    // an adapter that really goes away during the recovery ends the session
    const v = mockUsb(); installUsb([v]);
    v.releaseInterface = async () => { throw Object.assign(new Error('The device was disconnected.'), { name: 'NotFoundError' }); };
    const q = new TestPort('gone');
    await q.openDevice({ vid: v.vendorId, pid: v.productId }, handshake);
    q.events.length = 0;
    q.reconfigureRx(6, 512);
    await sleep(20);
    check(!q.usb && q.events.some(x => x.startsWith('lost:recovery failed')), 'an unplug during the recovery is a loss (' + q.events.join() + ')');

    // log lines reach the owner
    const w = mockUsb(); installUsb([w]);
    const lines = [];
    const r = new TestPort('log'); r.onLog = (level, text) => lines.push(level + ':' + text);
    await r.openDevice({ vid: w.vendorId, pid: w.productId }, handshake);
    w.ctrlBehavior = async () => { throw Object.assign(new Error('A transfer error has occurred.'), { name: 'NetworkError' }); };
    await r.controlTransfer(false, { requestType: 'vendor', recipient: 'interface', request: 1, value: 0, index: 0 }, 4);
    check(lines.length === 1 && lines[0] === 'error:control transfer failed: NetworkError: A transfer error has occurred.', 'warnings and errors go to onLog (' + lines + ')');
}

// planTx: the owner may merge the first queued items into one transfer, or hold the queue back until it calls pumpTx() again
async function testPlanTx() {
    const u = mockUsb(); u.configuration = null;
    u.selectConfiguration = async () => { u.configuration = mockUsb().configuration; };
    installUsb([]);
    const p = new TestPort('plan', { autoReceive: false, txInflight: 1 });
    await p.openDevice({ device: u, configuration: 1 }, handshake);
    const done = [];
    let hold = true;
    p.planTx = (queue, head) => {
        if (hold) return null;
        const n = Math.min(queue.length - head, 3);
        return n < 2 ? { count: 1 } : { count: n, data: new Uint8Array(n * 10).buffer };
    };
    for (let i = 1; i <= 7; i++) p.enqueueTx(new Uint8Array(i).buffer, 0, ok => done.push(i + (ok ? '+' : '-')));
    await sleep(5);
    check(u.outs.length === 0 && done.length === 0, 'planTx returning null holds the queue back');
    hold = false;
    p.pumpTx();
    await sleep(10);
    check(u.outs.map(o => o.n).join() === '30,30,7' && done.join() === '1+,2+,3+,4+,5+,6+,7+', 'merged items go out as one transfer of the given bytes, each callback gets the result (' + u.outs.map(o => o.n) + ' / ' + done + ')');
    check(p.txQueue.length - p.txHead === 0, 'the queue is empty afterwards');
    u.outBehavior = async () => ({ status: 'babble' });
    done.length = 0;
    const lines = [];
    p.onLog = (level, text) => lines.push(level + ':' + text);
    hold = true;
    for (let i = 1; i <= 3; i++) p.enqueueTx(new Uint8Array(i).buffer, 0, ok => done.push(i + (ok ? '+' : '-')));
    hold = false;
    p.pumpTx();
    await sleep(10);
    check(done.join() === '1-,2-,3-' && p.st.txErrors === 1, 'a failed merged transfer fails all its items (' + done + ')');
    check(lines.length === 1 && lines[0] === 'warn:transmit failed with status babble (30 bytes, 3 frames merged)', 'a failed transmit is logged with the status and the size (' + lines + ')');
    await p.closeDevice();
}

// A transfer the device stalls is sent again (same bytes, same place in the queue) before its frames are given up
async function testStallRetry() {
    const u = mockUsb(); u.configuration = null;
    u.selectConfiguration = async () => { u.configuration = mockUsb().configuration; };
    installUsb([]);
    const p = new TestPort('retry', { autoReceive: false, txInflight: 1, txStallRetries: 2, txRetryDelayMs: 1 });
    p.onLog = () => {};
    await p.openDevice({ device: u, configuration: 1 }, handshake);
    const done = [];
    let n = 0;
    u.outBehavior = async () => (++n <= 2 ? { status: 'stall' } : { status: 'ok' });
    for (let i = 1; i <= 3; i++) p.enqueueTx(new Uint8Array(i).buffer, 0, ok => done.push(i + (ok ? '+' : '-')));
    await sleep(60);
    check(u.outs.map(o => o.n).join() === '1,1,1,2,3' && done.join() === '1+,2+,3+', 'a stalled transfer is sent again and keeps its place (' + u.outs.map(o => o.n) + ' / ' + done + ')');
    check(p.st.txRetries === 2 && p.st.txErrors === 0 && u.halts.length === 2, 'retries are counted, no error while they succeed (' + p.st.txRetries + ' / ' + p.st.txErrors + ')');

    u.outBehavior = async () => ({ status: 'stall' });
    done.length = 0;
    u.outs.length = 0;
    p.enqueueTx(new Uint8Array(4).buffer, 0, ok => done.push('4' + (ok ? '+' : '-')));
    p.enqueueTx(new Uint8Array(5).buffer, 0, ok => done.push('5' + (ok ? '+' : '-')));
    await sleep(60);
    check(u.outs.map(o => o.n).join() === '4,4,4,5,5,5' && done.join() === '4-,5-' && p.st.txErrors === 2, 'after the retries the frames are given up and the queue goes on (' + u.outs.map(o => o.n) + ' / ' + done + ')');
    await p.closeDevice();
}

(async () => {
    await testStallRetry();
    await testPlanTx();
    await testRecoveryWindow();
    await testDirectAndOnDemand();
    await testMatchingAndOpen();
    await testReceive();
    await testControl();
    await testTransmit();
    await testWatchdogAndClose();
    console.log(C.n + ' checks, ' + C.fails + ' failed (' + logged.length + ' log lines of provoked failures hidden)');
    process.exit(C.fails ? 1 : 0);
})().catch(e => { process.stdout.write(String(e && e.stack || e) + '\n'); process.exit(2); });
