#include <kin/runtime/render_probe.hpp>

#include <kin/core/json.hpp>

#include <algorithm>
#include <array>
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

// Attribution weights, in "pixels moved" so the kinds of change compare.
constexpr f32 pop_weight = 64.0f;     // a draw shown or hidden
constexpr f32 swap_weight = 32.0f;    // a different texture or texture region
constexpr f32 color_divisor = 4.0f;   // a full channel swing counts as 64 px
constexpr f32 match_distance = 64.0f; // farther apart, two draws are not the same thing
constexpr f32 near_margin = 32.0f;    // draws this close to an event are looked at
constexpr std::size_t match_pairs_limit = 65536;

struct DrawChange {
    u8 bits = 0;
    f32 magnitude = 0.0f;
    f32 move = 0.0f;
};

Vec2f center(Rectf r) {
    return {r.x + r.w * 0.5f, r.y + r.h * 0.5f};
}

bool intersects(Rectf a, f32 x, f32 y, f32 w, f32 h) {
    return a.x < x + w && x < a.x + a.w && a.y < y + h && y < a.y + a.h;
}

bool same_rect(Rectf a, Rectf b) {
    return std::abs(a.x - b.x) < 0.01f && std::abs(a.y - b.y) < 0.01f && std::abs(a.w - b.w) < 0.01f &&
        std::abs(a.h - b.h) < 0.01f;
}

DrawChange compare(const DrawRecord* a, const DrawRecord* b) {
    namespace change = render_probe_change;
    if (!a && !b) {
        return {};
    }
    if (!a) {
        return {change::appeared, pop_weight, 0.0f};
    }
    if (!b) {
        return {change::disappeared, pop_weight, 0.0f};
    }
    DrawChange result;
    const Vec2f ca = center(a->bounds);
    const Vec2f cb = center(b->bounds);
    result.move = std::abs(ca.x - cb.x) + std::abs(ca.y - cb.y);
    if (result.move > 0.01f) {
        result.bits |= change::moved;
        result.magnitude += result.move;
    }
    const f32 resize = std::abs(a->bounds.w - b->bounds.w) + std::abs(a->bounds.h - b->bounds.h);
    if (resize > 0.01f) {
        result.bits |= change::resized;
        result.magnitude += resize;
    }
    if (a->texture != b->texture) {
        result.bits |= change::texture;
        result.magnitude += swap_weight;
    } else if (!same_rect(a->region, b->region)) {
        result.bits |= change::frame;
        result.magnitude += swap_weight;
    }
    const i32 color = std::max({std::abs(a->color.r - b->color.r), std::abs(a->color.g - b->color.g),
                                std::abs(a->color.b - b->color.b), std::abs(a->color.a - b->color.a)});
    if (color > 0) {
        result.bits |= change::color;
        result.magnitude += static_cast<f32>(color) / color_divisor;
    }
    const f32 turn = std::abs(a->rotation - b->rotation);
    if (turn > 0.01f) {
        result.bits |= change::rotated;
        result.magnitude += std::min(turn, pop_weight);
    }
    return result;
}

// Pairs up the draws of `a` and `b` that are most likely the same thing drawn
// twice: nearest first, the same kind only. Returns, for each draw of `b`, the
// index of its partner in `a` (or -1).
std::vector<i32> match(const std::vector<const DrawRecord*>& a, const std::vector<const DrawRecord*>& b) {
    std::vector<i32> partner(b.size(), -1);
    if (a.empty() || b.empty()) {
        return partner;
    }
    if (a.size() * b.size() > match_pairs_limit) {
        // Too many to compare pairwise: the same position in drawing order.
        for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i) {
            partner[i] = static_cast<i32>(i);
        }
        return partner;
    }
    struct Pair {
        f32 cost;
        u32 ia;
        u32 ib;
    };
    std::vector<Pair> pairs;
    for (u32 ib = 0; ib < b.size(); ++ib) {
        for (u32 ia = 0; ia < a.size(); ++ia) {
            if (a[ia]->kind != b[ib]->kind) {
                continue;
            }
            const Vec2f ca = center(a[ia]->bounds);
            const Vec2f cb = center(b[ib]->bounds);
            f32 cost = std::abs(ca.x - cb.x) + std::abs(ca.y - cb.y) + std::abs(a[ia]->bounds.w - b[ib]->bounds.w) +
                std::abs(a[ia]->bounds.h - b[ib]->bounds.h);
            if (cost > match_distance) {
                continue;
            }
            // Prefer the same texture and look, so a still draw pairs with itself.
            cost += a[ia]->texture != b[ib]->texture ? 0.5f : 0.0f;
            cost += !same_rect(a[ia]->region, b[ib]->region) ? 0.25f : 0.0f;
            cost += !(a[ia]->color == b[ib]->color) ? 0.125f : 0.0f;
            pairs.push_back({cost, ia, ib});
        }
    }
    std::stable_sort(pairs.begin(), pairs.end(), [](const Pair& x, const Pair& y) { return x.cost < y.cost; });
    std::vector<u8> used(a.size(), 0);
    for (const Pair& pair : pairs) {
        if (partner[pair.ib] < 0 && !used[pair.ia]) {
            partner[pair.ib] = static_cast<i32>(pair.ia);
            used[pair.ia] = 1;
        }
    }
    return partner;
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
    for (std::vector<DrawRecord>& draws : _draws) {
        draws.clear();
    }
    _max_history.clear();
    _open.clear();
}

