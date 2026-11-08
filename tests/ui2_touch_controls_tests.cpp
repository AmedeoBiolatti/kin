// On-screen controls: Input's screen actions and axes, ui2's TouchStick and
// TouchButton, and a screen file's touch_stick and touch_button.
#include <kin/renderer/backend.hpp>
#include <kin/platform/input.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/ui2/context.hpp>
#include <kin/ui2/screen.hpp>
#include <kin/ui2/touch_controls.hpp>

#include <cassert>
#include <cmath>
#include <functional>
#include <memory>
#include <string>

namespace {

using namespace kin;

class NullBackend final : public IRenderer2DBackend {
public:
    std::string_view name() const override { return "null"; }
    void clear(Color) override {}
    void present() override {}
    void set_logical_size(Vec2i) override {}
    void set_integer_logical_size(Vec2i) override {}
    Vec2i output_size() const override { return {1280, 720}; }
    Vec2f window_to_logical(Vec2f v) const override { return v; }
    Vec2f logical_to_window(Vec2f v) const override { return v; }
    Texture create_texture_from_rgba(const u8*, Vec2i) override { return {}; }
    void draw_texture(const Texture&, Rectf) override {}
    void draw_texture(const Texture&, Rectf, Rectf) override {}
    void fill_rect(Rectf, Color) override {}
    void draw_rect(Rectf, Color) override {}
    void draw_line(Vec2f, Vec2f, Color) override {}
    void set_viewport(Rectf) override {}
    void reset_viewport() override {}
    void push_viewport(Rectf) override {}
    void pop_viewport() override {}
};

bool near(f32 a, f32 b) {
    return std::abs(a - b) < 1e-3f;
}

// What an App frame does: its start, the system's events (`events`), a
// fixed step (`step`: what update() reads, then used up), then the UI drawn
// (`draw`), whose controls the next frame's step reads.
struct Harness {
    Input input;
    Renderer2D renderer{std::make_unique<NullBackend>()};
    ui2::Context ui;

