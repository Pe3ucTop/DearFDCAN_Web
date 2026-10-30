# webusb-io

The WebUSB transport for bulk adapters, as one JavaScript file (`webusb_io.js`) with no build step. It does not know a CAN protocol, WebAssembly
or `postMessage`; it runs in a window or in a worker. One global, `WebUsbIo`, is defined (load it with `importScripts('webusb_io.js')` or a
`<script>`).

```js
const { UsbPort } = WebUsbIo;

class MyAdapter extends UsbPort {
    onRxData(dv) { /* one completed transfer, a DataView, in completion order */ }
    onLost(reason) { /* unplugged, recovery failed */ }
    probeRequest() { return [{ requestType: 'vendor', recipient: 'interface', request: 4, value: 0, index: 0 }, 40]; }
}

const port = new MyAdapter('my adapter', { iface: 0, rxSlots: 6, inUse: () => otherPorts.map(p => p.usb) });
const hs = await port.openDevice({ vid: 0x1D50, pid: 0x606F, serial: 'ABC' },
    async (usb) => ({ layout: { rxEp: 1, txEp: 2, cmdEp: 0, rxSize: 2048 } }));   // the handshake reads the descriptor / talks to the adapter
port.enqueue(async () => { const r = await port.controlTransfer(false, setup, 64); /* { ok, data } */ });
port.enqueueTx(frameBytes);
setInterval(() => port.watchdog(), 1000);
```

What `UsbPort` does for you:

* **Open**: finds the *permitted* device (`navigator.usb.getDevices()`; the permission comes from `requestDevice()` in a window) by vendor id,
  product id and serial number that no other port holds, opens it, selects the configuration, claims the interface, runs your handshake, starts
  receiving. Ports that are opened at the same moment do not take the same one of two identical adapters (an open lock).
* **Receive**: `rxSlots` `transferIn` requests are always pending, each re-armed from its own completion. A pending request cannot be cancelled in
  WebUSB, so every request belongs to a generation and completions of an older generation are ignored. Stalls are cleared, errors are retried
  with a growing pause, and after `rxErrorsBeforeRecover` errors of a slot the interface is released and claimed again (`recover`).
* **Control and command transfers** (`controlTransfer`, `commandTransfer`): each with a timeout, never rejecting; call them from `enqueue()` to
  keep their order.
* **Transmit** (`enqueueTx`): a bounded queue with at most `txInflight` transfers in flight; returns `false` when the frame was dropped.
* **Watchdog** (`watchdog()` about once a second): an idle port is probed with the control read of `probeRequest()`; two failures in a row
  trigger a recovery. A silent bus is not a failure, a dead control pipe is.
* **Loss**: an unplugged adapter ends the session at once (`onLost`).

Also available: `openDevice({ device })` takes a `USBDevice` the caller already holds (e.g. from `requestDevice()`) instead of matching one, and
may select a configuration; with the option `autoReceive: false` nothing is received until `beginRx(endpoint, size)` (and `stopRx()` ends it);
`enqueueTx(data, ep, done)` calls `done(ok)` when the transfer finished or will never run. `libs/emscripten-webusb` builds a C API for
Emscripten programs on these.

The hooks, the options and the details are described at the top of `webusb_io.js`. `tests/test_webusb_io.js` runs it against a mock
`USBDevice`.
