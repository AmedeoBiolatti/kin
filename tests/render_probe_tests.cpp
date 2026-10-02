#include <kin/core/json.hpp>
#include <kin/runtime/render_probe.hpp>
#include <kin/runtime/scene_app.hpp>

#include <cassert>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr kin::Vec2i frame_size{128, 96};

struct Box {
    kin::i32 x = 0;
    kin::i32 y = 0;
    kin::i32 w = 0;
    kin::i32 h = 0;
    kin::u8 r = 0;
    kin::u8 g = 0;
    kin::u8 b = 0;
};

// A dark frame with a light checker pattern (so motion is visible everywhere)
// scrolled by `scroll` pixels, and the given boxes on top.
std::vector<kin::u8> make_frame(std::initializer_list<Box> boxes, kin::i32 scroll = 0, bool pattern = false) {
    std::vector<kin::u8> pixels(static_cast<std::size_t>(frame_size.x * frame_size.y) * 4u);
    for (kin::i32 y = 0; y < frame_size.y; ++y) {
        for (kin::i32 x = 0; x < frame_size.x; ++x) {
            const std::size_t i = static_cast<std::size_t>(y * frame_size.x + x) * 4u;
            const bool light = pattern && (((x + scroll) / 4 + y / 4) % 2 == 0);
            pixels[i] = light ? 60 : 20;
            pixels[i + 1] = light ? 64 : 22;
            pixels[i + 2] = light ? 70 : 28;
            pixels[i + 3] = 255;
        }
    }
    for (const Box& box : boxes) {
        for (kin::i32 y = std::max(box.y, 0); y < std::min(box.y + box.h, frame_size.y); ++y) {
            for (kin::i32 x = std::max(box.x, 0); x < std::min(box.x + box.w, frame_size.x); ++x) {
                const std::size_t i = static_cast<std::size_t>(y * frame_size.x + x) * 4u;
                pixels[i] = box.r;
                pixels[i + 1] = box.g;
                pixels[i + 2] = box.b;
            }
        }
    }
    return pixels;
}

bool contains(kin::RenderProbeRect outer, kin::i32 x, kin::i32 y, kin::i32 w, kin::i32 h) {
    return outer.x <= x && outer.y <= y && outer.x + outer.w >= x + w && outer.y + outer.h >= y + h;
}

bool overlaps_box(kin::RenderProbeRect rect, kin::i32 x, kin::i32 y, kin::i32 w, kin::i32 h) {
    return rect.x < x + w && x < rect.x + rect.w && rect.y < y + h && y < rect.y + rect.h;
}

std::string probe_json(const kin::RenderProbe& probe) {
    std::ostringstream out;
    kin::JsonWriter json(out);
    probe.write_json(json);
    return out.str();
}

// Nothing changes: no events, no change.
void test_static_frames() {
    kin::RenderProbe probe;
    const std::vector<kin::u8> frame = make_frame({{40, 30, 20, 20, 200, 80, 60}});
    for (int i = 0; i < 30; ++i) {
        probe.add_frame(frame, frame_size);
    }
    assert(probe.frame_count() == 30);
    assert(probe.events().empty());
    for (const kin::RenderProbeFrame& stats : probe.timeline()) {
        assert(stats.delta == 0.0f && stats.changed == 0.0f);
    }
}

// Steady motion is not an anomaly: a scrolling background, a sprite moving a few
// pixels a frame, and a small sprite crossing a whole tile every frame.
void test_steady_motion_is_quiet() {
    kin::RenderProbe probe;
    for (int i = 0; i < 60; ++i) {
        probe.add_frame(make_frame({{4 + i, 20, 18, 18, 230, 200, 90}, {(i * 20) % 140 - 10, 70, 6, 6, 250, 250, 250}},
                                   i, true),
                        frame_size);
    }
    assert(probe.events().empty());
    assert(probe.timeline().back().delta > 0.0f);
}