void RenderProbe::add_frame(std::span<const u8> rgba, Vec2i size, std::span<const DrawRecord> draws) {
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
    std::swap(_draws[2], _draws[1]);
    std::swap(_draws[1], _draws[0]);
    _draws[0].assign(draws.begin(), draws.end());
    _traced = _traced || !draws.empty();
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
        record(kind, frame, rect, peak_delta, peak_score, attribute(kind, rect));
    }
}

std::vector<RenderProbeCulprit> RenderProbe::attribute(RenderProbeEventKind kind, RenderProbeRect rect) const {
    std::vector<RenderProbeCulprit> culprits;
    if (!_traced) {
        return culprits;
    }
    // The frames compared, oldest first: a spike is the newest frame against the
    // one before; a flicker is the frame before the newest, against both sides.
    const bool flicker = kind == RenderProbeEventKind::Flicker;
    const std::size_t count = flicker ? 3 : 2;
    std::array<const std::vector<DrawRecord>*, 3> frames{};
    for (std::size_t i = 0; i < count; ++i) {
        frames[i] = &_draws[count - 1 - i];
    }

    const f32 x = static_cast<f32>(rect.x);
    const f32 y = static_cast<f32>(rect.y);
    const f32 w = static_cast<f32>(rect.w);
    const f32 h = static_cast<f32>(rect.h);
    std::vector<u32> sources;
    for (std::size_t f = 0; f < count; ++f) {
        for (const DrawRecord& draw : *frames[f]) {
            if (intersects(draw.bounds, x, y, w, h)) {
                sources.push_back(draw.source);
            }
        }
    }
    std::sort(sources.begin(), sources.end());
    sources.erase(std::unique(sources.begin(), sources.end()), sources.end());

    std::array<std::vector<const DrawRecord*>, 3> near;
    for (const u32 source : sources) {
        for (std::size_t f = 0; f < count; ++f) {
            near[f].clear();
            for (const DrawRecord& draw : *frames[f]) {
                if (draw.source == source &&
                    intersects(draw.bounds, x - near_margin, y - near_margin, w + 2 * near_margin, h + 2 * near_margin)) {
                    near[f].push_back(&draw);
                }
            }
        }
        RenderProbeCulprit culprit{.source = source, .frames = 1};
        const auto note = [&](const DrawChange& change, f32 evidence) {
            if (evidence > 0.0f) {
                culprit.score += evidence;
                culprit.changes |= change.bits;
                culprit.max_move = std::max(culprit.max_move, change.move);
            }
        };
        if (!flicker) {
            const std::vector<i32> partner = match(near[0], near[1]);
            std::vector<u8> matched(near[0].size(), 0);
            for (std::size_t ib = 0; ib < near[1].size(); ++ib) {
                const DrawRecord* a = partner[ib] >= 0 ? near[0][static_cast<std::size_t>(partner[ib])] : nullptr;
                if (partner[ib] >= 0) {
                    matched[static_cast<std::size_t>(partner[ib])] = 1;
                }
                const DrawChange change = compare(a, near[1][ib]);
                note(change, change.magnitude);
            }
            for (std::size_t ia = 0; ia < near[0].size(); ++ia) {
                if (!matched[ia]) {
                    const DrawChange change = compare(near[0][ia], nullptr);
                    note(change, change.magnitude);
                }
            }
        } else {
            // Evidence of A-B-A: B differs from both sides by more than the sides
            // differ from each other.
            const std::vector<i32> ab = match(near[0], near[1]);
            const std::vector<i32> cb = match(near[2], near[1]);
            const std::vector<i32> ac = match(near[2], near[0]);
            std::vector<u8> a_used(near[0].size(), 0);
            for (std::size_t ib = 0; ib < near[1].size(); ++ib) {
                const DrawRecord* b = near[1][ib];
                const DrawRecord* a = ab[ib] >= 0 ? near[0][static_cast<std::size_t>(ab[ib])] : nullptr;
                const DrawRecord* c = cb[ib] >= 0 ? near[2][static_cast<std::size_t>(cb[ib])] : nullptr;
                if (ab[ib] >= 0) {
                    a_used[static_cast<std::size_t>(ab[ib])] = 1;
                }
                const DrawChange to_b = compare(a, b);
                const DrawChange from_b = compare(b, c);
                const DrawChange across = compare(a, c);
                DrawChange both = to_b;
                both.bits |= from_b.bits;
                both.move = std::max(to_b.move, from_b.move);
                note(both, std::min(to_b.magnitude, from_b.magnitude) - across.magnitude);
            }
            // Missing for one frame: drawn before and after, not in between.
            for (std::size_t ia = 0; ia < near[0].size(); ++ia) {
                if (a_used[ia] || ac[ia] < 0) {
                    continue;
                }
                const DrawChange across = compare(near[0][ia], near[2][static_cast<std::size_t>(ac[ia])]);
                note({render_probe_change::disappeared | render_probe_change::appeared, pop_weight, 0.0f},
                     pop_weight - across.magnitude);
            }
        }
        if (culprit.score > 0.0f) {
            culprits.push_back(culprit);
        }
    }
    std::stable_sort(culprits.begin(), culprits.end(), [](const RenderProbeCulprit& a, const RenderProbeCulprit& b) {
        return a.score > b.score;
    });
    if (culprits.size() > max_culprits) {
        culprits.resize(max_culprits);
    }
    return culprits;
}

