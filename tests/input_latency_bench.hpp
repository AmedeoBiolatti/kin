#pragma once

#include <kin/core/json.hpp>
#include <array>
#include <iosfwd>
#include <vector>

// Timings stop at CPU callbacks; no physical keyboard or display is involved.
struct InputLatencySample {
    kin::u64 event_ns = 0;
    // poll, update action, render-time input read, render updated state.
    // -1 means the observation was missing, not a zero-latency sample.
    std::array<double, 4> ms{-1.0, -1.0, -1.0, -1.0};
};

struct InputLatencyResult {
    int sim_hz = 0;
    int render_hz = 0;
    int push_errors = 0;
    int duplicate_updates = 0;
    int duplicate_render_reads = 0;
    int unknown_events = 0;
    std::vector<InputLatencySample> samples;

    bool ok() const;
};

InputLatencyResult benchmark_input_latency(int sim_hz, int render_hz, int samples, int warmup);
void write_input_latency_text(std::ostream& out, const InputLatencyResult& result);
void write_input_latency_json(kin::JsonWriter& json, const InputLatencyResult& result);