// A sprite jittering one pixel back and forth in place is one flicker event,
// located on the sprite, lasting the whole run.
void test_jitter_is_flicker() {
    kin::RenderProbe probe;
    for (int i = 0; i < 40; ++i) {
        probe.add_frame(make_frame({{50 + (i % 2), 40, 20, 20, 220, 120, 60}}), frame_size);
    }
    assert(probe.event_count(kin::RenderProbeEventKind::Flicker) == 1);
    assert(probe.event_count(kin::RenderProbeEventKind::Spike) == 0);
    const kin::RenderProbeEvent& event = probe.events().front();
    assert(event.first_frame == 2);
    assert(event.last_frame == 39);
    assert(event.frames == 38);
    // Only the sprite's left and right edges change.
    assert(contains(event.rect, 50, 40, 1, 20));
    assert(contains(event.rect, 70, 40, 1, 20));
    assert(event.rect.x >= 48 && event.rect.x + event.rect.w <= 80);
}

// One wrong frame (a sprite with the wrong color for a frame) is a one-frame
// flicker at that frame, with something else moving steadily nearby; the spikes
// into and out of it are folded into the flicker.
void test_glitch_frame_is_flicker() {
    kin::RenderProbe probe;
    for (int i = 1; i <= 40; ++i) {
        const kin::u8 red = i == 20 ? 30 : 220;
        probe.add_frame(make_frame({{90, 10, 24, 24, red, 200, 60}, {10 + i, 60, 16, 16, 90, 140, 250}}), frame_size);
    }
    assert(probe.events().size() == 1);
    const kin::RenderProbeEvent& event = probe.events().front();
    assert(event.kind == kin::RenderProbeEventKind::Flicker);
    assert(event.first_frame == 20 && event.last_frame == 20 && event.frames == 1);
    assert(contains(event.rect, 90, 10, 24, 24));
    assert(event.peak_frame == 20);
    assert(event.peak_score >= probe.config().flicker_ratio);
}

// Under a scrolling background every tile changes every frame, which hides the
// A-B-A of a wrong frame; a large enough one is still caught, as a spike in and out.
void test_glitch_frame_over_scrolling_is_spike() {
    kin::RenderProbe probe;
    for (int i = 1; i <= 40; ++i) {
        const kin::u8 red = i == 20 ? 30 : 220;
        probe.add_frame(make_frame({{90, 10, 24, 24, red, 200, 60}}, i, true), frame_size);
    }
    assert(probe.events().size() == 1);
    const kin::RenderProbeEvent& event = probe.events().front();
    assert(event.kind == kin::RenderProbeEventKind::Spike);
    assert(event.first_frame == 20 && event.last_frame == 21 && event.frames == 2);
    assert(overlaps_box(event.rect, 90, 10, 24, 24));
}

// A sudden large change that stays (a region filled with garbage) is a spike, at
// the frame it happens and where it happens; slow motion before it is the baseline.
void test_sudden_change_is_spike() {
    kin::RenderProbe probe;
    for (int i = 1; i <= 50; ++i) {
        if (i < 30) {
            probe.add_frame(make_frame({{10 + i, 50, 16, 16, 120, 140, 200}}), frame_size);
        } else {
            probe.add_frame(make_frame({{10 + i, 50, 16, 16, 120, 140, 200}, {96, 0, 32, 32, 255, 0, 255}}), frame_size);
        }
    }
    assert(probe.event_count(kin::RenderProbeEventKind::Spike) == 1);
    assert(probe.event_count(kin::RenderProbeEventKind::Flicker) == 0);
    const kin::RenderProbeEvent& spike = probe.events().front();
    assert(spike.first_frame == 30 && spike.last_frame == 30);
    assert(spike.rect.x == 96 && spike.rect.y == 0 && spike.rect.w == 32 && spike.rect.h == 32);
    assert(spike.peak_delta > 0.5f);
}

// Spikes wait for a baseline: a change during warm-up is not reported.
void test_spikes_wait_for_warmup() {
    kin::RenderProbe probe({.warmup_frames = 10});
    for (int i = 1; i <= 20; ++i) {
        probe.add_frame(make_frame(i < 5 ? std::initializer_list<Box>{} : std::initializer_list<Box>{{0, 0, 64, 64, 255, 255, 255}}),
                        frame_size);
    }
    assert(probe.events().empty());
}

// A new frame size starts the comparison over instead of diffing mismatched frames.
void test_resize_restarts() {
    kin::RenderProbe probe;
    probe.add_frame(make_frame({}), frame_size);
    probe.add_frame(make_frame({}), frame_size);
    const std::vector<kin::u8> small(16u * 16u * 4u, 255);
    probe.add_frame(small, {16, 16});
    probe.add_frame(small, {16, 16});
    assert(probe.frame_count() == 4);
    assert(probe.frame_size() == (kin::Vec2i{16, 16}));
    assert(probe.events().empty());
    // Too few pixels for the size given: ignored.
    probe.add_frame(std::span<const kin::u8>(small.data(), 8), {16, 16});
    assert(probe.frame_count() == 4);
}