void RenderProbe::record(RenderProbeEventKind kind, i32 frame, RenderProbeRect rect, f32 delta, f32 score,
                         std::vector<RenderProbeCulprit> culprits) {
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
            if (kept != index) {
                _events[kept] = std::move(_events[index]);
            }
            ++kept;
        }
        _events.resize(kept);
        _open = std::move(open);
    }
    for (std::size_t index : _open) {
        RenderProbeEvent& event = _events[index];
        if (event.kind != kind || event.last_frame < frame - 1 || !overlaps(event.rect, rect, _config.tile_size)) {
            continue;
        }
        const bool new_frame = event.last_frame != frame;
        if (new_frame) {
            event.last_frame = frame;
            ++event.frames;
        }
        event.rect = merged(event.rect, rect);
        for (const RenderProbeCulprit& culprit : culprits) {
            const auto known = std::ranges::find(event.culprits, culprit.source, &RenderProbeCulprit::source);
            if (known == event.culprits.end()) {
                event.culprits.push_back(culprit);
                continue;
            }
            known->frames += new_frame ? 1 : 0;
            known->changes |= culprit.changes;
            known->score = std::max(known->score, culprit.score);
            known->max_move = std::max(known->max_move, culprit.max_move);
        }
        // Most often implicated first, then by evidence.
        std::stable_sort(event.culprits.begin(), event.culprits.end(),
                         [](const RenderProbeCulprit& a, const RenderProbeCulprit& b) {
                             return a.frames != b.frames ? a.frames > b.frames : a.score > b.score;
                         });
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
        .culprits = std::move(culprits),
    });
    _open.push_back(_events.size() - 1);
}

void RenderProbe::write_culprit(JsonWriter& json, const RenderProbeCulprit& culprit) const {
    static constexpr std::array<std::string_view, 8> change_names{
        "moved", "resized", "frame", "color", "texture", "rotated", "appeared", "disappeared",
    };
    json.begin_object();
    const DrawSourceInfo info = _trace ? _trace->source_info(culprit.source) : DrawSourceInfo{};
    if (culprit.source == 0) {
        json.field("unclaimed", true);
    } else if (info.entity != 0) {
        json.field("entity", info.entity);
        json.field("name", info.name);
        json.field("component", info.component);
    } else if (!info.name.empty()) {
        json.field("scope", info.name);
    } else {
        json.field("source", culprit.source);
    }
    json.field("frames", culprit.frames);
    json.key("changes").begin_array();
    for (std::size_t bit = 0; bit < change_names.size(); ++bit) {
        if (culprit.changes & (1u << bit)) {
            json.value(change_names[bit]);
        }
    }
    json.end_array();
    json.field("max_move", rounded(culprit.max_move));
    json.field("score", rounded(culprit.score));
    json.end_object();
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
        if (_traced) {
            json.key("culprits").begin_array();
            for (std::size_t i = 0; i < std::min(event.culprits.size(), max_culprits); ++i) {
                write_culprit(json, event.culprits[i]);
            }
            json.end_array();
        }
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
