#pragma once

// Charts: numbers plotted as lines, areas, bars or points on axes that zoom
// and pan; sparklines; and heat maps, scrolling ones too (a spectrogram).
// Widgets as in widgets.hpp: ui.widget(plot, layout), or run(ctx, plot) with
// its bounds set. The numbers stay the app's: a widget holds spans of them
// (it is copied when declared) and draws them as they are that frame.
//
// They look as a data-visualisation method has it: series take fixed colours
// in a fixed order (chart_color), lines are 2 wide with round joins, an area
// is a faint wash of its line's colour, bars are at most 24 wide with a
// rounded end and a gap between them, grid lines are hairlines; text keeps
// the theme's text colours, never a series'; a legend names two or more
// series; the readout lists every series at the pointer.

#include <kin/renderer/texture.hpp>
#include <kin/ui2/scale.hpp>
#include <kin/ui2/widgets.hpp>

#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace kin::ui2 {

// The colour of the series at `index` (0 the first), in an order checked for
// colour-blind readers, stepped for a dark surface or a light one. There are
// eight; past them a neutral grey (fold more series into one "other").
Color chart_color(i32 index, bool dark = true);

enum class SeriesKind : u8 {
    Line,
    Area,   // a line, washed below down to 0 (or the plot's bottom)
    Bars,   // from 0; bar series side by side at each x
    Points,
};

// Numbers to plot: y at x0 + i * dx, or at `x` when given (as many, rising).
struct Series {
    std::string_view name{}; // in the legend and the readout
    std::span<const f32> y{};
    std::span<const f32> x{};
    f64 x0 = 0.0;
    f64 dx = 1.0;
    SeriesKind kind = SeriesKind::Line;
    Color color = colors::transparent; // transparent: chart_color(its index)
};

// A value marked across a plot: a budget, a target, a limit.
struct PlotMark {
    f64 value = 0.0;
    std::string_view label{}; // at the line's end
};

struct PlotAxis {
    // The range shown. Unset (max not above min): fitted to the data, y's
    // rounded out to round values. A Log scale plots values above 0.
    Scale scale{.min = 0.0, .max = 0.0};
    std::string_view unit{}; // after its values ("ms", "Hz")
    i32 ticks = 6;           // about how many values marked (0: none, and no grid lines)
    bool grid = true;        // hairlines across at them
    std::span<const PlotMark> marks{}; // lines across the plot at these values (a Plot's), labelled
};

// Colours left transparent are the theme's.
struct PlotStyle {
    Color background = colors::transparent;
    Color grid = colors::transparent;
    Color text = colors::transparent; // the axes' values and the legend's names
    TextStyle text_style{};           // their size and font (the theme's small text unless set)
    f32 radius = 4.0f;
    f32 line_width = 2.0f;
    f32 bar_width = 24.0f; // at most
};

struct PlotPoint {
    i32 series = 0;
    i32 index = 0;
    friend bool operator==(const PlotPoint&, const PlotPoint&) = default;
};

// Series on two axes, with a legend above them when two or more are named.
// With `zoom`, the wheel zooms x around the pointer, a drag pans it and a
// double click shows all of it again (the view is kept by id); zoomed in on
// the newest samples of data that grows, it moves on with them. With
// `readout`, the pointer shows each series' value at the nearest sample of a
// line, area or points along x; on a bar, the bar's.
struct Plot {
    Id id{};
    Rectf bounds{};
    std::span<const Series> series{};
    PlotAxis x{};
    PlotAxis y{};
    bool zoom = true;
    bool readout = true;
    bool legend = true;
    Vec2f size{320.0f, 180.0f}; // what it asks for, laid out at its own size
    PlotStyle style{};
    i32 z = 0;
    // Results
    Interaction interaction{};
    Rectf area{};   // where the data is drawn: an app's own marks go here (a playhead)
    Scale view_x{}; // the ranges shown there
    Scale view_y{};
    std::optional<f64> pointer_x{};   // where the pointer is along x, while it is over the data
    std::optional<PlotPoint> hovered{}; // what the readout shows: the nearest sample, or the bar under the pointer
};

