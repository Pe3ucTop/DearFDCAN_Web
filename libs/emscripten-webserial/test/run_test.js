'use strict';
// Runs test_webserial.c (compiled with library_webserial.js) under Node against a mock navigator.serial that
// behaves like the browser's API: Web Streams for the data, a SerialPort that can be opened, closed and lost.
// Usage: node run_test.js <path of the built test_webserial.js>
const assert = require('assert');
const sleep = (ms) => new Promise(r => setTimeout(r, ms));

class MockPort {
  constructor(info) { this.info = info || { usbVendorId: 0x1209, usbProductId: 0x2323 }; this.opened = false; this.written = []; this.openCount = 0; this.writeDelay = 0; }
  getInfo() { return this.info; }
  async open(opts) {
    if (this.failOpen) { const e = new Error('Failed to open serial port.'); e.name = 'NetworkError'; throw e; }
    this.opts = opts; this.opened = true; this.openCount++;
    this.readable = new ReadableStream({ start: (c) => { this.ctrl = c; } });
    this.writable = new WritableStream({ write: async (chunk) => { if (this.writeDelay) await sleep(this.writeDelay); this.written.push(Buffer.from(chunk)); } });
  }
  async setSignals(s) { this.signals = s; }
  async close() { this.opened = false; this.readable = null; this.writable = null; }
  feed(bytes) { this.ctrl.enqueue(new Uint8Array(bytes)); }
}

const ports = [new MockPort()];
let nextPick = null;
const serial = {
  getPorts: async () => ports.slice(),
  requestPort: async () => { if (!nextPick) { const e = new Error('No port selected by the user.'); e.name = 'NotFoundError'; throw e; } return nextPick; },
  addEventListener() {}
};
// Newer Node versions have a read-only global navigator: add the property to it, else define a global
if (typeof navigator === 'undefined') global.navigator = { serial };
else Object.defineProperty(navigator, 'serial', { value: serial, configurable: true });

