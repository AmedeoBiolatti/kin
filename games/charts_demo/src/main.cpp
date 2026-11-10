// Charts demo: kin/ui2/charts.hpp's plots, heat map and sparklines, and a
// dial drawn round Context::drag_value.
//
//   Waveform     the wheel zooms, a drag pans, a double click shows it all
//   Frame times  three series and a legend; the readout lists them at the pointer
//   Histogram    how often each render time came up, as bars
//   Spectrogram  a column a frame, scrolling; the pointer reads a cell
//   Gain         drag it up or down (Shift: finely), turn the wheel, double
//                click to reset; Tab, then the arrows
//   Esc          quits

#include <kin/core/json.hpp>
#include <kin/platform/input.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/runtime/game_info.hpp>
#include <kin/runtime/scene_app.hpp>
#include <kin/scene/scene.hpp>
#include <kin/scene/scene_manager.hpp>
#include <kin/ui2/charts.hpp>
#include <kin/ui2/context.hpp>
#include <kin/ui2/text.hpp>
#include <kin/ui2/theme.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace demo {
namespace {

namespace ui2 = kin::ui2;
using kin::f32;
using kin::f64;
using kin::i32;

constexpr kin::Vec2f logical_size{1200.0f, 780.0f};
constexpr f32 pi = 3.14159265f;
constexpr i32 history = 240;  // frames the frame-time chart shows
constexpr i32 sample_rate = 48000;

kin::GameInfo make_game_info() {
    return {
        .id = "charts_demo",
        .title = "Kin Charts Demo",
        .version = "0.1",
        .description = "Plots, a scrolling spectrogram, sparklines and a dial built with kin::ui2's charts.",
        .author = "Kin contributors",
        .window = {
            .width = static_cast<i32>(logical_size.x),
            .height = static_cast<i32>(logical_size.y),
            .logical_width = static_cast<i32>(logical_size.x),
            .logical_height = static_cast<i32>(logical_size.y),
            .resizable = true,
        },
        .tags = {"sample", "ui"},
        .fields = {},
        .input_map = {},
    };
}

// A dial for a value, drawn round Context::drag_value: an arc from its
// lowest to its highest, lit up to where it is.
struct Dial {
    ui2::Id id{};
    kin::Rectf bounds{};
    std::string_view label;
    f64* value = nullptr;
    ui2::DragValueOptions options{};
    ui2::DragValueResult result{};
};

kin::Vec2f measure(const Dial&) {
    return {84.0f, 96.0f};
}

void run(ui2::Context& ctx, Dial& dial) {
    const ui2::Theme& theme = ctx.theme();
    constexpr f32 radius = 26.0f;
    const kin::Vec2f center{dial.bounds.x + dial.bounds.w * 0.5f, dial.bounds.y + 4.0f + radius};
    dial.result = ctx.drag_value(dial.id, {center.x - radius, center.y - radius, radius * 2.0f, radius * 2.0f}, *dial.value, dial.options);
    const f32 along = static_cast<f32>(std::clamp(dial.options.scale.fraction(*dial.value), 0.0, 1.0));
    constexpr f32 start = 135.0f; // the arc runs clockwise from lower left round to lower right
    constexpr f32 sweep = 270.0f;
    const kin::StrokeStyle stroke{.width = 4.0f, .cap = kin::LineCap::Round};
    const bool lit = dial.result.hot || dial.result.active;
    ctx.fill_circle(center, radius - 7.0f, lit ? theme.palette.surface_hover : theme.palette.surface_alt);
    ctx.arc(center, radius, start, start + sweep, theme.palette.surface_alt, stroke);
    if (along > 0.0f) {
        ctx.arc(center, radius, start, start + sweep * along, theme.palette.accent, stroke);
    }
    if (dial.result.focused && !dial.result.quiet_focus) {
        ctx.outline_circle(center, radius + 5.0f, theme.palette.accent, 1.5f);
    }
    const f32 angle = (start + sweep * along) * pi / 180.0f;
    const kin::Vec2f dir{std::cos(angle), std::sin(angle)};
    ctx.line({center.x + dir.x * 5.0f, center.y + dir.y * 5.0f}, {center.x + dir.x * (radius - 11.0f), center.y + dir.y * (radius - 11.0f)},
             theme.palette.text, 2.5f, kin::LineCap::Round);

    char value[32];
    std::snprintf(value, sizeof value, "%+.1f dB", *dial.value);
    const auto centered = [&](std::string_view text, f32 y, const ui2::TextStyle& style) {
        const kin::Vec2f size = ui2::measure_text(style.font, text, style.scale);
        ctx.text(text, {center.x - size.x * 0.5f, y}, style);
    };
    centered(dial.label, center.y + radius + 6.0f, theme.small_text);
    centered(value, center.y + radius + 22.0f, theme.body_text);
}

// A number, its name, and its recent values as a sparkline.
struct Gauge {
    std::string name;
    std::vector<f32> values;
    ui2::Sparkline line{.size = {96.0f, 22.0f}};
    std::string text;
};

class ChartsDemoScene final : public kin::Scene {
public:
    ChartsDemoScene() {
        _wave.resize(sample_rate * 2);
        make_wave();
        for (auto& times : _times) {
            times.assign(history, 0.0f);
        }
        _histogram.assign(16, 0.0f);
        _gauges[0].name = "Frame";
        _gauges[1].name = "Peak";
        _gauges[1].line.color = ui2::chart_color(1);
        _column.resize(static_cast<std::size_t>(_spectrum.size().y));
        // The series show the vectors above, now they are their size.
        _wave_series[0].y = _wave;
        for (std::size_t s = 0; s < _times.size(); ++s) {
            _times_series[s].y = _times[s];
        }
        _histogram_series[0].y = _histogram;
        for (i32 i = 0; i < history; ++i) { // a full history from the first frame
            step();
        }
    }

