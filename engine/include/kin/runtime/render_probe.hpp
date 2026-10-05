#pragma once

#include <kin/core/types.hpp>
#include <kin/renderer/draw_trace.hpp>

#include <span>
#include <string_view>
#include <vector>

namespace kin {

class JsonWriter;

struct RenderProbeConfig {
    // Side of the square tiles a frame is split into, in pixels. Events are
    // located to tiles.
    i32 tile_size = 16;
    // A pixel counts as changed (the "changed" fraction) when a color channel
    // moved by more than this, 0-255. Detection uses the mean change instead.
    i32 pixel_threshold = 0;
    // Frames of history the spike baseline is taken from, and how many must be
    // seen before spikes are reported.
    i32 baseline_frames = 60;
    i32 warmup_frames = 8;
    // Spike: a tile's mean change (0-1) is above the baseline of the frames'
    // largest tile change by this many robust deviations (each at least
    // spike_sigma_floor), and at least spike_min_delta.
    f32 spike_sigmas = 8.0f;
    f32 spike_sigma_floor = 0.01f;
    f32 spike_min_delta = 0.05f;
    // Flicker: a frame differs from both its neighbours by at least
    // flicker_min_delta, and by flicker_ratio times more than the neighbours
    // differ from each other (the A-B-A of jitter, popping, or a glitch frame).
    // The ratio must also hold over the tiles within flicker_radius, so that
    // something passing through a tile (it was in the next tile before and is in
    // another one after) is not taken for something that came back.
    f32 flicker_min_delta = 0.01f;
    f32 flicker_ratio = 4.0f;
    i32 flicker_radius = 2;
};

enum class RenderProbeEventKind : u8 {
    Spike,
    Flicker,
};

std::string_view render_probe_event_kind_name(RenderProbeEventKind kind);

struct RenderProbeRect {
    i32 x = 0;
    i32 y = 0;
    i32 w = 0;
    i32 h = 0;

    friend constexpr bool operator==(RenderProbeRect, RenderProbeRect) = default;
};

// How a draw changed between frames (bits of RenderProbeCulprit::changes).
namespace render_probe_change {
constexpr u8 moved = 1u << 0;
constexpr u8 resized = 1u << 1;
constexpr u8 frame = 1u << 2;   // a different region of its texture (an animation frame)
constexpr u8 color = 1u << 3;
constexpr u8 texture = 1u << 4;
constexpr u8 rotated = 1u << 5;
constexpr u8 appeared = 1u << 6;
constexpr u8 disappeared = 1u << 7;
} // namespace render_probe_change

// A draw source (an entity's render component, or a named scope; see
// draw_trace.hpp) whose own draws changed where an event happened, the way the
// event needs: for flicker, a change that came back (A-B-A), or a draw shown or
// hidden for a single frame; for a spike, any change.
struct RenderProbeCulprit {
    u32 source = 0;      // DrawTrace source id; 0: draws nothing claimed
    i32 frames = 0;      // frames of the event it was found in
    u8 changes = 0;      // render_probe_change bits
    f32 score = 0.0f;    // highest per-frame evidence (pixels moved, roughly)
    f32 max_move = 0.0f; // largest move of one of its draws, in pixels
};

// A run of consecutive frames with the same kind of anomaly in overlapping
// places: one jittering sprite is one event, not one per frame.
struct RenderProbeEvent {
    RenderProbeEventKind kind = RenderProbeEventKind::Spike;
    i32 first_frame = 0;
    i32 last_frame = 0;
    i32 frames = 0;          // frames flagged within [first_frame, last_frame]
    RenderProbeRect rect{};  // union of the flagged tiles, in captured pixels
    i32 peak_frame = 0;
    f32 peak_delta = 0.0f;   // largest mean tile change seen, 0-1
    f32 peak_score = 0.0f;   // spike: deviations over baseline; flicker: ratio
    // Who drew it, most likely first (at most RenderProbe::max_culprits). Empty
    // when frames came without draws, or when no traced draw explains the change.
    std::vector<RenderProbeCulprit> culprits;
};

// Per-frame change statistics, relative to the previous frame. Frame 1 has none.
struct RenderProbeFrame {
    f32 delta = 0.0f;          // mean change over the frame, 0-1
    f32 max_tile_delta = 0.0f; // mean change of the most changed tile, 0-1
    f32 changed = 0.0f;        // fraction of pixels over pixel_threshold
};

// Finds rendering anomalies in a stream of frames by comparing each one with the
// two before it, tile by tile. Pure pixel analysis: frames come from anywhere
// (run_scene_app feeds it the headless framebuffer with --probe-render), and
// the same frames always give the same result.
class RenderProbe {
public:
    explicit RenderProbe(RenderProbeConfig config = {});

