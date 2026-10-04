#pragma once

// A render target drawn again only when what it shows changes: a minimap, an
// icon, a panel, a static layer of the world. Its key says what it shows (the
// camera, a version, the values it is made from); while the key and size stay
// the same, the last drawing is reused and nothing is drawn into it.
//
//     if (minimap.stale(renderer, size, kin::cache_key(camera.x, camera.y, map.version()))) {
//         const auto bind = renderer.scoped_render_target(minimap.target());
//         renderer.clear(kin::Color::rgba(0, 0, 0, 0));
//         draw_minimap(renderer);
//     }
//     renderer.draw_texture(minimap.texture(), rect);

#include <kin/core/types.hpp>
#include <kin/renderer/render_target.hpp>

#include <cstring>
#include <type_traits>

namespace kin {

class Renderer2D;

// A key from plain values (numbers, enums, plain structs): FNV-1a of their
// bytes, in order.
template<typename... Values>
u64 cache_key(const Values&... values) {
    static_assert((std::is_trivially_copyable_v<Values> && ...), "cache_key takes plain values");
    u64 key = 1469598103934665603ull;
    const auto add = [&](const auto& value) {
        unsigned char bytes[sizeof(value)];
        std::memcpy(bytes, &value, sizeof(value));
        for (const unsigned char b : bytes) {
            key = (key ^ b) * 1099511628211ull;
        }
    };
    (add(values), ...);
    return key;
}

class CachedTarget {
public:
    explicit CachedTarget(ScaleMode mode = ScaleMode::Linear) : _mode(mode) {}

    // Whether its content must be drawn now: the first time, when `size` or
    // `key` changed, or after invalidate(). The target is (re)made at `size`
    // when needed. False on a backend without render targets.
    bool stale(Renderer2D& renderer, Vec2i size, u64 key);
    // Draw it again at the next stale(), whatever the key.
    void invalidate() { _valid = false; }

    const RenderTarget& target() const { return _target; }
    const Texture& texture() const { return _target.texture(); }
    bool valid() const { return _target.valid(); }

    // How often stale() found it current, and how often it did not.
    u64 reuses() const { return _reuses; }
    u64 redraws() const { return _redraws; }

private:
    ScaleMode _mode;
    RenderTarget _target;
    Vec2i _size{};
    u64 _key = 0;
    bool _valid = false;
    u64 _reuses = 0;
    u64 _redraws = 0;
};

} // namespace kin
