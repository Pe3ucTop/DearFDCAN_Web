#include "benchmark.h"
#include <algorithm>
#include <cmath>

namespace {
constexpr double kDrainMs = 1200.0;         // after the last frame: time for the echoes, the receiver's frames and the worker's once-a-second statistics
constexpr double kNoEchoMs = 1000.0;        // max mode: no echo this long after the start means the adapter does not send any
constexpr int kMaxDueAtOnce = 4000;
}

int TxBenchmark::NormalizeBytes(bool fd, int bytes) {
    bytes = std::clamp(bytes, 0, fd ? 64 : 8);
    if (!fd || bytes <= 8) return bytes;
    static const int sizes[] = {12, 16, 20, 24, 32, 48, 64};
    for (int s : sizes) if (bytes <= s) return s;
    return 64;
}

double TxBenchmark::FrameBusUs(bool fd, bool brs, bool extended, int bytes, int nom_kbps, int data_kbps) {
    bytes = NormalizeBytes(fd, bytes);
    nom_kbps = std::max(nom_kbps, 1);
    data_kbps = std::max(data_kbps, 1);
    if (!fd) return (((extended ? 67 : 47) + 8 * bytes) * 1000.0) / nom_kbps;
    // CAN FD: the arbitration part, BRS bit, ACK, EOF and the interframe space at the nominal rate; ESI, DLC, data, CRC (with its stuff count
    // and delimiter) at the data rate when bit rate switching is on
    const int head = (extended ? 35 : 16) + 13;
    const int crc = bytes <= 16 ? 17 : 21;
    const int data = 10 + 8 * bytes + crc;
    if (!brs) return ((head + data) * 1000.0) / nom_kbps;
    return (head * 1000.0) / nom_kbps + (data * 1000.0) / data_kbps;
}

void TxBenchmark::Start(const BenchConfig& cfg, int nom_kbps, int data_kbps, double now_ms, const BenchStart& base) {
    cfg_ = cfg;
    cfg_.bytes = NormalizeBytes(cfg_.fd, cfg_.bytes);
    cfg_.window = std::clamp(cfg_.window, 1, 200);
    cfg_.duration_s = std::max(cfg_.duration_s, 1);
    if (!cfg_.fd) cfg_.brs = false;
    nom_kbps_ = nom_kbps;
    data_kbps_ = data_kbps;
    base_ = base;
    state_ = State::Running;
    start_ms_ = now_ms;
    last_due_ms_ = now_ms;
    rate_credit_ = 0.0;
    sent_ = 0;
    feeds_ = 0;
    waiting_sum_ = 0.0;
    stop_ms_ = 0.0;
    reason_.clear();
    result_ = BenchResult();
}

void TxBenchmark::Stop(double now_ms, const char* reason) {
    if (state_ != State::Running) return;
    state_ = State::Draining;
    stop_ms_ = now_ms;
    reason_ = reason ? reason : "stopped";
}

int TxBenchmark::Due(double now_ms, uint64_t echo, uint64_t rx) {
    if (state_ != State::Running) return 0;
    const double elapsed = now_ms - start_ms_;
    if (elapsed >= cfg_.duration_s * 1000.0) { Stop(now_ms, "finished"); return 0; }

    ++feeds_;
    if (cfg_.rate_fps > 0) {
        rate_credit_ += (now_ms - last_due_ms_) * cfg_.rate_fps / 1000.0;
        last_due_ms_ = now_ms;
        rate_credit_ = std::min(rate_credit_, std::max(1.0, cfg_.rate_fps / 10.0));      // after a stall: no more than 100 ms of catching up
        const int n = std::min(static_cast<int>(rate_credit_), kMaxDueAtOnce);
        rate_credit_ -= n;
        return n;
    }

    const uint64_t echoed = echo >= base_.echo ? echo - base_.echo : 0;
    const uint64_t received = cfg_.rx_dev >= 0 && rx >= base_.rx ? rx - base_.rx : 0;
    const uint64_t done = std::max(echoed, received);
    if (elapsed >= kNoEchoMs && done == 0) { Stop(now_ms, "aborted: the adapter returns no tx echo (use a fixed rate)"); return 0; }
    const uint64_t waiting = sent_ > done ? sent_ - done : 0;
    waiting_sum_ += static_cast<double>(waiting);
    return waiting >= static_cast<uint64_t>(cfg_.window) ? 0 : static_cast<int>(cfg_.window - waiting);
}

