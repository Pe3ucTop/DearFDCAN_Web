#pragma once
#include "can_protocol_common.h"
#include <cstdint>
#include <string>

// Transmit stress test (the idea is the "100% bus load" demo of the ElmueSoft firmware): one adapter sends numbered frames with an ID of its own
// as fast as the bus takes them, or at a fixed rate; what came back (tx echoes of that adapter, the frames a second adapter on the same bus
// received) says how many were really sent and whether any were lost. The class holds the logic only (pacing, bus time, evaluation); the
// window and the counters it is fed come from the user interface, so it can be tested without a browser.

struct BenchConfig {
    int tx_dev = -1;             // adapter that sends
    int rx_dev = -1;             // adapter on the same bus that counts what it receives, -1 = none (the tx echoes count only)
    bool fd = true;
    bool brs = true;
    bool extended = false;
    int bytes = 8;               // data bytes: 0..8, or a CAN FD length up to 64
    int rate_fps = 0;            // 0 = as fast as the echoes allow; else frames per second (no echo needed)
    int window = 150;            // frames that may wait for their echo (max mode); below 255, the marker space of the protocol
    int duration_s = 10;
    uint32_t id = 0x500;         // ID of the test frames; the adapter number is added so that two adapters never collide
};

struct BenchStart {              // the counters at the start; the results are the differences
    uint64_t echo = 0;           // tx echoes of the sender with the test ID
    uint64_t rx = 0;             // frames of the receiver with the test ID
    uint32_t tx_errors = 0, tx_dropped = 0;
    uint32_t tx_gate_ms = 0, tx_transfers = 0, tx_frames = 0;     // the sender's transport counters (cumulative)
};

struct BenchCounters {           // the counters now
    uint64_t echo = 0;
    uint64_t rx = 0;
    double rx_max_gap_ms = 0.0;  // largest gap between two frames at the receiver (the table keeps it for the ID)
    uint32_t tx_errors = 0, tx_dropped = 0;
    uint32_t tx_gate_ms = 0, tx_transfers = 0, tx_frames = 0;
    int bus_load_pct = -1;       // reported by the sender's adapter, -1 = not
};

struct BenchResult {
    bool valid = false;
    std::string stop_reason;     // "finished", "stopped", or why it was aborted
    double seconds = 0.0;
    uint64_t sent = 0;           // frames handed to the adapter
    uint64_t echoed = 0;         // frames the sender reports as put on the bus
    uint64_t received = 0;       // frames the receiver got (when there is one)
    bool has_receiver = false;
    int64_t lost = 0;            // frames that never reached the receiver: sent (or echoed, if more) minus received
    int64_t echoes_missing = 0;  // frames the receiver got although the sender reported no echo for them (needs a receiver)
    double fps = 0.0;            // frames per second that were on the bus: received, else echoed, else sent
    double data_kbps = 0.0;      // payload bytes per second * 8 / 1000
    double bus_us_per_frame = 0.0;
    double max_fps = 0.0;        // what the bit rates allow at most (no stuff bits)
    double efficiency_pct = 0.0; // fps / max_fps
    double busload_pct = 0.0;    // computed from the achieved rate
    double rx_max_gap_ms = 0.0;
    uint32_t tx_errors = 0, tx_dropped = 0;
    int adapter_busload_pct = -1;
    // Where the rate is limited: the sender is fed once per page frame, so a slow page limits it (max mode: window / time between frames)
    double ui_fps = 0.0;         // times per second the page fed the sender
    double avg_waiting = 0.0;    // average number of frames that waited for their echo when the page fed the sender (max mode)
    int window = 0;              // the window of this run (0 in the fixed rate mode)
    int64_t unechoed = 0;        // sent minus echoed: frames that never reached the bus (a failed transfer loses all frames in it)
    // Where the rate is limited below the page: how long the adapter's FIFO had no free slot (echoes come back late), and how many frames
    // one USB transfer carried (a TX blob). Both are counted by the worker for the time of the run and the drain.
    double gate_closed_pct = 0.0;
    uint32_t tx_transfers = 0, tx_frames = 0;
    double frames_per_transfer = 0.0;
};

class TxBenchmark {
public:
    enum class State { Idle, Running, Draining, Done };

    // nom_kbps / data_kbps: the bit rates of the sender's channel
    void Start(const BenchConfig& cfg, int nom_kbps, int data_kbps, double now_ms, const BenchStart& base);
    // Ends the sending early (the result is evaluated after the drain time)
    void Stop(double now_ms, const char* reason);

    State state() const { return state_; }
    bool Active() const { return state_ == State::Running || state_ == State::Draining; }
    const BenchConfig& config() const { return cfg_; }
    uint32_t TestId() const { return TestIdOf(cfg_, cfg_.tx_dev); }
    uint64_t sent() const { return sent_; }
    // Time of the sending so far; once it is stopped (draining) the time stands still, so the average rate does not fall during the drain
    double elapsed_ms(double now_ms) const { return (state_ == State::Running ? now_ms : stop_ms_) - start_ms_; }

    // How many frames to hand to the adapter now (0 while waiting, finished or draining). `echo` = echoes so far (absolute).
    // In the max mode a frame counts as done when its tx echo came or the receiver got it (the sender's echoes can go missing under load although
    // the frames are on the bus, which would fill the window with frames nobody waits for): done = the larger of the two counts.
    int Due(double now_ms, uint64_t echo, uint64_t rx = 0);
    // The next frame; its first four data bytes are a little-endian sequence number
    CanMessage NextFrame();
    // Call regularly: ends the drain time and evaluates
    void Update(double now_ms, const BenchCounters& now);
    const BenchResult& result() const { return result_; }

    // Time on the bus and the most frames per second the bit rates allow (no stuff bits, interframe space included)
    static double FrameBusUs(bool fd, bool brs, bool extended, int bytes, int nom_kbps, int data_kbps);
    static uint32_t TestIdOf(const BenchConfig& cfg, int dev) { return cfg.id + static_cast<uint32_t>(dev < 0 ? 0 : dev); }
    static int NormalizeBytes(bool fd, int bytes);       // a length the DLC can express

private:
    BenchConfig cfg_;
    BenchStart base_;
    State state_ = State::Idle;
    double start_ms_ = 0.0, stop_ms_ = 0.0, last_due_ms_ = 0.0, rate_credit_ = 0.0;
    uint64_t sent_ = 0, feeds_ = 0;
    double waiting_sum_ = 0.0;
    int nom_kbps_ = 500, data_kbps_ = 2000;
    std::string reason_;
    BenchResult result_;
};