// Recent values as a small line without axes: a frame time beside its number.
struct Sparkline {
    Rectf bounds{};
    std::span<const f32> values{};
    Scale scale{.min = 0.0, .max = 0.0}; // unset: from their lowest to their highest
    Color color = colors::transparent;   // the theme's accent
    bool area = true;                    // a faint wash under the line
    f32 width = 1.5f;
    Vec2f size{80.0f, 20.0f};
};

// Colours for values from 0 to 1, darkest to brightest, evenly brighter
// (not a rainbow): Magma (black, red, yellow; a spectrogram's), Viridis
// (blue, green, yellow) and Gray.
enum class ColorMap : u8 { Magma, Viridis, Gray };
Color map_color(ColorMap map, f32 t);

// A grid of values shown as colours: `size.x` columns across, `size.y` rows
// up (row 0 at the bottom). Set cells, columns or all of it, or push columns
// in at the right to scroll it, the oldest going at the left (a spectrogram:
// a column a frame). The app keeps it and a Heatmap shows it; its texture is
// made and updated as that draws, where it changed.
class HeatmapImage {
public:
    HeatmapImage() = default;
    // Values coloured by where they are on `values` (beyond it: its ends).
    explicit HeatmapImage(Vec2i size, ColorMap map = ColorMap::Magma, Scale values = {});

    Vec2i size() const { return _size; }
    ColorMap map() const { return _map; }
    const Scale& values() const { return _values; }
    // Columns count from the left as shown, rows from the bottom.
    f32 at(i32 column, i32 row) const;
    void set(i32 column, i32 row, f32 value);
    void set_column(i32 column, std::span<const f32> values); // the bottom row's first
    void push_column(std::span<const f32> values);            // in at the right
    void fill(f32 value);
    // Colours them again (what is there already, too).
    void set_map(ColorMap map);
    void set_values(Scale values);

    // Draws it into `dest`: its texture made, or updated where it changed,
    // first. A Heatmap does this; `smooth` blends the cells into each other.
    void draw(Context& ctx, Rectf dest, bool smooth = false);

private:
    i32 stored(i32 column) const { return (_start + column) % _size.x; }
    void touch(i32 stored_column);

    Vec2i _size{};
    ColorMap _map = ColorMap::Magma;
    Scale _values{};
    std::vector<f32> _cells; // stored column by column, each from the bottom row
    i32 _start = 0;          // the stored column shown at the left
    Texture _texture;
    std::vector<u8> _changed; // a flag for each stored column whose texels are not yet in the texture
    bool _all_changed = true;
    std::vector<u8> _scratch; // texels on their way
    bool _smooth = false;
};

// A HeatmapImage on axes: what its columns span along x and its rows along y
// (spread evenly along each scale: for a Log y, rows that are log-spaced),
// a bar of its colours at the right with their values, and the value under
// the pointer.
struct Heatmap {
    Id id{};
    Rectf bounds{};
    HeatmapImage* image = nullptr;
    PlotAxis x{}; // unset: 0 to the column count
    PlotAxis y{}; // unset: 0 to the row count
    std::string_view unit{}; // the values' (the colour bar's and the readout's)
    bool legend = true;      // the colour bar
    bool readout = true;
    bool smooth = false;
    Vec2f size{320.0f, 180.0f};
    PlotStyle style{};
    i32 z = 0;
    // Results
    Interaction interaction{};
    Rectf area{}; // where the cells are drawn
    std::optional<Vec2i> hovered{}; // the cell under the pointer: column as shown, row from the bottom
};

Vec2f measure(const Plot& widget);
Vec2f measure(const Sparkline& widget);
Vec2f measure(const Heatmap& widget);

void run(Context& ctx, Plot& widget);
void run(Context& ctx, Sparkline& widget);
void run(Context& ctx, Heatmap& widget);

} // namespace kin::ui2
