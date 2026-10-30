// The mapping of an adapter's 32 bit microsecond clock onto the page clock (core: ClockSync): the fit through clock samples (answers to
// GS_USB_BREQ_TIMESTAMP) against a simulated clock with drift, noisy round trips and a 32 bit wrap.
#include "can_wire.h"
#include <algorithm>
#include <cstdio>
#include <random>

extern "C" {
uint8_t* core_stage_ptr(); void core_process(int); int core_publish(); uint8_t* core_out_ptr();
void core_reset_clock(int); void core_set_protocol(int, int); void core_clock_sample(int, uint32_t, double, double); double* core_clock_stats(int);
}
static int g_fail = 0, g_n = 0;
#define CHECK(c) do { ++g_n; if (!(c)) { ++g_fail; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

// One legacy gs_usb classic frame with timestamp, delivered at host time `host_ms`; returns the frame time the core assigned
static double feed(uint32_t hw_us, double host_ms, int dev = 0) {
    uint8_t f[24] = {}; uint32_t echo = 0xFFFFFFFF, id = 0x100; std::memcpy(f, &echo, 4); std::memcpy(f + 4, &id, 4); f[8] = 1; std::memcpy(f + 20, &hw_us, 4);
    uint8_t* st = core_stage_ptr(); uint16_t len = 24; std::memcpy(st, &len, 2); st[2] = (uint8_t)dev; st[3] = 0x81; std::memcpy(st + 4, &host_ms, 8); std::memcpy(st + 12, f, 24);
    core_process(36);
    core_publish(); SnapshotHeader h; std::memcpy(&h, core_out_ptr(), sizeof(h));
    if (!h.n_trace) return -1;
    TraceEntry te; std::memcpy(&te, core_out_ptr() + sizeof(h) + h.n_table * sizeof(WireTableRecord) + (h.n_trace - 1) * sizeof(TraceEntry), sizeof(te));
    return te.msg.host_time_ms;
}

int main() {
    std::mt19937 rng(7); std::uniform_real_distribution<double> U(0, 1); std::exponential_distribution<double> E(1.0 / 0.6);
    const double drift = 80e-6;                // the adapter clock runs 80 ppm fast
    const double hw0 = 4294967296.0 - 5.0e6;   // 5 s before a 32 bit wrap (µs)
    const double host0 = 1000.0;               // host ms at the start
    auto adapter_us = [&](double host_ms) { return (uint32_t)(uint64_t)(hw0 + (host_ms - host0) * 1000.0 * (1 + drift)); };

    core_set_protocol(0, 0); core_reset_clock(0);
    // frames only: the first frame is placed at its arrival time, then the minimum tracker takes over
    double t0 = feed(adapter_us(host0 + 5), host0 + 5.7); CHECK(t0 == host0 + 5.7);

    // samples: one per second for 70 s (the ring keeps 64)
    for (int i = 1; i <= 70; ++i) {
        double t_true = host0 + 1000.0 * i;                  // the moment the adapter latches its clock
        double rtt = 0.8 + E(rng) + (U(rng) < 0.15 ? 4.0 * U(rng) : 0);           // mostly short, sometimes long
        double lead = U(rng) * rtt;                           // the request was sent `lead` before the latch
        double s0 = t_true - lead, s1 = s0 + rtt;
        core_clock_sample(0, adapter_us(t_true), (s0 + s1) / 2, rtt);
    }
    double* s = core_clock_stats(0);
    std::printf("samples %.0f good %.0f drift %.1f ppm spread %.0f us worst %.0f us rtt min %.2f last %.2f\n", s[0], s[1], s[3], s[4], s[7], s[5], s[6]);
    CHECK(s[0] == 70 && s[1] >= 20 && s[1] < 70);
    CHECK(s[3] > 60 && s[3] < 100);             // +80 ppm: the adapter runs fast
    CHECK(s[4] > 50 && s[4] < 700);             // the spread is sub-millisecond
    CHECK(s[5] > 0.7 && s[5] < 1.5);

    // mapped frame time: a frame that really happened at T arrives 1.3 ms later
    double worst_err = 0;
    for (int k = 0; k < 20; ++k) {
        double T = host0 + 71000.0 + 137.0 * k;
        double got = feed(adapter_us(T), T + 1.3);
        worst_err = std::max(worst_err, std::abs(got - T));
        CHECK(got <= T + 1.3 + 1e-9);            // never later than the moment it was handled
    }
    std::printf("worst frame time error %.3f ms\n", worst_err);
    CHECK(worst_err < 1.5);

    // reset forgets the samples
    core_reset_clock(0); s = core_clock_stats(0); CHECK(s[0] == 0 && s[1] == 0);

    // a restart of the adapter clock (a big step backwards) in the samples starts over
    core_clock_sample(0, 5000000, 1000.0, 1.0); core_clock_sample(0, 6000000, 2000.0, 1.0); core_clock_sample(0, 7000000, 3000.0, 1.0);
    core_clock_sample(0, 100, 4000.0, 1.0);
    s = core_clock_stats(0); CHECK(s[0] == 1);
    // a small step backwards (an older frame behind a newer sample) is jitter, not a restart
    core_reset_clock(0); core_clock_sample(0, 1000000, 100.0, 1.0); core_clock_sample(0, 2000000, 1100.0, 1.0); core_clock_sample(0, 3000000, 2100.0, 1.0);
    feed(2999000, 2100.5); s = core_clock_stats(0); CHECK(s[0] == 3);
    // a negative round trip is clamped, one sample does not crash
    core_reset_clock(0); core_clock_sample(0, 1, 1.0, -3.0); s = core_clock_stats(0); CHECK(s[0] == 1 && s[1] == 1 && s[4] == 0);
    // without samples the output stays monotonic
    core_reset_clock(0); double a = feed(1000, 50.0), b = feed(2000, 50.4), c = feed(1500 + 2000, 51.0); CHECK(a == 50.0 && b >= a && c >= b);
    std::printf("%d checks, %d failed\n", g_n, g_fail);
    return g_fail != 0;
}
