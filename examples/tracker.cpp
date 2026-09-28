#include "workloads.hpp"
#include <kin/core/profile.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>

namespace examples {
namespace {
std::string number(double value, int digits = 2) {
    char buffer[64]; std::snprintf(buffer, sizeof(buffer), "%.*f", digits, value); return buffer;
}
constexpr Color background = Color::rgb(14, 20, 29), panel = Color::rgb(21, 30, 42), border = Color::rgb(43, 57, 72);
constexpr Color text = Color::rgb(225, 233, 240), muted = Color::rgb(144, 163, 183), accent = Color::rgb(86, 215, 185);
constexpr float row_height = 40;
}

Tracker::Tracker(int count, u64 seed) {
    count = std::clamp(count, 1, 100000);
    runs.reserve(count);
    RngKey key = make_key(seed);
    for (int i = 0; i < count; ++i) {
        auto [next, value] = split(key); key = next;
        TrainingRun run{.id = i, .step = 1200 + i * 17,
            .name = "experiment-" + std::to_string(10000 + i),
            .model = i % 3 == 0 ? "Llama 8B" : i % 3 == 1 ? "Qwen 7B" : "Mistral 7B",
            .status = i % 7 == 0 ? "RUNNING" : i % 5 == 0 ? "PAUSED" : "COMPLETE",
            .loss = rng_f32(value, .5f, 2.4f), .tokens = 8500.0 + i % 3000, .gpu = 65.0 + i % 34};
        for (std::size_t n = 0; n < run.history.size(); ++n)
            run.history[n] = float(run.loss + 1.5 * std::exp(-double(n) / 48) + .045 * std::sin(double(n) * .35 + i));
        runs.push_back(std::move(run));
    }
    logs.push_back("[info] Simulated telemetry connected. No external training service.");
    rebuild_filter();
}

void Tracker::step(float dt) {
    KIN_PROFILE_SCOPE("example.tracker.telemetry");
    ++ticks;
    if (!streaming) return;
    elapsed += dt; _sample_time += dt;
    if (_sample_time < .25f) return;
    _sample_time -= .25f; ++revisions;
    for (auto& run : runs) {
        if (run.status != "RUNNING") continue;
        run.step += 8;
        run.loss = std::max(.05, run.loss * .9995 + .002 * std::sin(run.step * .07));
        run.gpu = 82 + 15 * std::sin(run.step * .013 + run.id);
        std::move(run.history.begin() + 1, run.history.end(), run.history.begin());
        run.history.back() = float(run.loss);
    }
    if (revisions % 4 == 0) {
        logs.push_front("[step " + std::to_string(runs[selected].step) + "] " + runs[selected].name + " loss=" + number(runs[selected].loss, 4));
        if (logs.size() > 128) logs.pop_back();
    }
    if (_best_first) rebuild_filter();
}

void Tracker::filter(std::string value, bool best_first) {
    if (value == _filter && best_first == _best_first) return;
    _filter = std::move(value); _best_first = best_first; rebuild_filter();
}

void Tracker::rebuild_filter() {
    KIN_PROFILE_SCOPE("example.tracker.filter");
    visible.clear();
    for (const auto& run : runs)
        if (_filter.empty() || run.name.find(_filter) != std::string::npos || run.model.find(_filter) != std::string::npos || run.status.find(_filter) != std::string::npos)
            visible.push_back(run.id);
    if (_best_first) std::ranges::sort(visible, [&](int a, int b) {
        return runs[a].loss != runs[b].loss ? runs[a].loss < runs[b].loss : a < b;
    });
}

void Tracker::toggle_selected() {
    auto& run = runs[selected]; run.status = run.status == "RUNNING" ? "PAUSED" : "RUNNING";
    logs.push_front("[control] " + run.name + " -> " + run.status);
    if (logs.size() > 128) logs.pop_back();
    rebuild_filter();
}

u64 Tracker::checksum() const {
    u64 value = ticks * 31 + revisions;
    for (const auto& run : runs) value = value * 33 + run.step + u64(run.loss * 1000000);
    return value;
}

void TrackerDashboard::scripted_frame(Tracker& model, int frame, std::string_view scenario) {
    if (scenario == "idle") { model.streaming = false; return; }
    if (scenario == "scroll" || scenario == "churn") _offset = float((frame * 19) % std::max(1, int(model.visible.size()) * int(row_height)));
    if (scenario == "churn" && frame % 60 == 0) {
        _search.state.text = frame % 120 == 0 ? "Llama" : "";
        _best_first = frame % 180 == 0;
        model.filter(_search.state.text, _best_first);
        model.selected = (frame * 13) % int(model.runs.size());
    }
}

void TrackerDashboard::render(Tracker& model, Input& input, Renderer2D& renderer, Vec2f size, float dt, float display_scale) {
    KIN_PROFILE_SCOPE("example.tracker.dashboard");
    // Layout and scrolling use density-independent units. Only the final draw
    // and hit rectangles become pixels; TTF rasterizes at the requested scale.
    const float dpi = std::isfinite(display_scale) && display_scale > 0 ? display_scale : 1;
    const float w = std::max(640.0f, size.x / dpi), h = std::max(480.0f, size.y / dpi);
    renderer.clear(background); _ui.set_theme(_theme); _ui.begin(input, renderer, dt);
    const auto pixels = [dpi](Rectf r) { return Rectf{r.x * dpi, r.y * dpi, r.w * dpi, r.h * dpi}; };
    const auto fill = [&](Rectf r, Color color) { renderer.fill_rect(pixels(r), color); };
    const auto label = [&](std::string_view value, float x, float y, Color color, float points = 14, float width = 0) {
        if (width > 0) _ui.push_clip(pixels({x, y, width, points * 1.6f}));
        // Scale the font face, not a low-resolution text texture.
        ui2::draw_text(renderer, ui2::system_ui_font(15), value, {x * dpi, y * dpi}, points / 15 * dpi, color);
        if (width > 0) _ui.pop_clip();
    };
    const auto line = [&](Vec2f a, Vec2f b, Color color) {
        a = {a.x * dpi, a.y * dpi}; b = {b.x * dpi, b.y * dpi};
        const float dx = b.x - a.x, dy = b.y - a.y, length = std::hypot(dx, dy);
        const int strokes = std::max(1, int(std::round(dpi)));
        for (int i = 0; i < strokes; ++i) {
            const float offset = i - (strokes - 1) * .5f;
            const Vec2f n = length > 0 ? Vec2f{-dy / length * offset, dx / length * offset} : Vec2f{};
            renderer.draw_line({a.x + n.x, a.y + n.y}, {b.x + n.x, b.y + n.y}, color);
        }
    };
    const auto button = [&](std::string_view id, std::string title, Rectf bounds) {
        ui2::Button widget{.id = ui2::make_id(id), .bounds = pixels(bounds), .label = std::move(title),
            .text_style = {.font = ui2::system_ui_font(15), .scale = dpi, .color = text}, .style = _theme.button};
        ui2::run(_ui, widget); return widget.clicked;
    };
    if (!_search.state.active && input.frame_pressed(Key::Space)) model.streaming = !model.streaming;
    if (button("stream", model.streaming ? "LIVE / PAUSE" : "PAUSED / RESUME", {w - 190, 18, 168, 34})) model.streaming = !model.streaming;
    label("RUN OBSERVATORY", 22, 17, text, 22);
    label("TRAINING CONTROL ROOM  /  SIMULATED DATA", 22, 46, muted, 12, w - 230);
    line({20, 70}, {w - 20, 70}, border);

    const float card_w = (w - 64) / 4;
    const auto& selected = model.runs[model.selected];
    const std::array<std::string, 4> values{std::to_string(model.runs.size()), number(selected.loss, 4), number(selected.tokens, 0), number(selected.gpu, 1) + "%"};
    constexpr std::array titles{"TRACKED RUNS", "TRAIN LOSS", "TOKENS / SEC", "GPU UTILIZATION"};
    for (int i = 0; i < 4; ++i) {
        const float x = 20 + i * (card_w + 8);
        fill({x, 86, card_w, 80}, panel);
        label(titles[i], x + 12, 99, muted, 12, card_w - 24);
        label(values[i], x + 12, 123, i == 1 ? accent : text, 24, card_w - 24);
    }
    // Narrow windows retain the table and shrink the details column proportionally.
    const float table_w = (w - 52) * .57f, detail_x = table_w + 36, detail_w = w - detail_x - 20;
    _search.id = ui2::make_id("search-runs"); _search.bounds = pixels({20, 184, table_w - 112, 34});
    _search.text_style = {.font = ui2::system_ui_font(15), .scale = dpi, .color = text}; _search.style = _theme.input;
    _search.style.padding.left *= dpi; _search.style.padding.right *= dpi;
    _search.style.padding.top *= dpi; _search.style.padding.bottom *= dpi;
    ui2::run(_ui, _search);
    if (_search.state.text.empty() && !_search.state.active)
        label("Search runs, models or status...", 30, 192, muted, 14, table_w - 132);
    if (_search.result.changed) { model.filter(_search.state.text, _best_first); _offset = 0; }
    if (button("sort", _best_first ? "LOSS ^" : "SORT LOSS", {table_w - 82, 184, 102, 34})) {
        _best_first = !_best_first; model.filter(_search.state.text, _best_first); _offset = 0;
    }
    const Rectf table{20, 230, table_w, h - 290};
    fill(table, panel);
    const float loss_x = table.x + table.w - 60, state_x = loss_x - 90;
    label("EXPERIMENT / MODEL", 30, 240, muted, 12, state_x - 40);
    label("STATE", state_x, 240, muted, 12);
    label("LOSS", loss_x, 240, muted, 12);
    const Rectf rows{table.x, table.y + 32, table.w, std::max(1.0f, table.h - 32)};
    const Vec2f physical_pointer = _ui.pointer();
    const Vec2f pointer{physical_pointer.x / dpi, physical_pointer.y / dpi};
    if (pointer.x >= rows.x && pointer.x <= rows.x + rows.w && pointer.y >= rows.y && pointer.y <= rows.y + rows.h)
        _offset -= input.mouse_wheel_y() * 72;
    _offset = std::clamp(_offset, 0.0f, std::max(0.0f, float(model.visible.size()) * row_height - rows.h));
    const int first = int(_offset / row_height), last = std::min(int(model.visible.size()), first + int(rows.h / row_height) + 2);
    _drawn_rows = last - first;
    {
        _ui.push_clip(pixels(rows));
        for (int i = first; i < last; ++i) {
            const int id = model.visible[i]; const auto& run = model.runs[id];
            const float y = rows.y + i * row_height - _offset;
            const Rectf bounds{rows.x, y, rows.w - 5, row_height - 1};
            const auto interaction = _ui.region(ui2::make_id(ui2::make_id("run"), u64(id)), pixels(bounds));
            if (interaction.clicked) model.selected = id;
            if (id == model.selected || interaction.hot) fill(bounds, Color::rgb(32, 60, 69));
            label(run.name, 30, y + 2, text, 14, state_x - 40);
            label(run.model, 30, y + 21, muted, 12, state_x - 40);
            label(run.status, state_x, y + 11, run.status == "RUNNING" ? accent : muted, 12, 82);
            label(number(run.loss, 3), loss_x, y + 10, text, 14, 54);
        }
        _ui.pop_clip();
    }
    if (model.visible.empty()) label("No matching runs", 32, 284, muted);
    if (model.visible.size() * row_height > rows.h) {
        const float thumb = std::max(16.0f, rows.h * rows.h / (model.visible.size() * row_height));
        const float y = rows.y + (_offset / std::max(1.0f, model.visible.size() * row_height - rows.h)) * (rows.h - thumb);
        fill({table.x + table.w - 4, y, 3, thumb}, accent);
    }
    const auto& run = model.runs[model.selected];
    label(run.name, detail_x, 186, text, 18, detail_w);
    label(run.model + "  /  STEP " + std::to_string(run.step), detail_x, 214, muted, 12, detail_w);
    // Share the available height with the event stream; do not cap the plot at
    // 220 pixels on large windows. Compact windows keep a small usable graph.
    const float chart_h = std::max(100.0f, (h - 300) * .5f);
    const Rectf chart{detail_x, 240, detail_w, chart_h};
    fill(chart, panel); label("LOSS  /  LAST 192 SAMPLES", detail_x + 12, 252, muted, 12, detail_w - 24);
    const float graph_x = chart.x + 18, graph_y = chart.y + 40, graph_w = chart.w - 36, graph_h = chart.h - 62;
    for (int i = 0; i < 4; ++i) line({graph_x, graph_y + i * graph_h / 3}, {graph_x + graph_w, graph_y + i * graph_h / 3}, border);
    // Selected run plus two neighbors provide a repeatable three-series chart.
    float max_loss = 3;
    for (int s = 0; s < 3; ++s) {
        const auto& history = model.runs[(model.selected + s) % model.runs.size()].history;
        max_loss = std::max(max_loss, *std::max_element(history.begin(), history.end()) + .3f);
    }
    constexpr std::array chart_colors{accent, Color::rgb(103, 144, 224), Color::rgb(186, 138, 211)};
    for (int s = 2; s >= 0; --s) {
        const auto& series = model.runs[(model.selected + s) % model.runs.size()].history;
        for (std::size_t i = 1; i < series.size(); ++i) {
            const auto point = [&](std::size_t n) { return Vec2f{graph_x + float(n) * graph_w / 191, graph_y + graph_h * (1 - std::clamp(series[n] / max_loss, 0.0f, 1.0f))}; };
            line(point(i - 1), point(i), chart_colors[s]);
        }
    }
    const float controls_y = chart.y + chart.h + 12;
    if (button("run-control", run.status == "RUNNING" ? "PAUSE RUN" : "RESUME RUN", {detail_x, controls_y, detail_w, 32})) model.toggle_selected();
    const Rectf logs{detail_x, controls_y + 48, detail_w, std::max(30.0f, h - controls_y - 108)};
    fill(logs, panel); label("EVENT STREAM", logs.x + 12, logs.y + 10, muted, 12);
    {
        _ui.push_clip(pixels(logs));
        int line = 0;
        for (const auto& log : model.logs) {
            if (line * 19 + 50 > logs.h) break;
            label(log, logs.x + 12, logs.y + 32 + line++ * 19, muted, 12, logs.w - 24);
        }
        _ui.pop_clip();
    }
    label(std::to_string(model.visible.size()) + " MATCHES  /  " + std::to_string(_drawn_rows) + " ROWS DRAWN", 22, h - 35, muted, 12);
    label("SPACE live/pause   F1 timings   ESC quit", w < 960 ? 22 : w - 340, h - (w < 960 ? 17 : 35), muted, 12);
    _ui.end();
}
}