    const RenderProbeConfig& config() const { return _config; }

    static constexpr std::size_t max_culprits = 5;

    // Adds the next frame: tightly packed RGBA8, `size.x * size.y` pixels. Alpha
    // is ignored. A frame of a different size starts the comparison over.
    // `draws` are the frame's traced draws (DrawTrace::draws()), in the same
    // pixels; with them, events name the culprits.
    void add_frame(std::span<const u8> rgba, Vec2i size, std::span<const DrawRecord> draws = {});
    // Where culprits' names come from when writing the report (optional; must
    // outlive write_json).
    void set_draw_trace(const DrawTrace* trace) { _trace = trace; }

    i32 frame_count() const { return static_cast<i32>(_timeline.size()); }
    Vec2i frame_size() const { return _size; }
    std::span<const RenderProbeFrame> timeline() const { return _timeline; }
    std::span<const RenderProbeEvent> events() const { return _events; }
    i32 event_count(RenderProbeEventKind kind) const;

    // Writes the kin.render_probe/1 report as one JSON object.
    void write_json(JsonWriter& json) const;

private:
    struct TileDelta {
        f32 mean = 0.0f;
        u32 changed = 0;
        u64 sum = 0; // summed channel changes
    };

    void reset(Vec2i size);
    // Mean change of each tile between two frames, into `out`.
    void diff_tiles(std::span<const u8> a, std::span<const u8> b, std::vector<TileDelta>& out) const;
    RenderProbeRect tile_rect(i32 tile) const;
    // Summed change of the tiles within flicker_radius of `tile`.
    u64 neighbourhood_sum(const std::vector<TileDelta>& deltas, i32 tile) const;
    // Groups the flagged tiles (8-connected) and records each group as an event.
    void report_regions(RenderProbeEventKind kind,
                        i32 frame,
                        const std::vector<u8>& flagged,
                        const std::vector<f32>& delta,
                        const std::vector<f32>& score);
    void record(RenderProbeEventKind kind, i32 frame, RenderProbeRect rect, f32 delta, f32 score,
                std::vector<RenderProbeCulprit> culprits);
    // The sources whose draws near `rect` changed the way `kind` needs, between
    // the frames the event was found in.
    std::vector<RenderProbeCulprit> attribute(RenderProbeEventKind kind, RenderProbeRect rect) const;
    void write_culprit(JsonWriter& json, const RenderProbeCulprit& culprit) const;

    RenderProbeConfig _config;
    Vec2i _size{};
    Vec2i _tiles{};
    i32 _frames_since_reset = 0;
    // The last three frames and their draws, newest first.
    std::vector<u8> _frame[3];
    std::vector<DrawRecord> _draws[3];
    bool _traced = false; // some frame came with draws
    const DrawTrace* _trace = nullptr;
    std::vector<TileDelta> _d1;      // newest vs previous
    std::vector<TileDelta> _d2;      // newest vs the one before the previous
    std::vector<TileDelta> _prev_d1; // previous vs the one before it
    std::vector<f32> _max_history;   // largest tile change per frame, for the spike baseline
    std::vector<u8> _flagged;
    std::vector<f32> _flag_delta;
    std::vector<f32> _flag_score;
    std::vector<RenderProbeFrame> _timeline;
    std::vector<RenderProbeEvent> _events;
    // Events still open to extension, by index into _events.
    std::vector<std::size_t> _open;
};

} // namespace kin
