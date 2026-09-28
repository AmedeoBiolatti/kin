#include <kin/runtime/debug_overlay.hpp>

#include <kin/ui2/context.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace kin {
namespace {

constexpr std::size_t sample_capacity = 300;
constexpr f32 min_panel_w = 360.0f;
constexpr f32 min_panel_h = 520.0f;
constexpr f32 default_panel_w = 640.0f;
constexpr f32 default_panel_h = 680.0f;
constexpr f32 grip_size = 28.0f;
constexpr f32 debug_font_pt = 11.0f;
constexpr f32 debug_row_h = 18.0f;
constexpr u64 display_refresh_frames = 8;

f64 percentile(std::vector<f64> sorted, f64 percent) {
    if (sorted.empty()) {
        return 0.0;
    }
    std::ranges::sort(sorted);
    const f64 raw = (static_cast<f64>(sorted.size() - 1) * percent);
    const auto index = static_cast<std::size_t>(std::clamp(std::ceil(raw), 0.0, static_cast<f64>(sorted.size() - 1)));
    return sorted[index];
}

std::string fmt_ms(f64 value) {
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "%.2f", value);
    return buffer;
}

std::string fmt_fps(f64 frame_ms) {
    char buffer[32]{};
    const f64 fps = frame_ms > 0.0001 ? 1000.0 / frame_ms : 0.0;
    std::snprintf(buffer, sizeof(buffer), "%.1f FPS", fps);
    return buffer;
}

std::string fmt_u64(u64 value) {
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(value));
    return buffer;
}

std::string display_label(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (char c : value) {
        if (c >= 'a' && c <= 'z') {
            result.push_back(static_cast<char>(c - 'a' + 'A'));
        } else if (c == '.' || c == '_') {
            result.push_back(' ');
        } else {
            result.push_back(c);
        }
    }
    return result;
}

ui2::Font debug_font() {
    static const ui2::Font font = [] {
        const std::filesystem::path candidates[] = {
            "C:/Windows/Fonts/CascadiaMono.ttf",
            "C:/Windows/Fonts/CascadiaCode.ttf",
            "C:/Windows/Fonts/consola.ttf",
            "C:/Windows/Fonts/arial.ttf",
            "C:/Windows/Fonts/segoeui.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/Library/Fonts/Arial.ttf",
        };
        for (const std::filesystem::path& path : candidates) {
            if (!std::filesystem::exists(path)) {
                continue;
            }
            try {
                return ui2::load_ttf_font(path, debug_font_pt);
            } catch (...) {
            }
        }
        return ui2::bitmap_font();
    }();
    return font;
}

bool rect_contains(Rectf rect, Vec2f pos) {
    return pos.x >= rect.x && pos.y >= rect.y && pos.x < rect.x + rect.w && pos.y < rect.y + rect.h;
}

Rectf resize_grip(Rectf bounds) {
    return {bounds.x + bounds.w - grip_size, bounds.y + bounds.h - grip_size, grip_size, grip_size};
}

struct SelectableDebugLine {
    Rectf bounds{};
    std::string text;
};

ui2::SurfaceStyle debug_surface(Color fill, Color border) {
    return {
        .fill = fill,
        .border = border,
        .border_width = 1.0f,
        .radius = 0.0f,
        .border_mode = ui2::BorderMode::Inside,
        .draw_fill = true,
    };
}

ui2::WidgetStyle debug_toggle_style(Color fill, Color border) {
    ui2::WidgetStyle style{};
    style.surface.normal = debug_surface(fill, border);
    style.surface.hovered = debug_surface(Color::rgba(30, 34, 42, 230), border);
    style.surface.pressed = debug_surface(Color::rgba(38, 44, 56, 240), border);
    style.surface.focused = debug_surface(fill, Color::rgba(170, 190, 210, 220));
    style.surface.disabled = debug_surface(Color::rgba(24, 28, 38, 160), Color::rgba(42, 46, 52, 120));
    style.surface.selected = debug_surface(fill, border);
    style.track = Color::rgba(42, 46, 52, 220);
    style.accent = Color::rgb(86, 142, 220);
    return style;
}

} // namespace

void RuntimeDebugOverlay::begin_frame() {
}

void RuntimeDebugOverlay::record(std::string_view name, f64 ms) {
    record_to(_rows, name, ms);
    if (_capture_active) {
        record_to(_capture_rows, name, ms);
    }
}

