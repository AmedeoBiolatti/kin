#include <kin/runtime/render_probe.hpp>

#include <kin/core/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace kin {
namespace {

// Flicker scores divide by the neighbours' change; this keeps a perfect A-B-A
// (no change between the neighbours) finite.
constexpr f32 flicker_score_floor = 0.001f;

f64 rounded(f32 value) {
    return std::round(static_cast<f64>(value) * 100000.0) / 100000.0;
}

bool overlaps(RenderProbeRect a, RenderProbeRect b, i32 margin) {
    return a.x - margin < b.x + b.w && b.x - margin < a.x + a.w &&
        a.y - margin < b.y + b.h && b.y - margin < a.y + a.h;
}

RenderProbeRect merged(RenderProbeRect a, RenderProbeRect b) {
    const i32 x0 = std::min(a.x, b.x);
    const i32 y0 = std::min(a.y, b.y);
    const i32 x1 = std::max(a.x + a.w, b.x + b.w);
    const i32 y1 = std::max(a.y + a.h, b.y + b.h);
    return {x0, y0, x1 - x0, y1 - y0};
}

// Median of `values`, reordering them.
f32 median_of(std::vector<f32>& values) {
    const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
    std::nth_element(values.begin(), middle, values.end());
    return *middle;
}

} // namespace

std::string_view render_probe_event_kind_name(RenderProbeEventKind kind) {
    switch (kind) {
    case RenderProbeEventKind::Spike:
        return "spike";
    case RenderProbeEventKind::Flicker:
        return "flicker";
    }
    return "unknown";
}

RenderProbe::RenderProbe(RenderProbeConfig config)
    : _config(config) {
    _config.tile_size = std::max(_config.tile_size, 1);
    _config.baseline_frames = std::max(_config.baseline_frames, 1);
    _config.flicker_radius = std::max(_config.flicker_radius, 0);
}

void RenderProbe::reset(Vec2i size) {
    _size = size;
    _tiles = {(size.x + _config.tile_size - 1) / _config.tile_size, (size.y + _config.tile_size - 1) / _config.tile_size};
    _frames_since_reset = 0;
    for (std::vector<u8>& frame : _frame) {
        frame.clear();
    }
    _max_history.clear();
    _open.clear();
}

