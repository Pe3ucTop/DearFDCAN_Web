'use strict';
// Tests of libs/webserial-io (SerialConnection, findPermittedPort) against a mock SerialPort built on Web Streams: opening (signals, baud
// rate, timeout, busy port), reading, ordered writes with acknowledgements, close during open, close and loss, recoverable stream errors.
// Usage: node tests/test_webserial_io.js [path of webserial_io.js]
const fs = require('fs');
const path = require('path');
const { sleep, createChecks } = require('./worker_harness');
const C = createChecks();
const check = C.check;

const src = fs.readFileSync(process.argv[2] || path.join(__dirname, '..', 'libs', 'webserial-io', 'webserial_io.js'), 'utf8');
const WebSerialIo = new Function(src + '\n;return WebSerialIo;')();
const { SerialConnection, findPermittedPort } = WebSerialIo;

// The warnings the library logs for the failures the tests provoke are not shown
const logged = [];
console.warn = console.error = (...args) => logged.push(args.join(' '));

// A SerialPort: `push(bytes)` delivers a chunk, `fail(err)` errors the read stream, `renewReadable()` gives the port a fresh stream
function mockPort(info) {
    const p = {
        info: info || { usbVendorId: 0x1D50, usbProductId: 0x606F },
        openArgs: null, signals: null, closed: 0, openDelay: 0, openFail: null, written: [], writeDelay: 0, writeFail: null, ctl: null,
        getInfo() { return this.info; },
        async open(args) {
            this.openArgs = args;
            if (this.openDelay === Infinity) await new Promise(() => {});
            if (this.openDelay) await sleep(this.openDelay);
            if (this.openFail) throw this.openFail;
            this.renewReadable();
            this.writable = new WritableStream({
                write: async (chunk) => {
                    if (this.writeDelay) await sleep(this.writeDelay);
                    if (this.writeFail) throw this.writeFail;
                    this.written.push(Array.from(chunk));
                }
            });
        },
        async setSignals(s) { this.signals = s; },
        async close() { this.closed++; },
        renewReadable() { this.readable = new ReadableStream({ start: (c) => { this.ctl = c; } }); },
        push(bytes) { this.ctl.enqueue(new Uint8Array(bytes)); },
        fail(err) { this.ctl.error(err); },
    };
    return p;
}

// A connection that records what it reports
function makeConn(port, options, extraHooks) {
    const rec = { opened: null, closed: [], data: [], done: [] };
    const hooks = Object.assign({
        resolvePort: async () => port,
        onOpened: (baud, info) => { rec.opened = { baud, info }; },
        onClosed: (reason) => rec.closed.push(reason),
        onData: (buf, t) => rec.data.push({ bytes: Array.from(new Uint8Array(buf)), isBuffer: buf instanceof ArrayBuffer, t }),
        onWriteDone: (id, ok) => rec.done.push([id, ok]),
    }, extraHooks);
    const conn = new SerialConnection('test', Object.assign({ tag: 'test' }, options), hooks);
    return { conn, rec };
}

async function testFindPort() {
    const a = mockPort({ usbVendorId: 1, usbProductId: 2 }), b = mockPort({ usbVendorId: 1, usbProductId: 2 }), c = mockPort({});
    Object.defineProperty(globalThis, 'navigator', { value: { serial: { getPorts: async () => [a, c, b] } }, configurable: true, writable: true });
    check((await findPermittedPort({ port: a })) === a, 'a transferred port object is used as it is');
    check((await findPermittedPort({ vid: 1, pid: 2, idx: 1 })) === b, 'the idx-th permitted port with these USB ids');
    check((await findPermittedPort({ vid: 1, pid: 2, idx: 5 })) === a, 'an idx beyond the list falls back to the first');
    check((await findPermittedPort({ vid: 0, pid: 0, idx: 0 })) === c, 'a port without USB ids is matched by 0:0');
    let err = null;
    try { await findPermittedPort({ vid: 9, pid: 9, idx: 0 }); } catch (e) { err = e; }
    check(err && /not among the permitted/.test(err.message), 'no such port');
    err = null;
    try { await findPermittedPort(null); } catch (e) { err = e; }
    check(err && /no serial port was chosen/.test(err.message), 'nothing picked');
}

