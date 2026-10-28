// Test program for library_webserial.js: thin C wrappers over the POSIX calls, driven step by step from
// run_test.js (which provides a mock navigator.serial under Node). See ../README.md for the build command.
#include <emscripten.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "../webserial.h"

#define MAX_EVENTS 256
static int g_event[MAX_EVENTS], g_index[MAX_EVENTS], g_count;
static unsigned char g_buf[1 << 21];

static void on_event(int event, int index, void* user) {
    (void)user;
    if (g_count < MAX_EVENTS) { g_event[g_count] = event; g_index[g_count] = index; ++g_count; }
}

EMSCRIPTEN_KEEPALIVE void t_start(void) { g_count = 0; webserial_set_callback(on_event, 0); }
EMSCRIPTEN_KEEPALIVE int t_event_count(void) { return g_count; }
EMSCRIPTEN_KEEPALIVE int t_event(int i) { return g_event[i]; }
EMSCRIPTEN_KEEPALIVE int t_event_index(int i) { return g_index[i]; }
EMSCRIPTEN_KEEPALIVE void t_clear_events(void) { g_count = 0; }
EMSCRIPTEN_KEEPALIVE unsigned char* t_buf(void) { return g_buf; }

EMSCRIPTEN_KEEPALIVE int t_open(int index) {
    char path[32];
    strcpy(path, "/dev/ttyWeb0");
    path[11] = (char)('0' + index);
    return open(path, O_RDWR | O_NONBLOCK);
}
EMSCRIPTEN_KEEPALIVE int t_errno(void) { return errno; }
// Reads up to max bytes into t_buf(); returns the count, or -errno
EMSCRIPTEN_KEEPALIVE int t_read(int fd, int max) {
    ssize_t n = read(fd, g_buf, (size_t)max);
    return n < 0 ? -errno : (int)n;
}
EMSCRIPTEN_KEEPALIVE int t_write(int fd, int n) {
    ssize_t r = write(fd, g_buf, (size_t)n);
    return r < 0 ? -errno : (int)r;
}
EMSCRIPTEN_KEEPALIVE int t_poll(int fd) {
    struct pollfd p = { fd, POLLIN | POLLOUT, 0 };
    int r = poll(&p, 1, 0);
    return r < 0 ? -errno : p.revents;
}
EMSCRIPTEN_KEEPALIVE int t_close(int fd) { return close(fd); }
EMSCRIPTEN_KEEPALIVE int t_get_baud(int fd) {
    struct termios t;
    if (tcgetattr(fd, &t) != 0) return -errno;
    switch (cfgetospeed(&t)) {
        case B9600: return 9600;
        case B115200: return 115200;
        case B921600: return 921600;
        default: return 0;
    }
}
EMSCRIPTEN_KEEPALIVE int t_set_baud(int fd, int baud) {
    struct termios t;
    if (tcgetattr(fd, &t) != 0) return -errno;
    cfsetspeed(&t, baud == 921600 ? B921600 : baud == 9600 ? B9600 : B115200);
    return tcsetattr(fd, TCSANOW, &t) == 0 ? 0 : -errno;
}
EMSCRIPTEN_KEEPALIVE int t_isatty(int fd) { return isatty(fd); }

int main(void) { return 0; }
