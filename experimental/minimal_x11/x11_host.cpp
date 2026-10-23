#include "x11_host.hpp"
#include "pixel_pack.hpp"
#include <optional>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <limits>
#include <stdexcept>

namespace kin::experimental {
struct X11Host::State {
    Display* display = nullptr;
    ::Window window = 0;
    GC gc = nullptr;
    Atom wm_protocols = 0, wm_delete = 0;
    XImage* image = nullptr;
    Vec2i size;
    DesktopInput input;
    std::array<bool, 256> keys{};
    std::optional<PixelPacker> packer;
    ~State() {
        if (image) XDestroyImage(image);
        if (gc) XFreeGC(display, gc);
        if (window) XDestroyWindow(display, window);
        if (display) XCloseDisplay(display);
    }
    void resize_image() {
        if (image && image->width == size.x && image->height == size.y) return;
        if (image) { XDestroyImage(image); image = nullptr; }
        const auto screen = DefaultScreen(display);
        auto* visual = DefaultVisual(display, screen);
        if (visual->c_class != TrueColor) throw std::runtime_error("X11 host requires a TrueColor visual");
        image = XCreateImage(display, visual, DefaultDepth(display, screen), ZPixmap,
                             0, nullptr, size.x, size.y, 32, 0);
        if (!image) throw std::runtime_error("XCreateImage failed");
        packer.emplace(PixelPacker::Format{u32(image->red_mask), u32(image->green_mask), u32(image->blue_mask),
                                          image->bits_per_pixel, image->byte_order == LSBFirst});
        const auto bytes = std::size_t(image->bytes_per_line) * size.y;
        image->data = static_cast<char*>(std::calloc(bytes, 1));
        if (!image->data) throw std::bad_alloc();
    }
};
X11Host::X11Host(Vec2i size, bool headless, bool hidden) : _state(std::make_unique<State>()) {
    if (size.x <= 0 || size.y <= 0 || size.x > 16384 || size.y > 16384)
        throw std::invalid_argument("X11 host dimensions must be 1..16384");
    auto& s = *_state;
    s.size = size;
    if (headless) return;
    s.display = XOpenDisplay(nullptr);
    if (!s.display) throw std::runtime_error("Cannot open X11 display (use --headless for offline rendering)");
    const auto screen = DefaultScreen(s.display);
    s.window = XCreateSimpleWindow(s.display, RootWindow(s.display, screen),
                                  0, 0, size.x, size.y, 0, 0, 0);
    XStoreName(s.display, s.window, "Kin minimal X11 hex demo");
    XSelectInput(s.display, s.window, StructureNotifyMask | KeyPressMask | KeyReleaseMask |
                 ButtonPressMask | ButtonReleaseMask | PointerMotionMask | FocusChangeMask | ExposureMask);
    s.wm_protocols = XInternAtom(s.display, "WM_PROTOCOLS", False);
    s.wm_delete = XInternAtom(s.display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(s.display, s.window, &s.wm_delete, 1);
    s.gc = XCreateGC(s.display, s.window, 0, nullptr);
    if (!hidden) XMapWindow(s.display, s.window);
    XFlush(s.display);
}
X11Host::~X11Host() = default;
Vec2i X11Host::pixel_size() const { return _state->size; }
unsigned long X11Host::native_window() const { return _state->window; }
void X11Host::synchronize() { if (_state->display) XSync(_state->display, False); }
DesktopInput X11Host::poll() {
    auto& s = *_state;
    const auto mouse = s.input.mouse;
    const bool quit = s.input.quit;
    s.input = {.quit = quit, .mouse = mouse};
    while (s.display && XPending(s.display)) {
        XEvent event;
        XNextEvent(s.display, &event);
        if (event.xany.window != s.window) continue;
        switch (event.type) {
        case ClientMessage:
            if (event.xclient.message_type == s.wm_protocols && event.xclient.format == 32 &&
                Atom(event.xclient.data.l[0]) == s.wm_delete) s.input.quit = true;
            break;
        case DestroyNotify: s.window = 0; s.input.quit = true; break;
        case ConfigureNotify:
            s.size = {std::clamp(event.xconfigure.width, 1, 16384), std::clamp(event.xconfigure.height, 1, 16384)};
            break;
        case FocusOut: s.keys.fill(false); break;
        case MotionNotify: s.input.mouse = {float(event.xmotion.x), float(event.xmotion.y)}; break;
        case ButtonPress:
            s.input.mouse = {float(event.xbutton.x), float(event.xbutton.y)};
            s.input.left |= event.xbutton.button == Button1;
            s.input.right |= event.xbutton.button == Button3;
            break;
        case KeyRelease:
            // Classic X autorepeat is a release/press pair at the same time.
            if (XPending(s.display)) {
                XEvent next;
                XPeekEvent(s.display, &next);
                if (next.type == KeyPress && next.xkey.window == s.window &&
                    next.xkey.keycode == event.xkey.keycode && next.xkey.time == event.xkey.time) break;
            }
            if (event.xkey.keycode < s.keys.size()) s.keys[event.xkey.keycode] = false;
            break;
        case KeyPress:
            if (event.xkey.keycode >= s.keys.size() || s.keys[event.xkey.keycode]) break;
            s.keys[event.xkey.keycode] = true;
            switch (XLookupKeysym(&event.xkey, 0)) {
            case XK_Escape: s.input.quit = true; break;
            case XK_Tab: s.input.orientation = true; break;
            case XK_o: s.input.offset = true; break;
            case XK_r: s.input.reset = true; break;
            }
            break;
        }
    }
    return s.input;
}
void X11Host::present_rgba(std::span<const u8> pixels, Vec2i size) {
    auto& s = *_state;
    if (size.x <= 0 || size.y <= 0 || std::size_t(size.x) * size.y > pixels.size() / 4)
        throw std::invalid_argument("X11 host: invalid RGBA buffer");
    if (!s.display || !s.window) return;
    s.resize_image();
    s.packer->pack(pixels, size,
        {reinterpret_cast<u8*>(s.image->data), std::size_t(s.image->bytes_per_line) * s.size.y},
        s.size, s.image->bytes_per_line);
    XPutImage(s.display, s.window, s.gc, s.image, 0, 0, 0, 0, s.size.x, s.size.y);
    XFlush(s.display);
}
}
