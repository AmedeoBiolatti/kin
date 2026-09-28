#pragma once

#include <kin/core/types.hpp>
#include <kin/platform/input.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/runtime/debug_options.hpp>
#include <kin/ui2/context.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace kin {

struct DebugTimingStats {
    f64 last_ms = 0.0;
    f64 median_ms = 0.0;
    f64 p99_ms = 0.0;
};

class RuntimeDebugOverlay {
public:
    void set_visible(bool visible) { _visible = visible; }
    bool visible() const { return _visible; }
    void toggle() { _visible = !_visible; }

    void begin_frame();
    void record(std::string_view name, f64 ms);
    // Upserts a text row appended after the timing rows (value shown in the
    // LAST column); used for non-numeric diagnostics such as per-system
    // parallel execution decisions.
    void set_status(std::string_view name, std::string value);
    void render(Input& input, Renderer2D& renderer);
    void start_capture();
    void stop_capture();
    void toggle_capture();
    bool capture_active() const { return _capture_active; }
    bool capture_available() const { return _capture_available; }

    RuntimeDebugOptions& options() { return _options; }
    const RuntimeDebugOptions& options() const { return _options; }
    DebugTimingStats stats(std::string_view name) const;

private:
    struct TimingRow {
        std::string name;
        std::vector<f64> samples;
        std::size_t next = 0;
        bool filled = false;
    };

    struct DisplayRow {
        std::string label;
        std::string last;
        std::string median;
        std::string p99;
        std::string copy_text;
    };

    TimingRow& row(std::string_view name);
    TimingRow& row(std::vector<TimingRow>& rows, std::string_view name);
    const TimingRow* find(std::string_view name) const;
    const TimingRow* find(const std::vector<TimingRow>& rows, std::string_view name) const;
    void refresh_display_cache();
    const std::vector<TimingRow>& display_rows_source() const;
    DebugTimingStats stats_from(const std::vector<TimingRow>& rows, std::string_view name) const;
    void record_to(std::vector<TimingRow>& rows, std::string_view name, f64 ms);

    bool _visible = false;
    bool _bounds_initialized = false;
    bool _resizing = false;
    bool _selecting_text = false;
    i32 _selection_anchor = -1;
    i32 _selection_cursor = -1;
    Rectf _bounds{};
    Vec2f _resize_start_pointer{};
    Rectf _resize_start_bounds{};
    RuntimeDebugOptions _options;
    std::vector<TimingRow> _rows;
    std::vector<TimingRow> _capture_rows;
    std::vector<std::pair<std::string, std::string>> _status_rows;
    bool _capture_active = false;
    bool _capture_available = false;
    ui2::Context _ui;
    ui2::ScrollView _scroll;
    DebugTimingStats _display_frame_stats;
    std::string _display_fps;
    std::vector<DisplayRow> _display_rows;
    u64 _display_frame_counter = 0;
    Rectf _visual_cache_bounds{};
    bool _visual_cache_dirty = true;
};

} // namespace kin
