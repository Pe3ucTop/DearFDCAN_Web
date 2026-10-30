#ifndef WEBSERIAL_H_
#define WEBSERIAL_H_
// Web Serial ports as character devices of Emscripten's virtual file system: /dev/ttyWeb0 ... /dev/ttyWeb7.
// Link with  --js-library libs/emscripten-webserial/library_webserial.js  (see README.md).
//
// Typical use:
//     webserial_set_callback(on_event, NULL);
//     webserial_request_port();                 // from a click handler: browser chooser, asynchronous
//     ...on_event(WEBSERIAL_EV_PORT_ADDED, idx): webserial_open(idx, 115200);
//     ...on_event(WEBSERIAL_EV_OPENED, idx):     int fd = open("/dev/ttyWeb0", O_RDWR | O_NONBLOCK);
//                                                write(fd, "V\r", 2); read(fd, buf, sizeof buf); ...
#ifdef __cplusplus
extern "C" {
#endif

#define WEBSERIAL_MAX_PORTS 8

// State of a port (webserial_state)
enum {
    WEBSERIAL_EMPTY   = 0,   // no port in this slot
    WEBSERIAL_CLOSED  = 1,   // known, not open (the device node exists)
    WEBSERIAL_OPENING = 2,
    WEBSERIAL_OPEN    = 3,
    WEBSERIAL_ERROR   = 4    // open or I/O failed, webserial_last_error() has the text
};

// Events of the callback
enum {
    WEBSERIAL_EV_PORT_ADDED        = 1,   // index of the new port (also reported again if the user picks a known one)
    WEBSERIAL_EV_OPENED            = 2,
    WEBSERIAL_EV_CLOSED            = 3,   // closed by webserial_close() or the device went away
    WEBSERIAL_EV_ERROR             = 4,
    WEBSERIAL_EV_REQUEST_CANCELLED = 5,   // the chooser was dismissed (index -1)
    WEBSERIAL_EV_DATA              = 6,   // data arrived in an empty buffer: read it (once per empty -> non-empty change)
    WEBSERIAL_EV_SCAN_DONE         = 7    // index = number of permitted ports
};

typedef void (*webserial_callback)(int event, int index, void* user);

// 1 if the browser has Web Serial in this context (window or dedicated worker)
int webserial_supported(void);

// Receives all events; may be NULL. Called from the JavaScript event loop, not from inside other calls.
void webserial_set_callback(webserial_callback callback, void* user);

// Browser port chooser (needs a user gesture); the chosen port becomes /dev/ttyWeb<index>
void webserial_request_port(void);
// Registers the ports the user permitted earlier, without a chooser
void webserial_scan(void);

// Opens a port asynchronously at a baud rate (USB CDC adapters ignore it); the result is an event / webserial_state().
// DTR and RTS are asserted. Returns -1 for an index without a port.
int webserial_open(int index, int baud);
void webserial_close(int index);
// Closes a port and removes its device node; the browser's permission stays
void webserial_remove(int index);

int webserial_state(int index);
// USB ids of a USB serial adapter: returns 0 and stores them, -1 for a port without ids
int webserial_usb_ids(int index, int* vid, int* pid);
// Text of the last error, returns its length
int webserial_last_error(int index, char* buffer, int size);
// Received bytes waiting to be read
int webserial_available(int index);
// Control lines of an open port
int webserial_set_signals(int index, int dtr, int rts);

#ifdef __cplusplus
}
#endif
#endif
