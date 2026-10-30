#ifndef WEBUSB_H_
#define WEBUSB_H_
// WebUSB for programs compiled with Emscripten: find a device, open it, send control and bulk transfers, receive bulk data.
// The API follows WebUSB (devices, interfaces, endpoints, control setup packets, asynchronous transfers); it is not a libusb clone.
// Link with  --pre-js libs/webusb-io/webusb_io.js --js-library libs/emscripten-webusb/library_webusb.js  (see README.md).
//
// Typical use:
//     webusb_set_callback(on_event, NULL);
//     webusb_request_device(filters, 1);                 // from a click handler: browser chooser, asynchronous
//     ...on_event(WEBUSB_EV_DEVICE_ADDED, idx):          webusb_open(idx, 1, 0);          // configuration 1, interface 0
//     ...on_event(WEBUSB_EV_OPENED, idx):                webusb_start_receive(idx, 1, 2048, on_data, NULL);
//                                                        webusb_control_in(idx, WEBUSB_TYPE_VENDOR, WEBUSB_RECIPIENT_INTERFACE,
//                                                                          4, 0, 0, 40, on_done, NULL);
//
// Everything is asynchronous and runs in the JavaScript event loop of the context that loaded the module: a window or a dedicated worker
// (the chooser needs a window; a worker finds the permitted devices again with webusb_scan()). Callbacks are never called from inside
// another call of this API. Data pointers given to callbacks are valid only during the callback: copy what you need.
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WEBUSB_MAX_DEVICES 8

// State of a device (webusb_state)
enum {
    WEBUSB_EMPTY   = 0,   // no device in this slot
    WEBUSB_CLOSED  = 1,   // known (permitted), not open
    WEBUSB_OPENING = 2,
    WEBUSB_OPEN    = 3,   // opened, configuration selected, interface claimed
    WEBUSB_ERROR   = 4    // open failed or the device was lost; webusb_last_error() has the text
};

// Events of the callback
enum {
    WEBUSB_EV_DEVICE_ADDED      = 1,   // index of a device that became known (also reported again if the user picks a known one)
    WEBUSB_EV_OPENED            = 2,
    WEBUSB_EV_CLOSED            = 3,   // closed by webusb_close()
    WEBUSB_EV_ERROR             = 4,   // the open failed, or the device was unplugged / failed while open
    WEBUSB_EV_REQUEST_CANCELLED = 5,   // the chooser was dismissed (index -1)
    WEBUSB_EV_SCAN_DONE         = 6    // index = number of permitted devices
};

// Status of a transfer
enum {
    WEBUSB_OK         = 0,
    WEBUSB_ERR_FAILED = -1   // the device refused it (stall, babble), it timed out, the device is gone, or it was never sent
};

// Control transfers: the request type and recipient of the setup packet
enum { WEBUSB_TYPE_STANDARD = 0, WEBUSB_TYPE_CLASS = 1, WEBUSB_TYPE_VENDOR = 2 };
enum { WEBUSB_RECIPIENT_DEVICE = 0, WEBUSB_RECIPIENT_INTERFACE = 1, WEBUSB_RECIPIENT_ENDPOINT = 2, WEBUSB_RECIPIENT_OTHER = 3 };

// Strings of a device (webusb_string)
enum { WEBUSB_STRING_MANUFACTURER = 0, WEBUSB_STRING_PRODUCT = 1, WEBUSB_STRING_SERIAL = 2 };

// Counters of an open device (webusb_stat)
enum {
    WEBUSB_STAT_RX_TRANSFERS = 0,   // receive transfers delivered
    WEBUSB_STAT_RX_ERRORS    = 1,   // failed or partial receive transfers
    WEBUSB_STAT_TX_ERRORS    = 2,   // failed bulk OUT transfers
    WEBUSB_STAT_TX_DROPPED   = 3,   // bulk OUT transfers refused because the queue was full
    WEBUSB_STAT_RECOVERIES   = 4    // times the interface was released and claimed again after receive errors
};