(async () => {
  const create = require(require('path').resolve(process.argv[2]));
  const m = await create();
  const ev = () => { const a = []; for (let i = 0; i < m._t_event_count(); i++) a.push([m._t_event(i), m._t_event_index(i)]); return a; };
  const EV = { ADDED: 1, OPENED: 2, CLOSED: 3, ERROR: 4, CANCELLED: 5, DATA: 6, SCAN: 7 };
  const ST = { EMPTY: 0, CLOSED: 1, OPENING: 2, OPEN: 3, ERROR: 4 };
  const EAGAIN = 6, EIO = 29, POLLIN = 1, POLLOUT = 4, POLLHUP = 0x10, S_IFCHR = 0x2000;   // Emscripten errno numbers (WASI) and poll flags
  const bytes = (n, f) => Array.from({ length: n }, (_, i) => f(i) & 255);
  const put = (arr) => m.HEAPU8.set(arr, m._t_buf());
  const get = (n) => Array.from(m.HEAPU8.subarray(m._t_buf(), m._t_buf() + n));
  const P = ports[0];

  assert.equal(m.ccall('webserial_supported', 'number', [], []), 1);
  m._t_start();

  // scan: the permitted port gets a device node
  m._webserial_scan(); await sleep(20);
  assert.deepEqual(ev(), [[EV.ADDED, 0], [EV.SCAN, 1]]);
  assert.equal(m._webserial_state(0), ST.CLOSED); assert.equal(m._webserial_state(1), ST.EMPTY);
  const st = m.FS.stat('/dev/ttyWeb0'); assert.ok((st.mode & 0xF000) === S_IFCHR, 'not a character device');
  m._webserial_scan(); await sleep(20); assert.equal(m._webserial_state(1), ST.EMPTY);       // a known port is not added twice
  const vp = m._malloc(8); assert.equal(m._webserial_usb_ids(0, vp, vp + 4), 0); assert.equal(m.HEAP32[vp >> 2], 0x1209); assert.equal(m.HEAP32[(vp + 4) >> 2], 0x2323); m._free(vp);
  console.log('ok scan: permitted port registered, /dev/ttyWeb0 is a character device, USB ids available');

  // before open the device reads as end of file and refuses writes
  m._t_clear_events();
  let fd = m._t_open(0); assert.ok(fd >= 3);
  assert.equal(m._t_read(fd, 10), 0); put([1]); assert.equal(m._t_write(fd, 1), -EIO);          // EOF / EIO
  assert.ok((m._t_poll(fd) & POLLHUP) !== 0);
  m._t_close(fd);

  // open: baud rate, DTR/RTS, events
  assert.equal(m._webserial_open(0, 115200), 0); assert.equal(m._webserial_open(5, 9600), -1);
  assert.equal(m._webserial_state(0), ST.OPENING); await sleep(30);
  assert.equal(m._webserial_state(0), ST.OPEN); assert.deepEqual(ev(), [[EV.OPENED, 0]]);
  assert.equal(P.opts.baudRate, 115200); assert.ok(P.opts.bufferSize >= 4096); assert.deepEqual(P.signals, { dataTerminalReady: true, requestToSend: true });
  fd = m._t_open(0);
  assert.equal(m._t_read(fd, 10), -EAGAIN);                                                   // open but silent
  assert.equal(m._t_poll(fd) & (POLLIN | POLLHUP), 0); assert.ok((m._t_poll(fd) & POLLOUT) !== 0);
  assert.equal(m._t_isatty(fd), 1);
  console.log('ok open: 115200 baud with DTR/RTS, read answers EAGAIN, poll shows only POLLOUT');

  // receive: bytes in several chunks, one DATA event per empty -> non-empty change, partial reads keep the order
  m._t_clear_events();
  P.feed([1, 2, 3]); P.feed([4, 5]); P.feed(bytes(1000, i => i + 6)); await sleep(30);
  assert.deepEqual(ev(), [[EV.DATA, 0]]); assert.equal(m._webserial_available(0), 1005); assert.ok((m._t_poll(fd) & POLLIN) !== 0);
  assert.equal(m._t_read(fd, 4), 4); assert.deepEqual(get(4), [1, 2, 3, 4]);
  assert.equal(m._t_read(fd, 5000), 1001); assert.deepEqual(get(3), [5, 6, 7]); assert.equal(get(1001)[1000], (1000 - 1 + 6) & 255);
  assert.equal(m._t_read(fd, 10), -EAGAIN); m._t_clear_events(); P.feed([9]); await sleep(20); assert.deepEqual(ev(), [[EV.DATA, 0]]);
  assert.equal(m._t_read(fd, 10), 1);
  console.log('ok receive: chunks merged in order, partial reads, a new DATA event after the buffer was emptied');

  // transmit: data is copied at write() and sent in order
  put([72, 105, 13]); assert.equal(m._t_write(fd, 3), 3); put([33]); assert.equal(m._t_write(fd, 1), 1); await sleep(20);
  assert.equal(Buffer.concat(P.written).toString('latin1'), 'Hi\r!');
  P.written.length = 0; P.writeDelay = 60;
  const big = new Uint8Array(700000).fill(7); m.HEAPU8.set(big, m._t_buf()); assert.equal(m._t_write(fd, 700000), 700000);
  assert.equal(m._t_write(fd, 700000), (1 << 20) - 700000);                                  // partial write: what fits in the 1 MiB queue
  assert.equal(m._t_write(fd, 10), -EAGAIN); assert.equal(m._t_poll(fd) & POLLOUT, 0);        // full: EAGAIN, no POLLOUT
  P.writeDelay = 0; await sleep(400); assert.ok((m._t_poll(fd) & POLLOUT) !== 0); assert.equal(Buffer.concat(P.written).length, 1 << 20);
  console.log('ok transmit: bytes copied and ordered; a full queue gives a partial write, then EAGAIN without POLLOUT');

  // receive flow control: the loop stops reading at the cap, the rest waits in the stream
  P.feed(new Uint8Array(600000).fill(1)); P.feed(new Uint8Array(600000).fill(2)); P.feed(new Uint8Array(600000).fill(3)); await sleep(60);
  assert.ok(m._webserial_available(0) >= (1 << 20) && m._webserial_available(0) < 1800000, 'cap not honoured: ' + m._webserial_available(0));
  let total = 0, sum = [0, 0, 0, 0];
  while (total < 1800000) { const n = m._t_read(fd, 1 << 20); if (n === -EAGAIN) { await sleep(20); continue; } assert.ok(n > 0); for (let i = 0; i < n; i += 50000) sum[m.HEAPU8[m._t_buf() + i]]++; total += n; }
  assert.equal(total, 1800000); assert.ok(sum[1] > 0 && sum[2] > 0 && sum[3] > 0);
  console.log('ok flow control: reading paused at the 1 MiB cap and resumed by read(); all 1.8 MB arrived');

  // termios: baud rate visible and settable; a change reopens the port
  assert.equal(m._t_get_baud(fd), 115200); m._t_clear_events();
  assert.equal(m._t_set_baud(fd, 921600), 0); await sleep(80);
  assert.equal(P.opts.baudRate, 921600); assert.equal(m._webserial_state(0), ST.OPEN); assert.equal(m._t_get_baud(fd), 921600); assert.ok(P.openCount >= 2);
  assert.ok(ev().some(e => e[0] === EV.OPENED));
  console.log('ok termios: tcgetattr shows the baud rate, tcsetattr with another one reopens the port');

  // the device goes away: error event, EOF + POLLHUP, then reopen works
  m._t_clear_events(); P.ctrl.error(Object.assign(new Error('The device has been lost.'), { name: 'NetworkError' })); await sleep(40);
  assert.equal(m._webserial_state(0), ST.ERROR); assert.deepEqual(ev(), [[EV.ERROR, 0]]);
  const eb = m._malloc(128); const el = m._webserial_last_error(0, eb, 128); assert.equal(m.UTF8ToString(eb, el), 'The device has been lost.'); m._free(eb);
  assert.equal(m._t_read(fd, 10), 0); assert.ok((m._t_poll(fd) & (POLLHUP | POLLIN)) === (POLLHUP | POLLIN));
  m._t_clear_events(); m._webserial_open(0, 115200); await sleep(40); assert.equal(m._webserial_state(0), ST.OPEN); assert.deepEqual(ev(), [[EV.OPENED, 0]]);
  P.feed([42]); await sleep(20); assert.equal(m._t_read(fd, 1), 1); assert.equal(get(1)[0], 42);
  console.log('ok loss and reopen: ERROR event with the browser text, EOF/POLLHUP, reopening gives a working stream');

  // close
  m._t_clear_events(); m._webserial_close(0); await sleep(30);
  assert.equal(m._webserial_state(0), ST.CLOSED); assert.deepEqual(ev(), [[EV.CLOSED, 0]]); assert.equal(P.opened, false); assert.equal(m._t_read(fd, 1), 0);
  m._t_close(fd);
  // open failure
  P.failOpen = true; m._t_clear_events(); m._webserial_open(0, 9600); await sleep(30); assert.equal(m._webserial_state(0), ST.ERROR); assert.deepEqual(ev(), [[EV.ERROR, 0]]); P.failOpen = false;
  // closing while it is still opening leaves the port closed
  P.open = (function (orig) { return async function (o) { await sleep(40); return orig.call(this, o); }; })(MockPort.prototype.open);
  m._webserial_open(0, 9600); m._webserial_close(0); await sleep(120); assert.equal(m._webserial_state(0), ST.CLOSED); assert.equal(P.opened, false);
  delete P.open;
  console.log('ok close / failed open / close while opening');

  // chooser
  const P2 = new MockPort({ usbVendorId: 0x16d0, usbProductId: 0x117e }); nextPick = P2; ports.push(P2);
  m._t_clear_events(); m._webserial_request_port(); await sleep(20); assert.deepEqual(ev(), [[EV.ADDED, 1]]); assert.ok(m.FS.analyzePath('/dev/ttyWeb1').exists);
  nextPick = null; m._t_clear_events(); m._webserial_request_port(); await sleep(20); assert.deepEqual(ev(), [[EV.CANCELLED, -1]]);
  nextPick = P2; m._t_clear_events(); m._webserial_request_port(); await sleep(20); assert.deepEqual(ev(), [[EV.ADDED, 1]]);          // same port again: same slot
  m._webserial_remove(1); assert.equal(m._webserial_state(1), ST.EMPTY); assert.ok(!m.FS.analyzePath('/dev/ttyWeb1').exists);
  console.log('ok chooser: new port added, cancel reported, same port keeps its slot, remove deletes the node');
  console.log('ALL OK');
  process.exit(0);
})().catch(e => { console.error('FAIL', e); process.exit(1); });
