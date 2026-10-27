#pragma once
// Wire format shared by the worker module (can_core.cpp) and the page (webusb_bridge.cpp).
// Both are wasm32 builds of the same headers, so the plain-data records below are exchanged by memcpy.
#include "can_protocol.h"

// Unique 64-bit lookup key for the ID table (combines the 29-bit/11-bit ID and the EXT flag)
inline uint64_t MakeCanIdKey(uint32_t id, bool is_extended) {
    return (static_cast<uint64_t>(is_extended ? 1 : 0) << 32) | static_cast<uint64_t>(id);
}

// One changed ID table row
struct WireTableRecord {
    uint64_t key;
    IdTableEntry entry;
};

static_assert(std::is_trivially_copyable<WireTableRecord>::value, "WireTableRecord must be memcpy-able");

constexpr uint32_t kSnapshotMagic = 0x31534E43U; // "CNS1"

// Snapshot layout: SnapshotHeader | n_table x WireTableRecord | n_trace x TraceEntry
// Times inside the records are in the page's performance.now() timeline (milliseconds, trace in seconds).
struct SnapshotHeader {
    uint32_t magic;
    uint32_t seq;                  // consecutive per published snapshot
    uint32_t n_table;              // changed ID rows (each row holds the latest state: updates coalesce)
    uint32_t n_trace;              // new trace entries, oldest first
    uint32_t table_rec_size;       // sizeof(WireTableRecord) of the producer (layout sanity check)
    uint32_t trace_rec_size;       // sizeof(TraceEntry) of the producer
    uint32_t table_clear_ack;      // number of "clear table" commands the core has processed
    uint32_t trace_clear_ack;      // number of "clear trace" commands the core has processed
    uint32_t trace_dropped_total;  // trace entries discarded because the page did not keep up
    uint32_t frames_total;         // frames parsed so far
    uint32_t table_overflow_total; // frames of new IDs ignored because the table is full
    uint32_t reserved;
};

static_assert(sizeof(SnapshotHeader) % 8 == 0, "snapshot records must start 8-byte aligned");
static_assert(sizeof(WireTableRecord) % 8 == 0, "table records must stay 8-byte aligned");
static_assert(sizeof(TraceEntry) % 8 == 0, "trace records must stay 8-byte aligned");

// Read-only view of a validated snapshot handed to the UI. Records are not guaranteed to be aligned
// for direct access in every caller; copy them out with memcpy.
struct SnapshotView {
    SnapshotHeader header;
    const uint8_t* table = nullptr; // n_table records, or nullptr when stale (cleared meanwhile)
    uint32_t n_table = 0;
    const uint8_t* trace = nullptr;
    uint32_t n_trace = 0;
};