// A filter of the chooser; 0 means "any" (a filter with both 0 matches every device)
typedef struct {
    int vendor_id;
    int product_id;
} webusb_filter;

typedef struct {
    int vendor_id;
    int product_id;
    int device_class;
    int device_subclass;
    int device_protocol;
    int usb_version;        // BCD, 0x0200 = USB 2.0
    int device_version;     // BCD
} webusb_device_info;

// An endpoint of the claimed interface
typedef struct {
    int number;             // without the direction bit
    int direction;          // 0 = OUT (host to device), 1 = IN
    int type;               // 0 = bulk, 1 = interrupt, 2 = isochronous
    int packet_size;
} webusb_endpoint_info;

typedef void (*webusb_callback)(int event, int index, void* user);

// Completion of a control transfer or a bulk OUT transfer. `data`/`length`: the bytes of a control IN that succeeded (else NULL / 0).
typedef void (*webusb_transfer_callback)(int index, int id, int status, const uint8_t* data, int length, void* user);

// One completed bulk or interrupt IN transfer, in order. time_ms: performance.now() when it completed.
typedef void (*webusb_receive_callback)(int index, int endpoint, const uint8_t* data, int length, double time_ms, void* user);

// 1 if WebUSB is available in this context and the transport library (webusb_io.js) was linked in
int webusb_supported(void);

// Receives all events; may be NULL
void webusb_set_callback(webusb_callback callback, void* user);

// Browser device chooser (needs a user gesture; a window only). `count` filters, 0 = every device. The chosen device gets an index.
void webusb_request_device(const webusb_filter* filters, int count);
// Registers the devices the user permitted earlier (works in a worker too)
void webusb_scan(void);

// Opens a device asynchronously: selects `configuration` (0 = keep the selected one, or 1 if none is selected) and claims `interface_number`.
// The result is an event / webusb_state(). Returns -1 for an index without a device.
int webusb_open(int index, int configuration, int interface_number);
void webusb_close(int index);
// Closes a device and forgets it; the browser's permission stays
void webusb_remove(int index);

int webusb_state(int index);
// Text of the last error, returns its length
int webusb_last_error(int index, char* buffer, int size);

// Descriptor information (available once the device is known). Returns 0, or -1 for an index without a device.
int webusb_get_device_info(int index, webusb_device_info* info);
// A string of the device into buffer (UTF-8, terminated); returns its length, 0 if the device has none
int webusb_string(int index, int which, char* buffer, int size);
// The endpoints of the claimed interface (of interface 0 before the device is open); -1 for an index without a device
int webusb_endpoint_count(int index);
int webusb_endpoint(int index, int i, webusb_endpoint_info* info);

// Control transfers of an open device. They run strictly in the order they were submitted, each with a timeout. Return the id given to the
// callback (> 0), or -1 if the device is not open. `callback` may be NULL. The data of an OUT transfer is copied at the call.
int webusb_control_out(int index, int type, int recipient, int request, int value, int wIndex, const void* data, int length,
                       webusb_transfer_callback callback, void* user);
int webusb_control_in(int index, int type, int recipient, int request, int value, int wIndex, int length,
                      webusb_transfer_callback callback, void* user);

// A bulk (or interrupt) OUT transfer to an endpoint (number without the direction bit). Queued in order; a limited number is in flight at
// once. Returns the id given to the callback (> 0), or -1 if the device is not open or the queue is full (then no callback follows). The data is
// copied at the call. `callback` may be NULL.
int webusb_bulk_out(int index, int endpoint, const void* data, int length, webusb_transfer_callback callback, void* user);

// Keeps IN transfers of `size` bytes pending on an endpoint and hands every completed one to the callback, in order. Several are kept pending at
// once, so nothing is missed between two of them. Calling it again changes the endpoint / size / callback. Returns 0, or -1 if not open.
// A transfer that was pending when the reception stopped is dropped when the device answers.
int webusb_start_receive(int index, int endpoint, int size, webusb_receive_callback callback, void* user);
void webusb_stop_receive(int index);

int webusb_stat(int index, int which);

#ifdef __cplusplus
}
#endif
#endif
