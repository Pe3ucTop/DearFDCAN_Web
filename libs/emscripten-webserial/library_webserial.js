// library_webserial.js - Web Serial ports as POSIX character devices (/dev/ttyWeb0, /dev/ttyWeb1, ...) for Emscripten.
//
// Link with:  em++ ... --js-library libs/emscripten-webserial/library_webserial.js
// C API and device semantics: webserial.h and README.md next to this file.
//
// What it does
//   * keeps a table of up to 8 SerialPort objects (the user picks them with webserial_request_port(), or they
//     are the already permitted ones, webserial_scan())
//   * registers a device node /dev/ttyWeb<N> in Emscripten's virtual file system for every port, so C code can
//     use open()/read()/write()/poll()/close() and termios (tcgetattr/tcsetattr with cfsetspeed) on it
//   * a read loop moves the bytes of port.readable into a bounded buffer (when it is full the loop stops
//     reading, so the browser's flow control pushes back on the device); writes are queued and sent in order
//
// Everything runs where the module runs: Web Serial is available in the window and in dedicated workers.
// Reads never block (a blocking read would freeze the browser): they return the bytes that are there, EAGAIN
// when the port is open but silent, and 0 (EOF) when it is closed. Use poll() with a zero timeout, the
// webserial_set_callback() events, or -sASYNCIFY / -sJSPI when poll() has to wait.
//
// Not covered: SerialPort objects are not transferable between contexts in every browser; open the port in the
// context that picked it, or let the other context find it with webserial_scan().