void RuntimeDebugOverlay::set_status(std::string_view name, std::string value) {
    const auto found = std::ranges::find_if(_status_rows, [&](const std::pair<std::string, std::string>& row) {
        return row.first == name;
    });
    if (found != _status_rows.end()) {
        found->second = std::move(value);
        return;
    }
    _status_rows.emplace_back(std::string{name}, std::move(value));
    std::ranges::sort(_status_rows, [](const std::pair<std::string, std::string>& a, const std::pair<std::string, std::string>& b) {
        return a.first < b.first;
    });
}

void RuntimeDebugOverlay::record_to(std::vector<TimingRow>& rows, std::string_view name, f64 ms) {
    TimingRow& target = row(rows, name);
    if (target.samples.size() < sample_capacity) {
        target.samples.push_back(ms);
        target.next = target.samples.size() % sample_capacity;
        target.filled = target.samples.size() == sample_capacity;
        return;
    }

    target.samples[target.next] = ms;
    target.next = (target.next + 1) % sample_capacity;
    target.filled = true;
}

void RuntimeDebugOverlay::start_capture() {
    _capture_rows.clear();
    _capture_active = true;
    _capture_available = false;
    _display_frame_counter = 0;
    _visual_cache_dirty = true;
    refresh_display_cache();
}

void RuntimeDebugOverlay::stop_capture() {
    if (!_capture_active) {
        return;
    }
    _capture_active = false;
    _capture_available = !_capture_rows.empty();
    _display_frame_counter = 0;
    _visual_cache_dirty = true;
    refresh_display_cache();
}

void RuntimeDebugOverlay::toggle_capture() {
    if (_capture_active) {
        stop_capture();
    } else {
        start_capture();
    }
}

