# emscripten_webserial

Web Serial ports as POSIX character devices of Emscripten's virtual file system: `/dev/ttyWeb0` ... `/dev/ttyWeb7`.
C and C++ code that talks to a serial port with `open()` / `read()` / `write()` / `poll()` / `tcsetattr()` can run in the
browser without a JavaScript rewrite.

This is a standalone library (one JavaScript library file and one header); it does not depend on the rest of the repository.
The CAN monitor itself does not use it: its serial path is `apps/dearfdcan_web/serial_worker.js`, which moves the bytes into the CAN core
without going through a file system (see the main README). The library is the reusable answer to "how does a
Web Serial port look like a file descriptor", for programs that want the POSIX interface.

## Use

```sh
em++ app.cpp -o app.html --js-library libs/emscripten-webserial/library_webserial.js -Ilibs/emscripten-webserial
```

```c
#include <fcntl.h>
#include <unistd.h>
#include "webserial.h"

static void on_event(int event, int index, void* user) {
    if (event == WEBSERIAL_EV_PORT_ADDED)  webserial_open(index, 115200);
    if (event == WEBSERIAL_EV_OPENED) {
        int fd = open("/dev/ttyWeb0", O_RDWR | O_NONBLOCK);
        write(fd, "V\r", 2);                       // read() later, when WEBSERIAL_EV_DATA arrives or poll() says POLLIN
    }
}

webserial_set_callback(on_event, NULL);
webserial_request_port();                          // from a click handler: the browser's port chooser
```

`webserial.h` documents every function. The port index is the number in the device name; `webserial_scan()` registers the ports
the user permitted earlier, so a page can reconnect without the chooser.

## Behaviour of the device

| Call | Result |
|---|---|
| `read()` | the bytes that are buffered (at most what was asked), `EAGAIN` while the port is open but silent, `0` (end of file) when it is closed, lost or failed. It never blocks: a blocking read would freeze the browser. |
| `write()` | copies the data and queues it, the browser sends it in order. A full queue (1 MiB) gives a partial write and then `EAGAIN`; a port that is not open gives `EIO`. |
| `poll()` | `POLLIN` when data is buffered, `POLLOUT` when the queue has room, `POLLHUP` (with `POLLIN`) when closed or failed. Only a zero timeout is supported by Emscripten unless the module is built with `-sASYNCIFY` or `-sJSPI`. |
| `tcgetattr()` / `tcsetattr()` | shows and sets the baud rate (`cfsetspeed`); a different rate reopens the port, because Web Serial cannot change it while open. Other flags are ignored. |
| `isatty()` | true |

Receiving is flow controlled: the read loop stops at 1 MiB of unread data and continues when the program reads, so a program that is
busy only delays the device (the browser's buffer and the device's own flow control take over) instead of growing the heap.

Open ports assert DTR and RTS (CDC-ACM firmware often waits for DTR). `webserial_set_signals()` changes them.

## Limits

* Web Serial needs a Chromium based browser and a secure context (`https:` or `localhost`); it is available in a window and in dedicated
  workers. `SerialPort` objects are not transferable in every browser, so the port is opened in the context that picked it, or found
  again with `webserial_scan()` in the other context.
* At most 8 ports (`WEBSERIAL_MAX_PORTS`).
* Not tested with a real browser or a real adapter: the tests use a mock that implements the interface of `navigator.serial` with Web Streams.

## Test

`test/test_webserial.c` is driven by `test/run_test.js` under Node with a mock `navigator.serial` (scan, chooser, open with baud rate
and DTR/RTS, chunked receive, partial reads, receive and transmit flow control, termios baud change, device loss and reopen, close
during open, node creation and removal):

```sh
emcc libs/emscripten-webserial/test/test_webserial.c -o test_webserial.js -O1 -sENVIRONMENT=node -sMODULARIZE=1 -sEXPORT_NAME=createWS \
     -sFORCE_FILESYSTEM=1 --js-library libs/emscripten-webserial/library_webserial.js -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=64MB \
     -sEXPORTED_RUNTIME_METHODS='["FS","HEAPU8","HEAP32","ccall","UTF8ToString"]' \
     -sEXPORTED_FUNCTIONS='["_main","_malloc","_free","_webserial_supported","_webserial_scan","_webserial_open","_webserial_close","_webserial_remove","_webserial_state","_webserial_usb_ids","_webserial_last_error","_webserial_available","_webserial_request_port","_t_start","_t_event_count","_t_event","_t_event_index","_t_clear_events","_t_buf","_t_open","_t_errno","_t_read","_t_write","_t_poll","_t_close","_t_get_baud","_t_set_baud","_t_isatty"]'
node libs/emscripten-webserial/test/run_test.js ./test_webserial.js
```

Verified with Emscripten 6.0.10 and Node 22.

## Ideas that were considered

* **Sockets instead of a device**: pretending the port is a WebSocket (replacing the `WebSocket` class that Emscripten's POSIX sockets use)
  would let `socket()/connect()/send()/recv()` code work, but it hijacks a mechanism that has other users; a device node is the honest model.
* **Blocking reads**: possible with `-sJSPI` (suspend the wasm stack while `reader.read()` is pending), but it ties the program to a
  browser feature that is not available everywhere; the non-blocking interface works without it.
* **BYOB reads into the heap**: not possible, bring-your-own-buffer reads detach the buffer they use and the wasm heap cannot be detached;
  one copy from the chunk into the program's buffer is the minimum.
