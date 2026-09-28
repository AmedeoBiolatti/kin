#include "input_latency_bench.hpp"

#include <kin/platform/app.hpp>
#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <memory>
#include <ostream>
#include <stdexcept>
#include <thread>

namespace {
using Clock = std::chrono::steady_clock;
constexpr std::array names{"event_to_poll", "event_to_update", "event_to_render_read", "event_to_render_state"};

std::vector<double> metric(const InputLatencyResult& result, std::size_t index) {
    std::vector<double> values;
    for (const auto& sample : result.samples) {
        if (sample.ms[index] >= 0.0) values.push_back(sample.ms[index]);
    }
    std::ranges::sort(values);
    return values;
}

double percentile(const std::vector<double>& sorted, double p) {
    return sorted.empty() ? 0.0 : sorted[static_cast<std::size_t>(std::ceil((sorted.size() - 1) * p))];
}
}

bool InputLatencyResult::ok() const {
    return !push_errors && !duplicate_updates && !duplicate_render_reads && !unknown_events &&
        std::ranges::all_of(samples, [](const auto& sample) {
            return std::ranges::all_of(sample.ms, [](double ms) { return ms >= 0.0; });
        });
}

InputLatencyResult benchmark_input_latency(int sim_hz, int render_hz, int samples, int warmup) {
    if (sim_hz < 10 || sim_hz > 1000 || render_hz < 10 || render_hz > 1000 ||
        samples < 1 || warmup < 0 || samples > 100000 || warmup > 100000) {
        throw std::invalid_argument("input latency requires rates 10..1000 Hz, iterations 1..100000, warmup 0..100000");
    }
    InputLatencyResult result{.sim_hz = sim_hz, .render_hz = render_hz};
    const int count = samples + warmup;
    std::vector<InputLatencySample> observations(static_cast<std::size_t>(count));
    auto stamps = std::make_unique<std::atomic<kin::u64>[]>(count);
    std::atomic<bool> done{false};
    std::atomic<int> push_errors{0};
    kin::App app({.mode = kin::AppMode::Headless, .fixed_dt = 1.0f / sim_hz});
    kin::InputMap map;
    map.bind("latency_action", kin::Key::Space);
    app.input().set_map(std::move(map));

    // Keep taps separated by at least two periods of the slower consumer. A
    // host stall can still coalesce taps; those are reported as missing samples.
    const auto spacing = std::chrono::nanoseconds(2'000'000'000LL / std::min(sim_hz, render_hz));
    std::jthread producer([&](std::stop_token stop) {
        for (int i = 0; i < count && !stop.stop_requested(); ++i) {
            std::this_thread::sleep_for(spacing + std::chrono::microseconds((i * 7919LL) % 17000));
            if (stop.stop_requested()) break;
            SDL_Event event{};
            event.type = SDL_EVENT_KEY_DOWN;
            event.key.scancode = SDL_SCANCODE_SPACE;
            event.key.down = true;
            event.key.timestamp = SDL_GetTicksNS();
            stamps[i].store(event.key.timestamp, std::memory_order_release);
            if (!SDL_PushEvent(&event) && i >= warmup) ++push_errors;
            event.type = SDL_EVENT_KEY_UP;
            event.key.down = false;
            if (!SDL_PushEvent(&event) && i >= warmup) ++push_errors;
        }
        done = true;
    });

    const auto find_sample = [&](kin::u64 stamp) -> int {
        // Stamps are ordered. Published with release before SDL enqueues the tap.
        int lo = 0, hi = count;
        while (lo < hi) {
            const int mid = lo + (hi - lo) / 2;
            const auto value = stamps[mid].load(std::memory_order_acquire);
            if (value && value < stamp) lo = mid + 1;
            else hi = mid;
        }
        return lo < count && stamp && stamps[lo].load(std::memory_order_acquire) == stamp ? lo : -1;
    };
    int updated = -1, rendered = -1;
    auto deadline = Clock::now();
    auto drain_until = Clock::time_point::max();
    const auto render_period = std::chrono::nanoseconds(1'000'000'000LL / render_hz);
    app.run([&](float) {
        if (!app.input().pressed("latency_action")) return;
        const kin::u64 now = SDL_GetTicksNS();
        updated = find_sample(app.input().last_key_press_event_time_ns());
        if (updated < 0) { ++result.unknown_events; return; }
        auto& sample = observations[updated];
        if (sample.ms[1] >= 0.0) {
            if (updated >= warmup) ++result.duplicate_updates;
        } else {
            sample.ms[1] = static_cast<double>(now - app.input().last_key_press_event_time_ns()) / 1e6;
        }
    }, [&](float) {
        const kin::u64 now = SDL_GetTicksNS();
        if (app.input().frame_pressed("latency_action")) {
            const auto stamp = app.input().last_key_press_event_time_ns();
            const int index = find_sample(stamp);
            if (index < 0) ++result.unknown_events;
            else {
                auto& sample = observations[index];
                if (sample.ms[2] >= 0.0) {
                    if (index >= warmup) ++result.duplicate_render_reads;
                } else {
                    sample.ms[0] = static_cast<double>(app.input().last_key_press_detected_time_ns() - stamp) / 1e6;
                    sample.ms[2] = static_cast<double>(now - stamp) / 1e6;
                }
            }
        }
        if (updated >= 0 && updated != rendered) {
            rendered = updated;
            observations[updated].ms[3] = static_cast<double>(now - stamps[updated].load()) / 1e6;
        }
        if (done && drain_until == Clock::time_point::max()) drain_until = Clock::now() + spacing * 2;
        if (Clock::now() >= drain_until) app.quit();
        deadline += render_period;
        // Do not burst through obsolete render deadlines following a host stall.
        if (deadline < Clock::now()) deadline = Clock::now();
        std::this_thread::sleep_until(deadline);
    });
    producer.join();
    result.push_errors = push_errors;
    for (int i = warmup; i < count; ++i) {
        observations[i].event_ns = stamps[i].load();
        result.samples.push_back(observations[i]);
    }
    return result;
}

void write_input_latency_text(std::ostream& out, const InputLatencyResult& result) {
    out << "  software input latency (no keyboard hardware, GPU or display): sim=" << result.sim_hz
        << "Hz render=" << result.render_hz << "Hz taps=" << result.samples.size()
        << " push_errors=" << result.push_errors << " duplicate_updates=" << result.duplicate_updates
        << " duplicate_render_reads=" << result.duplicate_render_reads << " unknown_events=" << result.unknown_events << '\n';
    for (std::size_t i = 0; i < names.size(); ++i) {
        const auto values = metric(result, i);
        out << "    " << names[i] << " samples=" << values.size() << " missing=" << result.samples.size() - values.size();
        if (!values.empty()) out << std::fixed << std::setprecision(3)
            << " median_ms=" << percentile(values, 0.5) << " p95_ms=" << percentile(values, 0.95)
            << " p99_ms=" << percentile(values, 0.99) << " max_ms=" << values.back();
        out << '\n';
    }
}

void write_input_latency_json(kin::JsonWriter& json, const InputLatencyResult& result) {
    json.begin_object().field("measurement", "synthetic_sdl_event_to_cpu_callback")
        .field("sim_hz", result.sim_hz).field("render_hz", result.render_hz).field("ok", result.ok())
        .field("taps", static_cast<kin::u64>(result.samples.size())).field("push_errors", result.push_errors)
        .field("duplicate_updates", result.duplicate_updates).field("duplicate_render_reads", result.duplicate_render_reads)
        .field("unknown_events", result.unknown_events);
    json.key("metrics").begin_array();
    for (std::size_t i = 0; i < names.size(); ++i) {
        const auto values = metric(result, i);
        json.begin_object().field("name", names[i]).field("samples", static_cast<kin::u64>(values.size()))
            .field("missing", static_cast<kin::u64>(result.samples.size() - values.size()));
        if (!values.empty()) json.field("median_ms", percentile(values, 0.5)).field("p95_ms", percentile(values, 0.95))
            .field("p99_ms", percentile(values, 0.99)).field("max_ms", values.back());
        json.end_object();
    }
    json.end_array().key("sample_details").begin_array();
    for (const auto& sample : result.samples) {
        json.begin_object().field("event_ns", sample.event_ns);
        for (std::size_t i = 0; i < names.size(); ++i) {
            json.key(names[i]);
            if (sample.ms[i] < 0.0) json.value_null();
            else json.value(sample.ms[i]);
        }
        json.end_object();
    }
    json.end_array().end_object();
}
