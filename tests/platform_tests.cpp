#include <kin/platform/app.hpp>
#include <kin/platform/log.hpp>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <sstream>
#include <vector>

namespace {

void test_logging() {
    std::ostringstream text_out;
    std::vector<kin::LogEvent> events;
    kin::set_logger_config({
        .min_level = kin::LogLevel::Info,
        .format = kin::LogFormat::Text,
        .sdl_sink = false,
        .stream = &text_out,
        .memory_events = &events,
    });

    KIN_LOG_DEBUG("platform", "filtered");
    KIN_LOG_INFO_F("platform",
                   "hello",
                   (kin::LogFields{
                       {.name = "answer", .value = "42"},
                   }));

    assert(events.size() == 1);
    assert(events[0].level == kin::LogLevel::Info);
    assert(events[0].category == "platform");
    assert(events[0].message == "hello");
    assert(events[0].fields.size() == 1);
    assert(events[0].fields[0].name == "answer");
    assert(events[0].fields[0].value == "42");

    const std::string text = text_out.str();
    assert(text.find("INFO platform: hello answer=42") != std::string::npos);

    const int expected_line = __LINE__ + 1;
    KIN_LOG_WARN("platform", "macro source");
    assert(events.back().file != nullptr);
    assert(std::string{events.back().file}.find("platform_tests.cpp") != std::string::npos);
    assert(events.back().line == expected_line);

    kin::LogEvent escaped{
        .time_ns = 7,
        .level = kin::LogLevel::Error,
        .category = "platform",
        .message = "a\"b\\c\n",
        .file = "file.cpp",
        .line = 3,
        .thread = "main",
        .fields = {{.name = "path", .value = "c:\\tmp\\x"}},
    };
    const std::string json = kin::format_log_json_line(escaped);
    assert(json.find("\"schema\":\"kin.log/1\"") != std::string::npos);
    assert(json.find("\"message\":\"a\\\"b\\\\c\\n\"") != std::string::npos);
    assert(json.find("\"fields\":{\"path\":\"c:\\\\tmp\\\\x\"}") != std::string::npos);

    std::ostringstream compat_out;
    std::vector<kin::LogEvent> compat_events;
    kin::set_logger_config({
        .min_level = kin::LogLevel::Debug,
        .format = kin::LogFormat::JsonLines,
        .sdl_sink = false,
        .stream = &compat_out,
        .memory_events = &compat_events,
    });
    kin::log_error("compat");
    assert(compat_events.size() == 1);
    assert(compat_events[0].level == kin::LogLevel::Error);
    assert(compat_events[0].category == "app");
    assert(compat_out.str().find("\"level\":\"ERROR\"") != std::string::npos);

    const std::filesystem::path log_path = std::filesystem::temp_directory_path() / "kin-platform-log.jsonl";
    std::filesystem::remove(log_path);
    kin::set_logger_config({
        .min_level = kin::LogLevel::Trace,
        .format = kin::LogFormat::Text,
        .sdl_sink = false,
        .file_path = log_path,
    });
    KIN_LOG_INFO_F("platform", "file sink", (kin::LogFields{{.name = "ok", .value = "true"}}));
    kin::set_logger_config({
        .min_level = kin::LogLevel::Debug,
        .format = kin::LogFormat::Text,
        .sdl_sink = false,
    });
    std::ifstream file{log_path};
    std::string line;
    std::getline(file, line);
    assert(line.find("\"schema\":\"kin.log/1\"") != std::string::npos);
    assert(line.find("\"message\":\"file sink\"") != std::string::npos);
    assert(line.find("\"ok\":\"true\"") != std::string::npos);
    file.close();
    std::filesystem::remove(log_path);
}

} // namespace