void RuntimeDebugOverlay::render(Input& input, Renderer2D& renderer) {
    if (!_visible) {
        return;
    }

    const auto native_coordinates = renderer.scoped_native_coordinates();
    _ui.begin(input, renderer);

    ++_display_frame_counter;
    const std::vector<TimingRow>& source_rows = display_rows_source();
    if (_display_rows.size() != source_rows.size() + _status_rows.size() || _display_frame_counter % display_refresh_frames == 1) {
        refresh_display_cache();
        _visual_cache_dirty = true;
    }

    const Vec2i output = renderer.output_size();
    const f32 out_w = static_cast<f32>(output.x);
    const f32 out_h = static_cast<f32>(output.y);

    // Every size derives from the native window pixels, never the game's logical
    // resolution. The minimum scales down with the window so a fixed min larger
    // than the window (e.g. a 520px min on a 432px-tall window) can never force
    // the overlay to full coverage. min_panel_* acts only as an upper bound on
    // that window-relative floor for large desktop windows.
    const f32 win_min_w = std::min(min_panel_w, out_w * 0.5f);
    const f32 win_min_h = std::min(min_panel_h, out_h * 0.5f);

    if (!_bounds_initialized) {
        constexpr f32 default_fraction = 0.55f;
        const f32 fit_w = std::max(0.0f, out_w - 20.0f);
        const f32 fit_h = std::max(0.0f, out_h - 20.0f);
        _bounds = {
            10.0f,
            10.0f,
            std::clamp(out_w * default_fraction, std::min(win_min_w, fit_w), std::min(default_panel_w, fit_w)),
            std::clamp(out_h * default_fraction, std::min(win_min_h, fit_h), std::min(default_panel_h, fit_h)),
        };
        _bounds_initialized = true;
    }

    const f32 available_w = std::max(1.0f, out_w - _bounds.x - 10.0f);
    const f32 available_h = std::max(1.0f, out_h - _bounds.y - 10.0f);
    const f32 panel_min_w = std::min(win_min_w, available_w);
    const f32 panel_min_h = std::min(win_min_h, available_h);
    _bounds.w = std::clamp(_bounds.w, panel_min_w, available_w);
    _bounds.h = std::clamp(_bounds.h, panel_min_h, available_h);
    _bounds.x = std::clamp(_bounds.x, 0.0f, std::max(0.0f, out_w - _bounds.w));
    _bounds.y = std::clamp(_bounds.y, 0.0f, std::max(0.0f, out_h - _bounds.h));

    const Vec2f pointer = _ui.pointer();
    Rectf grip = resize_grip(_bounds);
    if (input.mouse_pressed(MouseButton::Left) && rect_contains(grip, pointer)) {
        _resizing = true;
        _resize_start_pointer = pointer;
        _resize_start_bounds = _bounds;
    }
    if (!input.mouse_held(MouseButton::Left)) {
        _resizing = false;
    }
    if (_resizing) {
        const f32 resize_available_w = std::max(1.0f, out_w - _resize_start_bounds.x);
        const f32 resize_available_h = std::max(1.0f, out_h - _resize_start_bounds.y);
        const f32 delta_x = pointer.x - _resize_start_pointer.x;
        const f32 delta_y = pointer.y - _resize_start_pointer.y;
        _bounds.w = std::clamp(_resize_start_bounds.w + delta_x,
                               std::min(win_min_w, resize_available_w),
                               resize_available_w);
        _bounds.h = std::clamp(_resize_start_bounds.h + delta_y,
                               std::min(win_min_h, resize_available_h),
                               resize_available_h);
    }
    grip = resize_grip(_bounds);

    const f32 row_h = debug_row_h;
    const f32 header_h = 50.0f;
    const f32 table_y = 56.0f;
    constexpr i32 control_toggle_rows = 5;
    constexpr i32 batch_stat_rows = 7;
    constexpr i32 render_stat_rows = 25;
    const f32 controls_h = 32.0f +
        static_cast<f32>(control_toggle_rows) * row_h +
        static_cast<f32>(control_toggle_rows - 1) * 6.0f +
        12.0f +
        row_h +
        static_cast<f32>(batch_stat_rows) * row_h +
        12.0f +
        row_h +
        static_cast<f32>(render_stat_rows) * row_h +
        12.0f;
    const Rectf panel = _bounds;

    const ui2::SurfaceStyle style = debug_surface(Color::rgba(10, 11, 13, 236), Color::rgba(118, 126, 140, 220));
    const ui2::SurfaceStyle header_style = debug_surface(Color::rgba(32, 35, 41, 236), Color::rgba(82, 88, 98, 200));
    const ui2::SurfaceStyle row_style = debug_surface(Color::rgba(18, 19, 22, 210), Color::rgba(42, 46, 52, 140));
    const ui2::SurfaceStyle row_alt_style = debug_surface(Color::rgba(24, 25, 29, 210), Color::rgba(42, 46, 52, 140));
    const ui2::WidgetStyle toggle_style = debug_toggle_style(Color::rgba(18, 19, 22, 210), Color::rgba(42, 46, 52, 140));
    const ui2::Font font = debug_font();
    ui2::TextStyle title{.font = font, .scale = 1.0f, .color = Color::rgb(245, 247, 250)};
    ui2::TextStyle text{.font = font, .scale = 1.0f, .color = Color::rgb(232, 235, 240)};
    ui2::TextStyle muted{.font = font, .scale = 1.0f, .color = Color::rgb(170, 176, 186)};
    ui2::TextStyle number = text;
    number.color = Color::rgb(235, 238, 242);

    constexpr f32 scrollbar_w = 10.0f;
    const Rectf scroll_bounds{
        panel.x + 8.0f,
        panel.y + table_y + row_h,
        std::max(0.0f, panel.w - 16.0f),
        std::max(0.0f, panel.h - table_y - row_h - grip_size - 8.0f),
    };
    const f32 content_h = static_cast<f32>(_display_rows.size()) * row_h + 8.0f + controls_h + 8.0f;
    _scroll.id = ui2::make_id("debug.overlay.scroll");
    _scroll.bounds = scroll_bounds;
    _scroll.content_height = content_h;
    _scroll.content_size = {std::max(0.0f, scroll_bounds.w - scrollbar_w), content_h};
    _scroll.wheel_step2 = {48.0f, 48.0f};
    _scroll.scrollbar_thickness = scrollbar_w;
    _scroll.min_thumb = 18.0f;
    _scroll.draw_background = false;
    _scroll.show_scrollbar = true;
    _scroll.horizontal = false;
    _scroll.enabled = !_resizing;
    _scroll.scrollbar_track = Color::rgba(24, 28, 38, 210);
    _scroll.scrollbar_thumb = Color::rgba(86, 142, 220, 220);
    ui2::run(_ui, _scroll);

    const f32 name_x = panel.x + 14.0f;
    const f32 p99_r = panel.x + panel.w - 34.0f - scrollbar_w;
    const f32 med_r = p99_r - 92.0f;
    const f32 last_r = med_r - 92.0f;
    std::vector<SelectableDebugLine> selectable_lines;
    const auto add_selectable = [&](Rectf bounds, std::string text_value) -> i32 {
        selectable_lines.push_back({bounds, std::move(text_value)});
        return static_cast<i32>(selectable_lines.size()) - 1;
    };
    const auto hit_selectable = [&](Vec2f pos) -> i32 {
        for (i32 i = 0; i < static_cast<i32>(selectable_lines.size()); ++i) {
            const bool fixed_header = i == 0;
            if ((fixed_header || rect_contains(_scroll.viewport, pos)) &&
                rect_contains(selectable_lines[static_cast<std::size_t>(i)].bounds, pos)) {
                return i;
            }
        }
        return -1;
    };
    const auto selection_active = [&] {
        return _selection_anchor >= 0 && _selection_cursor >= 0;
    };
    const auto selection_contains = [&](i32 index) {
        if (!selection_active()) {
            return false;
        }
        const i32 first = std::min(_selection_anchor, _selection_cursor);
        const i32 last = std::max(_selection_anchor, _selection_cursor);
        return index >= first && index <= last;
    };
    const auto copy_selection = [&] {
        if (!selection_active()) {
            return;
        }
        const i32 first = std::min(_selection_anchor, _selection_cursor);
        const i32 last = std::max(_selection_anchor, _selection_cursor);
        std::string copy;
        for (i32 i = first; i <= last && i < static_cast<i32>(selectable_lines.size()); ++i) {
            if (!copy.empty()) {
                copy.push_back('\n');
            }
            copy += selectable_lines[static_cast<std::size_t>(i)].text;
        }
        if (!copy.empty()) {
            input.set_clipboard_text(copy);
        }
    };
    const std::string capture_label = _capture_active ? "CAPTURING F2" : (_capture_available ? "CAPTURE FROZEN" : "LIVE");
    add_selectable({panel.x + 8.0f, panel.y + 8.0f, panel.w - 16.0f, row_h},
                   "KIN DEBUG  " + _display_fps + "  " + capture_label);

    f32 y = _scroll.content.y;
    for (const DisplayRow& timing : _display_rows) {
        const Rectf row_bounds{_scroll.viewport.x, y - 3.0f, _scroll.viewport.w, row_h};
        add_selectable(row_bounds, timing.copy_text);
        y += row_h;
    }

    const f32 controls_top = _scroll.content.y + static_cast<f32>(_display_rows.size()) * row_h + 8.0f;
    const Rectf controls_panel{_scroll.viewport.x, controls_top, _scroll.viewport.w, controls_h};
    const f32 toggle_w = std::max(128.0f, (controls_panel.w - 32.0f) * 0.5f);
    const f32 left_x = controls_panel.x + 8.0f;
    const f32 right_x = std::min(controls_panel.x + controls_panel.w - toggle_w - 8.0f, left_x + toggle_w + 16.0f);
    f32 control_y = controls_panel.y + 32.0f;
    control_y += row_h + 6.0f;
    control_y += row_h + 6.0f;
    control_y += row_h + 6.0f;
    control_y += row_h + 6.0f;

    const RendererBackendStats backend = renderer.backend_stats();
    const f32 stats_y = control_y + row_h + 12.0f;
    const f32 stat_name_x = controls_panel.x + 8.0f;
    const f32 stat_value_r = controls_panel.x + controls_panel.w - 18.0f;
    const f32 stat_label_w = std::min(260.0f, controls_panel.w * 0.58f);
    const f32 stat_value_x = stat_name_x + stat_label_w;
    const f32 stat_value_w = std::max(64.0f, stat_value_r - stat_value_x);
    const auto stat_text_row = [&](std::string_view name, std::string_view value, f32 row_y, i32 row) {
        const Rectf bounds{stat_name_x - 2.0f, row_y - 3.0f, controls_panel.w - 14.0f, row_h};
        (void)row;
        add_selectable(bounds, std::string{name} + "\t" + std::string{value});
    };
    const auto stat_row = [&](std::string_view name, u64 value, f32 row_y, i32 row) {
        stat_text_row(name, fmt_u64(value), row_y, row);
    };

    stat_row("Texture draws", backend.texture_draws_submitted, stats_y + row_h, 0);
    stat_row("Batch flushes", backend.texture_batch_flushes, stats_y + row_h * 2.0f, 1);
    stat_row("Saved texture draws", backend.saved_texture_draws(), stats_y + row_h * 3.0f, 2);
    stat_row("Batch breaks", backend.texture_batch_breaks, stats_y + row_h * 4.0f, 3);
    stat_row("Rect fills", backend.rect_fills_submitted, stats_y + row_h * 5.0f, 4);
    stat_row("Rect batch flushes", backend.rect_batch_flushes, stats_y + row_h * 6.0f, 5);
    stat_row("Saved rect draws", backend.saved_rect_draws(), stats_y + row_h * 7.0f, 6);

    const RuntimeRenderDebugStats render_stats = _options.render_stats;
    const std::string_view last_cache_dirty = render_stats.static_cache_last_dirty_reason.empty()
        ? std::string_view{"none"}
        : std::string_view{render_stats.static_cache_last_dirty_reason};
    const f32 render_stats_y = stats_y + row_h * 8.0f + 12.0f;
    stat_row("Static commands", render_stats.static_commands, render_stats_y + row_h, 0);
    stat_row("Dynamic commands", render_stats.dynamic_commands, render_stats_y + row_h * 2.0f, 1);
    stat_row("Dynamic before cull", render_stats.commands_before_cull, render_stats_y + row_h * 3.0f, 2);
    stat_row("Dynamic culled", render_stats.commands_culled, render_stats_y + row_h * 4.0f, 3);
    stat_row("Cache rebuilds", render_stats.static_cache_rebuilds, render_stats_y + row_h * 5.0f, 4);
    stat_text_row("Static cache used", render_stats.static_cache_used ? "yes" : "no", render_stats_y + row_h * 6.0f, 5);
    stat_text_row("Culling used", render_stats.culling_used ? "yes" : "no", render_stats_y + row_h * 7.0f, 6);
    stat_row("Render stat frames", render_stats.frames, render_stats_y + row_h * 8.0f, 7);
    stat_text_row("Last cache dirty", last_cache_dirty, render_stats_y + row_h * 9.0f, 8);
    stat_row("Cache initial", render_stats.static_cache_rebuilds_initial, render_stats_y + row_h * 10.0f, 9);
    stat_row("Cache explicit", render_stats.static_cache_rebuilds_explicit, render_stats_y + row_h * 11.0f, 10);
    stat_row("Cache clear", render_stats.static_cache_rebuilds_clear, render_stats_y + row_h * 12.0f, 11);
    stat_row("Cache other", render_stats.static_cache_rebuilds_other, render_stats_y + row_h * 13.0f, 12);
    stat_row("Dynamic queue cap", render_stats.dynamic_queue_capacity, render_stats_y + row_h * 14.0f, 13);
    stat_row("Dynamic queue grows", render_stats.dynamic_queue_capacity_grows, render_stats_y + row_h * 15.0f, 14);
    stat_row("UI static commands", render_stats.ui_static_commands, render_stats_y + row_h * 16.0f, 15);
    stat_row("UI dynamic commands", render_stats.ui_dynamic_commands, render_stats_y + row_h * 17.0f, 16);
    stat_row("UI static cap", render_stats.ui_static_command_capacity, render_stats_y + row_h * 18.0f, 17);
    stat_row("UI dynamic cap", render_stats.ui_dynamic_command_capacity, render_stats_y + row_h * 19.0f, 18);
    stat_row("UI static grows", render_stats.ui_static_command_capacity_grows, render_stats_y + row_h * 20.0f, 19);
    stat_row("UI dynamic grows", render_stats.ui_dynamic_command_capacity_grows, render_stats_y + row_h * 21.0f, 20);
    stat_row("UI static record cap", render_stats.ui_static_record_capacity, render_stats_y + row_h * 22.0f, 21);
    stat_row("UI dynamic record cap", render_stats.ui_dynamic_record_capacity, render_stats_y + row_h * 23.0f, 22);
    stat_row("UI static record grows", render_stats.ui_static_record_capacity_grows, render_stats_y + row_h * 24.0f, 23);
    stat_row("UI dynamic record grows", render_stats.ui_dynamic_record_capacity_grows, render_stats_y + row_h * 25.0f, 24);

    const bool bounds_changed = panel.x != _visual_cache_bounds.x ||
        panel.y != _visual_cache_bounds.y ||
        panel.w != _visual_cache_bounds.w ||
        panel.h != _visual_cache_bounds.h;
    if (bounds_changed) {
        _visual_cache_bounds = panel;
        _visual_cache_dirty = true;
    }

    if (true) {
        ui2::Context& ui = _ui;
        const auto label = [&](std::string_view value, f32 x, f32 y, const ui2::TextStyle& style_value) {
            ui.text(value, {x, y}, style_value);
        };
        const auto right_label = [&](std::string_view value, f32 right, f32 y, const ui2::TextStyle& style_value) {
            const Vec2f size = ui2::measure_text(style_value.font, value, style_value.scale);
            ui.text(value, {right - size.x, y}, style_value);
        };
        ui.surface(panel, style);
        ui.push_clip(panel);
        label("KIN DEBUG", panel.x + 10.0f, panel.y + 10.0f, title);
        right_label(_display_fps, panel.x + panel.w - 14.0f, panel.y + 10.0f, text);
        label(capture_label, panel.x + 10.0f, panel.y + 30.0f, muted);

        const Rectf table_header{_scroll.viewport.x, panel.y + header_h, _scroll.viewport.w, row_h};
        ui.surface(table_header, header_style);
        label("STEP", name_x, table_header.y + 3.0f, muted);
        right_label("LAST", last_r, table_header.y + 3.0f, muted);
        right_label("MED", med_r, table_header.y + 3.0f, muted);
        right_label("P99", p99_r, table_header.y + 3.0f, muted);

        ui.push_clip(_scroll.viewport);
        f32 row_y = _scroll.content.y;
        i32 row_index = 0;
        for (const DisplayRow& timing : _display_rows) {
            const Rectf row_bounds{_scroll.viewport.x, row_y - 3.0f, _scroll.viewport.w, row_h};
            ui.surface(row_bounds, row_index % 2 == 0 ? row_style : row_alt_style);
            label(timing.label, name_x, row_y, text);
            right_label(timing.last, last_r, row_y, number);
            right_label(timing.median, med_r, row_y, number);
            right_label(timing.p99, p99_r, row_y, number);
            row_y += row_h;
            ++row_index;
        }

        ui.surface(controls_panel, header_style);
        label("RENDER CONTROLS", controls_panel.x + 8.0f, controls_panel.y + 8.0f, muted);

        const auto cached_stat_text_row = [&](std::string_view name, std::string_view value, f32 row_y_value, i32 row) {
            const Rectf bounds{stat_name_x - 2.0f, row_y_value - 3.0f, controls_panel.w - 14.0f, row_h};
            ui.surface(bounds, row % 2 == 0 ? row_style : row_alt_style);
            label(name, stat_name_x, row_y_value, text);
            right_label(value, stat_value_x + stat_value_w, row_y_value, number);
        };
        const auto cached_stat_row = [&](std::string_view name, u64 value, f32 row_y_value, i32 row) {
            cached_stat_text_row(name, fmt_u64(value), row_y_value, row);
        };

        label("BATCH STATS", stat_name_x, stats_y, muted);
        cached_stat_row("Texture draws", backend.texture_draws_submitted, stats_y + row_h, 0);
        cached_stat_row("Batch flushes", backend.texture_batch_flushes, stats_y + row_h * 2.0f, 1);
        cached_stat_row("Saved texture draws", backend.saved_texture_draws(), stats_y + row_h * 3.0f, 2);
        cached_stat_row("Batch breaks", backend.texture_batch_breaks, stats_y + row_h * 4.0f, 3);
        cached_stat_row("Rect fills", backend.rect_fills_submitted, stats_y + row_h * 5.0f, 4);
        cached_stat_row("Rect batch flushes", backend.rect_batch_flushes, stats_y + row_h * 6.0f, 5);
        cached_stat_row("Saved rect draws", backend.saved_rect_draws(), stats_y + row_h * 7.0f, 6);

        label("SCENE RENDER STATS", stat_name_x, render_stats_y, muted);
        cached_stat_row("Static commands", render_stats.static_commands, render_stats_y + row_h, 0);
        cached_stat_row("Dynamic commands", render_stats.dynamic_commands, render_stats_y + row_h * 2.0f, 1);
        cached_stat_row("Dynamic before cull", render_stats.commands_before_cull, render_stats_y + row_h * 3.0f, 2);
        cached_stat_row("Dynamic culled", render_stats.commands_culled, render_stats_y + row_h * 4.0f, 3);
        cached_stat_row("Cache rebuilds", render_stats.static_cache_rebuilds, render_stats_y + row_h * 5.0f, 4);
        cached_stat_text_row("Static cache used", render_stats.static_cache_used ? "yes" : "no", render_stats_y + row_h * 6.0f, 5);
        cached_stat_text_row("Culling used", render_stats.culling_used ? "yes" : "no", render_stats_y + row_h * 7.0f, 6);
        cached_stat_row("Render stat frames", render_stats.frames, render_stats_y + row_h * 8.0f, 7);
        cached_stat_text_row("Last cache dirty", last_cache_dirty, render_stats_y + row_h * 9.0f, 8);
        cached_stat_row("Cache initial", render_stats.static_cache_rebuilds_initial, render_stats_y + row_h * 10.0f, 9);
        cached_stat_row("Cache explicit", render_stats.static_cache_rebuilds_explicit, render_stats_y + row_h * 11.0f, 10);
        cached_stat_row("Cache clear", render_stats.static_cache_rebuilds_clear, render_stats_y + row_h * 12.0f, 11);
        cached_stat_row("Cache other", render_stats.static_cache_rebuilds_other, render_stats_y + row_h * 13.0f, 12);
        cached_stat_row("Dynamic queue cap", render_stats.dynamic_queue_capacity, render_stats_y + row_h * 14.0f, 13);
        cached_stat_row("Dynamic queue grows", render_stats.dynamic_queue_capacity_grows, render_stats_y + row_h * 15.0f, 14);
        cached_stat_row("UI static commands", render_stats.ui_static_commands, render_stats_y + row_h * 16.0f, 15);
        cached_stat_row("UI dynamic commands", render_stats.ui_dynamic_commands, render_stats_y + row_h * 17.0f, 16);
        cached_stat_row("UI static cap", render_stats.ui_static_command_capacity, render_stats_y + row_h * 18.0f, 17);
        cached_stat_row("UI dynamic cap", render_stats.ui_dynamic_command_capacity, render_stats_y + row_h * 19.0f, 18);
        cached_stat_row("UI static grows", render_stats.ui_static_command_capacity_grows, render_stats_y + row_h * 20.0f, 19);
        cached_stat_row("UI dynamic grows", render_stats.ui_dynamic_command_capacity_grows, render_stats_y + row_h * 21.0f, 20);
        cached_stat_row("UI static record cap", render_stats.ui_static_record_capacity, render_stats_y + row_h * 22.0f, 21);
        cached_stat_row("UI dynamic record cap", render_stats.ui_dynamic_record_capacity, render_stats_y + row_h * 23.0f, 22);
        cached_stat_row("UI static record grows", render_stats.ui_static_record_capacity_grows, render_stats_y + row_h * 24.0f, 23);
        cached_stat_row("UI dynamic record grows", render_stats.ui_dynamic_record_capacity_grows, render_stats_y + row_h * 25.0f, 24);

        ui.pop_clip();
        if (_scroll.show_scrollbar && _scroll.scrollbar_thickness > 0.0f && _scroll.thumb.h < _scroll.track.h) {
            ui.fill_rect(_scroll.track, _scroll.scrollbar_track);
            ui.fill_rect(_scroll.thumb, _scroll.scrollbar_thumb);
        }
        ui.pop_clip();
        _visual_cache_dirty = false;
    }

    const i32 hit = hit_selectable(pointer);
    const bool pointer_over_grip = rect_contains(grip, pointer);
    const bool grip_pressed = input.mouse_pressed(MouseButton::Left) && pointer_over_grip;
    if (!grip_pressed && input.mouse_pressed(MouseButton::Left) && hit >= 0) {
        _selecting_text = true;
        _selection_anchor = hit;
        _selection_cursor = hit;
    } else if (_selecting_text && input.mouse_held(MouseButton::Left)) {
        if (hit >= 0) {
            _selection_cursor = hit;
        }
    } else if (_selecting_text && input.mouse_released(MouseButton::Left)) {
        _selecting_text = false;
    } else if (input.mouse_pressed(MouseButton::Left) && hit < 0 && !pointer_over_grip) {
        _selection_anchor = -1;
        _selection_cursor = -1;
        _selecting_text = false;
    }

    if (input.modifier_held(KeyModifiers::Ctrl) && input.pressed(Key::C)) {
        copy_selection();
    }

    ui2::Context& ui = _ui;
    ui.push_clip(_scroll.viewport);
    for (i32 i = 0; i < static_cast<i32>(selectable_lines.size()); ++i) {
        if (selection_contains(i)) {
            ui.surface(selectable_lines[static_cast<std::size_t>(i)].bounds,
                       debug_surface(Color::rgba(86, 142, 220, 96), Color::rgba(130, 180, 245, 120)));
        }
    }

    const auto toggle = [&](std::string_view id, std::string_view text_value, Rectf bounds, bool& value) {
        const bool previous = value;
        ui2::Toggle widget{
            .id = ui2::make_id(id),
            .bounds = bounds,
            .label = std::string{text_value},
            .text_style = text,
            .style = toggle_style,
            .value = value,
            .enabled = true,
        };
        ui2::run(ui, widget);
        value = widget.value;
        if (value != previous) {
            _visual_cache_dirty = true;
            if (id == std::string_view{"debug.detail_timings"}) {
                refresh_display_cache();
            }
        }
    };

    control_y = controls_panel.y + 32.0f;
    toggle("debug.static_cache", "Static cache", {left_x, control_y, toggle_w, row_h}, _options.static_render_cache_enabled);
    toggle("debug.pass_world", "World pass", {right_x, control_y, toggle_w, row_h}, _options.pass_world_enabled);
    control_y += row_h + 6.0f;
    toggle("debug.culling", "Camera culling", {left_x, control_y, toggle_w, row_h}, _options.camera_culling_enabled);
    toggle("debug.pass_effects", "Effects pass", {right_x, control_y, toggle_w, row_h}, _options.pass_effects_enabled);
    control_y += row_h + 6.0f;
    toggle("debug.pass_ui", "UI pass", {left_x, control_y, toggle_w, row_h}, _options.pass_ui_enabled);
    toggle("debug.pass_debug", "Debug pass", {right_x, control_y, toggle_w, row_h}, _options.pass_debug_enabled);
    control_y += row_h + 6.0f;
    toggle("debug.texture_batching", "Texture batching", {left_x, control_y, toggle_w, row_h}, _options.texture_batching_enabled);
    toggle("debug.detail_timings", "Detailed timings", {right_x, control_y, toggle_w, row_h}, _options.detailed_render_timings_enabled);
    control_y += row_h + 6.0f;
    toggle("debug.cull_diagnostics", "Cull diagnostics", {left_x, control_y, toggle_w, row_h}, _options.render_culling_diagnostics_enabled);

    ui.pop_clip();
    ui.surface(grip, row_alt_style);
    ui.fill_rect({grip.x + grip.w - 20.0f, grip.y + grip.h - 7.0f, 16.0f, 1.0f}, Color::rgba(170, 190, 210, 220));
    ui.fill_rect({grip.x + grip.w - 14.0f, grip.y + grip.h - 13.0f, 10.0f, 1.0f}, Color::rgba(170, 190, 210, 220));
    ui.fill_rect({grip.x + grip.w - 8.0f, grip.y + grip.h - 19.0f, 4.0f, 1.0f}, Color::rgba(170, 190, 210, 220));
    _ui.end();
}

