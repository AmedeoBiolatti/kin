// Windows in points where SDL's units are not (Android: pixels; here
// KIN_WINDOW_POINT_SCALE=2, which ctest sets): the window's size, the mouse,
// the fingers and the safe area in points, its pixels as they are.
#include <kin/platform/app.hpp>
#include <kin/platform/input.hpp>
#include <kin/platform/window.hpp>

#include <SDL3/SDL.h>

#include <cassert>
#include <cmath>
#include <cstdlib>
#include <string_view>

namespace kin {
struct InputFrameTestHook {
    static void event(Input& input, const SDL_Event& event) { input.process_native_event(&event); }
};
} // namespace kin

int main() {
    using namespace kin;
    const char* forced = std::getenv("KIN_WINDOW_POINT_SCALE");
    assert(forced && std::string_view(forced) == "2");
    App app{{.mode = AppMode::Headless}};
    Window& window = app.create_window({.title = "points", .width = 320, .height = 180, .hidden = true});
    assert((window.size() == Vec2i{320, 180}));
    assert((window.pixel_size() == Vec2i{640, 360})); // two pixels a point
    assert(window.display_scale() == 2.0f);
    const Rectf safe = window.safe_area();
    assert(safe.x == 0.0f && safe.y == 0.0f && safe.w == 320.0f && safe.h == 180.0f);
    window.set_size({200, 100});
    window.sync();
    assert((window.size() == Vec2i{200, 100}) && (window.pixel_size() == Vec2i{400, 200}));
    const Vec2i display = primary_display_size();
    assert(display.x > 0 && display.y > 0);

    Input input;
    SDL_Event motion{};
    motion.type = SDL_EVENT_MOUSE_MOTION;
    motion.motion.windowID = static_cast<SDL_WindowID>(window.id());
    motion.motion.x = 100.0f;
    motion.motion.y = 50.0f;
    motion.motion.xrel = 10.0f;
    InputFrameTestHook::event(input, motion);
    assert((input.mouse_pos() == Vec2f{50.0f, 25.0f}) && (input.mouse_motion() == Vec2f{5.0f, 0.0f}));
    SDL_Event press{};
    press.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    press.button.windowID = motion.motion.windowID;
    press.button.button = SDL_BUTTON_LEFT;
    press.button.x = 40.0f;
    press.button.y = 20.0f;
    InputFrameTestHook::event(input, press);
    assert((input.mouse_pos() == Vec2f{20.0f, 10.0f}));
    SDL_Event finger{};
    finger.type = SDL_EVENT_FINGER_DOWN;
    finger.tfinger.windowID = motion.motion.windowID;
    finger.tfinger.fingerID = 1;
    finger.tfinger.x = 0.5f; // of the window
    finger.tfinger.y = 0.25f;
    InputFrameTestHook::event(input, finger);
    assert(input.touches().size() == 1 && (input.touches()[0].pos == Vec2f{100.0f, 25.0f}));
    return 0;
}