    void update(kin::SceneContext& ctx) override {
        if (ctx.input.pressed(kin::Key::Escape) && !_ui.typing()) {
            ctx.app.quit();
        }
    }

    void render(kin::SceneContext& ctx) override {
        kin::Renderer2D& r = ctx.renderer;
        if (_oversample != std::max(1.0f, r.content_scale())) {
            _oversample = std::max(1.0f, r.content_scale()); // text as sharp as the display
            _ui.set_theme(make_theme(_oversample));
        }
        r.clear(_ui.theme().palette.background);
        step();

        _ui.begin(ctx.input, r, ctx.dt);
        _ui.begin_layout({0.0f, 0.0f, logical_size.x, logical_size.y});
        {
            auto page = _ui.box(ui2::make_id("page"), ui2::vbox().fill().pad(16).gap(8).passive());
            _ui.label("Waveform: the wheel zooms, a drag pans, a double click shows it all", _ui.theme().small_text);
            _ui.widget(_wave_plot, ui2::LayoutStyle{}.w(ui2::grow()).h(200));
            {
                auto row = _ui.box(ui2::make_id("middle"), ui2::hbox().w(ui2::grow()).h(230).gap(12).passive());
                {
                    auto column = _ui.box(ui2::make_id("times"), ui2::vbox().w(ui2::grow()).h(ui2::grow()).gap(6).passive());
                    _ui.label("Frame times", _ui.theme().small_text);
                    _ui.widget(_times_plot, ui2::LayoutStyle{}.fill());
                }
                {
                    auto column = _ui.box(ui2::make_id("hist"), ui2::vbox().w(380).h(ui2::grow()).gap(6).passive());
                    _ui.label("Render times, last 240 frames", _ui.theme().small_text);
                    _ui.widget(_histogram_plot, ui2::LayoutStyle{}.fill());
                }
            }
            {
                auto row = _ui.box(ui2::make_id("bottom"), ui2::hbox().w(ui2::grow()).h(ui2::grow()).gap(12).passive());
                {
                    auto column = _ui.box(ui2::make_id("spectrum"), ui2::vbox().w(ui2::grow()).h(ui2::grow()).gap(6).passive());
                    _ui.label("Spectrogram: a column a frame", _ui.theme().small_text);
                    _ui.widget(_heatmap, ui2::LayoutStyle{}.fill());
                }
                {
                    auto column = _ui.box(ui2::make_id("side"), ui2::vbox().w(200).h(ui2::grow()).gap(10).passive());
                    _ui.widget(_dial);
                    for (Gauge& gauge : _gauges) {
                        auto line = _ui.box(ui2::make_id(ui2::make_id("gauge"), gauge.name), ui2::hbox().w(ui2::grow()).gap(8).items(ui2::UiAlign::Center).passive());
                        _ui.label(gauge.name, _ui.theme().small_text, ui2::LayoutStyle{}.w(40));
                        _ui.widget(gauge.line);
                        _ui.label(gauge.text, _ui.theme().body_text);
                    }
                }
            }
        }
        _ui.end_layout();
        _ui.end();
        if (_dial.result.changed) {
            make_wave();
        }
    }