DebugTimingStats RuntimeDebugOverlay::stats(std::string_view name) const {
    return stats_from(_rows, name);
}

DebugTimingStats RuntimeDebugOverlay::stats_from(const std::vector<TimingRow>& rows, std::string_view name) const {
    const TimingRow* target = find(rows, name);
    if (!target || target->samples.empty()) {
        return {};
    }

    std::vector<f64> samples;
    if (!target->filled) {
        samples = target->samples;
    } else {
        samples.reserve(target->samples.size());
        samples.insert(samples.end(), target->samples.begin() + static_cast<std::ptrdiff_t>(target->next), target->samples.end());
        samples.insert(samples.end(), target->samples.begin(), target->samples.begin() + static_cast<std::ptrdiff_t>(target->next));
    }
    return {
        .last_ms = samples.back(),
        .median_ms = percentile(samples, 0.5),
        .p99_ms = percentile(samples, 0.99),
    };
}

RuntimeDebugOverlay::TimingRow& RuntimeDebugOverlay::row(std::string_view name) {
    return row(_rows, name);
}

RuntimeDebugOverlay::TimingRow& RuntimeDebugOverlay::row(std::vector<TimingRow>& rows, std::string_view name) {
    if (TimingRow* existing = const_cast<TimingRow*>(find(rows, name))) {
        return *existing;
    }

    rows.push_back({.name = std::string{name}});
    rows.back().samples.reserve(sample_capacity);
    return rows.back();
}