async function testOpenAndRead() {
    const port = mockPort();
    const { conn, rec } = makeConn(port);
    await conn.open(250000);
    check(rec.opened && rec.opened.baud === 250000 && rec.opened.info === 'vid=1D50;pid=606F', 'opened with the baud rate and the USB ids ' + JSON.stringify(rec.opened));
    check(port.openArgs.baudRate === 250000 && port.openArgs.bufferSize === 65536 && port.signals && port.signals.dataTerminalReady && port.signals.requestToSend, 'baud rate, buffer size, DTR and RTS asserted');
    check(conn.active && !conn.opening, 'active');

    port.push([1, 2, 3]); port.push([4]);
    await sleep(5);
    check(rec.data.length === 2 && rec.data[0].bytes.join() === '1,2,3' && rec.data[1].bytes.join() === '4' && rec.data[0].isBuffer && typeof rec.data[0].t === 'number' && rec.data[0].t > 1e12, 'chunks arrive in order as exact ArrayBuffers with a time stamp');
    // a view into a bigger buffer is copied to an exact buffer
    const big = new Uint8Array([9, 9, 7, 8, 9, 9]);
    port.ctl.enqueue(big.subarray(2, 4));
    await sleep(5);
    check(rec.data[2].bytes.join() === '7,8' && rec.data[2].isBuffer, 'a view into a larger buffer is delivered as exactly its bytes');
    check(conn.takeStats().rx === 3 && conn.takeStats().rx === 0, 'takeStats gives the reads since the last call');

    // a recoverable stream error: the port brings a new stream and reading goes on
    const old = port.ctl;
    port.renewReadable();
    old.error(new Error('framing error'));
    await sleep(10);
    port.push([5, 5]);
    await sleep(5);
    check(rec.data.length === 4 && rec.data[3].bytes.join() === '5,5' && conn.st.rxErrors === 1 && conn.active && rec.closed.length === 0, 'reading continues on the new stream after a recoverable error');

    // a fatal error ends the connection and closes the port
    port.fail(Object.assign(new Error('unplugged'), { name: 'NetworkError' }));
    await sleep(10);
    check(rec.closed.join() === 'unplugged' && !conn.active && port.closed === 1, 'a NetworkError ends the connection: onClosed with the reason, port closed');
}

async function testOpenFailures() {
    // default baud rate
    let port = mockPort();
    let { conn, rec } = makeConn(port);
    await conn.open(0);
    check(rec.opened.baud === 115200 && port.openArgs.baudRate === 115200, 'the default baud rate');
    await conn.close('closed by host');
    check(rec.closed.join() === 'closed by host' && port.closed === 1 && !conn.active, 'close at the owner\'s request');

    // the open itself fails / times out
    port = mockPort(); port.openFail = new Error('busy'); ({ conn, rec } = makeConn(port));
    await conn.open(115200);
    check(rec.closed.join() === 'open failed: busy' && !rec.opened && !conn.opening, 'a failing open is reported');
    port = mockPort(); port.openDelay = Infinity; ({ conn, rec } = makeConn(port, { openTimeoutMs: 40 }));
    await conn.open(115200);
    check(rec.closed.join() === 'open failed: open timeout', 'a port that never opens times out');

    // no streams after the open
    port = mockPort(); port.open = async function () { this.readable = null; this.writable = null; }; ({ conn, rec } = makeConn(port));
    await conn.open(115200);
    check(/no readable\/writable stream/.test(rec.closed[0]) && port.closed === 1, 'a port without streams is refused and closed');

    // the port is used by another connection
    port = mockPort(); ({ conn, rec } = makeConn(port, {}, { isPortBusy: () => true }));
    await conn.open(115200);
    check(rec.closed.join() === 'open failed: the port is already in use' && port.openArgs === null, 'a busy port is not opened');

    // nothing picked
    ({ conn, rec } = makeConn(port, {}, { resolvePort: () => findPermittedPort(null) }));
    await conn.open(115200);
    check(/open failed: no serial port was chosen/.test(rec.closed[0]), 'no port to open');

    // closing while the port is still opening: the open finishes, then everything is undone, and the owner hears about it once
    port = mockPort(); port.openDelay = 40; ({ conn, rec } = makeConn(port));
    const opening = conn.open(115200);
    await sleep(5);
    await conn.close('closed by host');
    await opening;
    check(rec.closed.join() === '' && rec.closed.length === 1 && !rec.opened && port.closed === 1 && !conn.active, 'a close during the open is carried out when the open ends');

    // closing a connection that is not open
    ({ conn, rec } = makeConn(mockPort()));
    await conn.close('x');
    check(rec.closed.join() === 'x', 'closing a closed connection still answers');
}