// The same frames give the same report.
void test_report_is_deterministic() {
    const auto run = [] {
        kin::RenderProbe probe;
        for (int i = 0; i < 30; ++i) {
            probe.add_frame(make_frame({{30 + (i % 2) * 2, 30, 20, 20, 250, 90, 90}, {i * 3, 70, 10, 10, 90, 250, 90}}),
                            frame_size);
        }
        return probe_json(probe);
    };
    const std::string first = run();
    assert(first == run());
    assert(first.find("\"schema\": \"kin.render_probe/1\"") != std::string::npos);
    assert(first.find("\"kind\": \"flicker\"") != std::string::npos);
    assert(first.find("\"timeline\"") != std::string::npos);
}

// Draws a box that either moves steadily or jitters one pixel in place.
class BoxScene final : public kin::Scene {
public:
    explicit BoxScene(bool jitter)
        : _jitter(jitter) {
    }

    void update(kin::SceneContext&) override { ++_frame; }

    void render(kin::SceneContext& ctx) override {
        ctx.renderer.clear(kin::Color::rgb(16, 18, 24));
        const kin::f32 x = _jitter ? 20.0f + static_cast<kin::f32>(_frame % 2) : 4.0f + static_cast<kin::f32>(_frame);
        ctx.renderer.fill_rect(kin::Rectf{x, 20.0f, 16.0f, 16.0f}, kin::Color::rgb(230, 200, 90));
    }

private:
    bool _jitter = false;
    kin::i32 _frame = 0;
};

// Every run_scene_app game gets the probe: it renders headless, reads back each
// frame, and reports; --probe-fail turns a finding into a failed run.
void test_scene_app_probe() {
    {
        kin::SceneManager scenes;
        scenes.push(std::make_unique<BoxScene>(false));
        std::ostringstream probe;
        const int code = kin::run_scene_app({
            .window = {.title = "probe-steady", .width = 64, .height = 64},
            .headless = {.enabled = true, .frames = 20},
            .probe_output = &probe,
        }, scenes);
        assert(code == 0);
        const std::string text = probe.str();
        assert(text.find("\"schema\": \"kin.render_probe/1\"") != std::string::npos);
        assert(text.find("\"frames\": 20") != std::string::npos);
        assert(text.find("\"events\": 0") != std::string::npos);
    }
    {
        kin::SceneManager scenes;
        scenes.push(std::make_unique<BoxScene>(true));
        std::ostringstream probe;
        std::ostringstream report;
        const int code = kin::run_scene_app({
            .window = {.title = "probe-jitter", .width = 64, .height = 64},
            .headless = {.enabled = true, .frames = 20, .probe_fail = true},
            .report_output = &report,
            .probe_output = &probe,
        }, scenes);
        assert(code == 1);
        assert(probe.str().find("\"flickers\": 1") != std::string::npos);
        const std::string text = report.str();
        assert(text.find("\"status\": \"failed\"") != std::string::npos);
        assert(text.find("render probe found 1 event(s); first: flicker") != std::string::npos);
    }

    const char* argv[] = {"game", "--probe-render=out/probe.json", "--probe-fail", "--probe-tile=8"};
    const kin::HeadlessOptions options = kin::parse_headless_options(4, const_cast<char**>(argv));
    assert(options.enabled);
    assert(options.probe_render_path == "out/probe.json");
    assert(options.probe_fail);
    assert(options.probe_tile_size == 8);
    const char* bare[] = {"game", "--probe-render"};
    assert(kin::parse_headless_options(2, const_cast<char**>(bare)).probe_render_path == "-");
}

} // namespace

int main() {
    test_static_frames();
    test_steady_motion_is_quiet();
    test_jitter_is_flicker();
    test_glitch_frame_is_flicker();
    test_glitch_frame_over_scrolling_is_spike();
    test_sudden_change_is_spike();
    test_spikes_wait_for_warmup();
    test_resize_restarts();
    test_report_is_deterministic();
    test_scene_app_probe();
    return 0;
}
