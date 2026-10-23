#pragma once
#include <kin/platform/framebuffer_host.hpp>
#include <memory>

namespace kin::experimental {
struct DesktopInput {
    bool quit = false, reset = false, orientation = false, offset = false;
    bool left = false, right = false;
    Vec2f mouse{};
};
// A deliberately narrow Linux host: one TrueColor window, mouse, four keys,
// and copied RGBA framebuffer presentation. No SDL or full App compatibility.
class X11Host final : public FramebufferHost {
public:
    X11Host(Vec2i size, bool headless = false, bool hidden = false);
    ~X11Host() override;
    X11Host(const X11Host&) = delete;
    X11Host& operator=(const X11Host&) = delete;
    DesktopInput poll();
    Vec2i pixel_size() const override;
    Vec2i window_size() const override { return pixel_size(); }
    void present_rgba(std::span<const u8> pixels, Vec2i size) override;
    unsigned long native_window() const;
    // Wait for server processing (diagnostics/tests, not the frame loop).
    void synchronize();
private:
    struct State;
    std::unique_ptr<State> _state;
};
}