void RenderProbe::add_frame(std::span<const u8> rgba, Vec2i size) {
    const std::size_t pixels = size.x > 0 && size.y > 0
        ? static_cast<std::size_t>(size.x) * static_cast<std::size_t>(size.y)
        : 0;
    if (pixels == 0 || rgba.size() < pixels * 4u) {
        return;
    }
    if (size != _size) {
        reset(size);
    }

    std::swap(_frame[2], _frame[1]);
    std::swap(_frame[1], _frame[0]);
    // Alpha is not compared: making it opaque lets the diff sum all four bytes.
    _frame[0].resize(pixels * 4u);
    std::memcpy(_frame[0].data(), rgba.data(), pixels * 4u);
    constexpr u8 alpha_bytes[4] = {0, 0, 0, 255};
    u32 alpha = 0;
    std::memcpy(&alpha, alpha_bytes, 4);
    for (std::size_t i = 0; i < pixels * 4u; i += 4) {
        u32 pixel = 0;
        std::memcpy(&pixel, _frame[0].data() + i, 4);
        pixel |= alpha;
        std::memcpy(_frame[0].data() + i, &pixel, 4);
    }
    ++_frames_since_reset;
    const i32 frame = frame_count() + 1;
    const std::size_t tiles = static_cast<std::size_t>(_tiles.x) * static_cast<std::size_t>(_tiles.y);

    RenderProbeFrame stats;
    if (_frames_since_reset >= 2) {
        std::swap(_prev_d1, _d1);
        diff_tiles(_frame[0], _frame[1], _d1);

        f64 weighted = 0.0;
        u64 changed = 0;
        for (std::size_t tile = 0; tile < tiles; ++tile) {
            const RenderProbeRect rect = tile_rect(static_cast<i32>(tile));
            weighted += static_cast<f64>(_d1[tile].mean) * static_cast<f64>(rect.w * rect.h);
            changed += _d1[tile].changed;
            stats.max_tile_delta = std::max(stats.max_tile_delta, _d1[tile].mean);
        }
        stats.delta = static_cast<f32>(weighted / static_cast<f64>(pixels));
        stats.changed = static_cast<f32>(static_cast<f64>(changed) / static_cast<f64>(pixels));

        // Spikes: against the recent frames' largest tile change rather than each
        // tile's own history, so a sprite moving over a still background (every
        // tile it enters "wakes up") is the baseline, not an anomaly.
        if (static_cast<i32>(_max_history.size()) >= _config.warmup_frames) {
            std::vector<f32> history = _max_history;
            const f32 median = median_of(history);
            for (f32& value : history) {
                value = std::abs(value - median);
            }
            const f32 sigma = std::max(1.4826f * median_of(history), _config.spike_sigma_floor);
            const f32 threshold = std::max(median + _config.spike_sigmas * sigma, _config.spike_min_delta);
            _flagged.assign(tiles, 0);
            _flag_delta.assign(tiles, 0.0f);
            _flag_score.assign(tiles, 0.0f);
            bool any = false;
            for (std::size_t tile = 0; tile < tiles; ++tile) {
                if (_d1[tile].mean > threshold) {
                    _flagged[tile] = 1;
                    _flag_delta[tile] = _d1[tile].mean;
                    _flag_score[tile] = (_d1[tile].mean - median) / sigma;
                    any = true;
                }
            }
            if (any) {
                report_regions(RenderProbeEventKind::Spike, frame, _flagged, _flag_delta, _flag_score);
            }
        }
        _max_history.push_back(stats.max_tile_delta);
        if (static_cast<i32>(_max_history.size()) > _config.baseline_frames) {
            _max_history.erase(_max_history.begin());
        }
    }

    // Flicker: the previous frame against both its neighbours. Steady motion keeps
    // moving away, so the neighbours differ at least as much as each differs from
    // the middle frame; a frame that jumps and comes back does not.
    if (_frames_since_reset >= 3) {
        diff_tiles(_frame[0], _frame[2], _d2);
        _flagged.assign(tiles, 0);
        _flag_delta.assign(tiles, 0.0f);
        _flag_score.assign(tiles, 0.0f);
        const auto local_flicker = [&](i32 tile) {
            const f64 before = static_cast<f64>(neighbourhood_sum(_prev_d1, tile));
            const f64 after = static_cast<f64>(neighbourhood_sum(_d1, tile));
            const f64 across = static_cast<f64>(neighbourhood_sum(_d2, tile));
            return std::min(before, after) >= static_cast<f64>(_config.flicker_ratio) * across;
        };
        bool any = false;
        for (std::size_t tile = 0; tile < tiles; ++tile) {
            const f32 middle = std::min(_prev_d1[tile].mean, _d1[tile].mean);
            const f32 across = _d2[tile].mean;
            if (middle >= _config.flicker_min_delta && middle >= _config.flicker_ratio * across &&
                local_flicker(static_cast<i32>(tile))) {
                _flagged[tile] = 1;
                _flag_delta[tile] = std::max(_prev_d1[tile].mean, _d1[tile].mean);
                _flag_score[tile] = middle / std::max(across, flicker_score_floor);
                any = true;
            }
        }
        if (any) {
            report_regions(RenderProbeEventKind::Flicker, frame - 1, _flagged, _flag_delta, _flag_score);
        }
    }

    _timeline.push_back(stats);
    std::erase_if(_open, [&](std::size_t index) { return _events[index].last_frame < frame - 2; });
}

void RenderProbe::diff_tiles(std::span<const u8> a, std::span<const u8> b, std::vector<TileDelta>& out) const {
    const std::size_t tiles = static_cast<std::size_t>(_tiles.x) * static_cast<std::size_t>(_tiles.y);
    out.assign(tiles, {});
    const i32 tile_size = _config.tile_size;
    const i32 threshold = _config.pixel_threshold;
    for (i32 y = 0; y < _size.y; ++y) {
        const std::size_t row = static_cast<std::size_t>(y) * static_cast<std::size_t>(_size.x) * 4u;
        const std::size_t tile_row = static_cast<std::size_t>(y / tile_size) * static_cast<std::size_t>(_tiles.x);
        for (i32 tx = 0; tx < _tiles.x; ++tx) {
            const i32 x0 = tx * tile_size;
            const i32 x1 = std::min(x0 + tile_size, _size.x);
            const u8* pa = a.data() + row + static_cast<std::size_t>(x0) * 4u;
            const u8* pb = b.data() + row + static_cast<std::size_t>(x0) * 4u;
            const std::size_t bytes = static_cast<std::size_t>(x1 - x0) * 4u;
            // Most of a frame is usually unchanged.
            if (std::memcmp(pa, pb, bytes) == 0) {
                continue;
            }
            // Plain byte and word loops, so they vectorize (alpha is always 255).
            u32 sum = 0;
            for (std::size_t i = 0; i < bytes; ++i) {
                sum += static_cast<u32>(std::abs(static_cast<i32>(pa[i]) - static_cast<i32>(pb[i])));
            }
            u32 changed = 0;
            if (threshold == 0) {
                for (std::size_t i = 0; i < bytes; i += 4) {
                    u32 wa = 0;
                    u32 wb = 0;
                    std::memcpy(&wa, pa + i, 4);
                    std::memcpy(&wb, pb + i, 4);
                    changed += wa != wb ? 1u : 0u;
                }
            } else {
                for (std::size_t i = 0; i < bytes; i += 4) {
                    const i32 dr = std::abs(static_cast<i32>(pa[i]) - static_cast<i32>(pb[i]));
                    const i32 dg = std::abs(static_cast<i32>(pa[i + 1]) - static_cast<i32>(pb[i + 1]));
                    const i32 db = std::abs(static_cast<i32>(pa[i + 2]) - static_cast<i32>(pb[i + 2]));
                    changed += std::max({dr, dg, db}) > threshold ? 1u : 0u;
                }
            }
            out[tile_row + static_cast<std::size_t>(tx)].sum += sum;
            out[tile_row + static_cast<std::size_t>(tx)].changed += changed;
        }
    }
    for (std::size_t tile = 0; tile < tiles; ++tile) {
        const RenderProbeRect rect = tile_rect(static_cast<i32>(tile));
        const u64 scale = static_cast<u64>(rect.w) * static_cast<u64>(rect.h) * 3u * 255u;
        out[tile].mean = static_cast<f32>(static_cast<f64>(out[tile].sum) / static_cast<f64>(scale));
    }
}

