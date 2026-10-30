// library_webusb.js - WebUSB for programs compiled with Emscripten (C API in webusb.h).
//
// Link with:  em++ ... --pre-js libs/webusb-io/webusb_io.js --js-library libs/emscripten-webusb/library_webusb.js
// The transport itself (finding and opening the device, receive slots with generations, recovery, ordered control transfers with timeouts,
// the transmit queue, loss) is webusb_io.js (UsbPort); this file only gives it a C face: a table of up to 8 devices, event and completion
// callbacks, and the marshalling of bytes between the wasm heap and the transfers.
//
// Everything runs where the module runs, in a window or in a dedicated worker. The callbacks are called from the JavaScript event loop, never
// from inside another call of the API. The data pointers they get point into a scratch buffer of the heap and are valid during the callback only.

addToLibrary({
  $WebUsb__deps: ['malloc', 'free'],
  $WebUsb: {
    MAX_DEVICES: 8,
    // device states, same numbers as the WEBUSB_* constants in webusb.h
    EMPTY: 0, CLOSED: 1, OPENING: 2, OPEN: 3, ERROR: 4,
    // events of the C callback
    EV_DEVICE_ADDED: 1, EV_OPENED: 2, EV_CLOSED: 3, EV_ERROR: 4, EV_REQUEST_CANCELLED: 5, EV_SCAN_DONE: 6,
    REQUEST_TYPES: ['standard', 'class', 'vendor'],
    RECIPIENTS: ['device', 'interface', 'endpoint', 'other'],

    devices: [],
    callback: 0,
    callbackUser: 0,
    nextId: 1,
    scratch: 0,
    scratchSize: 0,
    listening: false,

    supported() {
      return typeof navigator !== 'undefined' && !!navigator.usb && typeof WebUsbIo !== 'undefined';
    },

    notify(event, index) {
      if (WebUsb.callback) {{{ makeDynCall('viip', 'WebUsb.callback') }}}(event, index, WebUsb.callbackUser);
    },

    // A buffer in the heap for the data handed to a callback (valid until the next call)
    scratchFor(size) {
      if (size > WebUsb.scratchSize) {
        if (WebUsb.scratch) _free(WebUsb.scratch);
        WebUsb.scratchSize = Math.max(size, 4096);
        WebUsb.scratch = _malloc(WebUsb.scratchSize);
      }
      return WebUsb.scratch;
    },

    // Calls a transfer callback; `bytes` (an ArrayBuffer / typed array view, or null) is copied to the scratch buffer for the call
    callTransfer(cb, user, index, id, status, bytes) {
      if (!cb) return;
      var ptr = 0, len = 0;
      if (bytes && bytes.byteLength) {
        len = bytes.byteLength;
        ptr = WebUsb.scratchFor(len);
        HEAPU8.set(new Uint8Array(bytes.buffer || bytes, bytes.byteOffset || 0, len), ptr);
      }
      {{{ makeDynCall('viiipip', 'cb') }}}(index, id, status, ptr, len, user);
    },

    // Registers a USBDevice and returns its index (-1: table full)
    add(usb) {
      for (var i = 0; i < WebUsb.devices.length; i++) {
        if (WebUsb.devices[i] && WebUsb.devices[i].usb === usb) return i;
      }
      var index = -1;
      for (var i = 0; i < WebUsb.MAX_DEVICES; i++) {
        if (!WebUsb.devices[i]) { index = i; break; }
      }
      if (index < 0) return -1;
      WebUsb.devices[index] = {
        index: index, usb: usb, port: null, state: WebUsb.CLOSED, error: '', gen: 0, iface: 0,
        rxCb: 0, rxUser: 0
      };
      WebUsb.listen();
      WebUsb.notify(WebUsb.EV_DEVICE_ADDED, index);
      return index;
    },

    // An unplugged device that is idle (no transfer pending) would otherwise go unnoticed
    listen() {
      if (WebUsb.listening || !WebUsb.supported()) return;
      WebUsb.listening = true;
      navigator.usb.addEventListener('disconnect', function (ev) {
        for (var i = 0; i < WebUsb.devices.length; i++) {
          var d = WebUsb.devices[i];
          if (d && d.usb === ev.device && d.port && d.port.isOpen) d.port.lost('device disconnected');
        }
      });
    },

    // The transport of one device: a UsbPort whose events come back here
    makePort(d, iface) {
      var Port = class extends WebUsbIo.UsbPort {
        onRxData(dv) {
          if (!d.rxCb) return;
          var len = dv.byteLength;
          var ptr = WebUsb.scratchFor(len);
          HEAPU8.set(new Uint8Array(dv.buffer, dv.byteOffset, len), ptr);
          {{{ makeDynCall('viipidp', 'd.rxCb') }}}(d.index, this.rxEp, ptr, len, performance.now(), d.rxUser);
        }
        onLost(reason) {
          d.state = WebUsb.ERROR;
          d.error = reason;
          WebUsb.notify(WebUsb.EV_ERROR, d.index);
        }
      };
      return new Port('webusb ' + d.index, { iface: iface, autoReceive: false, tag: 'webusb' });
    },

    open(d, configuration, iface) {
      if (d.state === WebUsb.OPENING || d.state === WebUsb.OPEN) return;
      d.state = WebUsb.OPENING;
      d.error = '';
      d.iface = iface;
      var gen = ++d.gen;
      var port = d.port = WebUsb.makePort(d, iface);
      port.openDevice({ device: d.usb, configuration: configuration }, function () {
        // No endpoint is chosen before webusb_start_receive(); the values here are only placeholders
        return Promise.resolve({ layout: { rxEp: 1, txEp: 1, cmdEp: 0, rxSize: 512 } });
      }).then(function () {
        if (gen !== d.gen) { port.closeDevice(); return; }      // closed while opening
        d.state = WebUsb.OPEN;
        WebUsb.notify(WebUsb.EV_OPENED, d.index);
      }, function (err) {
        if (gen !== d.gen) return;
        d.state = WebUsb.ERROR;
        d.error = (err && err.message) ? err.message : String(err);
        WebUsb.notify(WebUsb.EV_ERROR, d.index);
      });
    },

    close(d) {
      if (d.state !== WebUsb.OPEN && d.state !== WebUsb.OPENING) return;
      var wasOpen = d.state === WebUsb.OPEN;
      d.state = WebUsb.CLOSED;
      d.gen++;
      d.rxCb = 0;
      var port = d.port;
      if (port && port.isOpen) {
        port.closeDevice().then(function () { if (wasOpen) WebUsb.notify(WebUsb.EV_CLOSED, d.index); });
      } else if (wasOpen) {
        WebUsb.notify(WebUsb.EV_CLOSED, d.index);
      }
    },

    ready(index) {
      var d = WebUsb.devices[index];
      return (d && d.state === WebUsb.OPEN && d.port && d.port.isOpen) ? d : null;
    },

    setup(type, recipient, request, value, windex) {
      return {
        requestType: WebUsb.REQUEST_TYPES[type] || 'vendor', recipient: WebUsb.RECIPIENTS[recipient] || 'interface',
        request: request, value: value, index: windex
      };
    },

    // The endpoints of the claimed interface (interface 0 before the open)
    endpoints(d) {
      try {
        var cfg = d.usb.configuration;
        var iface = cfg && cfg.interfaces.find(function (i) { return i.interfaceNumber === d.iface; });
        var alt = iface && iface.alternate;
        return alt ? alt.endpoints : [];
      } catch (e) { return []; }
    },

    bcd(major, minor, sub) {
      return ((major | 0) << 8) | ((minor | 0) << 4) | (sub | 0);
    }
  },

  webusb_supported__deps: ['$WebUsb'],
  webusb_supported: () => WebUsb.supported() ? 1 : 0,

  webusb_set_callback__deps: ['$WebUsb'],
  webusb_set_callback: (callback, user) => {
    WebUsb.callback = callback;
    WebUsb.callbackUser = user;
  },

  // Chooser; needs a user gesture. Reports EV_DEVICE_ADDED (also for a device that was already known) or EV_REQUEST_CANCELLED.
  webusb_request_device__deps: ['$WebUsb'],
  webusb_request_device: (filtersPtr, count) => {
    if (!WebUsb.supported()) { WebUsb.notify(WebUsb.EV_REQUEST_CANCELLED, -1); return; }
    var filters = [];
    for (var i = 0; i < count; i++) {
      var vid = {{{ makeGetValue('filtersPtr', 'i * 8', 'i32') }}};
      var pid = {{{ makeGetValue('filtersPtr', 'i * 8 + 4', 'i32') }}};
      var f = {};
      if (vid) f.vendorId = vid;
      if (pid) f.productId = pid;
      filters.push(f);
    }
    navigator.usb.requestDevice({ filters: filters }).then(function (usb) {
      var known = WebUsb.devices.findIndex(function (d) { return d && d.usb === usb; });
      var index = WebUsb.add(usb);                       // reports EV_DEVICE_ADDED itself for a new device
      if (index < 0) WebUsb.notify(WebUsb.EV_REQUEST_CANCELLED, -1);        // table full
      else if (known >= 0) WebUsb.notify(WebUsb.EV_DEVICE_ADDED, index);    // the user picked a known device again
    }, function () {
      WebUsb.notify(WebUsb.EV_REQUEST_CANCELLED, -1);
    });
  },

  // Registers the devices the user permitted before; EV_DEVICE_ADDED for each new one, then EV_SCAN_DONE
  webusb_scan__deps: ['$WebUsb'],
  webusb_scan: () => {
    if (!WebUsb.supported()) { WebUsb.notify(WebUsb.EV_SCAN_DONE, 0); return; }
    navigator.usb.getDevices().then(function (list) {
      for (var i = 0; i < list.length; i++) WebUsb.add(list[i]);
      WebUsb.notify(WebUsb.EV_SCAN_DONE, list.length);
    }, function () {
      WebUsb.notify(WebUsb.EV_SCAN_DONE, 0);
    });
  },

  webusb_open__deps: ['$WebUsb'],
  webusb_open: (index, configuration, iface) => {
    var d = WebUsb.devices[index];
    if (!d) return -1;
    WebUsb.open(d, configuration, iface);
    return 0;
  },

  webusb_close__deps: ['$WebUsb'],
  webusb_close: (index) => {
    var d = WebUsb.devices[index];
    if (d) WebUsb.close(d);
  },

  webusb_remove__deps: ['$WebUsb'],
  webusb_remove: (index) => {
    var d = WebUsb.devices[index];
    if (!d) return;
    WebUsb.close(d);
    WebUsb.devices[index] = null;
  },

  webusb_state__deps: ['$WebUsb'],
  webusb_state: (index) => {
    var d = WebUsb.devices[index];
    return d ? d.state : WebUsb.EMPTY;
  },

  webusb_last_error__deps: ['$WebUsb', '$stringToUTF8', '$lengthBytesUTF8'],
  webusb_last_error: (index, buf, len) => {
    var d = WebUsb.devices[index];
    if (!d || !buf || len <= 0) return 0;
    stringToUTF8(d.error, buf, len);
    return Math.min(lengthBytesUTF8(d.error), len - 1);
  },

  webusb_get_device_info__deps: ['$WebUsb'],
  webusb_get_device_info: (index, ptr) => {
    var d = WebUsb.devices[index];
    if (!d || !ptr) return -1;
    var u = d.usb;
    var v = [u.vendorId, u.productId, u.deviceClass, u.deviceSubclass, u.deviceProtocol,
             WebUsb.bcd(u.usbVersionMajor, u.usbVersionMinor, u.usbVersionSubminor),
             WebUsb.bcd(u.deviceVersionMajor, u.deviceVersionMinor, u.deviceVersionSubminor)];
    for (var i = 0; i < v.length; i++) {{{ makeSetValue('ptr', 'i * 4', 'v[i] | 0', 'i32') }}};
    return 0;
  },

  webusb_string__deps: ['$WebUsb', '$stringToUTF8', '$lengthBytesUTF8'],
  webusb_string: (index, which, buf, len) => {
    var d = WebUsb.devices[index];
    if (!d || !buf || len <= 0) return 0;
    var u = d.usb;
    var text = (which === 0 ? u.manufacturerName : which === 1 ? u.productName : u.serialNumber) || '';
    stringToUTF8(text, buf, len);
    return Math.min(lengthBytesUTF8(text), len - 1);
  },

  webusb_endpoint_count__deps: ['$WebUsb'],
  webusb_endpoint_count: (index) => {
    var d = WebUsb.devices[index];
    return d ? WebUsb.endpoints(d).length : -1;
  },

  webusb_endpoint__deps: ['$WebUsb'],
  webusb_endpoint: (index, i, ptr) => {
    var d = WebUsb.devices[index];
    if (!d || !ptr) return -1;
    var e = WebUsb.endpoints(d)[i];
    if (!e) return -1;
    var types = { bulk: 0, interrupt: 1, isochronous: 2 };
    var v = [e.endpointNumber, e.direction === 'in' ? 1 : 0, types[e.type] | 0, e.packetSize];
    for (var k = 0; k < v.length; k++) {{{ makeSetValue('ptr', 'k * 4', 'v[k] | 0', 'i32') }}};
    return 0;
  },

  webusb_control_out__deps: ['$WebUsb'],
  webusb_control_out: (index, type, recipient, request, value, windex, dataPtr, length, cb, user) => {
    var d = WebUsb.ready(index);
    if (!d) return -1;
    var id = WebUsb.nextId++;
    var data = length > 0 ? HEAPU8.slice(dataPtr, dataPtr + length) : new Uint8Array(0);     // copied now: the heap may change
    var setup = WebUsb.setup(type, recipient, request, value, windex);
    d.port.enqueue(function () {
      return d.port.controlTransfer(true, setup, data).then(function (r) {
        WebUsb.callTransfer(cb, user, index, id, r.ok ? 0 : -1, null);
      });
    });
    return id;
  },

  webusb_control_in__deps: ['$WebUsb'],
  webusb_control_in: (index, type, recipient, request, value, windex, length, cb, user) => {
    var d = WebUsb.ready(index);
    if (!d) return -1;
    var id = WebUsb.nextId++;
    var setup = WebUsb.setup(type, recipient, request, value, windex);
    d.port.enqueue(function () {
      return d.port.controlTransfer(false, setup, length).then(function (r) {
        WebUsb.callTransfer(cb, user, index, id, r.ok ? 0 : -1, r.ok ? r.data : null);
      });
    });
    return id;
  },

  webusb_bulk_out__deps: ['$WebUsb'],
  webusb_bulk_out: (index, endpoint, dataPtr, length, cb, user) => {
    var d = WebUsb.ready(index);
    if (!d) return -1;
    var id = WebUsb.nextId++;
    var data = HEAPU8.slice(dataPtr, dataPtr + length);
    var queued = d.port.enqueueTx(data, endpoint, function (ok) {
      WebUsb.callTransfer(cb, user, index, id, ok ? 0 : -1, null);
    });
    return queued ? id : -1;
  },

  webusb_start_receive__deps: ['$WebUsb'],
  webusb_start_receive: (index, endpoint, size, cb, user) => {
    var d = WebUsb.ready(index);
    if (!d) return -1;
    d.rxCb = cb;
    d.rxUser = user;
    d.port.beginRx(endpoint, size);
    return 0;
  },

  webusb_stop_receive__deps: ['$WebUsb'],
  webusb_stop_receive: (index) => {
    var d = WebUsb.devices[index];
    if (!d || !d.port) return;
    d.port.stopRx();
    d.rxCb = 0;
  },

  webusb_stat__deps: ['$WebUsb'],
  webusb_stat: (index, which) => {
    var d = WebUsb.devices[index];
    if (!d || !d.port) return 0;
    var s = d.port.st;
    return [s.rx, s.rxErrors, s.txErrors, s.txDropped, s.recoveries][which] | 0;
  },
});
