#include "x11_host.hpp"
#include "pixel_pack.hpp"
#include "compact_renderer2d_backend.hpp"
#include "hex_game.hpp"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <cassert>
#include <bit>
#include <fstream>
#include <sstream>
#include <iostream>
#include <thread>
#include <chrono>

static void pixel_pack_tests() {
    using kin::experimental::PixelPacker;
    const PixelPacker::Format formats[] = {
        {0xff0000, 0xff00, 0xff, 32, true}, {0xff0000, 0xff00, 0xff, 32, false},
        {0xff, 0xff00, 0xff0000, 32, true}, {0xff, 0xff00, 0xff0000, 32, false},
        {0xff0000, 0xff00, 0xff, 24, true}, {0xff0000, 0xff00, 0xff, 24, false},
        {0xf800, 0x7e0, 0x1f, 16, true}, {0xf800, 0x7e0, 0x1f, 16, false},
        {0x3ff00000, 0xffc00, 0x3ff, 32, true}, {0x3ff00000, 0xffc00, 0x3ff, 32, false},
    };
    constexpr kin::Vec2i source_size{17, 16};
    std::vector<kin::u8> source(source_size.x * source_size.y * 4 + 2, 0xa5);
    for (int i = 0; i < source_size.x * source_size.y; ++i) {
        source[1 + i * 4] = kin::u8(i);
        source[2 + i * 4] = kin::u8(i * 37);
        source[3 + i * 4] = kin::u8((i * 83) ^ 63);
        source[4 + i * 4] = kin::u8(i * 13); // alpha must not leak into unused bits
    }
    for (auto format : formats) for (auto output : {kin::Vec2i{19, 18}, kin::Vec2i{13, 11}}) {
        const int bytes = format.bits / 8, pitch = output.x * bytes + 3;
        std::vector<kin::u8> actual(pitch * output.y + 2, 0xa5), expected = actual;
        PixelPacker packer(format);
        packer.pack({source.data() + 1, source.size() - 2}, source_size,
                    {actual.data() + 1, actual.size() - 2}, output, pitch);
        for (int y = 0; y < std::min(output.y, source_size.y); ++y)
            for (int x = 0; x < std::min(output.x, source_size.x); ++x) {
                const auto* p = source.data() + 1 + (y * source_size.x + x) * 4;
                kin::u32 value = 0;
                const kin::u32 masks[] = {format.red, format.green, format.blue};
                for (int c = 0; c < 3; ++c) {
                    const auto shift = std::countr_zero(masks[c]);
                    value |= kin::u32((kin::u64(p[c]) * (masks[c] >> shift) + 127) / 255) << shift;
                }
                for (int b = 0; b < bytes; ++b)
                    expected[1 + y * pitch + x * bytes + b] = kin::u8(value >>
                        ((format.lsb_first ? b : bytes - b - 1) * 8));
            }
        assert(actual == expected); // includes guards, untouched pixels and pitch padding
        bool rejected = false;
        try { packer.pack({}, source_size, actual, output, pitch); }
        catch (const std::invalid_argument&) { rejected = true; }
        assert(rejected);
    }
}