addToLibrary({
  $WebSerial__deps: ['$FS'],
  $WebSerial: {
    MAJOR: 0x7A,                      // device major number of the nodes (FS.makedev(major, port index))
    MAX_PORTS: 8,
    RX_CAP: 1 << 20,                  // buffered received bytes per port before the read loop pauses
    TX_CAP: 1 << 20,                  // queued bytes per port before write() answers EAGAIN
    // port states, same numbers as the WEBSERIAL_* constants in webserial.h
    EMPTY: 0, CLOSED: 1, OPENING: 2, OPEN: 3, ERROR: 4,
    // events of the C callback
    EV_PORT_ADDED: 1, EV_OPENED: 2, EV_CLOSED: 3, EV_ERROR: 4, EV_REQUEST_CANCELLED: 5, EV_DATA: 6, EV_SCAN_DONE: 7,

    ports: [],
    callback: 0,
    callbackUser: 0,

    // Linux termios baud codes (CBAUD field of c_cflag, musl/Linux values)
    BAUD_CODES: {
      0: 0, 50: 1, 75: 2, 110: 3, 134: 4, 150: 5, 200: 6, 300: 7, 600: 8, 1200: 9, 1800: 10, 2400: 11, 4800: 12,
      9600: 13, 19200: 14, 38400: 15, 57600: 0x1001, 115200: 0x1002, 230400: 0x1003, 460800: 0x1004, 500000: 0x1005,
      576000: 0x1006, 921600: 0x1007, 1000000: 0x1008, 1152000: 0x1009, 1500000: 0x100A, 2000000: 0x100B,
      2500000: 0x100C, 3000000: 0x100D, 3500000: 0x100E, 4000000: 0x100F
    },
    CBAUD: 0x100F,

    supported() {
      return typeof navigator !== 'undefined' && !!navigator.serial;
    },

    notify(event, index) {
      if (WebSerial.callback) {{{ makeDynCall('viip', 'WebSerial.callback') }}}(event, index, WebSerial.callbackUser);
    },

    fail(p, err) {
      p.state = WebSerial.ERROR;
      p.error = (err && err.message) ? err.message : String(err);
      WebSerial.notify(WebSerial.EV_ERROR, p.index);
    },

    // Registers a SerialPort and creates its device node; returns the port index (-1: table full)
    add(serialPort) {
      for (var i = 0; i < WebSerial.ports.length; i++) {
        if (WebSerial.ports[i] && WebSerial.ports[i].port === serialPort) return i;
      }
      var index = -1;
      for (var i = 0; i < WebSerial.MAX_PORTS; i++) {
        if (!WebSerial.ports[i]) { index = i; break; }
      }
      if (index < 0) return -1;

      var info = {};
      try { info = serialPort.getInfo() || {}; } catch (e) { /* not a USB port */ }
      var p = WebSerial.ports[index] = {
        index: index, port: serialPort, info: info, state: WebSerial.CLOSED, error: '', baud: 115200,
        reader: null, writer: null, gen: 0,
        rx: [], rxBytes: 0, rxResume: null, dataSignalled: false,
        tx: [], txBytes: 0, txBusy: false
      };

      var dev = FS.makedev(WebSerial.MAJOR, index);
      FS.registerDevice(dev, WebSerial.streamOps(p));
      var path = '/dev/ttyWeb' + index;
      try { FS.unlink(path); } catch (e) { /* first use */ }
      FS.mkdev(path, dev);                // character device, mode 0666
      WebSerial.notify(WebSerial.EV_PORT_ADDED, index);
      return index;
    },

    // ---- the open / read loop / close cycle of one port ----

    open(p, baud) {
      if (p.state === WebSerial.OPENING || p.state === WebSerial.OPEN) return;
      p.state = WebSerial.OPENING;
      p.baud = baud > 0 ? baud : 115200;
      p.error = '';
      var gen = ++p.gen;
      p.port.open({ baudRate: p.baud, bufferSize: 1 << 16 }).then(function () {
        if (gen !== p.gen) { p.port.close().catch(function () {}); return; }   // closed while opening
        // CDC-ACM firmware often waits for DTR before it talks
        return Promise.resolve(p.port.setSignals ? p.port.setSignals({ dataTerminalReady: true, requestToSend: true }) : 0)
          .catch(function () { /* not supported */ })
          .then(function () {
            if (gen !== p.gen) { p.port.close().catch(function () {}); return; }
            p.writer = p.port.writable.getWriter();
            p.state = WebSerial.OPEN;
            WebSerial.notify(WebSerial.EV_OPENED, p.index);
            WebSerial.readLoop(p, gen);
          });
      }).catch(function (err) {
        if (gen === p.gen) WebSerial.fail(p, err);
      });
    },

    // Moves port.readable into p.rx until the port is closed. A recoverable stream error (framing, parity,
    // buffer overrun) replaces port.readable with a new stream; a fatal one ends the loop.
    async readLoop(p, gen) {
      var reason = '';
      while (gen === p.gen && p.port.readable) {
        var reader = p.reader = p.port.readable.getReader();
        try {
          for (;;) {
            if (p.rxBytes >= WebSerial.RX_CAP) {          // the program is not reading: stop reading too
              await new Promise(function (resolve) { p.rxResume = resolve; });
              p.rxResume = null;
              if (gen !== p.gen) break;
              continue;
            }
            var r = await reader.read();
            if (r.done) break;
            if (r.value && r.value.byteLength) {
              p.rx.push(r.value);
              p.rxBytes += r.value.byteLength;
              if (!p.dataSignalled) { p.dataSignalled = true; WebSerial.notify(WebSerial.EV_DATA, p.index); }
            }
          }
        } catch (err) {
          reason = err && err.message ? err.message : String(err);
          if (err && (err.name === 'NetworkError' || err.name === 'NotFoundError')) { gen = -1; }   // the device is gone
        } finally {
          try { reader.releaseLock(); } catch (e) { /* already released */ }
          if (p.reader === reader) p.reader = null;
        }
        if (gen === -1) break;
      }
      // Ended by close() (the generation changed) or by loss of the port
      if (gen !== -1 && gen !== p.gen) return;
      if (p.state === WebSerial.OPEN) {
        WebSerial.teardown(p).then(function () {
          p.state = reason ? WebSerial.ERROR : WebSerial.CLOSED;
          p.error = reason;
          WebSerial.notify(reason ? WebSerial.EV_ERROR : WebSerial.EV_CLOSED, p.index);
        });
      }
    },

    // Gives up the streams and closes the port (every step is best effort)
    async teardown(p) {
      p.gen++;
      var reader = p.reader, writer = p.writer;
      p.reader = null; p.writer = null;
      p.tx = []; p.txBytes = 0;
      if (p.rxResume) p.rxResume();
      if (reader) { try { await reader.cancel(); } catch (e) { /* already errored */ } }
      if (writer) { try { writer.releaseLock(); } catch (e) { /* write pending */ } }
      try { await p.port.close(); } catch (e) { /* closed or lost */ }
    },

    close(p) {
      if (p.state !== WebSerial.OPEN && p.state !== WebSerial.OPENING) return;
      var wasOpen = p.state === WebSerial.OPEN;
      p.state = WebSerial.CLOSED;
      WebSerial.teardown(p).then(function () {
        if (wasOpen) WebSerial.notify(WebSerial.EV_CLOSED, p.index);
      });
    },

    pump(p) {
      if (p.txBusy) return;
      p.txBusy = true;
      var gen = p.gen;
      (async function () {
        try {
          while (gen === p.gen && p.tx.length) {
            var chunk = p.tx[0];
            await p.writer.write(chunk);
            if (gen !== p.gen) return;
            p.tx.shift();
            p.txBytes -= chunk.byteLength;
          }
        } catch (err) {
          if (gen === p.gen) WebSerial.fail(p, err);
        } finally {
          p.txBusy = false;
        }
      })();
    },

    // ---- termios view of the port (baud rate only) ----
    termiosBaud(cflag) {
      var code = cflag & WebSerial.CBAUD;
      for (var b in WebSerial.BAUD_CODES) if (WebSerial.BAUD_CODES[b] === code) return +b;
      return 0;
    },

    // ---- the device ----
    streamOps(p) {
      var CS8 = 0x30, CREAD = 0x80, CLOCAL = 0x800;
      return {
        open(stream) {
          stream.seekable = false;
          stream.tty = {
            ops: {
              ioctl_tcgets() {
                return { c_iflag: 0, c_oflag: 0, c_lflag: 0, c_cc: [],
                         c_cflag: (WebSerial.BAUD_CODES[p.baud] || 0) | CS8 | CREAD | CLOCAL };
              },
              // A new baud rate reopens the port (Web Serial cannot change it while open)
              ioctl_tcsets(tty, op, data) {
                var baud = WebSerial.termiosBaud(data.c_cflag);
                if (baud > 0 && baud !== p.baud) {
                  p.baud = baud;
                  if (p.state === WebSerial.OPEN) {
                    p.state = WebSerial.CLOSED;
                    WebSerial.teardown(p).then(function () { WebSerial.open(p, baud); });
                  }
                }
                return 0;
              }
            }
          };
        },
        close(stream) {},
        read(stream, buffer, offset, length, pos) {
          if (p.rxBytes === 0) {
            if (p.state === WebSerial.OPEN || p.state === WebSerial.OPENING) throw new FS.ErrnoError({{{ cDefs.EAGAIN }}});
            return 0;                                      // closed: end of file
          }
          var n = 0;
          while (n < length && p.rx.length) {
            var chunk = p.rx[0];
            var take = Math.min(length - n, chunk.byteLength);
            buffer.set(chunk.subarray(0, take), offset + n);
            n += take;
            if (take === chunk.byteLength) p.rx.shift(); else p.rx[0] = chunk.subarray(take);
          }
          p.rxBytes -= n;
          if (p.rxBytes === 0) p.dataSignalled = false;
          if (p.rxResume && p.rxBytes < WebSerial.RX_CAP / 2) p.rxResume();
          return n;
        },
        write(stream, buffer, offset, length, pos) {
          if (p.state !== WebSerial.OPEN) throw new FS.ErrnoError({{{ cDefs.EIO }}});
          // Non-blocking semantics: accept what fits in the queue (a partial write), EAGAIN when nothing does
          var room = WebSerial.TX_CAP - p.txBytes;
          if (room <= 0) throw new FS.ErrnoError({{{ cDefs.EAGAIN }}});
          if (length > room) length = room;
          // copy now: the heap view may change before the asynchronous write happens
          var data = new Uint8Array(buffer.buffer, buffer.byteOffset + offset, length).slice();
          p.tx.push(data);
          p.txBytes += length;
          WebSerial.pump(p);
          return length;
        },
        llseek() { return 0; },
        poll(stream) {
          var flags = 0;
          if (p.rxBytes > 0) flags |= {{{ cDefs.POLLIN }}};
          if (p.state === WebSerial.OPEN && p.txBytes < WebSerial.TX_CAP) flags |= {{{ cDefs.POLLOUT }}};
          if (p.state === WebSerial.CLOSED || p.state === WebSerial.ERROR) flags |= {{{ cDefs.POLLHUP }}} | {{{ cDefs.POLLIN }}};
          return flags;
        }
      };
    }
  },

  webserial_supported__deps: ['$WebSerial'],
  webserial_supported: () => WebSerial.supported() ? 1 : 0,

  webserial_set_callback__deps: ['$WebSerial'],
  webserial_set_callback: (callback, user) => {
    WebSerial.callback = callback;
    WebSerial.callbackUser = user;
  },

  // Chooser; needs a user gesture. Reports EV_PORT_ADDED (also for a port that was already known) or
  // EV_REQUEST_CANCELLED.
  webserial_request_port__deps: ['$WebSerial'],
  webserial_request_port: () => {
    if (!WebSerial.supported()) { WebSerial.notify(WebSerial.EV_REQUEST_CANCELLED, -1); return; }
    navigator.serial.requestPort({}).then(function (port) {
      var known = WebSerial.ports.findIndex(function (q) { return q && q.port === port; });
      var index = WebSerial.add(port);                 // reports EV_PORT_ADDED itself for a new port
      if (index < 0) WebSerial.notify(WebSerial.EV_REQUEST_CANCELLED, -1);   // table full
      else if (known >= 0) WebSerial.notify(WebSerial.EV_PORT_ADDED, index); // the user picked a known port again
    }, function () {
      WebSerial.notify(WebSerial.EV_REQUEST_CANCELLED, -1);
    });
  },

  // Registers the ports the user has permitted before; EV_PORT_ADDED for each new one, then EV_SCAN_DONE
  webserial_scan__deps: ['$WebSerial'],
  webserial_scan: () => {
    if (!WebSerial.supported()) { WebSerial.notify(WebSerial.EV_SCAN_DONE, 0); return; }
    navigator.serial.getPorts().then(function (ports) {
      for (var i = 0; i < ports.length; i++) WebSerial.add(ports[i]);
      WebSerial.notify(WebSerial.EV_SCAN_DONE, ports.length);
    }, function () {
      WebSerial.notify(WebSerial.EV_SCAN_DONE, 0);
    });
  },

  webserial_open__deps: ['$WebSerial'],
  webserial_open: (index, baud) => {
    var p = WebSerial.ports[index];
    if (!p) return -1;
    WebSerial.open(p, baud);
    return 0;
  },

  webserial_close__deps: ['$WebSerial'],
  webserial_close: (index) => {
    var p = WebSerial.ports[index];
    if (p) WebSerial.close(p);
  },

  webserial_state__deps: ['$WebSerial'],
  webserial_state: (index) => {
    var p = WebSerial.ports[index];
    return p ? p.state : WebSerial.EMPTY;
  },

  webserial_usb_ids__deps: ['$WebSerial'],
  webserial_usb_ids: (index, vidPtr, pidPtr) => {
    var p = WebSerial.ports[index];
    if (!p || p.info.usbVendorId === undefined) return -1;
    if (vidPtr) {{{ makeSetValue('vidPtr', 0, 'p.info.usbVendorId', 'i32') }}};
    if (pidPtr) {{{ makeSetValue('pidPtr', 0, 'p.info.usbProductId', 'i32') }}};
    return 0;
  },

  webserial_last_error__deps: ['$WebSerial', '$stringToUTF8', '$lengthBytesUTF8'],
  webserial_last_error: (index, buf, len) => {
    var p = WebSerial.ports[index];
    if (!p || !buf || len <= 0) return 0;
    stringToUTF8(p.error, buf, len);
    return Math.min(lengthBytesUTF8(p.error), len - 1);
  },

  webserial_available__deps: ['$WebSerial'],
  webserial_available: (index) => {
    var p = WebSerial.ports[index];
    return p ? p.rxBytes : 0;
  },

  webserial_set_signals__deps: ['$WebSerial'],
  webserial_set_signals: (index, dtr, rts) => {
    var p = WebSerial.ports[index];
    if (!p || p.state !== WebSerial.OPEN || !p.port.setSignals) return -1;
    p.port.setSignals({ dataTerminalReady: !!dtr, requestToSend: !!rts }).catch(function () {});
    return 0;
  },

  // Forgets a port: closes it and removes its device node (the browser's permission stays)
  webserial_remove__deps: ['$WebSerial'],
  webserial_remove: (index) => {
    var p = WebSerial.ports[index];
    if (!p) return;
    WebSerial.close(p);
    try { FS.unlink('/dev/ttyWeb' + index); } catch (e) { /* already gone */ }
    WebSerial.ports[index] = null;
  },
});