RenderProbeRect RenderProbe::tile_rect(i32 tile) const {
    const i32 tx = tile % _tiles.x;
    const i32 ty = tile / _tiles.x;
    const i32 x = tx * _config.tile_size;
    const i32 y = ty * _config.tile_size;
    return {x, y, std::min(_config.tile_size, _size.x - x), std::min(_config.tile_size, _size.y - y)};
}

u64 RenderProbe::neighbourhood_sum(const std::vector<TileDelta>& deltas, i32 tile) const {
    const i32 tx = tile % _tiles.x;
    const i32 ty = tile / _tiles.x;
    const i32 radius = _config.flicker_radius;
    u64 sum = 0;
    for (i32 y = std::max(ty - radius, 0); y <= std::min(ty + radius, _tiles.y - 1); ++y) {
        for (i32 x = std::max(tx - radius, 0); x <= std::min(tx + radius, _tiles.x - 1); ++x) {
            sum += deltas[static_cast<std::size_t>(y) * static_cast<std::size_t>(_tiles.x) + static_cast<std::size_t>(x)].sum;
        }
    }
    return sum;
}

void RenderProbe::report_regions(RenderProbeEventKind kind,
                                 i32 frame,
                                 const std::vector<u8>& flagged,
                                 const std::vector<f32>& delta,
                                 const std::vector<f32>& score) {
    std::vector<u8> seen(flagged.size(), 0);
    std::vector<i32> stack;
    for (std::size_t start = 0; start < flagged.size(); ++start) {
        if (!flagged[start] || seen[start]) {
            continue;
        }
        RenderProbeRect rect = tile_rect(static_cast<i32>(start));
        f32 peak_delta = 0.0f;
        f32 peak_score = 0.0f;
        seen[start] = 1;
        stack.assign(1, static_cast<i32>(start));
        while (!stack.empty()) {
            const i32 tile = stack.back();
            stack.pop_back();
            rect = merged(rect, tile_rect(tile));
            peak_delta = std::max(peak_delta, delta[static_cast<std::size_t>(tile)]);
            peak_score = std::max(peak_score, score[static_cast<std::size_t>(tile)]);
            const i32 tx = tile % _tiles.x;
            const i32 ty = tile / _tiles.x;
            for (i32 ny = std::max(ty - 1, 0); ny <= std::min(ty + 1, _tiles.y - 1); ++ny) {
                for (i32 nx = std::max(tx - 1, 0); nx <= std::min(tx + 1, _tiles.x - 1); ++nx) {
                    const std::size_t next = static_cast<std::size_t>(ny) * static_cast<std::size_t>(_tiles.x) +
                        static_cast<std::size_t>(nx);
                    if (flagged[next] && !seen[next]) {
                        seen[next] = 1;
                        stack.push_back(static_cast<i32>(next));
                    }
                }
            }
        }
        record(kind, frame, rect, peak_delta, peak_score);
    }
}

