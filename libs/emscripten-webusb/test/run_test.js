'use strict';
// Runs test_webusb.c (compiled with webusb_io.js and library_webusb.js) under Node against a mock navigator.usb that behaves like the
// browser's API: a device that is permitted, picked in a chooser, opened, configured, claimed, transferred to and unplugged.
// Usage: node run_test.js <path of the built test_webusb.js>
const assert = require('assert');
const sleep = (ms) => new Promise(r => setTimeout(r, ms));

class MockUsb {
  constructor(o) {
    Object.assign(this, {
      vendorId: 0x1D50, productId: 0x606F, deviceClass: 0xFF, deviceSubclass: 1, deviceProtocol: 2,
      usbVersionMajor: 2, usbVersionMinor: 0, usbVersionSubminor: 0, deviceVersionMajor: 1, deviceVersionMinor: 2, deviceVersionSubminor: 3,
      manufacturerName: 'ACME', productName: 'CAN Adapter', serialNumber: 'SN-1'
    }, o);
    this.opened = false; this.configuration = null; this.selected = null; this.claimed = []; this.released = 0;
    this.ctrl = []; this.outs = []; this.rxWaiters = []; this.ctrlDelay = () => 0; this.stallRequest = -1; this.failOpen = false; this.outBlock = false;
  }
  async open() { if (this.failOpen) { const e = new Error('Access denied.'); e.name = 'SecurityError'; throw e; } this.opened = true; }
  async close() { this.opened = false; }
  async selectConfiguration(v) {
    this.selected = v;
    this.configuration = { configurationValue: v, interfaces: [{ interfaceNumber: 0, alternate: { endpoints: [
      { endpointNumber: 1, direction: 'in', type: 'bulk', packetSize: 64 }, { endpointNumber: 2, direction: 'out', type: 'bulk', packetSize: 64 },
      { endpointNumber: 3, direction: 'in', type: 'interrupt', packetSize: 8 }] } }] };
  }
  async claimInterface(n) { this.claimed.push(n); }
  async releaseInterface() { this.released++; this.rxWaiters = []; }
  async clearHalt() {}
  transferIn(ep, len) { return new Promise((resolve, reject) => this.rxWaiters.push({ ep, len, resolve, reject })); }
  async transferOut(ep, data) {
    this.outs.push({ ep, bytes: Array.from(new Uint8Array(data.buffer ? data.buffer : data, data.byteOffset || 0, data.byteLength)) });
    if (this.outBlock) await new Promise(() => {});
    return { status: 'ok' };
  }
  async controlTransferOut(setup, data) {
    this.ctrl.push({ dir: 'out', setup, bytes: Array.from(new Uint8Array(data.buffer || data, data.byteOffset || 0, data.byteLength)) });
    await sleep(this.ctrlDelay(setup));
    return { status: setup.request === this.stallRequest ? 'stall' : 'ok' };
  }
  async controlTransferIn(setup, len) {
    this.ctrl.push({ dir: 'in', setup, len });
    await sleep(this.ctrlDelay(setup));
    if (setup.request === this.stallRequest) return { status: 'stall' };
    const bytes = new Uint8Array(len).map((_, i) => (setup.request + i) & 255);
    return { status: 'ok', data: new DataView(bytes.buffer) };
  }
  feed(bytes) { const w = this.rxWaiters.shift(); w.resolve({ status: 'ok', data: new DataView(new Uint8Array(bytes).buffer) }); }
}

const dev1 = new MockUsb();
const dev2 = new MockUsb({ vendorId: 0x0C72, productId: 0x0012, serialNumber: 'SN-2', productName: 'Other' });
let nextPick = null, requested = null, onDisconnect = null;
const usb = {
  getDevices: async () => [dev1],
  requestDevice: async (opts) => { requested = opts; if (!nextPick) { const e = new Error('No device selected.'); e.name = 'NotFoundError'; throw e; } return nextPick; },
  addEventListener: (type, fn) => { if (type === 'disconnect') onDisconnect = fn; }
};
// Newer Node versions have a read-only global navigator: add the property to it, else define a global
if (typeof navigator === 'undefined') global.navigator = { usb };
else Object.defineProperty(navigator, 'usb', { value: usb, configurable: true });