CanMessage TxBenchmark::NextFrame() {
    CanMessage m;
    m.id = TestId();
    m.is_extended = cfg_.extended;
    m.is_fd = cfg_.fd;
    m.is_brs = cfg_.fd && cfg_.brs;
    m.dlc = cfg_.fd ? CanProtocolUtils::BytesToDlc(static_cast<uint8_t>(cfg_.bytes)) : static_cast<uint8_t>(cfg_.bytes);
    m.payload.resize(static_cast<size_t>(cfg_.bytes));
    const uint32_t seq = static_cast<uint32_t>(sent_);
    for (int i = 0; i < cfg_.bytes; ++i) m.payload[i] = i < 4 ? static_cast<uint8_t>(seq >> (8 * i)) : static_cast<uint8_t>(i);
    m.dev = static_cast<uint8_t>(cfg_.tx_dev < 0 ? 0 : cfg_.tx_dev);
    ++sent_;
    return m;
}

void TxBenchmark::Update(double now_ms, const BenchCounters& now) {
    if (state_ == State::Running) {
        if (now_ms - start_ms_ >= cfg_.duration_s * 1000.0) Stop(now_ms, "finished");
        return;
    }
    if (state_ != State::Draining || now_ms - stop_ms_ < kDrainMs) return;

    BenchResult r;
    r.valid = true;
    r.stop_reason = reason_;
    r.seconds = std::max((stop_ms_ - start_ms_) / 1000.0, 0.001);
    r.sent = sent_;
    r.echoed = now.echo >= base_.echo ? now.echo - base_.echo : 0;
    r.has_receiver = cfg_.rx_dev >= 0;
    r.received = now.rx >= base_.rx ? now.rx - base_.rx : 0;
    // What was on the bus: the receiver's count when there is one (the sender's echoes can go missing although the frames arrive), else the echoes
    const uint64_t basis = r.has_receiver && r.received > 0 ? r.received : (r.echoed > 0 ? r.echoed : r.sent);
    const uint64_t known = std::max(r.sent, r.echoed);
    r.lost = r.has_receiver ? static_cast<int64_t>(known) - static_cast<int64_t>(r.received) : 0;
    r.echoes_missing = r.has_receiver && r.received > r.echoed ? static_cast<int64_t>(r.received - r.echoed) : 0;
    r.fps = basis / r.seconds;
    r.data_kbps = r.fps * cfg_.bytes * 8.0 / 1000.0;
    r.bus_us_per_frame = FrameBusUs(cfg_.fd, cfg_.brs, cfg_.extended, cfg_.bytes, nom_kbps_, data_kbps_);
    r.max_fps = r.bus_us_per_frame > 0.0 ? 1e6 / r.bus_us_per_frame : 0.0;
    r.efficiency_pct = r.max_fps > 0.0 ? 100.0 * r.fps / r.max_fps : 0.0;
    r.busload_pct = r.fps * r.bus_us_per_frame / 1e4;
    r.rx_max_gap_ms = now.rx_max_gap_ms;
    r.tx_errors = now.tx_errors >= base_.tx_errors ? now.tx_errors - base_.tx_errors : 0;
    r.tx_dropped = now.tx_dropped >= base_.tx_dropped ? now.tx_dropped - base_.tx_dropped : 0;
    r.adapter_busload_pct = now.bus_load_pct;
    r.ui_fps = feeds_ / r.seconds;
    r.avg_waiting = feeds_ > 0 ? waiting_sum_ / feeds_ : 0.0;
    r.window = cfg_.rate_fps > 0 ? 0 : cfg_.window;
    r.unechoed = static_cast<int64_t>(r.sent) - static_cast<int64_t>(r.echoed);
    const uint32_t gate_ms = now.tx_gate_ms >= base_.tx_gate_ms ? now.tx_gate_ms - base_.tx_gate_ms : 0;
    r.gate_closed_pct = std::min(100.0, gate_ms / ((stop_ms_ - start_ms_) > 1.0 ? (stop_ms_ - start_ms_) : 1.0) * 100.0);
    r.tx_transfers = now.tx_transfers >= base_.tx_transfers ? now.tx_transfers - base_.tx_transfers : 0;
    r.tx_frames = now.tx_frames >= base_.tx_frames ? now.tx_frames - base_.tx_frames : 0;
    r.frames_per_transfer = r.tx_transfers > 0 ? static_cast<double>(r.tx_frames) / r.tx_transfers : 0.0;
    result_ = r;
    state_ = State::Done;
}
