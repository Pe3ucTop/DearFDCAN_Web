# webserial-io

The Web Serial transport for adapters on a serial port, as one JavaScript file (`webserial_io.js`) with no build step. It does not know a CAN
protocol, WebAssembly or `postMessage`; it runs in a window or in a dedicated worker. One global, `WebSerialIo`, is defined.

```js
const { SerialConnection, findPermittedPort } = WebSerialIo;

const conn = new SerialConnection('my port', { defaultBaud: 115200 }, {
    resolvePort: () => findPermittedPort({ vid: 0x1D50, pid: 0x606F, idx: 0 }),   // or { port } with a SerialPort object
    isPortBusy: (port) => others.some(c => c.port === port && c.active),
    onOpened: (baud, info) => {},          // info = "vid=1D50;pid=606F" for USB serial ports
    onClosed: (reason) => {},              // '' = closed by you
    onData: (buffer, t) => {},             // an exact ArrayBuffer you may keep or transfer; t = timeOrigin + now
    onWriteDone: (id, ok) => {}
});
await conn.open(250000);
conn.write(bytes, 1);                      // in order, from a bounded queue; a non-zero id is reported through onWriteDone
await conn.close();
```

`open()` asserts DTR and RTS (CDC-ACM firmware often waits for DTR), reads until the port is closed or lost, and survives the recoverable
stream errors (framing, parity, buffer overrun) that give the port a new `readable`. A close during the open is carried out when the open ends.
Writes that never ran when the connection ends are answered with `ok: false`. Note: a failed write errors the whole Web Streams sink, so
later writes fail too until the port is opened again; reading is not affected.

`tests/test_webserial_io.js` runs it against a mock `SerialPort` built on Web Streams; `tests/test_serial_worker.js` runs the monitor's
`serial_worker.js` over it. For programs compiled with Emscripten that want a POSIX interface to a serial port there is the separate library
`libs/emscripten-webserial`.