(async () => {
  const create = require(require('path').resolve(process.argv[2]));
  const m = await create();
  const EV = { ADDED: 1, OPENED: 2, CLOSED: 3, ERROR: 4, CANCELLED: 5, SCAN: 6 };
  const ST = { EMPTY: 0, CLOSED: 1, OPENING: 2, OPEN: 3, ERROR: 4 };
  const ev = () => { const a = []; for (let i = 0; i < m._t_event_count(); i++) a.push([m._t_event(i), m._t_event_index(i)]); return a; };
  const put = (arr) => m.HEAPU8.set(arr, m._t_buf());
  const text = (n) => m.UTF8ToString(m._t_buf(), n);
  const xfers = () => { const a = []; for (let i = 0; i < m._t_xfer_count(); i++) a.push({ index: m._t_xfer_field(i, 0), id: m._t_xfer_field(i, 1), status: m._t_xfer_field(i, 2), length: m._t_xfer_field(i, 3), user: m._t_xfer_field(i, 4), data: [0, 1, 2, 3, 4, 5, 6, 7].slice(0, Math.min(8, m._t_xfer_field(i, 3))).map(k => m._t_xfer_byte(i, k)) }); return a; };

  assert.equal(m._t_supported(), 1, 'WebUSB should be supported (webusb_io.js linked in)');
  m._t_start();

  // scan: the permitted device becomes known; descriptor information is available before the open
  m._t_scan(); await sleep(20);
  assert.deepEqual(ev(), [[EV.ADDED, 0], [EV.SCAN, 1]]);
  assert.equal(m._t_state(0), ST.CLOSED); assert.equal(m._t_state(1), ST.EMPTY);
  assert.deepEqual([0, 1, 2, 3, 4, 5, 6].map(f => m._t_info(0, f)), [0x1D50, 0x606F, 0xFF, 1, 2, 0x0200, 0x0123]);
  assert.equal(text(m._t_string(0, 0)), 'ACME'); assert.equal(text(m._t_string(0, 1)), 'CAN Adapter'); assert.equal(text(m._t_string(0, 2)), 'SN-1');
  assert.equal(m._t_info(5, 0), -1); assert.equal(m._t_endpoint_count(0), 0);
  m._t_scan(); await sleep(20); assert.equal(m._t_state(1), ST.EMPTY);                          // a known device is not added twice
  console.log('ok scan: permitted device known, ids / class / BCD versions / strings available');

  // chooser: filters reach the browser, a new device gets the next index, cancel is reported, the same device keeps its index
  m._t_clear(); nextPick = dev2; m._t_request(1, 0x0C72, 0); await sleep(20);
  assert.deepEqual(requested, { filters: [{ vendorId: 0x0C72 }] }); assert.deepEqual(ev(), [[EV.ADDED, 1]]);
  nextPick = null; m._t_clear(); m._t_request(0, 0, 0); await sleep(20);
  assert.deepEqual(requested, { filters: [] }); assert.deepEqual(ev(), [[EV.CANCELLED, -1]]);
  nextPick = dev2; m._t_clear(); m._t_request(1, 0x0C72, 0x0012); await sleep(20);
  assert.deepEqual(requested, { filters: [{ vendorId: 0x0C72, productId: 0x0012 }] }); assert.deepEqual(ev(), [[EV.ADDED, 1]]);
  console.log('ok chooser: filters passed on, new device indexed, cancel reported, a known device keeps its slot');

  // open: configuration selected, interface claimed, endpoints readable
  m._t_clear();
  assert.equal(m._t_open(0, 1, 0), 0); assert.equal(m._t_open(5, 1, 0), -1); assert.equal(m._t_state(0), ST.OPENING);
  await sleep(30);
  assert.equal(m._t_state(0), ST.OPEN); assert.deepEqual(ev(), [[EV.OPENED, 0]]);
  assert.equal(dev1.selected, 1); assert.deepEqual(dev1.claimed, [0]); assert.ok(dev1.opened);
  assert.equal(m._t_endpoint_count(0), 3);
  assert.deepEqual([0, 1, 2, 3].map(f => m._t_endpoint(0, 0, f)), [1, 1, 0, 64]); assert.deepEqual([0, 1, 2, 3].map(f => m._t_endpoint(0, 1, f)), [2, 0, 0, 64]);
  assert.deepEqual([0, 1, 2, 3].map(f => m._t_endpoint(0, 2, f)), [3, 1, 1, 8]); assert.equal(m._t_endpoint(0, 3, 0), -1);
  assert.equal(dev1.rxWaiters.length, 0);                                                         // nothing is received before webusb_start_receive
  m._t_clear(); m._t_open(0, 1, 0); await sleep(10); assert.deepEqual(ev(), []);                   // opening an open device does nothing
  console.log('ok open: configuration 1 selected, interface 0 claimed, endpoints listed, no receive before it is asked for');

  // failed open
  dev2.failOpen = true; m._t_clear(); m._t_open(1, 1, 0); await sleep(30);
  assert.equal(m._t_state(1), ST.ERROR); assert.deepEqual(ev(), [[EV.ERROR, 1]]);
  const n = m._t_last_error(1); assert.equal(text(n), 'Access denied.'); assert.equal(dev2.opened, false); dev2.failOpen = false;
  console.log('ok failed open: ERROR event with the browser text');

  // control transfers: setup packets, copied data, strict order whatever each takes, status, data of an IN, user pointer
  m._t_clear();
  put([0xAA, 0xBB, 0xCC]);
  dev1.ctrlDelay = (s) => s.request === 1 ? 30 : s.request === 2 ? 10 : 1;
  const id1 = m._t_control_out(0, 2, 1, 1, 0x1234, 7, 3, 1001);
  m.HEAPU8.fill(0, m._t_buf(), m._t_buf() + 3);                                                     // the call copied the data already
  const id2 = m._t_control_in(0, 2, 1, 2, 0, 0, 4, 1002);
  const id3 = m._t_control_out(0, 0, 0, 3, 0, 0, 0, 1003);
  assert.ok(id1 > 0 && id2 > id1 && id3 > id2);
  await sleep(120);
  assert.deepEqual(dev1.ctrl.map(c => c.setup.request), [1, 2, 3]);
  assert.deepEqual(dev1.ctrl[0].setup, { requestType: 'vendor', recipient: 'interface', request: 1, value: 0x1234, index: 7 });
  assert.deepEqual(dev1.ctrl[0].bytes, [0xAA, 0xBB, 0xCC]);
  assert.deepEqual(dev1.ctrl[2].setup.requestType, 'standard'); assert.deepEqual(dev1.ctrl[2].setup.recipient, 'device');
  assert.deepEqual(xfers(), [
    { index: 0, id: id1, status: 0, length: 0, user: 1001, data: [] },
    { index: 0, id: id2, status: 0, length: 4, user: 1002, data: [2, 3, 4, 5] },
    { index: 0, id: id3, status: 0, length: 0, user: 1003, data: [] }]);
  dev1.ctrlDelay = () => 0; dev1.stallRequest = 9; m._t_clear();
  m._t_control_out(0, 2, 1, 9, 0, 0, 0, 1); m._t_control_in(0, 2, 1, 9, 0, 0, 4, 2); await sleep(30);
  assert.deepEqual(xfers().map(x => [x.status, x.length]), [[-1, 0], [-1, 0]]); dev1.stallRequest = -1;
  assert.equal(m._t_control_out_nocb(0, 5, 1) > 0, true); await sleep(20);                       // no callback is fine
  assert.equal(m._t_control_out(1, 2, 1, 1, 0, 0, 0, 0), -1);                                     // a device that is not open
  console.log('ok control: setup packets, data copied at the call, strict order, status -1 on a stall, IN data in the callback');

  // bulk OUT: ordered, copied, completion callbacks; a full queue refuses
  m._t_clear(); dev1.outs.length = 0;
  put([1, 2, 3, 4]); const b1 = m._t_bulk_out(0, 2, 4, 2001); put([9]); const b2 = m._t_bulk_out(0, 2, 1, 2002); await sleep(20);
  assert.deepEqual(dev1.outs, [{ ep: 2, bytes: [1, 2, 3, 4] }, { ep: 2, bytes: [9] }]);
  assert.deepEqual(xfers().map(x => [x.id, x.status, x.user]), [[b1, 0, 2001], [b2, 0, 2002]]);
  dev1.outBlock = true; m._t_clear(); put([7]);
  let refused = 0, accepted = 0; for (let i = 0; i < 4200; i++) { if (m._t_bulk_out(0, 2, 1, 0) < 0) refused++; else accepted++; }
  assert.ok(refused > 0 && accepted >= 4096 && accepted <= 4110, 'queue: accepted ' + accepted + ', refused ' + refused); assert.equal(m._t_stat(0, 3), refused);
  console.log('ok bulk out: data copied, order kept, callbacks with ids and user pointers, a full queue refuses (' + refused + ' of 4200)');

  // the blocked queue is abandoned with the session at the end of the test; use a fresh session for the receive tests
  m._t_close(0); await sleep(30);
  assert.equal(m._t_state(0), ST.CLOSED);
  dev1.outBlock = false; dev1.outs.length = 0;
  m._t_clear(); m._t_open(0, 0, 0); await sleep(30); assert.equal(m._t_state(0), ST.OPEN); assert.equal(dev1.selected, 1);

  // receive: slots pending on the chosen endpoint, data delivered in order with a time, big transfers, stop
  m._t_clear();
  assert.equal(m._t_start_receive(0, 1, 2048, 3001), 0); assert.equal(m._t_start_receive(1, 1, 2048, 0), -1);
  assert.equal(dev1.rxWaiters.length, 6); assert.ok(dev1.rxWaiters.every(w => w.ep === 1 && w.len === 2048));
  dev1.feed([10, 11, 12]); dev1.feed(Array.from({ length: 1500 }, (_, i) => i & 255)); dev1.feed([]); dev1.feed(Array.from({ length: 20000 }, () => 5)); await sleep(20);
  assert.equal(m._t_rx_count(), 3, 'empty transfers are not delivered');
  assert.deepEqual([m._t_rx_field(0, 0), m._t_rx_field(0, 1), m._t_rx_field(0, 2), m._t_rx_field(0, 3)], [0, 1, 3, 3001]);
  assert.deepEqual([0, 1, 2].map(k => m._t_rx_byte(0, k)), [10, 11, 12]);
  assert.equal(m._t_rx_field(1, 2), 1500); assert.equal(m._t_rx_field(1, 4), (1499) & 255); assert.equal(m._t_rx_byte(1, 5), 5);
  assert.equal(m._t_rx_field(2, 2), 20000); assert.equal(m._t_rx_field(2, 4), 5);
  assert.ok(m._t_rx_time(0) > 0 && m._t_rx_time(1) >= m._t_rx_time(0)); assert.equal(dev1.rxWaiters.length, 6);
  assert.equal(m._t_stat(0, 0), 3);
  m._t_stop_receive(0); m._t_clear(); dev1.feed([1]); await sleep(10);
  assert.equal(m._t_rx_count(), 0); assert.equal(dev1.rxWaiters.length, 5);                        // dropped, no slot re-armed
  assert.equal(m._t_start_receive(0, 3, 64, 3002), 0); await sleep(10);
  dev1.rxWaiters.splice(0, dev1.rxWaiters.length - 6); dev1.feed([77]); await sleep(10);
  assert.equal(m._t_rx_count(), 1); assert.equal(m._t_rx_field(0, 1), 3); assert.equal(m._t_rx_field(0, 3), 3002);
  console.log('ok receive: slots on the endpoint / size asked for, in-order delivery with times, 20 kB transfer, stop drops what was pending');

  // the device is unplugged while open
  m._t_clear(); onDisconnect({ device: dev1 }); await sleep(30);
  assert.equal(m._t_state(0), ST.ERROR); assert.deepEqual(ev(), [[EV.ERROR, 0]]); assert.equal(text(m._t_last_error(0)), 'device disconnected');
  assert.equal(m._t_control_in(0, 2, 1, 1, 0, 0, 1, 0), -1); assert.equal(m._t_bulk_out(0, 2, 1, 0), -1);
  m._t_clear(); m._t_open(0, 1, 0); await sleep(30); assert.equal(m._t_state(0), ST.OPEN); assert.deepEqual(ev(), [[EV.OPENED, 0]]);
  console.log('ok unplug: ERROR event with the reason, transfers refused, reopening works');

  // close, close while opening, remove
  m._t_clear(); m._t_close(0); await sleep(30);
  assert.equal(m._t_state(0), ST.CLOSED); assert.deepEqual(ev(), [[EV.CLOSED, 0]]); assert.equal(dev1.opened, false);
  const origOpen = dev1.open.bind(dev1); dev1.open = async function () { await sleep(40); return origOpen(); };
  m._t_clear(); m._t_open(0, 1, 0); m._t_close(0); await sleep(120);
  assert.equal(m._t_state(0), ST.CLOSED); assert.equal(dev1.opened, false); assert.deepEqual(ev(), []); dev1.open = origOpen;
  m._t_remove(1); assert.equal(m._t_state(1), ST.EMPTY); m._t_remove(0); assert.equal(m._t_state(0), ST.EMPTY);
  console.log('ok close / close while opening / remove');
  console.log('ALL OK');
  process.exit(0);
})().catch(e => { console.error('FAIL', e); process.exit(1); });
