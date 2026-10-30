// Test program for library_webusb.js: thin wrappers over the C API that record every callback, driven step by step from run_test.js
// (which provides a mock navigator.usb under Node). See ../README.md for the build command.
#include <emscripten.h>
#include <string.h>

#include "../webusb.h"

#define MAX_EVENTS 256
static int g_event[MAX_EVENTS], g_index[MAX_EVENTS], g_events;

// completed control / bulk OUT transfers
#define MAX_XFER 256
typedef struct { int index, id, status, length; long user; unsigned char data[64]; } xfer_t;
static xfer_t g_xfer[MAX_XFER];
static int g_xfers;

// received transfers
#define MAX_RX 1024
typedef struct { int index, endpoint, length; double time; long user; unsigned char head[8]; unsigned char last; } rx_t;
static rx_t g_rx[MAX_RX];
static int g_rxs;
static double g_rx_bytes;

static unsigned char g_buf[1 << 17];

static void on_event(int event, int index, void* user) {
    (void)user;
    if (g_events < MAX_EVENTS) { g_event[g_events] = event; g_index[g_events] = index; ++g_events; }
}

static void on_transfer(int index, int id, int status, const uint8_t* data, int length, void* user) {
    if (g_xfers >= MAX_XFER) return;
    xfer_t* x = &g_xfer[g_xfers++];
    x->index = index; x->id = id; x->status = status; x->length = length; x->user = (long)user;
    if (data && length > 0) memcpy(x->data, data, length > 64 ? 64 : (size_t)length);
}

static void on_receive(int index, int endpoint, const uint8_t* data, int length, double time_ms, void* user) {
    g_rx_bytes += length;
    if (g_rxs >= MAX_RX) return;
    rx_t* r = &g_rx[g_rxs++];
    r->index = index; r->endpoint = endpoint; r->length = length; r->time = time_ms; r->user = (long)user;
    memcpy(r->head, data, length > 8 ? 8 : (size_t)length);
    r->last = length > 0 ? data[length - 1] : 0;
}

EMSCRIPTEN_KEEPALIVE void t_start(void) { g_events = g_xfers = g_rxs = 0; g_rx_bytes = 0; webusb_set_callback(on_event, 0); }
EMSCRIPTEN_KEEPALIVE int t_event_count(void) { return g_events; }
EMSCRIPTEN_KEEPALIVE int t_event(int i) { return g_event[i]; }
EMSCRIPTEN_KEEPALIVE int t_event_index(int i) { return g_index[i]; }
EMSCRIPTEN_KEEPALIVE void t_clear(void) { g_events = g_xfers = g_rxs = 0; g_rx_bytes = 0; }
EMSCRIPTEN_KEEPALIVE unsigned char* t_buf(void) { return g_buf; }

EMSCRIPTEN_KEEPALIVE int t_supported(void) { return webusb_supported(); }
EMSCRIPTEN_KEEPALIVE void t_scan(void) { webusb_scan(); }
// One filter {vid, pid}, or none
EMSCRIPTEN_KEEPALIVE void t_request(int count, int vid, int pid) {
    webusb_filter f = { vid, pid };
    webusb_request_device(count ? &f : 0, count);
}
EMSCRIPTEN_KEEPALIVE int t_open(int index, int configuration, int iface) { return webusb_open(index, configuration, iface); }
EMSCRIPTEN_KEEPALIVE void t_close(int index) { webusb_close(index); }
EMSCRIPTEN_KEEPALIVE void t_remove(int index) { webusb_remove(index); }
EMSCRIPTEN_KEEPALIVE int t_state(int index) { return webusb_state(index); }
EMSCRIPTEN_KEEPALIVE int t_last_error(int index) { return webusb_last_error(index, (char*)g_buf, 256); }
EMSCRIPTEN_KEEPALIVE int t_info(int index, int field) {
    webusb_device_info i;
    if (webusb_get_device_info(index, &i) != 0) return -1;
    const int v[] = { i.vendor_id, i.product_id, i.device_class, i.device_subclass, i.device_protocol, i.usb_version, i.device_version };
    return v[field];
}
EMSCRIPTEN_KEEPALIVE int t_string(int index, int which) { return webusb_string(index, which, (char*)g_buf, 64); }
EMSCRIPTEN_KEEPALIVE int t_endpoint_count(int index) { return webusb_endpoint_count(index); }
EMSCRIPTEN_KEEPALIVE int t_endpoint(int index, int i, int field) {
    webusb_endpoint_info e;
    if (webusb_endpoint(index, i, &e) != 0) return -1;
    const int v[] = { e.number, e.direction, e.type, e.packet_size };
    return v[field];
}

// Data of the transfer is g_buf; `user` of the callback is the request code so the test can see it come back
EMSCRIPTEN_KEEPALIVE int t_control_out(int index, int type, int recipient, int request, int value, int windex, int length, long user) {
    return webusb_control_out(index, type, recipient, request, value, windex, g_buf, length, on_transfer, (void*)user);
}
EMSCRIPTEN_KEEPALIVE int t_control_out_nocb(int index, int request, int length) {
    return webusb_control_out(index, WEBUSB_TYPE_VENDOR, WEBUSB_RECIPIENT_INTERFACE, request, 0, 0, g_buf, length, 0, 0);
}
EMSCRIPTEN_KEEPALIVE int t_control_in(int index, int type, int recipient, int request, int value, int windex, int length, long user) {
    return webusb_control_in(index, type, recipient, request, value, windex, length, on_transfer, (void*)user);
}
EMSCRIPTEN_KEEPALIVE int t_bulk_out(int index, int endpoint, int length, long user) {
    return webusb_bulk_out(index, endpoint, g_buf, length, on_transfer, (void*)user);
}
EMSCRIPTEN_KEEPALIVE int t_start_receive(int index, int endpoint, int size, long user) {
    return webusb_start_receive(index, endpoint, size, on_receive, (void*)user);
}
EMSCRIPTEN_KEEPALIVE void t_stop_receive(int index) { webusb_stop_receive(index); }
EMSCRIPTEN_KEEPALIVE int t_stat(int index, int which) { return webusb_stat(index, which); }

EMSCRIPTEN_KEEPALIVE int t_xfer_count(void) { return g_xfers; }
EMSCRIPTEN_KEEPALIVE int t_xfer_field(int i, int field) {
    const xfer_t* x = &g_xfer[i];
    const long v[] = { x->index, x->id, x->status, x->length, x->user };
    return (int)v[field];
}
EMSCRIPTEN_KEEPALIVE int t_xfer_byte(int i, int k) { return g_xfer[i].data[k]; }
EMSCRIPTEN_KEEPALIVE int t_rx_count(void) { return g_rxs; }
EMSCRIPTEN_KEEPALIVE int t_rx_field(int i, int field) {
    const rx_t* r = &g_rx[i];
    const long v[] = { r->index, r->endpoint, r->length, r->user, r->last };
    return (int)v[field];
}
EMSCRIPTEN_KEEPALIVE int t_rx_byte(int i, int k) { return g_rx[i].head[k]; }
EMSCRIPTEN_KEEPALIVE double t_rx_time(int i) { return g_rx[i].time; }
EMSCRIPTEN_KEEPALIVE double t_rx_bytes(void) { return g_rx_bytes; }

int main(void) { return 0; }