    void frame(const std::function<void()>& events, const std::function<void()>& draw, const std::function<void()>& step = {}) {
        input.begin_frame();
        if (events) {
            events();
        }
        if (step) {
            step();
        }
        input.advance_step_edges();
        ui.begin(input, renderer);
        if (draw) {
            draw();
        }
        ui.end();
    }
};

void test_screen_actions_and_axes() {
    Input input;
    InputMap map;
    map.bind("jump", Key::Space);
    map.set_axis({.name = "move_x", .negative = {{.device = InputBindingDevice::Key, .code = static_cast<i32>(Key::A)}}});
    input.set_map(map);
    input.begin_frame();
    input.set_screen_action("jump", true);
    input.set_screen_action("fire", true); // not in the map: held all the same
    input.set_screen_axis("move_x", 0.5f);
    input.set_screen_axis("look", -2.0f);  // clamped
    input.begin_frame(); // the next frame's step
    assert(input.pressed("jump") && input.held("jump") && input.held("fire") && input.held(0, "jump"));
    assert(!input.held(1, "jump")); // player 1 has no keyboard, nor the screen's controls
    assert(near(input.axis("move_x"), 0.5f) && near(input.axis("look"), -1.0f) && near(input.axis(0, "look"), -1.0f));
    assert(near(input.axis(1, "look"), 0.0f));
    // The keys pushed further win.
    input.set_key_pressed(Key::A);
    assert(near(input.axis("move_x"), -1.0f));
    input.set_key_released(Key::A);
    input.advance_step_edges(); // the step used the press
    // Set again this frame: still held; then not set for a frame: let go.
    input.set_screen_action("jump", true);
    input.set_screen_axis("move_x", 0.5f);
    input.begin_frame();
    assert(input.held("jump") && !input.pressed("jump") && near(input.axis("move_x"), 0.5f));
    input.advance_step_edges();
    input.begin_frame();
    assert(!input.held("jump") && input.released("jump") && near(input.axis("move_x"), 0.0f) && !input.held("fire"));
    input.advance_step_edges();
    // A tap between two steps: pressed and released, both seen by the next.
    input.set_screen_action("jump", true);
    input.set_screen_action("jump", false);
    input.begin_frame();
    assert(input.pressed("jump"));
    input.begin_frame(); // a frame with no step: still there for one
    assert(input.pressed("jump"));
    input.advance_step_edges();
    assert(!input.pressed("jump"));
    // Slots let go are reused: more names than slots over time.
    for (int i = 0; i < 100; ++i) {
        input.set_screen_action("a" + std::to_string(i), true);
        input.begin_frame();
        input.advance_step_edges();
        input.begin_frame();
        input.advance_step_edges();
    }
    input.set_screen_action("last", true);
    input.begin_frame();
    assert(input.held("last"));
}

void test_stick_follows_its_finger() {
    Harness h;
    ui2::TouchStick stick{.id = ui2::make_id("move"), .bounds = {0, 0, 640, 720}, .radius = 50.0f, .dead_zone = 0.0f,
                          .shown = ui2::TouchControlsShown::Always};
    const auto draw = [&] { ui2::run(h.ui, stick); };
    h.frame([&] { h.input.set_touch_down(1, {200, 400}); }, draw);
    assert(stick.active && stick.finger == 1u && h.input.touch(1)->claimed && !h.input.pointer_touch_active());
    h.frame([&] { h.input.set_touch_moved(1, {225, 400}); }, draw); // half its radius right
    assert(near(stick.value.x, 0.5f) && near(stick.value.y, 0.0f));
    h.frame([&] { h.input.set_touch_moved(1, {200, 500}); }, draw); // past it, downwards
    assert(near(stick.value.x, 0.0f) && near(stick.value.y, 1.0f));
    bool stepped = false;
    h.frame(nullptr, draw, [&] {
        assert(near(h.input.axis("move_y"), 1.0f) && near(h.input.axis("move_x"), 0.0f));
        assert(!h.input.mouse_held(MouseButton::Left)); // the finger pressed nothing
        stepped = true;
    });
    assert(stepped);
    h.frame([&] { h.input.set_touch_up(1); }, draw);
    assert(!stick.active && !stick.finger && near(stick.value.y, 0.0f));
    // A finger landing outside it is not its own: the pointer's.
    h.frame([&] { h.input.set_touch_down(2, {900, 400}); }, draw);
    assert(!stick.active && !h.input.touch(2)->claimed && h.input.pointer_touch_active());
    h.frame([&] { h.input.set_touch_up(2); }, draw);
    // Fixed: the middle is the bounds'.
    stick.floating = false;
    stick.bounds = {100, 100, 100, 100};
    h.frame([&] { h.input.set_touch_down(3, {150, 125}); }, draw);
    assert(near(stick.value.x, 0.0f) && near(stick.value.y, -0.5f));
}

void test_button_holds_its_action() {
    Harness h;
    ui2::TouchButton jump{.id = ui2::make_id("jump"), .bounds = {1100, 500, 100, 100}, .action = "jump",
                          .shown = ui2::TouchControlsShown::Always};
    ui2::TouchStick stick{.id = ui2::make_id("move"), .bounds = {0, 0, 640, 720}, .shown = ui2::TouchControlsShown::Always};
    const auto draw = [&] {
        ui2::run(h.ui, stick);
        ui2::run(h.ui, jump);
    };
    // A finger on the stick, then another on the button: both at once.
    h.frame([&] { h.input.set_touch_down(1, {200, 400}); }, draw);
    h.frame([&] { h.input.set_touch_down(2, {1150, 550}); }, draw);
    assert(jump.held && jump.pressed && stick.active);
    h.frame(nullptr, draw, [&] { assert(h.input.pressed("jump") && h.input.held("jump")); });
    assert(jump.held && !jump.pressed);
    h.frame(nullptr, draw, [&] { assert(!h.input.pressed("jump") && h.input.held("jump")); });
    // Slid well off it: let go; back: held again.
    h.frame([&] { h.input.set_touch_moved(2, {900, 550}); }, draw);
    assert(!jump.held);
    h.frame([&] { h.input.set_touch_moved(2, {1150, 550}); }, draw);
    assert(jump.held && jump.pressed);
    h.frame([&] { h.input.set_touch_up(2); }, draw);
    assert(!jump.held);
    h.frame(nullptr, draw, [&] { assert(!h.input.held("jump") && h.input.released("jump")); });
    // Not shown (Touch, a desktop, no finger yet): they hold nothing, and the mouse is the pointer.
    Harness desk;
    ui2::TouchButton fire{.id = ui2::make_id("fire"), .bounds = {0, 0, 100, 100}, .action = "fire"};
    desk.frame([&] {
        desk.input.set_mouse_pos({50, 50});
        desk.input.set_mouse_pressed(MouseButton::Left);
    }, [&] { ui2::run(desk.ui, fire); });
    assert(!ui2::touch_controls_shown(desk.ui) && !fire.held);
    desk.frame(nullptr, [&] { ui2::run(desk.ui, fire); }, [&] { assert(!desk.input.held("fire")); });
    // Always: the mouse works it.
    fire.shown = ui2::TouchControlsShown::Always;
    desk.frame([&] { desk.input.set_mouse_held(MouseButton::Left, false); }, [&] { ui2::run(desk.ui, fire); });
    desk.frame([&] { desk.input.set_mouse_pressed(MouseButton::Left); }, [&] { ui2::run(desk.ui, fire); });
    desk.frame([&] { desk.input.set_mouse_held(MouseButton::Left, true); }, [&] { ui2::run(desk.ui, fire); });
    assert(fire.held);
}

void test_screen_touch_elements() {
    const std::string text = R"({"schema": "kin.ui/1", "root": {"kind": "box", "width": "grow", "height": "grow", "children": [
        {"id": "move", "kind": "touch_stick", "width": 400, "height": 400, "anchor": "bottom_left", "radius": 40, "axis_x": "walk"},
        {"id": "talk", "kind": "touch_button", "action": "talk", "text": "Talk", "width": 100, "height": 100,
         "anchor": "bottom_right", "offset": {"x": -20, "y": -20}}]}})";
    ui2::ScreenDocument doc;
    std::string error;
    assert(ui2::parse_screen(text, doc, error));
    const ui2::ScreenElement* move = doc.find("move");
    const ui2::ScreenElement* talk = doc.find("talk");
    assert(move && move->kind == ui2::ScreenKind::TouchStick && move->radius == 40.0f && move->axis_x == "walk" && move->axis_y.empty());
    assert(talk && talk->kind == ui2::ScreenKind::TouchButton && talk->action == "talk" && talk->text == "Talk");
    ui2::ScreenDocument again;
    assert(ui2::parse_screen(ui2::serialize_screen(doc), again, error) && again == doc);
    assert(!ui2::parse_screen(R"({"schema": "kin.ui/1", "root": {"kind": "touch_pad"}})", again, error));

    Harness h;
    ui2::UiScreen screen{doc};
    const auto draw = [&] { screen.draw(h.ui, {0, 0, 1280, 720}); };
    // A finger makes touch the pointer, so they show; the first frame lays them out.
    h.frame([&] { h.input.set_touch_down(9, {640, 100}); }, draw);
    h.frame([&] { h.input.set_touch_up(9); }, draw);
    h.frame([&] {
        h.input.set_touch_down(1, {100, 600});  // in the stick (0, 320, 400, 400)
        h.input.set_touch_down(2, {1210, 650}); // on the button (1160, 600, 100, 100)
    }, draw);
    h.frame([&] { h.input.set_touch_moved(1, {140, 600}); }, draw);
    assert(screen.clicked("talk"));
    h.frame(nullptr, draw, [&] { assert(h.input.held("talk") && h.input.axis("walk") > 0.9f && near(h.input.axis("move_y"), 0.0f)); });
    // Hidden by the game: let go.
    screen.set_visible("talk", false);
    h.frame(nullptr, draw);
    h.frame(nullptr, draw, [&] { assert(!h.input.held("talk") && h.input.axis("walk") > 0.9f); });
}

} // namespace

int main() {
    test_screen_actions_and_axes();
    test_stick_follows_its_finger();
    test_button_holds_its_action();
    test_screen_touch_elements();
    return 0;
}