const RuntimeDebugOverlay::TimingRow* RuntimeDebugOverlay::find(std::string_view name) const {
    return find(_rows, name);
}

const RuntimeDebugOverlay::TimingRow* RuntimeDebugOverlay::find(const std::vector<TimingRow>& rows, std::string_view name) const {
    const auto found = std::ranges::find_if(rows, [&](const TimingRow& row) {
        return row.name == name;
    });
    return found == rows.end() ? nullptr : &*found;
}

const std::vector<RuntimeDebugOverlay::TimingRow>& RuntimeDebugOverlay::display_rows_source() const {
    if (_capture_active || _capture_available) {
        return _capture_rows;
    }
    return _rows;
}

void RuntimeDebugOverlay::refresh_display_cache() {
    const std::vector<TimingRow>& rows = display_rows_source();
    _display_frame_stats = stats_from(rows, "frame");
    _display_fps = fmt_fps(_display_frame_stats.last_ms);
    _display_rows.clear();
    _display_rows.reserve(rows.size());
    for (const TimingRow& timing : rows) {
        if (!_options.detailed_render_timings_enabled && timing.name.starts_with("render.scene.")) {
            continue;
        }
        const DebugTimingStats row_stats = stats_from(rows, timing.name);
        DisplayRow row;
        row.label = display_label(timing.name);
        row.last = fmt_ms(row_stats.last_ms);
        row.median = fmt_ms(row_stats.median_ms);
        row.p99 = fmt_ms(row_stats.p99_ms);
        row.copy_text = row.label + "\tlast=" + row.last + "\tmedian=" + row.median + "\tp99=" + row.p99;
        _display_rows.push_back(std::move(row));
    }
    for (const auto& [name, value] : _status_rows) {
        DisplayRow row;
        row.label = name;
        row.last = value;
        row.copy_text = name + "\t" + value;
        _display_rows.push_back(std::move(row));
    }
}

} // namespace kin