    void write_report(kin::JsonWriter& json) const override {
        json.field("frames", static_cast<kin::i64>(_frame));
        json.field("spectrogram_columns", static_cast<kin::i64>(_spectrum.size().x));
        json.field("wave_view_max", _wave_plot.view_x.max);
        json.field("times_view_y_max", _times_plot.view_y.max);
        json.field("histogram_bars", static_cast<kin::i64>(_histogram.size()));
        json.field("gain_db", _gain);
    }

private:
    static ui2::Theme make_theme(f32 oversample) {
        ui2::Theme theme = ui2::editor_theme();
        theme.small_text = {.font = ui2::system_ui_font(12, ui2::TextRendering::Bitmap, oversample), .scale = 1.0f,
                            .color = theme.palette.text_muted};
        theme.body_text = {.font = ui2::system_ui_font(13, ui2::TextRendering::Bitmap, oversample), .scale = 1.0f,
                           .color = theme.palette.text};
        return theme;
    }

    // Two seconds of a plucked note and a bell, at the dial's gain.
    void make_wave() {
        const f32 gain = std::pow(10.0f, static_cast<f32>(_gain) / 20.0f);
        for (std::size_t i = 0; i < _wave.size(); ++i) {
            const f32 t = static_cast<f32>(i) / sample_rate;
            const f32 pluck = std::sin(2.0f * pi * 220.0f * t) * std::exp(-t * 2.5f) * (1.0f - std::exp(-t * 400.0f));
            const f32 bell_t = std::max(0.0f, t - 0.8f);
            const f32 bell = t > 0.8f ? std::sin(2.0f * pi * 1320.0f * bell_t) * std::exp(-bell_t * 5.0f) : 0.0f;
            _wave[i] = std::clamp(gain * (0.55f * pluck + 0.3f * bell), -1.0f, 1.0f);
        }
    }

    f32 noise() {
        _seed = _seed * 1664525u + 1013904223u;
        return static_cast<f32>(_seed >> 8) / static_cast<f32>(1u << 24);
    }

    // A frame's worth of made-up measurements.
    void step() {
        ++_frame;
        const f32 t = static_cast<f32>(_frame) / 60.0f;
        // Frame times: three series scrolled one to the left.
        const std::array<f32, 3> now{2.5f + noise() * 1.2f,
                                     7.0f + 2.0f * std::sin(t * 0.7f) + noise() * 1.5f + (noise() > 0.97f ? 6.0f : 0.0f),
                                     1.2f + noise() * 0.4f};
        for (std::size_t s = 0; s < _times.size(); ++s) {
            std::rotate(_times[s].begin(), _times[s].begin() + 1, _times[s].end());
            _times[s].back() = now[s];
        }
        std::ranges::fill(_histogram, 0.0f);
        for (const f32 v : _times[1]) {
            _histogram[static_cast<std::size_t>(std::clamp(static_cast<i32>(v), 0, static_cast<i32>(_histogram.size()) - 1))] += 1.0f;
        }
        for (std::size_t s = 0; s < _times.size(); ++s) {
            _times_series[s].x0 = static_cast<f64>(_frame - history + 1);
        }

        // The spectrogram: a sweeping note, its harmonics, a hum, a noise floor.
        const f32 rows = static_cast<f32>(_spectrum.size().y);
        const f32 note = 1000.0f * std::pow(2.0f, 1.5f + 1.5f * std::sin(t * 0.5f)); // Hz
        f32 peak = -90.0f;
        for (i32 row = 0; row < _spectrum.size().y; ++row) {
            const f32 hz = (static_cast<f32>(row) + 0.5f) / rows * 22050.0f;
            f32 level = -82.0f + noise() * 10.0f;
            for (i32 h = 1; h <= 4; ++h) {
                const f32 off = (hz - note * static_cast<f32>(h)) / 180.0f;
                level = std::max(level, -6.0f * static_cast<f32>(h) - off * off * 3.0f);
            }
            const f32 hum = (hz - 120.0f) / 160.0f;
            level = std::max(level, -30.0f - hum * hum * 4.0f);
            level += static_cast<f32>(_gain);
            _column[static_cast<std::size_t>(row)] = level;
            peak = std::max(peak, level);
        }
        _spectrum.push_column(_column);

        const auto keep = [](Gauge& gauge, f32 value, const char* format) {
            if (gauge.values.size() >= 120) {
                gauge.values.erase(gauge.values.begin());
            }
            gauge.values.push_back(value);
            gauge.line.values = gauge.values;
            char text[32];
            std::snprintf(text, sizeof text, format, value);
            gauge.text = text;
        };
        keep(_gauges[0], now[0] + now[1] + now[2], "%.1f ms");
        keep(_gauges[1], peak, "%.0f dB");
    }

