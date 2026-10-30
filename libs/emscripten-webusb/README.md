# emscripten-webusb

WebUSB for programs compiled with Emscripten: find a USB device, open it, send control and bulk transfers, receive bulk data, from C or C++.
The API (`webusb.h`) follows WebUSB itself, with devices, interfaces, endpoints, setup packets and asynchronous transfers; it is **not** a
libusb clone (libusb's synchronous event loop does not fit the browser, and nothing here needs it). The transport underneath is
`libs/webusb-io` (`UsbPort`): receive slots that are always pending, recovery after errors, ordered control transfers with timeouts, a
transmit queue, loss detection.

## Use

```sh
em++ app.cpp -o app.html -Ilibs/emscripten-webusb \
     --pre-js libs/webusb-io/webusb_io.js --js-library libs/emscripten-webusb/library_webusb.js
```

Both files are needed: `webusb_io.js` is the transport (a plain script that defines `WebUsbIo`), `library_webusb.js` the C face.

```c
#include "webusb.h"

static void on_data(int index, int ep, const uint8_t* data, int length, double time_ms, void* user) { /* copy what you need */ }
static void on_done(int index, int id, int status, const uint8_t* data, int length, void* user) { /* status 0 = ok */ }

static void on_event(int event, int index, void* user) {
    if (event == WEBUSB_EV_DEVICE_ADDED) webusb_open(index, 1, 0);               // configuration 1, interface 0
    if (event == WEBUSB_EV_OPENED) {
        webusb_start_receive(index, 1, 2048, on_data, NULL);                      // IN endpoint 1, transfers of up to 2048 bytes
        webusb_control_in(index, WEBUSB_TYPE_VENDOR, WEBUSB_RECIPIENT_INTERFACE, 4, 0, 0, 40, on_done, NULL);
        webusb_bulk_out(index, 2, "hello", 5, NULL, NULL);                        // OUT endpoint 2
    }
}

webusb_set_callback(on_event, NULL);
webusb_filter filter = { 0x1D50, 0 };                                             // 0 = any
webusb_request_device(&filter, 1);                                                // from a click handler: the browser's chooser
```

`webusb.h` documents every function. The index of a device is its slot (up to 8, `WEBUSB_MAX_DEVICES`); `webusb_scan()` registers the
devices the user permitted earlier, so a page can reconnect without the chooser (and a worker, which has no chooser, finds its devices
this way).

## Behaviour

| Call | Result |
|---|---|
| `webusb_open` | asynchronous: selects the configuration (0 = keep the selected one, or 1 if none), claims the interface; `WEBUSB_EV_OPENED` or `WEBUSB_EV_ERROR` with the browser's text in `webusb_last_error()` |
| `webusb_control_out` / `_in` | run strictly in the order submitted, each with a 2 s timeout; the completion callback gets `WEBUSB_OK` or `WEBUSB_ERR_FAILED` (stall, babble, timeout, device gone, never sent) and, for an IN, the bytes; OUT data is copied at the call |
| `webusb_bulk_out` | queued in order, up to 4 transfers in flight, a queue of 4096; returns `-1` (and no callback follows) when it is full; the callback reports each transfer; transfers still queued when the device closes are reported as failed |
| `webusb_start_receive` | keeps 6 IN transfers pending on the endpoint and calls back for every completed one, in order, so nothing is missed between two transfers. After errors the interface is released and claimed again |
| `webusb_stop_receive` | WebUSB cannot cancel a pending transfer; the ones in flight complete when the device sends something and what they bring is dropped |
| unplug | `WEBUSB_EV_ERROR` with the reason, state `WEBUSB_ERROR`, later calls return `-1`; opening again works after the device is plugged in |

The callbacks come from the JavaScript event loop, never from inside another call. The `data` pointers they get point into a scratch buffer
and are valid during the callback only.

## Limits

* Chromium based browsers, a secure context (`https:` or `localhost`). Not available in Firefox and Safari.
* `webusb_request_device` needs a window and a user gesture. A `USBDevice` object is not transferable between contexts in every browser, so a
  worker uses `webusb_scan()` for the devices the user permitted in the window.
* One claimed interface per device, no isochronous transfers, no alternate settings.
* Operating systems may already have a driver bound to the device (see the notes in the main README).
* Not tested with a real browser or a real device: the test uses a mock that implements the interface of `navigator.usb`.

## Test

`test/test_webusb.c` is driven by `test/run_test.js` under Node with a mock `navigator.usb` (scan, chooser with filters, open and failed
open, configuration and interface, endpoint lists, control transfers with their order and statuses, bulk OUT with a full queue, receive with
large transfers and stop, unplug and reopen, close during open, remove). From the repository root, after `make`:

```sh
cmake --build build/em --target run_test_webusb
```

or by hand:

```sh
emcc libs/emscripten-webusb/test/test_webusb.c -o test_webusb.js -O1 -sENVIRONMENT=node -sMODULARIZE=1 -sEXPORT_NAME=createWU \
     --pre-js libs/webusb-io/webusb_io.js --js-library libs/emscripten-webusb/library_webusb.js \
     -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=64MB -sEXPORTED_RUNTIME_METHODS='["HEAPU8","UTF8ToString"]'
node libs/emscripten-webusb/test/run_test.js ./test_webusb.js
```

## Related

`libs/emscripten-webserial` gives Web Serial ports as `/dev/ttyWebN` device nodes. USB has no POSIX counterpart, hence a C API of its own.