static void native_test() {
    kin::experimental::X11Host host({80, 48}, false, true);
    host.synchronize();
    auto* display = XOpenDisplay(nullptr);
    assert(display);
    const auto window = host.native_window();
    // Bypass WM sizing policy so the resize assertion is deterministic on
    // tiling compositors and Xvfb alike; still use the real X server/pixels.
    XSetWindowAttributes settings{};
    settings.override_redirect = True;
    XChangeWindowAttributes(display, window, CWOverrideRedirect, &settings);
    XMapWindow(display, window);
    host.poll();
    XSync(display, False);
    host.synchronize();
    XWindowAttributes attributes{};
    for (int attempt = 0; attempt < 100; ++attempt) {
        XGetWindowAttributes(display, window, &attributes);
        if (attributes.map_state == IsViewable) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        host.synchronize();
        host.poll();
    }
    assert(attributes.map_state == IsViewable);
    std::vector<kin::u8> pixels(80 * 48 * 4);
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i] = 220; pixels[i + 1] = 80; pixels[i + 2] = 30; pixels[i + 3] = 255;
    }
    host.present_rgba(pixels, {80, 48});
    host.synchronize();
    auto* image = XGetImage(display, window, 0, 0, 1, 1, AllPlanes, ZPixmap);
    assert(image);
    const auto value = XGetPixel(image, 0, 0);
    const auto channel = [&](unsigned long mask) {
        return ((value & mask) >> std::countr_zero(mask)) * 255 / (mask >> std::countr_zero(mask));
    };
    assert(std::abs(int(channel(image->red_mask)) - 220) <= 4);
    assert(std::abs(int(channel(image->green_mask)) - 80) <= 4);
    assert(std::abs(int(channel(image->blue_mask)) - 30) <= 4);
    XDestroyImage(image);
    auto send_key = [&](int type, unsigned long time) {
        XEvent event{};
        event.xkey = {.type = type, .display = display, .window = window, .time = time,
                      .keycode = XKeysymToKeycode(display, XK_Tab), .same_screen = True};
        XSendEvent(display, window, False, type == KeyPress ? KeyPressMask : KeyReleaseMask, &event);
        XSync(display, False);
        host.synchronize();
    };
    send_key(KeyPress, 1);
    assert(host.poll().orientation);
    assert(!host.poll().orientation);
    send_key(KeyPress, 2); // held key: no second edge
    assert(!host.poll().orientation);
    send_key(KeyRelease, 3);
    send_key(KeyPress, 3); // classic repeat pair
    assert(!host.poll().orientation);
    send_key(KeyRelease, 4);
    host.poll();
    send_key(KeyPress, 5);
    assert(host.poll().orientation);
    XEvent event{};
    event.xbutton = {.type = ButtonPress, .display = display, .window = window,
                     .x = 32, .y = 24, .button = Button3, .same_screen = True};
    XSendEvent(display, window, False, ButtonPressMask, &event);
    XSync(display, False);
    host.synchronize();
    auto input = host.poll();
    assert(input.right && !input.left && (input.mouse == kin::Vec2f{32, 24}));
    assert(!host.poll().right);
    XResizeWindow(display, window, 120, 72);
    XSync(display, False);
    host.synchronize();
    host.poll();
    for (int attempt = 0; attempt < 100 && host.pixel_size() != kin::Vec2i{120, 72}; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        host.synchronize();
        host.poll();
    }
    assert((host.pixel_size() == kin::Vec2i{120, 72}));
    pixels.assign(120 * 72 * 4, 255);
    host.present_rgba(pixels, {120, 72});
    host.synchronize();
    image = XGetImage(display, window, 119, 71, 1, 1, AllPlanes, ZPixmap);
    assert(image);
    assert((XGetPixel(image, 0, 0) & (image->red_mask | image->green_mask | image->blue_mask)) ==
           (image->red_mask | image->green_mask | image->blue_mask));
    XDestroyImage(image);
    kin::Renderer2D renderer(std::make_unique<kin::CompactRenderer2DBackend>(host));
    renderer.set_logical_size(960, 600);
    demo::HexGame game;
    game.render(renderer);
    renderer.present();
    host.synchronize();
    event = {};
    event.xclient.type = ClientMessage;
    event.xclient.window = window;
    event.xclient.message_type = XInternAtom(display, "WM_PROTOCOLS", False);
    event.xclient.format = 32;
    event.xclient.data.l[0] = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSendEvent(display, window, False, NoEventMask, &event);
    XSync(display, False);
    host.synchronize();
    assert(host.poll().quit);
    XCloseDisplay(display);
}

int main(int argc, char**) {
    pixel_pack_tests();
    if (argc > 1) { native_test(); return 0; }
    kin::experimental::X11Host host({960, 600}, true);
    kin::Renderer2D renderer(std::make_unique<kin::CompactRenderer2DBackend>(host));
    renderer.set_logical_size(960, 600);
    demo::HexGame game;
    auto report = [&] {
        std::ostringstream out;
        kin::JsonWriter json(out);
        json.begin_object(); game.write_report(json); json.end_object();
        return out.str();
    };
    assert(report().find("\"path_cells\": 17") != std::string::npos);
    assert(report().find("\"path_cost\": 170") != std::string::npos);
    assert(report().find("\"reachable_cells\": 28") != std::string::npos);
    for (int layout = 0; layout < 4; ++layout) {
        assert(game.update({.reset = true}));
        assert(game.update({.orientation = bool(layout & 1), .offset = bool(layout & 2)}));
        const auto state = report();
        assert(state.find(layout & 1 ? "flat-top" : "pointy-top") != std::string::npos);
        assert(state.find(layout & 2 ? "even" : "odd") != std::string::npos);
        game.render(renderer);
        renderer.present();
        std::vector<kin::u8> pixels;
        kin::Vec2i size;
        assert(renderer.read_rgba({0, 0, 960, 600}, pixels, size));
        if (const char* capture = std::getenv("KIN_HEX_TEST_CAPTURE")) {
            std::ofstream out(std::string(capture) + "-" + std::to_string(layout) + ".ppm", std::ios::binary);
            out << "P6\n960 600\n255\n";
            for (std::size_t p = 0; p < pixels.size(); p += 4) out.write(reinterpret_cast<const char*>(pixels.data() + p), 3);
        }
        bool picked = false;
        for (int y = 95; y < 575 && !picked; ++y) for (int x = 25; x < 935; ++x) {
            const auto p = (y * 960 + x) * 4;
            if (pixels[p] == 236 && pixels[p + 1] == 92 && pixels[p + 2] == 84) {
                game.update({.left = true, .mouse = {float(x), float(y)}});
                assert(report().find("\"path_cells\": 1,") != std::string::npos);
                picked = true; break;
            }
        }
        assert(picked);
    }
    assert(!game.update({.quit = true}));
}
