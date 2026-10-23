#include <SDL3/SDL.h>

#include <cassert>
#include <string_view>

int main() {
    assert(SDL_Init(SDL_INIT_VIDEO));
    // Disabled gamepad support must fail explicitly; keyboard/mouse video
    // initialization above must keep working without a joystick subsystem.
    const bool joystick = SDL_InitSubSystem(SDL_INIT_JOYSTICK);
    assert(joystick == bool(KIN_TEST_SDL_GAMEPAD));
    if (!joystick) {
        assert(std::string_view{SDL_GetError()}.size() > 0);
        assert(SDL_hid_enumerate(0, 0) == nullptr);
    }

    bool x11 = false;
    bool wayland = false;
    for (int i = 0; i < SDL_GetNumVideoDrivers(); ++i) {
        const std::string_view driver{SDL_GetVideoDriver(i)};
        x11 |= driver == "x11";
        wayland |= driver == "wayland";
    }
    // Check the linked library, rather than SDL's requested cache options.
    assert(x11 == bool(KIN_TEST_SDL_HAS_X11));
    assert(wayland == bool(KIN_TEST_SDL_HAS_WAYLAND));
    SDL_Quit();
}