async function testWrite() {
    const port = mockPort(); port.writeDelay = 8;
    const { conn, rec } = makeConn(port, { txQueueMax: 3 });
    check((conn.write(new Uint8Array([1]), 5), rec.done.join('|') === '5,false'), 'a write on a closed connection is dropped and reported');
    rec.done.length = 0;
    await conn.open(115200);

    conn.write(new Uint8Array([1]).buffer, 11);
    conn.write(new Uint8Array([2, 2]).buffer, 0);
    conn.write(new Uint8Array([3]), 12);
    conn.write(new Uint8Array([4]), 13);        // queue: 3 waiting behind the one being written
    conn.write(new Uint8Array([5]), 14);        // this one is over the limit of 3 waiting
    check(rec.done.join('|') === '14,false' && conn.st.txDropped === 2, 'a full queue drops the write and reports it (' + rec.done.join('|') + ')');
    await sleep(60);
    check(port.written.map(w => w.join('.')).join('|') === '1|2.2|3|4', 'writes reach the port in the order given');
    check(rec.done.map(d => d.join(',')).join('|') === '14,false|11,true|12,true|13,true', 'writes with an id are acknowledged, the one without is not (' + rec.done.join('|') + ')');

    // a failing write: not ok and counted. A Web Streams sink that fails errors the whole stream, so later writes fail as well until
    // the port is opened again (the connection stays up for reading)
    rec.done.length = 0;
    port.writeFail = new Error('nak');
    conn.write(new Uint8Array([6]), 21);
    await sleep(30);
    port.writeFail = null;
    check(rec.done.join('|') === '21,false' && conn.st.txErrors >= 1, 'a failed write is reported and counted');
    conn.write(new Uint8Array([7]), 22);
    await sleep(30);
    check(rec.done.join('|') === '21,false|22,false' && conn.active, 'the write stream stays errored, reading is not affected');

    // closing with writes waiting: the ones with an id are answered with failure
    rec.done.length = 0;
    conn.write(new Uint8Array([8]), 31); conn.write(new Uint8Array([9]), 32); conn.write(new Uint8Array([10]), 33);
    await conn.close('closed by host');
    check(rec.done.some(d => d[0] === 33 && d[1] === false) && rec.closed.at(-1) === 'closed by host' && port.closed === 1, 'writes that never ran are answered with failure on close (' + rec.done.join('|') + ')');
    // a write that is a fatal failure ends the connection
    const p2 = mockPort(); const c2 = makeConn(p2);
    await c2.conn.open(115200);
    p2.writeFail = Object.assign(new Error('gone'), { name: 'NotFoundError' });
    c2.conn.write(new Uint8Array([1]), 41);
    await sleep(30);
    check(c2.rec.closed.join() === 'gone' && c2.rec.done.some(d => d[0] === 41 && d[1] === false) && !c2.conn.active, 'a fatal write error ends the connection');
}

(async () => {
    await testFindPort();
    await testOpenAndRead();
    await testOpenFailures();
    await testWrite();
    console.log(C.n + ' checks, ' + C.fails + ' failed (' + logged.length + ' log lines of provoked failures hidden)');
    process.exit(C.fails ? 1 : 0);
})().catch(e => { process.stdout.write(String(e && e.stack || e) + '\n'); process.exit(2); });
