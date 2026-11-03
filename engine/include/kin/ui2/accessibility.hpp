#pragma once

#include <kin/ui2/semantics.hpp>
#include <memory>
#include <vector>

namespace kin {
class Window;
class Renderer2D;
} // namespace kin
namespace kin::ui2 {
class Context;

// One bridge per window, destroyed before its Window and Renderer2D. Call
// update() after Context::end(). Native actions are queued across threads and
// applied to the next UI frame. No native callback accesses the UI context.
class Accessibility {
public:
    Accessibility(Window& window, Renderer2D& renderer, std::string title = {});
    ~Accessibility();
    Accessibility(const Accessibility&) = delete;
    Accessibility& operator=(const Accessibility&) = delete;
    void update(Context& context);
    void announce(std::string text, bool assertive = false);
    // Thread-safe entry point for alternative assistive input adapters. False
    // for unavailable/disabled targets or a full queue. Revalidated on dispatch.
    bool request_action(Id id, SemanticAction action, std::string text = {});
    std::vector<SemanticElement> snapshot() const;
    bool native_available() const;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
} // namespace kin::ui2
