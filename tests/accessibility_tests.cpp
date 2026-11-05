#include <cassert>
#include <kin/platform/app.hpp>
#include <kin/ui2/accessibility.hpp>
#include <kin/ui2/context.hpp>
#include <thread>

using namespace kin;
int main() {
    App app{{.mode = AppMode::Headless}};
    auto& window = app.create_window({.title = "Accessible UI", .width = 160, .height = 100, .hidden = true});
    Renderer2D renderer{window};
    ui2::Context ui;
    Input input;
    ui2::Accessibility bridge{window, renderer};
    const auto id = ui2::make_id("play");
    bool enabled = true, present = true, clicked = false;
    ui2::Slider slider{.id = ui2::make_id("volume"), .bounds = {10, 60, 100, 20}, .value = .5f, .step = .1f};
    auto frame = [&] {
        input.begin_frame();
        ui.begin(input, renderer, .016f);
        if (present) {
            ui.semantic_label(id, "Start game");
            ui.describe(id, "button", "", {10, 10, 100, 30}, enabled, true);
            clicked = ui.region(id, {10, 10, 100, 30}).clicked;
        }
        ui2::run(ui, slider);
        ui.end();
        bridge.update(ui);
    };
    frame();
    assert(bridge.snapshot().at(0).label == "Start game");
    std::thread reader([&] { assert(bridge.request_action(id, ui2::SemanticAction::Click)); });
    reader.join();
    frame();
    assert(!clicked); // main thread dispatches it for the next frame
    frame();
    assert(clicked);
    enabled = false;
    frame();
    assert(!bridge.request_action(id, ui2::SemanticAction::Click));
    enabled = true;
    frame();
    assert(bridge.request_action(id, ui2::SemanticAction::Click));
    present = false;
    frame();
    present = true;
    frame();
    assert(!clicked); // removed native target is never recycled
    bridge.announce("Saved");
    frame();
    bridge.announce("Saved", true);
    frame();
    assert(bridge.request_action(slider.id, ui2::SemanticAction::Increment));
    frame();
    frame();
    assert(std::abs(slider.value - .6f) < .001f);
    assert(bridge.request_action(slider.id, ui2::SemanticAction::SetNumber, "0.9"));
    frame();
    frame();
    assert(std::abs(slider.value - .9f) < .001f);
    assert(!bridge.request_action(slider.id, ui2::SemanticAction::SetNumber, "loud")); // not a number
    assert(bridge.request_action(slider.id, ui2::SemanticAction::SetNumber, "0.1234567"));
    frame();
    frame();
    assert(std::abs(slider.value - 0.1234567f) < 1e-7f); // the number arrives whole, not as 6-decimal text
    assert(!bridge.snapshot().at(0).focused && bridge.snapshot().at(1).focused); // as end() left the focus
    ui.set_reduced_motion(true);
    assert(ui.reduced_motion());
    ui2::InteractiveSurfaceStyle style;
    style.normal.fill = colors::black;
    style.hovered.fill = colors::white;
    const auto result = ui.resolve_animated(id, style, {.hot = true}, false, true);
    assert(result.fill == colors::white);
}
