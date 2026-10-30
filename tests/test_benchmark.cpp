// The logic of the transmit benchmark (apps/dearfdcan_web/benchmark.*): pacing, frames, bus time and the evaluation
#include "benchmark.h"
#include <cmath>
#include <cstdio>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_fails; std::printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

static BenchConfig MakeConfig() {
    BenchConfig c;
    c.tx_dev = 1;
    c.rx_dev = 2;
    c.fd = true;
    c.brs = true;
    c.bytes = 8;
    c.window = 100;
    c.duration_s = 2;
    return c;
}

static void TestBitTime() {
    // classic 8 bytes at 500 kbit/s: 47 + 64 bits = 111 bits = 222 us
    CHECK(std::fabs(TxBenchmark::FrameBusUs(false, false, false, 8, 500, 2000) - 222.0) < 0.01);
    CHECK(std::fabs(TxBenchmark::FrameBusUs(false, false, true, 8, 500, 2000) - 262.0) < 0.01);       // 67 + 64
    CHECK(std::fabs(TxBenchmark::FrameBusUs(false, false, false, 0, 1000, 2000) - 47.0) < 0.01);
    // CAN FD 64 bytes with BRS: the data phase is faster than without
    const double brs = TxBenchmark::FrameBusUs(true, true, false, 64, 500, 2000);
    const double no_brs = TxBenchmark::FrameBusUs(true, false, false, 64, 500, 2000);
    CHECK(brs < no_brs && brs > 0.0);
    CHECK(std::fabs(brs - ((16 + 13) * 2.0 + (10 + 512 + 21) * 0.5)) < 0.01);
    // lengths: classic stays within 8, FD rounds up to a DLC length
    CHECK(TxBenchmark::NormalizeBytes(false, 20) == 8 && TxBenchmark::NormalizeBytes(true, 9) == 12 && TxBenchmark::NormalizeBytes(true, 33) == 48 &&
          TxBenchmark::NormalizeBytes(true, 100) == 64 && TxBenchmark::NormalizeBytes(true, -3) == 0);
}

static void TestMaxMode() {
    TxBenchmark b;
    CHECK(b.state() == TxBenchmark::State::Idle && b.Due(0, 0) == 0);
    BenchStart base;
    base.echo = 0;
    b.Start(MakeConfig(), 500, 2000, 1000.0, base);
    CHECK(b.state() == TxBenchmark::State::Running && b.TestId() == 0x501);

    // the window is filled at once, then nothing more until echoes come back
    int n = b.Due(1000.0, 0);
    CHECK(n == 100);
    for (int i = 0; i < n; ++i) {
        CanMessage m = b.NextFrame();
        CHECK(m.id == 0x501 && m.is_fd && m.is_brs && m.dlc == 8 && m.payload.size() == 8);
        if (i == 5) CHECK(m.payload[0] == 5 && m.payload[1] == 0);
        if (i == 99) CHECK(m.payload[0] == 99);
    }
    CHECK(b.sent() == 100 && b.Due(1010.0, 0) == 0);
    CHECK(b.Due(1020.0, 40) == 40);                   // 40 echoes: 40 free places
    for (int i = 0; i < 40; ++i) b.NextFrame();
    CHECK(b.Due(1030.0, 40) == 0 && b.Due(1040.0, 140) == 100);
    for (int i = 0; i < 100; ++i) b.NextFrame();

    // the sequence number is 32 bit little endian
    CanMessage m = b.NextFrame();
    CHECK(m.payload[0] == (240 & 255) && m.payload[1] == 0);

    // the duration ends the sending; the result is made after the drain time
    CHECK(b.Due(3000.0, 240) == 0 && b.state() == TxBenchmark::State::Draining && b.Active());
    BenchCounters now;
    now.echo = 241;
    now.rx = 240;
    now.rx_max_gap_ms = 3.5;
    now.tx_errors = 0;
    now.tx_gate_ms = 1000;                                        // the FIFO gate held the queue for half of the 2 s
    now.tx_transfers = 40;
    now.tx_frames = 241;
    b.Update(3100.0, now);
    CHECK(b.state() == TxBenchmark::State::Draining);            // too early
    b.Update(4300.0, now);
    CHECK(b.state() == TxBenchmark::State::Done && !b.Active());
    const BenchResult& r = b.result();
    CHECK(r.valid && r.stop_reason == "finished" && r.sent == 241 && r.echoed == 241 && r.received == 240 && r.has_receiver);
    CHECK(r.lost == 1 && r.echoes_missing == 0 && std::fabs(r.seconds - 2.0) < 1e-9 && std::fabs(r.fps - 120.0) < 1e-6);   // the rate counts what the receiver got
    CHECK(r.max_fps > 0 && std::fabs(r.efficiency_pct - 100.0 * r.fps / r.max_fps) < 1e-9 && std::fabs(r.rx_max_gap_ms - 3.5) < 1e-9);
    CHECK(std::fabs(r.data_kbps - 120.0 * 64 / 1000.0) < 1e-6);
    CHECK(std::fabs(b.elapsed_ms(9000.0) - 2000.0) < 1e-9);       // the time of the sending stands still once it is stopped
    CHECK(std::fabs(r.gate_closed_pct - 50.0) < 1e-9 && r.tx_transfers == 40 && r.tx_frames == 241 && std::fabs(r.frames_per_transfer - 241.0 / 40.0) < 1e-9);
    CHECK(r.window == 100 && std::fabs(r.ui_fps - 2.5) < 1e-9 && std::fabs(r.avg_waiting - 52.0) < 1e-9 && r.unechoed == 0);   // 5 feeds in 2 s, 260 waiting in all
    CHECK(b.Due(4000.0, 241) == 0);                               // done: nothing more
}