// Steps per frame for `frames` frame times, as App::run's accumulator takes them.
std::vector<int> steps_per_frame(const std::vector<float>& frame_times, float tolerance, double* debt = nullptr) {
    constexpr float dt = 1.0f / 60.0f;
    kin::FrameTimeSnapper snapper;
    float accumulator = 0.0001f; // a remainder right at the edge: the worst case
    std::vector<int> steps;
    for (const float t : frame_times) {
        accumulator += snapper.advance(t, dt, tolerance);
        int n = 0;
        while (accumulator >= dt) {
            accumulator -= dt;
            ++n;
        }
        steps.push_back(n);
    }
    if (debt) {
        *debt = snapper.debt();
    }
    return steps;
}

void test_frame_time_snapper() {
    // 60 Hz with +-0.4 ms of jitter: unsnapped, frames run 0 or 2 steps.
    std::vector<float> jittery;
    for (int i = 0; i < 600; ++i) {
        jittery.push_back(1.0f / 60.0f + (i % 2 ? 0.0004f : -0.0004f) * static_cast<float>((i * 7) % 5) / 4.0f);
    }
    const auto uneven = [](const std::vector<int>& steps) {
        return std::count_if(steps.begin(), steps.end(), [](int n) { return n != 1; });
    };
    assert(uneven(steps_per_frame(jittery, 0.0f)) > 100);
    assert(uneven(steps_per_frame(jittery, 0.001f)) == 0);

    // A 59.94 Hz display: snapped every frame, and game time keeps up by
    // skipping one step once the lag adds up to a whole one.
    std::vector<float> ntsc(3000, 1.0f / 59.94f);
    double debt = 0.0;
    const std::vector<int> steps = steps_per_frame(ntsc, 0.001f, &debt);
    const long total = std::accumulate(steps.begin(), steps.end(), 0L);
    assert(uneven(steps) >= 2 && uneven(steps) <= 4); // about one every 1000 frames
    assert(std::abs(static_cast<double>(total) / 60.0 + debt - 3000.0 / 59.94) < 1.0 / 60.0);
    assert(std::abs(debt) < 1.0 / 60.0);

    // A hitch near three steps counts as three; one off the grid is untouched.
    kin::FrameTimeSnapper snapper;
    assert(std::abs(snapper.advance(0.0502f, 1.0f / 60.0f, 0.001f) - 0.05f) < 1e-6f);
    assert(snapper.advance(0.0420f, 1.0f / 60.0f, 0.001f) == 0.0420f);
    assert(snapper.advance(0.0069f, 1.0f / 60.0f, 0.001f) == 0.0069f); // 144 Hz, unpaced
}

void push_wheel_notch() {
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_WHEEL;
    event.wheel.y = 1.0f;
    const bool pushed = SDL_PushEvent(&event);
    assert(pushed);
    (void)pushed;
}

// Each wheel notch reaches exactly one fixed step through App::run: mouse_wheel_y()
// adds up across 0-step frames and is used up by a frame's first step, while
// frame_mouse_wheel_y() (what ui2 scrolls with) lives one rendered frame, stepped or
// not. time_scale picks the steps: 0 runs none, a huge one runs max_steps.
void test_mouse_wheel_reaches_one_step() {
    kin::App app{{.mode = kin::AppMode::Headless, .max_steps = 3}};
    std::vector<float> step_wheel;
    std::vector<int> step_numbers;
    std::vector<float> render_wheel;
    std::vector<int> render_steps;
    app.set_time_scale(0.0f);
    push_wheel_notch();
    app.run([&](float) {
        step_wheel.push_back(app.input().mouse_wheel_y());
        step_numbers.push_back(app.frame_stats().update_steps);
    },
            [&](float) {
        render_wheel.push_back(app.input().frame_mouse_wheel_y());
        render_steps.push_back(app.frame_stats().update_steps);
        switch (render_wheel.size()) {
        case 1: // 0-step frame with a notch; another follows
            push_wheel_notch();
            break;
        case 2: // the next frame steps and sees both
            app.set_time_scale(1e9f);
            break;
        case 3: // a frame with one notch and three steps
            push_wheel_notch();
            break;
        default:
            app.quit();
            break;
        }
    });
    assert((render_steps == std::vector<int>{0, 0, 3, 3}));
    assert((render_wheel == std::vector<float>{1.0f, 1.0f, 0.0f, 1.0f}));
    assert((step_wheel == std::vector<float>{2.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f}));
    // update_steps counts the step being run, as under run_for.
    assert((step_numbers == std::vector<int>{1, 2, 3, 1, 2, 3}));
}