void RenderProbe::record(RenderProbeEventKind kind, i32 frame, RenderProbeRect rect, f32 delta, f32 score) {
    // A frame that jumps and comes back is also a spike into it and one out of
    // it; the flicker explains both, so it replaces them.
    if (kind == RenderProbeEventKind::Flicker) {
        std::vector<std::size_t> open;
        std::size_t kept = 0;
        for (std::size_t index = 0; index < _events.size(); ++index) {
            const RenderProbeEvent& event = _events[index];
            const bool absorbed = event.kind == RenderProbeEventKind::Spike && event.first_frame >= frame &&
                event.last_frame <= frame + 1 && overlaps(event.rect, rect, _config.tile_size);
            if (absorbed) {
                continue;
            }
            if (std::ranges::find(_open, index) != _open.end()) {
                open.push_back(kept);
            }
            _events[kept++] = event;
        }
        _events.resize(kept);
        _open = std::move(open);
    }
    for (std::size_t index : _open) {
        RenderProbeEvent& event = _events[index];
        if (event.kind != kind || event.last_frame < frame - 1 || !overlaps(event.rect, rect, _config.tile_size)) {
            continue;
        }
        if (event.last_frame != frame) {
            event.last_frame = frame;
            ++event.frames;
        }
        event.rect = merged(event.rect, rect);
        if (delta > event.peak_delta) {
            event.peak_delta = delta;
            event.peak_frame = frame;
        }
        event.peak_score = std::max(event.peak_score, score);
        return;
    }
    _events.push_back({
        .kind = kind,
        .first_frame = frame,
        .last_frame = frame,
        .frames = 1,
        .rect = rect,
        .peak_frame = frame,
        .peak_delta = delta,
        .peak_score = score,
    });
    _open.push_back(_events.size() - 1);
}

i32 RenderProbe::event_count(RenderProbeEventKind kind) const {
    return static_cast<i32>(std::ranges::count_if(_events, [&](const RenderProbeEvent& event) {
        return event.kind == kind;
    }));
}

void RenderProbe::write_json(JsonWriter& json) const {
    f32 peak_delta = 0.0f;
    f64 total_delta = 0.0;
    for (const RenderProbeFrame& frame : _timeline) {
        peak_delta = std::max(peak_delta, frame.delta);
        total_delta += frame.delta;
    }

    json.begin_object();
    json.field("schema", "kin.render_probe/1");
    json.field("width", _size.x);
    json.field("height", _size.y);
    json.field("frames", frame_count());
    json.key("config").begin_object();
    json.field("tile_size", _config.tile_size);
    json.field("pixel_threshold", _config.pixel_threshold);
    json.field("baseline_frames", _config.baseline_frames);
    json.field("warmup_frames", _config.warmup_frames);
    json.field("spike_sigmas", rounded(_config.spike_sigmas));
    json.field("spike_sigma_floor", rounded(_config.spike_sigma_floor));
    json.field("spike_min_delta", rounded(_config.spike_min_delta));
    json.field("flicker_min_delta", rounded(_config.flicker_min_delta));
    json.field("flicker_ratio", rounded(_config.flicker_ratio));
    json.field("flicker_radius", _config.flicker_radius);
    json.end_object();

    json.key("summary").begin_object();
    json.field("events", static_cast<i32>(_events.size()));
    json.field("spikes", event_count(RenderProbeEventKind::Spike));
    json.field("flickers", event_count(RenderProbeEventKind::Flicker));
    json.field("peak_delta", rounded(peak_delta));
    json.field("mean_delta", _timeline.empty() ? 0.0 : rounded(static_cast<f32>(total_delta / static_cast<f64>(_timeline.size()))));
    json.end_object();

    json.key("events").begin_array();
    for (const RenderProbeEvent& event : _events) {
        json.begin_object();
        json.field("kind", render_probe_event_kind_name(event.kind));
        json.field("first_frame", event.first_frame);
        json.field("last_frame", event.last_frame);
        json.field("frames", event.frames);
        json.key("rect").begin_object();
        json.field("x", event.rect.x);
        json.field("y", event.rect.y);
        json.field("w", event.rect.w);
        json.field("h", event.rect.h);
        json.end_object();
        json.field("peak_frame", event.peak_frame);
        json.field("peak_delta", rounded(event.peak_delta));
        json.field("peak_score", rounded(event.peak_score));
        json.end_object();
    }
    json.end_array();

    // Columns, one value per frame, so the series are easy to plot.
    const auto column = [&](std::string_view name, auto value) {
        json.key(name).begin_array();
        for (const RenderProbeFrame& frame : _timeline) {
            json.value(rounded(value(frame)));
        }
        json.end_array();
    };
    json.key("timeline").begin_object();
    column("delta", [](const RenderProbeFrame& frame) { return frame.delta; });
    column("max_tile_delta", [](const RenderProbeFrame& frame) { return frame.max_tile_delta; });
    column("changed", [](const RenderProbeFrame& frame) { return frame.changed; });
    json.end_object();
    json.end_object();
}

} // namespace kin