    ui2::Context _ui;
    f32 _oversample = 0.0f;
    i32 _frame = 0;
    kin::u32 _seed = 12345u;
    f64 _gain = 0.0;

    std::vector<f32> _wave;
    std::array<ui2::Series, 1> _wave_series{{{.name = "signal", .dx = 1.0 / sample_rate}}};
    ui2::Plot _wave_plot{.id = ui2::make_id("wave"),
                         .series = _wave_series,
                         .x = {.unit = "s"},
                         .y = {.scale = {.min = -1.0, .max = 1.0}, .ticks = 5}};

    std::array<std::vector<f32>, 3> _times;
    std::array<ui2::Series, 3> _times_series{{{.name = "update"}, {.name = "render"}, {.name = "present"}}};
    ui2::Plot _times_plot{.id = ui2::make_id("times"), .series = _times_series, .x = {.ticks = 5}, .y = {.unit = "ms", .ticks = 4}, .zoom = false};

    std::vector<f32> _histogram;
    std::array<ui2::Series, 1> _histogram_series{{{.name = "frames", .x0 = 0.5, .kind = ui2::SeriesKind::Bars}}};
    ui2::Plot _histogram_plot{.id = ui2::make_id("histogram"), .series = _histogram_series, .x = {.unit = "ms", .grid = false},
                              .y = {.ticks = 4}, .zoom = false};

    ui2::HeatmapImage _spectrum{{360, 128}, ui2::ColorMap::Magma, {.min = -90.0, .max = 0.0}};
    std::vector<f32> _column;
    ui2::Heatmap _heatmap{.id = ui2::make_id("spectrogram"),
                          .image = &_spectrum,
                          .x = {.scale = {.min = -6.0, .max = 0.0}, .unit = "s"},
                          .y = {.scale = {.min = 0.0, .max = 22050.0}, .unit = "Hz", .ticks = 5},
                          .unit = "dB"};

    Dial _dial{.id = ui2::make_id("gain"),
               .label = "Gain",
               .value = &_gain,
               .options = {.scale = {.min = -24.0, .max = 12.0}, .step = 0.1, .reset = 0.0}};
    std::array<Gauge, 2> _gauges;
};

} // namespace
} // namespace demo

int main(int argc, char** argv) {
    kin::GameInfo game = demo::make_game_info();
    kin::SceneManager scenes;

    const auto build_scenes = [](kin::SceneManager& target) {
        target.push(std::make_unique<demo::ChartsDemoScene>());
    };
    build_scenes(scenes);

    return kin::run_scene_app({
        .window = kin::window_config(game),
        .headless = kin::parse_headless_options(argc, argv),
        .game = &game,
        .render_headless = true, // headless runs draw the charts, so the report shows them laid out
        .reset_scenes = build_scenes,
    }, scenes);
}