int main() {
    test_logging();
    test_frame_time_snapper();
    // Its own App, before the one below: each App inits and quits SDL.
    test_mouse_wheel_reaches_one_step();

    kin::App app{{.mode = kin::AppMode::Headless}};

    kin::Window& first = app.create_window({
        .title = "first",
        .width = 320,
        .height = 180,
        .hidden = true,
        .borderless = true,
    });

    kin::Window& second = app.create_window({
        .title = "second",
        .width = 160,
        .height = 90,
        .hidden = true,
    });

    assert(app.window_count() == 2);
    assert(app.open_window_count() == 2);
    assert(first.id() != 0);
    assert(second.id() != 0);
    assert(first.id() != second.id());
    assert(first.borderless());
    assert(!second.borderless());
    assert((first.pixel_size() == kin::Vec2i{320, 180}));
    assert(first.display_scale() > 0);
    first.set_minimum_size({160, 90});
    assert(app.find_window(first.id()) == &first);
    assert(app.find_window(second.id()) == &second);

    first.request_close();
    assert(first.close_requested());
    assert(app.open_window_count() == 1);

    kin::InputMap map;
    map.bind("confirm", kin::Key::Enter);
    map.bind("confirm", kin::MouseButton::Left);
    map.bind("move_left", {kin::Key::A, kin::Key::Left});
    map.bind("copy", kin::Key::C, kin::KeyModifiers::Ctrl);

    const auto actions = map.actions();
    assert(actions.size() == 3);
    assert(actions[0].name == "confirm");
    assert(actions[0].bindings.size() == 2);
    assert(actions[0].bindings[0].device == kin::InputBindingDevice::Key);
    assert(actions[0].bindings[0].code == static_cast<kin::i32>(kin::Key::Enter));
    assert(kin::binding_name(actions[0].bindings[0]) == "Enter");
    assert(kin::binding_name(actions[0].bindings[1]) == "MouseLeft");
    assert(actions[1].name == "copy");
    assert(actions[1].bindings.size() == 1);
    assert(actions[1].bindings[0].modifiers == kin::KeyModifiers::Ctrl);
    assert(actions[2].name == "move_left");
    assert(actions[2].bindings.size() == 2);
    assert(kin::binding_name(actions[2].bindings[0]) == "A");
    assert(kin::binding_name(actions[2].bindings[1]) == "Left");

    const std::filesystem::path input_path = std::filesystem::temp_directory_path() / "kin-platform-input.kininput";
    std::vector<kin::LogEvent> input_events;
    kin::set_logger_config({
        .min_level = kin::LogLevel::Debug,
        .sdl_sink = false,
        .memory_events = &input_events,
    });
    assert(kin::save_input_map(map, input_path));
    const kin::InputMap loaded_map = kin::load_input_map(input_path);
    bool saw_save = false;
    bool saw_load = false;
    bool saw_bind = false;
    for (const kin::LogEvent& event : input_events) {
        saw_save = saw_save || (event.category == "input" && event.message == "input map saved");
        saw_load = saw_load || (event.category == "input" && event.message == "input map loaded");
        saw_bind = saw_bind || (event.category == "input" && event.message == "input binding added");
    }
    assert(saw_save);
    assert(saw_load);
    assert(saw_bind);
    kin::set_logger_config({.sdl_sink = false});
    const auto loaded_actions = loaded_map.actions();
    assert(loaded_actions.size() == 3);
    assert(loaded_actions[0].name == "confirm");
    assert(loaded_actions[0].bindings.size() == 2);
    assert(kin::binding_name(loaded_actions[0].bindings[0]) == "Enter");
    assert(kin::binding_name(loaded_actions[0].bindings[1]) == "MouseLeft");
    assert(loaded_actions[1].name == "copy");
    assert(loaded_actions[1].bindings[0].modifiers == kin::KeyModifiers::Ctrl);
    assert(loaded_actions[2].name == "move_left");
    assert(kin::binding_name(loaded_actions[2].bindings[0]) == "A");
    assert(kin::binding_name(loaded_actions[2].bindings[1]) == "Left");

    kin::InputActionContext global{"global"};
    global.add("quit", "Quit");
    kin::InputActionContext menu{"menu"};
    menu.add("confirm", "Confirm");
    menu.add("quit", "Back");
    global.include(menu);

    const auto flattened = global.flattened_actions();
    assert(flattened.size() == 2);
    assert(flattened[0].name == "quit");
    assert(flattened[0].label == "Back");
    assert(flattened[1].name == "confirm");

    const auto available = global.resolve(map);
    assert(available.size() == 2);
    assert(available[0].name == "quit");
    assert(available[0].bindings.empty());
    assert(available[1].name == "confirm");
    assert(available[1].bindings.size() == 2);

    app.input().set_map(map);
    app.input().set_action_pressed("confirm");
    kin::Input keypad;
    keypad.bind("install",kin::Key::KeypadEnter);
    keypad.set_action_pressed("install");
    assert(keypad.frame_pressed(kin::Key::KeypadEnter));
    keypad.advance_step_edges();
    assert(keypad.frame_pressed("install"));
    assert(kin::binding_name(keypad.map().bindings("install")->front())=="KeypadEnter");
    keypad.begin_frame();
    assert(!keypad.frame_pressed("install"));
    assert(app.input().pressed("confirm"));
    app.input().unbind("confirm");
    assert(!app.input().pressed("confirm"));

    app.input().set_map(map);
    app.input().begin_frame();
    app.input().set_action_pressed("copy");
    assert(app.input().modifier_held(kin::KeyModifiers::Ctrl));
    assert(app.input().pressed("copy"));
    assert(app.input().frame_pressed("copy"));
    app.input().begin_frame();
    assert(!app.input().pressed("copy"));
    // A per-frame edge lives exactly one rendered frame: begin_frame clears it so a
    // press read by ui2 in render() (which runs every frame) fires once, not once per
    // frame. (begin_frame(bool) is private/App-only, so this covers the public path.)
    assert(!app.input().frame_pressed("copy"));
    app.input().set_clipboard_text("copied");
    assert(app.input().clipboard_text() == "copied");

    // Mid-frame keyboard advance (run between the first fixed step and render)
    // consumes only the STICKY keyboard edge (pressed(), read by update). The
    // per-frame keyboard edge (frame_pressed(): text-field backspace/arrows/enter
    // read in render) and all mouse/text edges MUST survive to render. Regression:
    // clearing the frame edge dropped ~half of render-time keystrokes, and clearing
    // the mouse edge dropped ~half of clicks.
    app.input().begin_frame();
    app.input().set_action_pressed("copy");
    app.input().set_mouse_pressed(kin::MouseButton::Left);
    assert(app.input().pressed("copy"));
    assert(app.input().frame_pressed("copy"));
    assert(app.input().mouse_frame_pressed(kin::MouseButton::Left));
    app.input().advance_step_edges();
    assert(!app.input().pressed("copy"));                                    // sticky consumed (update)
    assert(app.input().frame_pressed("copy"));                              // frame edge survives (render)
    assert(app.input().mouse_frame_pressed(kin::MouseButton::Left));         // mouse preserved (render)
    app.input().begin_frame();
    assert(!app.input().frame_pressed("copy"));                             // cleared next frame
    assert(!app.input().mouse_frame_pressed(kin::MouseButton::Left));        // cleared next frame

    app.run_for(2, [&app](kin::f32 dt, kin::i32 frame) {
        assert(dt > 0.0f);
        assert(frame >= 0);
        assert(app.frame_stats().update_steps == 1);
    });

    return 0;
}