static void TestBaseAndFixedRate() {
    TxBenchmark b;
    BenchConfig c = MakeConfig();
    c.rate_fps = 100;
    c.duration_s = 30;
    c.rx_dev = -1;
    c.fd = false;                 // classic: no BRS, 8 bytes at most
    c.bytes = 20;
    BenchStart base;
    base.echo = 7;
    base.tx_errors = 3;
    b.Start(c, 500, 2000, 0.0, base);
    CHECK(b.config().bytes == 8 && !b.config().brs);
    CanMessage m = b.NextFrame();
    CHECK(!m.is_fd && !m.is_brs && m.dlc == 8);

    // a fixed rate does not look at the echoes: 100 frames/s is one frame per 10 ms
    CHECK(b.Due(5.0, 7) == 0);
    CHECK(b.Due(10.0, 7) == 1);
    CHECK(b.Due(110.0, 7) == 10);
    CHECK(b.Due(5000.0, 7) == 10);                                // after a stall: at most 100 ms of catching up
    b.Stop(5000.0, "stopped");
    BenchCounters now;
    now.echo = 7 + 50;
    now.tx_errors = 5;
    b.Update(6300.0, now);
    CHECK(b.state() == TxBenchmark::State::Done && b.result().echoed == 50 && !b.result().has_receiver && b.result().lost == 0);
    CHECK(b.result().tx_errors == 2 && b.result().stop_reason == "stopped");
}

static void TestNoEchoAbort() {
    TxBenchmark b;
    BenchConfig c = MakeConfig();
    c.duration_s = 30;
    b.Start(c, 500, 2000, 0.0, BenchStart());
    CHECK(b.Due(0.0, 0) == 100);
    for (int i = 0; i < 100; ++i) b.NextFrame();
    CHECK(b.Due(500.0, 0) == 0 && b.state() == TxBenchmark::State::Running);
    CHECK(b.Due(1100.0, 0) == 0 && b.state() == TxBenchmark::State::Draining);
    b.Update(2400.0, BenchCounters());
    CHECK(b.state() == TxBenchmark::State::Done && b.result().stop_reason.find("no tx echo") != std::string::npos);
    CHECK(b.result().sent == 100 && b.result().echoed == 0 && b.result().fps > 0);      // without echoes the sent frames are the basis
}

// Frames that arrived without an echo are not lost; frames that never arrived are, whatever the echoes say
static void TestLostAndMissingEchoes() {
    for (int variant = 0; variant < 2; ++variant) {
        TxBenchmark b;
        BenchConfig c = MakeConfig();
        c.rate_fps = 100;
        c.duration_s = 30;
        b.Start(c, 500, 2000, 0.0, BenchStart());
        for (int i = 0; i < 100; ++i) b.NextFrame();
        b.Stop(1000.0, "finished");
        BenchCounters now;
        if (variant == 0) { now.echo = 90; now.rx = 100; }        // 10 echoes went missing, every frame arrived
        else              { now.echo = 90; now.rx = 85; }         // 15 frames never arrived (5 of them had an echo)
        b.Update(2300.0, now);
        const BenchResult& r = b.result();
        CHECK(r.valid && r.has_receiver && r.unechoed == 10);
        if (variant == 0) CHECK(r.lost == 0 && r.echoes_missing == 10 && std::fabs(r.fps - 100.0) < 1e-6);
        else              CHECK(r.lost == 15 && r.echoes_missing == 0 && std::fabs(r.fps - 85.0) < 1e-6);
    }
}

// With a receiver the window is also freed by the frames it counts, so echoes that go missing do not clog the window
static void TestWindowFollowsReceiver() {
    TxBenchmark b;
    BenchConfig c = MakeConfig();                                 // has a receiver, window 100
    c.duration_s = 30;
    b.Start(c, 500, 2000, 0.0, BenchStart());
    CHECK(b.Due(0.0, 0, 0) == 100);
    for (int i = 0; i < 100; ++i) b.NextFrame();
    CHECK(b.Due(10.0, 0, 0) == 0);
    CHECK(b.Due(20.0, 10, 60) == 60);                             // 60 arrived, 10 echoes only: the larger count frees the window
    for (int i = 0; i < 60; ++i) b.NextFrame();
    CHECK(b.Due(30.0, 70, 60) == 10);                             // echoes ahead of the receiver count the same way
    BenchConfig noRx = c;
    noRx.rx_dev = -1;
    TxBenchmark e;
    e.Start(noRx, 500, 2000, 0.0, BenchStart());
    CHECK(e.Due(0.0, 0, 0) == 100);
    for (int i = 0; i < 100; ++i) e.NextFrame();
    CHECK(e.Due(20.0, 10, 60) == 10);                             // no receiver chosen: a receiver count would be ignored
}

int main() {
    TestWindowFollowsReceiver();
    TestLostAndMissingEchoes();
    TestBitTime();
    TestMaxMode();
    TestBaseAndFixedRate();
    TestNoEchoAbort();
    std::printf("%d checks, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
