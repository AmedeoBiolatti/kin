// Charts: plots of series, sparklines and heat maps.

#include <kin/ui2/charts.hpp>

#include <kin/ui2/context.hpp>

#include "style_internal.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>

namespace kin::ui2 {
namespace {

// The data-visualisation palette's categorical slots, in its order (checked
// for colour-blind readers adjacent pair by adjacent pair), light and dark steps.
constexpr std::array<Color, 8> series_light{Color::rgb(0x2a, 0x78, 0xd6), Color::rgb(0xeb, 0x68, 0x34),
                                            Color::rgb(0x1b, 0xaf, 0x7a), Color::rgb(0xed, 0xa1, 0x00),
                                            Color::rgb(0xe8, 0x7b, 0xa4), Color::rgb(0x00, 0x83, 0x00),
                                            Color::rgb(0x62, 0x50, 0xd6), Color::rgb(0xe3, 0x49, 0x48)};
constexpr std::array<Color, 8> series_dark{Color::rgb(0x39, 0x87, 0xe5), Color::rgb(0xd9, 0x59, 0x26),
                                           Color::rgb(0x19, 0x9e, 0x70), Color::rgb(0xc9, 0x85, 0x00),
                                           Color::rgb(0xd5, 0x51, 0x81), Color::rgb(0x00, 0x83, 0x00),
                                           Color::rgb(0x90, 0x85, 0xe9), Color::rgb(0xe6, 0x67, 0x67)};
constexpr Color series_other = Color::rgb(0x89, 0x87, 0x81);

// matplotlib's maps at nine even stops.
constexpr std::array<Color, 9> magma{Color::rgb(0x00, 0x00, 0x04), Color::rgb(0x1c, 0x10, 0x44), Color::rgb(0x4f, 0x12, 0x7b),
                                     Color::rgb(0x81, 0x25, 0x81), Color::rgb(0xb5, 0x36, 0x7a), Color::rgb(0xe5, 0x50, 0x64),
                                     Color::rgb(0xfb, 0x87, 0x61), Color::rgb(0xfe, 0xc2, 0x87), Color::rgb(0xfc, 0xfd, 0xbf)};
constexpr std::array<Color, 9> viridis{Color::rgb(0x44, 0x01, 0x54), Color::rgb(0x48, 0x28, 0x78), Color::rgb(0x3e, 0x49, 0x89),
                                       Color::rgb(0x31, 0x68, 0x8e), Color::rgb(0x26, 0x82, 0x8e), Color::rgb(0x1f, 0x9e, 0x89),
                                       Color::rgb(0x35, 0xb7, 0x79), Color::rgb(0x6e, 0xce, 0x58), Color::rgb(0xfd, 0xe7, 0x25)};

constexpr u8 wash_alpha = 26; // an area's wash: a tenth of its colour
constexpr f32 bar_gap = 2.0f;
constexpr f32 point_radius = 4.0f;
constexpr f32 ring = 2.0f; // the surface's colour round a point, where it crosses a line

Color with_alpha(Color color, u8 alpha) {
    color.a = alpha;
    return color;
}

bool dark_surface(Color c) {
    return 0.2126f * static_cast<f32>(c.r) + 0.7152f * static_cast<f32>(c.g) + 0.0722f * static_cast<f32>(c.b) < 128.0f;
}

// What a chart draws with: the style's colours, the theme's where it gives none.
struct ChartLook {
    Color background{};
    Color grid{};
    Color baseline{};
    TextStyle text{};   // the axes' values, the legend's names
    TextStyle strong{}; // the readout's values
    TextStyle faint{};  // the readout's names
    Color readout_fill{};
    Color readout_border{};
    f32 line_height = 0.0f;
    bool dark = true;
};

ChartLook chart_look(const Context& ctx, const PlotStyle& style) {
    const Theme& theme = ctx.theme();
    ChartLook look;
    look.background = style.background.a > 0 ? style.background : theme.palette.surface;
    look.dark = dark_surface(look.background);
    look.grid = style.grid.a > 0 ? style.grid : mix(look.background, theme.palette.text, 0.08f);
    look.baseline = mix(look.background, theme.palette.text, 0.18f);
    look.text = detail::themed_text_style(style.text_style, theme.small_text);
    look.text.color = style.text.a > 0 ? style.text : theme.palette.text_muted;
    const Context::TooltipOptions tooltip = ctx.tooltip_style();
    look.readout_fill = tooltip.fill;
    look.readout_border = tooltip.border;
    look.strong = look.text;
    look.strong.color = theme.palette.text;
    look.faint = look.text;
    look.line_height = measure_text(look.text.font, "0", look.text.scale).y;
    return look;
}

Vec2f text_size(const TextStyle& style, std::string_view text) {
    return measure_text(style.font, text, style.scale);
}

i32 length(const Series& s) {
    return s.x.empty() ? static_cast<i32>(s.y.size()) : static_cast<i32>(std::min(s.x.size(), s.y.size()));
}

f64 x_at(const Series& s, i32 i) {
    return s.x.empty() ? s.x0 + static_cast<f64>(i) * s.dx : static_cast<f64>(s.x[static_cast<std::size_t>(i)]);
}

f32 y_at(const Series& s, i32 i) {
    return s.y[static_cast<std::size_t>(i)];
}

// The samples from the last at or before `lo` to the first at or after `hi`.
std::pair<i32, i32> visible(const Series& s, f64 lo, f64 hi) {
    const i32 n = length(s);
    if (n == 0) {
        return {0, -1};
    }
    if (s.x.empty()) {
        if (s.dx <= 0.0) {
            return {0, n - 1};
        }
        const f64 first = std::floor((lo - s.x0) / s.dx);
        const f64 last = std::ceil((hi - s.x0) / s.dx);
        return {static_cast<i32>(std::clamp(first, 0.0, static_cast<f64>(n - 1))),
                static_cast<i32>(std::clamp(last, 0.0, static_cast<f64>(n - 1)))};
    }
    const auto begin = s.x.begin();
    const auto end = s.x.begin() + n;
    const i32 first = static_cast<i32>(std::lower_bound(begin, end, static_cast<f32>(lo)) - begin) - 1;
    const i32 last = static_cast<i32>(std::lower_bound(begin, end, static_cast<f32>(hi)) - begin);
    return {std::max(0, first), std::min(n - 1, last)};
}

// The sample nearest `at` along x.
i32 nearest(const Series& s, f64 at) {
    const i32 n = length(s);
    if (n == 0) {
        return -1;
    }
    if (s.x.empty()) {
        return s.dx > 0.0 ? static_cast<i32>(std::clamp(std::round((at - s.x0) / s.dx), 0.0, static_cast<f64>(n - 1))) : 0;
    }
    const auto begin = s.x.begin();
    const i32 after = static_cast<i32>(std::lower_bound(begin, begin + n, static_cast<f32>(at)) - begin);
    if (after <= 0) {
        return 0;
    }
    if (after >= n) {
        return n - 1;
    }
    return at - x_at(s, after - 1) <= x_at(s, after) - at ? after - 1 : after;
}

f64 to_t(const Scale& scale, f64 v) {
    return scale.kind == ScaleKind::Log ? std::log10(std::max(v, std::numeric_limits<f64>::min())) : v;
}

f64 from_t(const Scale& scale, f64 t) {
    return scale.kind == ScaleKind::Log ? std::pow(10.0, t) : t;
}

// Where values map to on the plot.
struct Mapping {
    Rectf area{};
    Scale x{};
    Scale y{};
    f32 px(f64 v) const { return area.x + static_cast<f32>(x.fraction(v)) * area.w; }
    f32 py(f64 v) const { return area.y + area.h - static_cast<f32>(y.fraction(v)) * area.h; }
    // Where a bar or an area starts: 0, or the edge nearest it.
    f32 base() const {
        if (y.kind == ScaleKind::Log || y.min >= 0.0) {
            return area.y + area.h;
        }
        return y.max <= 0.0 ? area.y : py(0.0);
    }
};

// A line through samples i0 to i1. Fewer samples than twice the pixels: each
// one; more: each pixel column's lowest and highest, the column before's end
// included so the columns join, in the order that carries on from it.
void line_points(std::vector<Vec2f>& out, const Series& s, i32 i0, i32 i1, const Mapping& m) {
    out.clear();
    if (i1 < i0) {
        return;
    }
    if (static_cast<f32>(i1 - i0 + 1) <= m.area.w * 2.0f) {
        for (i32 i = i0; i <= i1; ++i) {
            if (std::isfinite(y_at(s, i))) {
                out.push_back({m.px(x_at(s, i)), m.py(y_at(s, i))});
            }
        }
        return;
    }
    const auto emit = [&](f32 column, f32 lo, f32 hi) {
        const f32 x = column + 0.5f;
        if (lo == hi) {
            out.push_back({x, lo});
        } else if (!out.empty() && std::abs(out.back().y - hi) < std::abs(out.back().y - lo)) {
            out.push_back({x, hi});
            out.push_back({x, lo});
        } else {
            out.push_back({x, lo});
            out.push_back({x, hi});
        }
    };
    f32 column = std::numeric_limits<f32>::quiet_NaN();
    f32 lo = 0.0f, hi = 0.0f, last = std::numeric_limits<f32>::quiet_NaN();
    for (i32 i = i0; i <= i1; ++i) {
        if (!std::isfinite(y_at(s, i))) {
            continue;
        }
        const f32 x = std::floor(m.px(x_at(s, i)));
        const f32 y = m.py(y_at(s, i));
        if (x != column) {
            if (!std::isnan(column)) {
                emit(column, lo, hi);
            }
            column = x;
            lo = hi = std::isnan(last) ? y : last;
        }
        lo = std::min(lo, y);
        hi = std::max(hi, y);
        last = y;
    }
    if (!std::isnan(column)) {
        emit(column, lo, hi);
    }
}

// Triangles from a line down (or up) to `base`, in one colour.
void wash_mesh(ShapeMesh& mesh, std::span<const Vec2f> points, f32 base, Color color) {
    mesh.clear();
    if (points.size() < 2) {
        return;
    }
    f32 x0 = points[0].x, x1 = x0, y0 = base, y1 = base;
    for (std::size_t k = 0; k < points.size(); ++k) {
        mesh.vertices.push_back({.position = points[k], .color = color});
        mesh.vertices.push_back({.position = {points[k].x, base}, .color = color});
        if (k > 0) {
            const u32 a = static_cast<u32>(2 * k - 2);
            mesh.indices.insert(mesh.indices.end(), {a, a + 2, a + 3, a, a + 3, a + 1});
        }
        x0 = std::min(x0, points[k].x);
        x1 = std::max(x1, points[k].x);
        y0 = std::min(y0, points[k].y);
        y1 = std::max(y1, points[k].y);
    }
    mesh.bounds = {x0, y0, x1 - x0, y1 - y0};
}

// A bar from `base` to `top`, its far end rounded, square where it starts.
void draw_bar(Context& ctx, f32 x, f32 w, f32 base, f32 top, Color color) {
    const Rectf bar{x, std::min(base, top), w, std::abs(top - base)};
    if (bar.h <= 0.0f || w <= 0.0f) {
        return;
    }
    const f32 r = std::min({4.0f, w * 0.5f, bar.h});
    if (r < 1.0f || w < 4.0f) {
        ctx.fill_rect(bar, color);
        return;
    }
    ctx.fill_rounded_rect(bar, r, color);
    ctx.fill_rect(top <= base ? Rectf{bar.x, bar.y + bar.h - r, w, r} : Rectf{bar.x, bar.y, w, r}, color);
}

// A legend's key for a series: a short line, a square for bars, a dot for points.
void draw_key(Context& ctx, Vec2f at, f32 height, SeriesKind kind, Color color) {
    const f32 mid = at.y + height * 0.5f;
    switch (kind) {
    case SeriesKind::Bars:
        ctx.fill_rounded_rect({at.x + 2.0f, mid - 5.0f, 10.0f, 10.0f}, 2.0f, color);
        break;
    case SeriesKind::Points:
        ctx.fill_circle({at.x + 7.0f, mid}, point_radius, color);
        break;
    default:
        ctx.line({at.x, mid}, {at.x + 14.0f, mid}, color, 2.0f, LineCap::Round);
        break;
    }
}

// A readout's rows: a key and a value, and what it is (a series' name).
struct ReadoutRow {
    std::string value;
    std::string name;
    SeriesKind kind = SeriesKind::Line;
    Color key = colors::transparent; // transparent: none (a heading)
};

// The readout's box by `pointer`, on the side with room, over everything.
void show_readout(Context& ctx, Vec2f pointer, std::vector<ReadoutRow> rows, const ChartLook& look) {
    ctx.overlay([pointer, rows = std::move(rows), look](Context& c) {
        constexpr f32 pad = 8.0f;
        constexpr f32 key_w = 20.0f;
        const f32 lh = look.line_height + 3.0f;
        f32 value_w = 0.0f, name_w = 0.0f;
        for (const ReadoutRow& row : rows) {
            value_w = std::max(value_w, text_size(look.strong, row.value).x);
            name_w = std::max(name_w, text_size(look.faint, row.name).x);
        }
        const Vec2f size{pad * 2.0f + key_w + value_w + (name_w > 0.0f ? 8.0f + name_w : 0.0f),
                         pad * 2.0f + lh * static_cast<f32>(rows.size()) - 3.0f};
        const Rectf room = c.layout_root().w > 0.0f ? c.layout_root() : Rectf{0.0f, 0.0f, 1.0e9f, 1.0e9f};
        Vec2f at{pointer.x + 14.0f, pointer.y + 14.0f};
        if (at.x + size.x > room.x + room.w) {
            at.x = pointer.x - 14.0f - size.x;
        }
        if (at.y + size.y > room.y + room.h) {
            at.y = pointer.y - 14.0f - size.y;
        }
        at = {std::max(at.x, room.x), std::max(at.y, room.y)};
        c.fill_rounded_rect({at.x, at.y, size.x, size.y}, 6.0f, look.readout_fill);
        c.outline_rounded_rect({at.x, at.y, size.x, size.y}, 6.0f, look.readout_border);
        f32 y = at.y + pad;
        for (const ReadoutRow& row : rows) {
            const f32 x = at.x + pad;
            if (row.key.a > 0) {
                draw_key(c, {x, y}, look.line_height, row.kind, row.key);
            }
            c.text(row.value, {x + key_w, y}, row.key.a > 0 ? look.strong : look.faint);
            if (!row.name.empty()) {
                c.text(row.name, {x + key_w + value_w + 8.0f, y}, look.faint);
            }
            y += lh;
        }
    });
}

// A value as the readout shows it: as fine as a thousandth of the range, but
// to three significant figures at most (112, not 112.0; 9.51 ms; -33.0 dB),
// whole numbers in full.
std::string readout_label(f64 value, const Scale& scale, std::string_view unit) {
    const f64 fine = (scale.kind == ScaleKind::Log ? std::abs(value) : scale.max - scale.min) / 1000.0;
    const f64 figures = value != 0.0 ? std::pow(10.0, std::floor(std::log10(std::abs(value))) - 2.0) : fine;
    return tick_label(value, std::max(fine, figures), unit, false); // in full: 5620, not 5.62k
}

// The axes' marks, laid out: values and where they go.
struct AxisMarks {
    std::vector<Tick> x;
    std::vector<Tick> y;
    f32 y_width = 0.0f; // the widest y value
};

AxisMarks axis_marks(const PlotAxis& x_axis, const Scale& x, const PlotAxis& y_axis, const Scale& y, const ChartLook& look) {
    AxisMarks marks;
    if (x_axis.ticks > 0) {
        marks.x = ticks(x, x_axis.ticks, x_axis.unit);
    }
    if (y_axis.ticks > 0) {
        marks.y = ticks(y, y_axis.ticks, y_axis.unit);
    }
    for (const Tick& tick : marks.y) {
        marks.y_width = std::max(marks.y_width, text_size(look.text, tick.label).x);
    }
    return marks;
}

// Where the data goes in `bounds`: past the y values on the left, above the
// x values, below a legend's row; `right` kept clear at the right.
Rectf plot_area(Rectf bounds, const AxisMarks& marks, const ChartLook& look, bool legend, f32 right) {
    const f32 left = marks.y.empty() ? 6.0f : marks.y_width + 12.0f;
    const f32 top = 8.0f + (legend ? look.line_height + 8.0f : 0.0f);
    const f32 bottom = marks.x.empty() ? 6.0f : look.line_height + 8.0f;
    return {bounds.x + left, bounds.y + top, std::max(1.0f, bounds.w - left - right), std::max(1.0f, bounds.h - top - bottom)};
}

// The values along the axes; grid lines across the area when `grid`.
void draw_axes(Context& ctx, const AxisMarks& marks, const Mapping& m, Rectf bounds, const ChartLook& look,
               bool x_grid, bool y_grid) {
    const Rectf a = m.area;
    for (const Tick& tick : marks.y) {
        const f32 y = std::round(m.py(tick.value)) + 0.5f;
        if (y_grid) {
            ctx.line({a.x, y}, {a.x + a.w, y}, look.grid);
        }
        const Vec2f size = text_size(look.text, tick.label);
        ctx.text(tick.label, {a.x - 8.0f - size.x, std::max(bounds.y, std::min(y - size.y * 0.5f, bounds.y + bounds.h - size.y))}, look.text);
    }
    f32 free_from = bounds.x; // labels do not overlap: one that would is left out
    for (const Tick& tick : marks.x) {
        const f32 x = std::round(m.px(tick.value)) + 0.5f;
        if (x_grid) {
            ctx.line({x, a.y}, {x, a.y + a.h}, look.grid);
        }
        const Vec2f size = text_size(look.text, tick.label);
        // Not into the y values' column (the corner where the lowest of them is).
        const f32 left = std::max(a.x, std::min(x - size.x * 0.5f, bounds.x + bounds.w - size.x));
        if (left >= free_from) {
            ctx.text(tick.label, {left, a.y + a.h + 4.0f}, look.text);
            free_from = left + size.x + 8.0f;
        }
    }
}

// What a plot keeps by id: its zoom, and the vectors it draws with.
struct PlotView {
    bool zoomed = false;
    bool at_end = true; // showing the newest: kept there as the data grows
    f64 t0 = 0.0, t1 = 0.0; // the view, in scale units (log10 of them on a Log axis)
    f32 grab_x = 0.0f;
    f64 grab_t0 = 0.0, grab_t1 = 0.0;
    bool moved = false; // since pressed: a pan, not a click
    std::vector<Vec2f> points;
    ShapeMesh wash;
};

Scale fitted_x(const Plot& plot) {
    if (plot.x.scale.valid()) {
        return plot.x.scale;
    }
    f64 lo = std::numeric_limits<f64>::infinity(), hi = -lo;
    for (const Series& s : plot.series) {
        const i32 n = length(s);
        if (n == 0) {
            continue;
        }
        f64 first = x_at(s, 0), last = x_at(s, n - 1);
        if (s.kind == SeriesKind::Bars) { // room for the end bars
            const f64 half = n > 1 ? (last - first) / static_cast<f64>(n - 1) * 0.5 : 0.5;
            first -= half;
            last += half;
        }
        lo = std::min(lo, first);
        hi = std::max(hi, last);
    }
    Scale out{.min = lo, .max = hi, .kind = plot.x.scale.kind};
    if (!std::isfinite(lo)) {
        out.min = 0.0;
        out.max = 1.0;
    } else if (hi <= lo) {
        out.min = lo - 0.5;
        out.max = hi + 0.5;
    }
    if (!out.valid()) {
        out.kind = ScaleKind::Linear;
    }
    return out;
}

Scale fitted_y(const Plot& plot) {
    if (plot.y.scale.valid()) {
        return plot.y.scale;
    }
    const bool log = plot.y.scale.kind == ScaleKind::Log;
    f64 lo = std::numeric_limits<f64>::infinity(), hi = -lo;
    bool from_zero = false;
    for (const Series& s : plot.series) {
        from_zero = from_zero || s.kind == SeriesKind::Bars || s.kind == SeriesKind::Area;
        for (const f32 v : s.y) {
            if (std::isfinite(v) && (!log || v > 0.0f)) {
                lo = std::min(lo, static_cast<f64>(v));
                hi = std::max(hi, static_cast<f64>(v));
            }
        }
    }
    if (!std::isfinite(lo)) {
        return nice_scale(log ? 1.0 : 0.0, log ? 10.0 : 1.0, plot.y.scale.kind);
    }
    if (from_zero && !log) {
        lo = std::min(lo, 0.0);
        hi = std::max(hi, 0.0);
    }
    return nice_scale(lo, hi, plot.y.scale.kind, std::max(2, plot.y.ticks));
}

} // namespace

Color chart_color(i32 index, bool dark) {
    if (index < 0 || index >= static_cast<i32>(series_dark.size())) {
        return series_other;
    }
    return (dark ? series_dark : series_light)[static_cast<std::size_t>(index)];
}

Color map_color(ColorMap map, f32 t) {
    t = std::isfinite(t) ? std::clamp(t, 0.0f, 1.0f) : 0.0f;
    if (map == ColorMap::Gray) {
        const u8 v = static_cast<u8>(std::lround(t * 255.0f));
        return Color::rgb(v, v, v);
    }
    const std::array<Color, 9>& table = map == ColorMap::Viridis ? viridis : magma;
    const f32 at = t * 8.0f;
    const i32 k = std::min(7, static_cast<i32>(at));
    return mix(table[static_cast<std::size_t>(k)], table[static_cast<std::size_t>(k + 1)], at - static_cast<f32>(k));
}

Vec2f measure(const Plot& widget) {
    return widget.size;
}

Vec2f measure(const Sparkline& widget) {
    return widget.size;
}

Vec2f measure(const Heatmap& widget) {
    return widget.size;
}

void run(Context& ctx, Plot& plot) {
    const Rectf b = plot.bounds;
    plot.interaction = ctx.region(plot.id, b, plot.z);
    plot.pointer_x.reset();
    plot.hovered.reset();
    if (b.w <= 0.0f || b.h <= 0.0f) {
        return;
    }
    const Interaction& it = plot.interaction;
    const ChartLook look = chart_look(ctx, plot.style);
    PlotView& view = ctx.state<PlotView>(plot.id);

    // The x range shown: all of it, or where the wheel zoomed and a drag panned.
    const Scale full = fitted_x(plot);
    const f64 full_t0 = to_t(full, full.min), full_t1 = to_t(full, full.max);
    if (!plot.zoom) {
        view.zoomed = false;
    }
    if (!view.zoomed) {
        view.t0 = full_t0;
        view.t1 = full_t1;
    }
    const Scale y = fitted_y(plot);
    const i32 named = static_cast<i32>(std::ranges::count_if(plot.series, [](const Series& s) { return !s.name.empty(); }));
    const bool legend = plot.legend && named >= 2;
    // The area is laid out with the full range's marks: as wide whatever the zoom.
    const AxisMarks layout_marks = axis_marks(plot.x, full, plot.y, y, look);
    const Rectf area = plot_area(b, layout_marks, look, legend, 10.0f);
    const Vec2f p = ctx.pointer();
    const bool in_area = contains(area, p);
    if (plot.zoom) {
        ctx.take_wheel(plot.id);
        const f64 full_span = full_t1 - full_t0;
        // Not narrower than a few samples.
        f64 least = full_span * 1e-6;
        for (const Series& s : plot.series) {
            if (s.x.empty() && s.dx > 0.0 && full.kind == ScaleKind::Linear) {
                least = std::max(least, s.dx * 4.0);
            }
        }
        least = std::min(least, full_span);
        const auto keep_in = [&](f64 span) {
            if (view.t0 < full_t0) {
                view.t0 = full_t0;
            }
            if (view.t0 + span > full_t1) {
                view.t0 = full_t1 - span;
            }
            view.t1 = view.t0 + span;
            view.zoomed = span < full_span * (1.0 - 1e-9);
        };
        // Zoomed in on data that grows (a live chart): as wide, and on the
        // newest while it showed them.
        if (view.zoomed) {
            const f64 span = view.t1 - view.t0;
            if (view.at_end) {
                view.t0 = full_t1 - span;
            }
            keep_in(span);
        }
        const f32 wheel = ctx.mouse_wheel_y();
        if (it.hot && in_area && wheel != 0.0f && !it.active) {
            const f64 span = view.t1 - view.t0;
            const f64 at = view.t0 + static_cast<f64>((p.x - area.x) / area.w) * span;
            const f64 next = std::clamp(span * std::pow(0.8, static_cast<f64>(wheel)), least, full_span);
            view.t0 = at - (at - view.t0) / span * next;
            keep_in(next);
        }
        if (it.pressed) {
            view.grab_x = p.x;
            view.grab_t0 = view.t0;
            view.grab_t1 = view.t1;
            view.moved = false;
        } else if (it.active) {
            view.moved = view.moved || std::abs(p.x - view.grab_x) >= 3.0f;
            if (view.moved && view.zoomed) {
                const f64 span = view.grab_t1 - view.grab_t0;
                view.t0 = view.grab_t0 - static_cast<f64>((p.x - view.grab_x) / area.w) * span;
                keep_in(span);
            }
        }
        if (it.clicked && !view.moved && ctx.double_click(plot.id)) {
            view.zoomed = false;
            view.t0 = full_t0;
            view.t1 = full_t1;
        }
        view.at_end = view.t1 >= full_t1 - (view.t1 - view.t0) * 0.02; // the newest in view, near enough
    }
    const Mapping m{.area = area, .x = {.min = from_t(full, view.t0), .max = from_t(full, view.t1), .kind = full.kind}, .y = y};
    plot.area = area;
    plot.view_x = m.x;
    plot.view_y = y;

    // The surface, the axes, the series.
    ctx.fill_rounded_rect(b, plot.style.radius, look.background);
    AxisMarks marks = axis_marks(plot.x, m.x, plot.y, y, look);
    draw_axes(ctx, marks, m, b, look, plot.x.grid, plot.y.grid);
    if (y.kind == ScaleKind::Linear && y.min < 0.0 && y.max > 0.0) {
        const f32 zero = std::round(m.py(0.0)) + 0.5f;
        ctx.line({area.x, zero}, {area.x + area.w, zero}, look.baseline);
    }

    // The marks: a line across at each value, its label at its end.
    const Color mark_color = with_alpha(look.text.color, 170);
    for (const PlotMark& mark : plot.y.marks) {
        if (mark.value < y.min || mark.value > y.max) {
            continue;
        }
        const f32 at = std::round(m.py(mark.value)) + 0.5f;
        ctx.line({area.x, at}, {area.x + area.w, at}, mark_color);
        const Vec2f size = text_size(look.text, mark.label);
        const f32 above = at - size.y - 1.0f;
        ctx.text(mark.label, {area.x + area.w - size.x - 4.0f, above >= area.y ? above : at + 2.0f}, look.text);
    }
    for (const PlotMark& mark : plot.x.marks) {
        if (mark.value < m.x.min || mark.value > m.x.max) {
            continue;
        }
        const f32 at = std::round(m.px(mark.value)) + 0.5f;
        ctx.line({at, area.y}, {at, area.y + area.h}, mark_color);
        ctx.text(mark.label, {at + 4.0f, area.y + 2.0f}, look.text);
    }

    const i32 bar_series = static_cast<i32>(std::ranges::count_if(plot.series, [](const Series& s) { return s.kind == SeriesKind::Bars; }));
    const auto color_of = [&](i32 si) {
        const Series& s = plot.series[static_cast<std::size_t>(si)];
        return s.color.a > 0 ? s.color : chart_color(si, look.dark);
    };
    // Bars: each x's slot shared by the bar series, a gap between bars.
    const auto bar_rect = [&](const Series& s, i32 i, i32 slot_index) {
        const i32 n = length(s);
        f64 step = s.x.empty() ? s.dx : (n > 1 ? (x_at(s, n - 1) - x_at(s, 0)) / static_cast<f64>(n - 1) : 1.0);
        const f32 slot = std::abs(m.px(x_at(s, i) + step * 0.5) - m.px(x_at(s, i) - step * 0.5));
        const f32 room = std::max(1.0f, slot - bar_gap);
        const f32 n_bars = static_cast<f32>(std::max(1, bar_series));
        const f32 w = std::max(1.0f, std::min(plot.style.bar_width, (room - bar_gap * (n_bars - 1.0f)) / n_bars));
        const f32 group = w * n_bars + bar_gap * (n_bars - 1.0f);
        const f32 x = m.px(x_at(s, i)) - group * 0.5f + static_cast<f32>(slot_index) * (w + bar_gap);
        const f32 top = m.py(y_at(s, i));
        return Rectf{x, std::min(top, m.base()), w, std::abs(top - m.base())};
    };

    // Over bars, the bar under the pointer; else the nearest sample along x.
    const bool reading = plot.readout && it.hot && in_area && !(it.active && view.moved);
    std::optional<PlotPoint> bar_hovered;
    if (reading) {
        plot.pointer_x = m.x.value(static_cast<f64>((p.x - area.x) / area.w));
        i32 slot_index = 0;
        for (i32 si = 0; si < static_cast<i32>(plot.series.size()); ++si) {
            const Series& s = plot.series[static_cast<std::size_t>(si)];
            if (s.kind != SeriesKind::Bars) {
                continue;
            }
            const i32 i = nearest(s, *plot.pointer_x);
            if (i >= 0 && contains(outset(bar_rect(s, i, slot_index), 2.0f), p)) {
                bar_hovered = PlotPoint{si, i};
            }
            ++slot_index;
        }
    }

    ctx.push_clip(area);
    i32 slot_index = 0;
    for (i32 si = 0; si < static_cast<i32>(plot.series.size()); ++si) {
        const Series& s = plot.series[static_cast<std::size_t>(si)];
        const Color color = color_of(si);
        const auto [i0, i1] = visible(s, m.x.min, m.x.max);
        switch (s.kind) {
        case SeriesKind::Line:
        case SeriesKind::Area:
            line_points(view.points, s, i0, i1, m);
            if (s.kind == SeriesKind::Area) {
                wash_mesh(view.wash, view.points, m.base(), with_alpha(color, wash_alpha));
                ctx.shape(view.wash);
            }
            ctx.polyline(view.points, color, {.width = plot.style.line_width, .join = LineJoin::Round, .cap = LineCap::Round});
            break;
        case SeriesKind::Bars:
            for (i32 i = i0; i1 >= 0 && i <= i1; ++i) {
                if (!std::isfinite(y_at(s, i))) {
                    continue;
                }
                const Rectf r = bar_rect(s, i, slot_index);
                const bool lit = bar_hovered && *bar_hovered == PlotPoint{si, i};
                const f32 top = y_at(s, i) >= 0.0f || y.kind == ScaleKind::Log ? r.y : r.y + r.h;
                const f32 base = top == r.y ? r.y + r.h : r.y;
                draw_bar(ctx, r.x, r.w, base, top, lit ? mix(color, look.dark ? colors::white : colors::black, 0.2f) : color);
            }
            ++slot_index;
            break;
        case SeriesKind::Points:
            for (i32 i = i0; i1 >= 0 && i <= i1; ++i) {
                if (std::isfinite(y_at(s, i))) {
                    const Vec2f at{m.px(x_at(s, i)), m.py(y_at(s, i))};
                    ctx.fill_circle(at, point_radius + ring, look.background);
                    ctx.fill_circle(at, point_radius, color);
                }
            }
            break;
        }
    }

    // The readout: a bar's value, or every series' at the nearest x.
    if (reading) {
        std::vector<ReadoutRow> rows;
        const auto value_row = [&](i32 si, i32 i) {
            const Series& s = plot.series[static_cast<std::size_t>(si)];
            return ReadoutRow{.value = readout_label(y_at(s, i), y, plot.y.unit), .name = std::string{s.name}, .kind = s.kind, .key = color_of(si)};
        };
        if (bar_hovered) {
            const Series& s = plot.series[static_cast<std::size_t>(bar_hovered->series)];
            plot.hovered = bar_hovered;
            rows.push_back({.value = readout_label(x_at(s, bar_hovered->index), m.x, plot.x.unit)});
            rows.push_back(value_row(bar_hovered->series, bar_hovered->index));
        } else {
            // The sample nearest the pointer along x, of a line, area or points
            // (bars answer only under it); then each series' nearest to that.
            f64 best = std::numeric_limits<f64>::infinity();
            for (i32 si = 0; si < static_cast<i32>(plot.series.size()); ++si) {
                const Series& s = plot.series[static_cast<std::size_t>(si)];
                if (s.kind == SeriesKind::Bars) {
                    continue;
                }
                const i32 i = nearest(s, *plot.pointer_x);
                if (i >= 0 && std::isfinite(y_at(s, i)) && std::abs(m.px(x_at(s, i)) - p.x) < best) {
                    best = std::abs(m.px(x_at(s, i)) - p.x);
                    plot.hovered = PlotPoint{si, i};
                }
            }
            if (plot.hovered) {
                const Series& first = plot.series[static_cast<std::size_t>(plot.hovered->series)];
                const f64 at = x_at(first, plot.hovered->index);
                const f32 x = std::round(m.px(at)) + 0.5f;
                ctx.line({x, area.y}, {x, area.y + area.h}, look.baseline);
                rows.push_back({.value = readout_label(at, m.x, plot.x.unit)});
                for (i32 si = 0; si < static_cast<i32>(plot.series.size()); ++si) {
                    const Series& s = plot.series[static_cast<std::size_t>(si)];
                    const i32 i = nearest(s, at);
                    if (i < 0 || !std::isfinite(y_at(s, i))) {
                        continue;
                    }
                    if (s.kind != SeriesKind::Bars) {
                        const Vec2f dot{m.px(x_at(s, i)), m.py(y_at(s, i))};
                        ctx.fill_circle(dot, point_radius + ring, look.background);
                        ctx.fill_circle(dot, point_radius, color_of(si));
                    }
                    rows.push_back(value_row(si, i));
                }
            }
        }
        if (!rows.empty()) {
            show_readout(ctx, p, std::move(rows), look);
        }
    }
    ctx.pop_clip();

    if (legend) {
        f32 x = area.x;
        const f32 top = b.y + 8.0f;
        ctx.push_clip(b);
        for (i32 si = 0; si < static_cast<i32>(plot.series.size()); ++si) {
            const Series& s = plot.series[static_cast<std::size_t>(si)];
            if (s.name.empty()) {
                continue;
            }
            draw_key(ctx, {x, top}, look.line_height, s.kind, color_of(si));
            ctx.text(s.name, {x + 20.0f, top}, look.text);
            x += 20.0f + text_size(look.text, s.name).x + 16.0f;
        }
        ctx.pop_clip();
    }
}

void run(Context& ctx, Sparkline& line) {
    const Rectf b = line.bounds;
    if (b.w <= 0.0f || b.h <= 0.0f || line.values.size() < 2) {
        return;
    }
    Scale y = line.scale;
    if (!y.valid()) {
        f32 lo = std::numeric_limits<f32>::infinity(), hi = -lo;
        for (const f32 v : line.values) {
            if (std::isfinite(v)) {
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
        }
        if (!std::isfinite(lo)) {
            return;
        }
        y = hi > lo ? Scale{.min = lo, .max = hi} : Scale{.min = lo - 1.0, .max = lo + 1.0};
    }
    const f32 pad = line.width * 0.5f + 1.0f; // the line, its round ends and soft edge inside the bounds
    const Mapping m{.area = {b.x + pad, b.y + pad, std::max(1.0f, b.w - pad * 2.0f), std::max(1.0f, b.h - pad * 2.0f)},
                    .x = {.min = 0.0, .max = static_cast<f64>(line.values.size() - 1)},
                    .y = y};
    const Series series{.y = line.values};
    std::vector<Vec2f> points;
    line_points(points, series, 0, static_cast<i32>(line.values.size()) - 1, m);
    const Color color = line.color.a > 0 ? line.color : ctx.theme().palette.accent;
    if (line.area) {
        ShapeMesh wash;
        wash_mesh(wash, points, b.y + b.h, with_alpha(color, wash_alpha));
        ctx.shape(wash);
    }
    ctx.polyline(points, color, {.width = line.width, .join = LineJoin::Round, .cap = LineCap::Round});
}

HeatmapImage::HeatmapImage(Vec2i size, ColorMap map, Scale values)
    : _size{std::max(0, size.x), std::max(0, size.y)},
      _map(map),
      _values(values.valid() ? values : Scale{}),
      _cells(static_cast<std::size_t>(_size.x) * static_cast<std::size_t>(_size.y), static_cast<f32>(_values.min)),
      _changed(static_cast<std::size_t>(_size.x), 1) {}

f32 HeatmapImage::at(i32 column, i32 row) const {
    if (column < 0 || row < 0 || column >= _size.x || row >= _size.y) {
        return 0.0f;
    }
    return _cells[static_cast<std::size_t>(stored(column)) * static_cast<std::size_t>(_size.y) + static_cast<std::size_t>(row)];
}

void HeatmapImage::set(i32 column, i32 row, f32 value) {
    if (column < 0 || row < 0 || column >= _size.x || row >= _size.y) {
        return;
    }
    const i32 s = stored(column);
    _cells[static_cast<std::size_t>(s) * static_cast<std::size_t>(_size.y) + static_cast<std::size_t>(row)] = value;
    touch(s);
}

void HeatmapImage::set_column(i32 column, std::span<const f32> values) {
    if (column < 0 || column >= _size.x) {
        return;
    }
    const i32 s = stored(column);
    std::copy_n(values.begin(), std::min(values.size(), static_cast<std::size_t>(_size.y)),
                _cells.begin() + static_cast<std::ptrdiff_t>(s) * _size.y);
    touch(s);
}

void HeatmapImage::push_column(std::span<const f32> values) {
    if (_size.x <= 0 || _size.y <= 0) {
        return;
    }
    const i32 s = _start; // the oldest goes
    const auto column = _cells.begin() + static_cast<std::ptrdiff_t>(s) * _size.y;
    const std::size_t given = std::min(values.size(), static_cast<std::size_t>(_size.y));
    std::copy_n(values.begin(), given, column);
    std::fill(column + static_cast<std::ptrdiff_t>(given), column + _size.y, static_cast<f32>(_values.min));
    _start = (_start + 1) % _size.x;
    touch(s);
}

void HeatmapImage::fill(f32 value) {
    std::ranges::fill(_cells, value);
    _all_changed = true;
}

void HeatmapImage::set_map(ColorMap map) {
    _all_changed = _all_changed || map != _map;
    _map = map;
}

void HeatmapImage::set_values(Scale values) {
    if (values.valid() && !(values == _values)) {
        _values = values;
        _all_changed = true;
    }
}

void HeatmapImage::touch(i32 stored_column) {
    _changed[static_cast<std::size_t>(stored_column)] = 1;
}

void HeatmapImage::draw(Context& ctx, Rectf dest, bool smooth) {
    Renderer2D* renderer = ctx.renderer();
    if (!renderer || _size.x <= 0 || _size.y <= 0 || dest.w <= 0.0f || dest.h <= 0.0f) {
        return;
    }
    const std::size_t rows = static_cast<std::size_t>(_size.y);
    // Columns [first, last) of the store into _scratch, the texture's rows top down.
    const auto pack = [&](i32 first, i32 last) {
        const std::size_t w = static_cast<std::size_t>(last - first);
        _scratch.resize(w * rows * 4);
        for (std::size_t c = 0; c < w; ++c) {
            const f32* cells = _cells.data() + (static_cast<std::size_t>(first) + c) * rows;
            for (std::size_t r = 0; r < rows; ++r) {
                const Color color = map_color(_map, static_cast<f32>(_values.fraction(cells[r])));
                u8* texel = _scratch.data() + ((rows - 1 - r) * w + c) * 4;
                texel[0] = color.r;
                texel[1] = color.g;
                texel[2] = color.b;
                texel[3] = 255;
            }
        }
    };
    const auto make = [&] {
        pack(0, _size.x);
        _texture = renderer->create_texture_from_rgba(_scratch.data(), _size);
        renderer->set_scale_mode(_texture, smooth ? ScaleMode::Linear : ScaleMode::Nearest);
        _smooth = smooth;
    };
    if (!_texture.valid()) {
        make();
    } else if (_all_changed) {
        pack(0, _size.x);
        if (!renderer->update_texture(_texture, {0, 0}, _size, _scratch.data())) {
            make();
        }
    } else {
        // Runs of changed columns, a region each.
        for (i32 c = 0; c < _size.x;) {
            if (!_changed[static_cast<std::size_t>(c)]) {
                ++c;
                continue;
            }
            i32 end = c + 1;
            while (end < _size.x && _changed[static_cast<std::size_t>(end)]) {
                ++end;
            }
            pack(c, end);
            if (!renderer->update_texture(_texture, {c, 0}, {end - c, _size.y}, _scratch.data())) {
                make();
                break;
            }
            c = end;
        }
    }
    std::ranges::fill(_changed, 0);
    _all_changed = false;
    if (smooth != _smooth) {
        renderer->set_scale_mode(_texture, smooth ? ScaleMode::Linear : ScaleMode::Nearest);
        _smooth = smooth;
    }

    // Shown from the oldest column: stored [start, end), then [0, start).
    const f32 w = static_cast<f32>(_size.x);
    const f32 h = static_cast<f32>(_size.y);
    const f32 older = w - static_cast<f32>(_start);
    const f32 split = dest.x + dest.w * older / w;
    ctx.sprite({.texture = _texture, .source = {static_cast<f32>(_start), 0.0f, older, h}}, {dest.x, dest.y, split - dest.x, dest.h});
    if (_start > 0) {
        ctx.sprite({.texture = _texture, .source = {0.0f, 0.0f, static_cast<f32>(_start), h}}, {split, dest.y, dest.x + dest.w - split, dest.h});
    }
}

void run(Context& ctx, Heatmap& map) {
    const Rectf b = map.bounds;
    map.interaction = ctx.region(map.id, b, map.z);
    map.hovered.reset();
    if (!map.image || map.image->size().x <= 0 || map.image->size().y <= 0 || b.w <= 0.0f || b.h <= 0.0f) {
        return;
    }
    HeatmapImage& image = *map.image;
    const Vec2i cells = image.size();
    const ChartLook look = chart_look(ctx, map.style);
    const Scale xs = map.x.scale.valid() ? map.x.scale : Scale{.min = 0.0, .max = static_cast<f64>(cells.x)};
    const Scale ys = map.y.scale.valid() ? map.y.scale : Scale{.min = 0.0, .max = static_cast<f64>(cells.y)};
    const AxisMarks marks = axis_marks(map.x, xs, map.y, ys, look);

    // The colour bar's room at the right: the bar and its values.
    const std::vector<Tick> value_marks = map.legend ? ticks(image.values(), 6, map.unit) : std::vector<Tick>{};
    f32 value_width = 0.0f;
    for (const Tick& tick : value_marks) {
        value_width = std::max(value_width, text_size(look.text, tick.label).x);
    }
    const f32 bar_w = 10.0f;
    const f32 right = map.legend ? 12.0f + bar_w + 6.0f + value_width + 8.0f : 10.0f;
    const Rectf area = plot_area(b, marks, look, false, right);
    const Mapping m{.area = area, .x = xs, .y = ys};
    map.area = area;

    ctx.fill_rounded_rect(b, map.style.radius, look.background);
    image.draw(ctx, area, map.smooth);
    draw_axes(ctx, marks, m, b, look, false, false);

    if (map.legend) {
        const Rectf bar{area.x + area.w + 12.0f, area.y, bar_w, area.h};
        constexpr i32 slices = 64;
        for (i32 k = 0; k < slices; ++k) {
            const f32 t0 = static_cast<f32>(k) / slices;
            const f32 t1 = static_cast<f32>(k + 1) / slices;
            ctx.fill_rect({bar.x, bar.y + bar.h * (1.0f - t1), bar.w, bar.h * (t1 - t0) + 0.5f}, map_color(image.map(), (t0 + t1) * 0.5f));
        }
        const Mapping value_m{.area = bar, .y = image.values()};
        for (const Tick& tick : value_marks) {
            const f32 y = value_m.py(tick.value);
            const Vec2f size = text_size(look.text, tick.label);
            ctx.text(tick.label, {bar.x + bar.w + 6.0f, std::max(b.y, std::min(y - size.y * 0.5f, b.y + b.h - size.y))}, look.text);
        }
    }

    const Vec2f p = ctx.pointer();
    if (map.readout && map.interaction.hot && contains(area, p)) {
        const i32 column = std::clamp(static_cast<i32>((p.x - area.x) / area.w * static_cast<f32>(cells.x)), 0, cells.x - 1);
        const i32 row = std::clamp(static_cast<i32>((area.y + area.h - p.y) / area.h * static_cast<f32>(cells.y)), 0, cells.y - 1);
        map.hovered = Vec2i{column, row};
        const Rectf cell{area.x + area.w * static_cast<f32>(column) / static_cast<f32>(cells.x),
                         area.y + area.h * (1.0f - static_cast<f32>(row + 1) / static_cast<f32>(cells.y)),
                         area.w / static_cast<f32>(cells.x), area.h / static_cast<f32>(cells.y)};
        if (cell.w >= 4.0f && cell.h >= 4.0f) {
            ctx.outline_rect(cell, look.dark ? colors::white : colors::black);
        }
        // The cell's middle along each scale.
        const f64 x = xs.value((static_cast<f64>(column) + 0.5) / static_cast<f64>(cells.x));
        const f64 y = ys.value((static_cast<f64>(row) + 0.5) / static_cast<f64>(cells.y));
        std::vector<ReadoutRow> rows;
        rows.push_back({.value = readout_label(image.at(column, row), image.values(), map.unit),
                        .kind = SeriesKind::Bars,
                        .key = map_color(image.map(), static_cast<f32>(image.values().fraction(image.at(column, row))))});
        rows.push_back({.value = readout_label(x, xs, map.x.unit)});
        rows.push_back({.value = readout_label(y, ys, map.y.unit)});
        show_readout(ctx, p, std::move(rows), look);
    }
}

} // namespace kin::ui2
