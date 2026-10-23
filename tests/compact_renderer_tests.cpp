#include <kin/platform/app.hpp>
#include <kin/renderer/renderer2d.hpp>

#include <SDL3/SDL.h>

#include <array>
#include <cassert>
#include <cmath>
#include <limits>
#include <stdexcept>

int main() {
    kin::App app{{.mode = kin::AppMode::Headless}};
    auto& window = app.create_window({.width = 32, .height = 24, .hidden = true});
    kin::Texture surviving;
    {
        kin::Renderer2D r{window};
        assert(r.backend_name() == "compact");
        const auto caps = r.capabilities();
        assert(caps.immediate_2d && caps.text);
        assert(!caps.render_targets && !caps.transforms && !caps.shapes && !caps.blend_modes);
        assert(!r.create_render_target({4, 4}));
        const auto pixel = [&](int x, int y) {
            std::vector<kin::u8> bytes;
            kin::Vec2i size;
            assert(r.read_rgba({float(x), float(y), 1, 1}, bytes, size));
            assert((size == kin::Vec2i{1, 1}));
            return kin::Color::rgba(bytes[0], bytes[1], bytes[2], bytes[3]);
        };
        // Clear is replacement regardless of blend mode, viewport or clip.
        // Compare the entire physical framebuffer, including fractional and
        // integer letterbox edges, native-coordinate scopes and alpha bytes.
        const auto check_clear = [&](kin::Vec2i logical, bool integer, kin::Rectf colored, bool native) {
            if (integer) r.set_integer_logical_size(logical.x, logical.y);
            else r.set_logical_size(logical.x, logical.y);
            r.set_blend_mode(kin::BlendMode::Alpha);
            r.push_viewport({2, 2, 3, 3});
            r.push_clip({0, 0, 1, 1});
            std::optional<kin::Renderer2D::NativeCoordinateGuard> native_guard;
            if (native) native_guard.emplace(r.scoped_native_coordinates());
            const kin::Color value{17, 51, 93, 0};
            r.clear(value);
            std::vector<kin::u8> bytes;
            kin::Vec2i size;
            // Read native coordinates so bars are included in the comparison.
            if (!native) native_guard.emplace(r.scoped_native_coordinates());
            assert(r.read_rgba({0, 0, 32, 24}, bytes, size));
            assert((size == kin::Vec2i{32, 24}));
            for (int y = 0; y < 24; ++y) for (int x = 0; x < 32; ++x) {
                const bool inside = x >= colored.x && x < colored.x + colored.w &&
                                    y >= colored.y && y < colored.y + colored.h;
                const auto expected = inside ? value : kin::colors::black;
                const auto at = (y * 32 + x) * 4;
                assert(bytes[at] == expected.r && bytes[at + 1] == expected.g &&
                       bytes[at + 2] == expected.b && bytes[at + 3] == expected.a);
            }
            native_guard.reset();
            r.pop_clip();
            r.pop_viewport();
            // Clearing must not leave replacement blending enabled.
            r.set_logical_size(32, 24);
            r.clear(kin::colors::black);
            r.fill_rect({0, 0, 1, 1}, kin::Color{255, 0, 0, 128});
            assert(pixel(0, 0).r == 128);
        };
        check_clear({0, 0}, false, {0, 0, 32, 24}, false);
        check_clear({32, 24}, false, {0, 0, 32, 24}, false);
        check_clear({16, 8}, false, {0, 4, 32, 16}, false);
        check_clear({8, 16}, false, {10, 0, 12, 24}, false);
        check_clear({16, 7}, false, {0, 5, 32, 14}, false);
        check_clear({9, 17}, false, {10, 0, 12, 24}, false);
        check_clear({9, 17}, true, {11, 3, 9, 17}, false);
        check_clear({16, 8}, false, {0, 0, 32, 24}, true);
        r.clear(kin::Color::rgb(10, 20, 30));
        r.fill_rect({2, 3, 4, 2}, kin::Color::rgba(255, 0, 0, 128));
        auto c = pixel(2, 3);
        assert(c.r == 133 && c.g == 10 && c.b == 15 && c.a == 255);
        assert(pixel(1, 3).r == 10 && pixel(6, 3).r == 10);
        r.set_blend_mode(kin::BlendMode::Replace);
        r.fill_rect({0, 0, 1, 1}, kin::Color::rgba(2, 4, 6, 0));
        assert(pixel(0, 0).r == 2 && pixel(0, 0).a == 0);
        r.set_blend_mode(kin::BlendMode::Alpha);

        const std::array<kin::u8, 8> texels{255, 0, 0, 255, 0, 0, 255, 255};
        surviving = r.create_texture_from_rgba(texels.data(), {2, 1});
        r.draw_texture(surviving, {0, 0, 4, 1});
        assert(pixel(1, 0).r == 255 && pixel(2, 0).b == 255);
        r.set_scale_mode(surviving, kin::ScaleMode::Linear);
        r.draw_texture(surviving, {0, 1, 4, 1});
        c = pixel(1, 1);
        assert(c.r == 191 && c.b == 64);
        r.set_scale_mode(surviving, kin::ScaleMode::Nearest);
        r.draw_texture(surviving, {2, 0, -2, 1}, {0, 2, 4, 1}, kin::colors::white);
        assert(pixel(0, 2).b == 255 && pixel(3, 2).r == 255);
        r.draw_texture(surviving, {0, 0, 1, 1}, {0, 3, 1, 1}, kin::Color::rgb(128, 255, 255));
        assert(pixel(0, 3).r == 128);
        const std::array<kin::u8, 8> mask{255, 255, 255, 128, 255, 255, 255, 0};
        const auto coverage = r.create_texture_from_rgba(mask.data(), {2, 1});
        r.clear(kin::colors::black);
        r.draw_texture(coverage, {0, 0, 2, 1}, {0, 0, 2, 1}, kin::Color::rgb(128, 0, 0));
        assert(pixel(0, 0).r == 64 && pixel(1, 0).r == 0);
        r.set_blend_mode(kin::BlendMode::Replace);
        r.draw_texture(coverage, {0, 0, 2, 1}, {0, 1, 2, 1}, kin::Color::rgb(128, 0, 0));
        assert(pixel(0, 1).r == 128 && pixel(0, 1).a == 128);
        assert(pixel(1, 1).r == 128 && pixel(1, 1).a == 0);
        r.set_blend_mode(kin::BlendMode::Alpha);

        r.clear(kin::colors::black);
        r.push_viewport({2, 2, 8, 8});
        r.push_clip({1, 1, 2, 2});
        r.fill_rect({0, 0, 20, 20}, kin::colors::white);
        r.pop_clip();
        r.pop_viewport();
        assert(pixel(3, 3).r == 255 && pixel(4, 4).r == 255);
        assert(pixel(2, 3).r == 0 && pixel(5, 4).r == 0);
        r.push_clip({-100, -100, 1, 1});
        r.fill_rect({0, 0, 32, 24}, kin::colors::white);
        r.pop_clip();
        r.fill_rect({1e30f, 1e30f, 1, 1}, kin::colors::white);
        r.fill_rect({std::numeric_limits<float>::quiet_NaN(), 0, 1, 1}, kin::colors::white);
        assert(pixel(0, 0).r == 0);
        r.draw_line({-100, 10}, {100, 10}, kin::colors::white);
        assert(pixel(0, 10).r == 255 && pixel(31, 10).r == 255);

        const auto rejects = [](auto action) {
            bool rejected = false;
            try { action(); } catch (const std::invalid_argument&) { rejected = true; }
            assert(rejected);
        };
        rejects([&] { r.set_blend_mode(kin::BlendMode::Additive); });
        rejects([&] { r.push_transform(kin::Affine2::scaling({2, 2})); });
        // A refused transform must leave both facade state and stack usable.
        assert(r.transform().is_identity());
        rejects([&] { r.draw_texture(surviving, {0, 0, 2, 1}, {0, 0, 4, 4}, kin::colors::white, 30, {0.5f, 0.5f}); });

        r.set_logical_size(16, 8);
        assert((r.window_to_logical({0, 4}) == kin::Vec2f{0, 0}));
        assert((r.logical_to_window({16, 8}) == kin::Vec2f{32, 20}));
        r.clear(kin::Color::rgb(17, 51, 93));
        r.present();
        auto* surface = SDL_GetWindowSurface(SDL_GetWindowFromID(window.id()));
        kin::u8 red, green, blue, alpha;
        assert(SDL_ReadSurfacePixel(surface, 4, 6, &red, &green, &blue, &alpha));
        assert(red == 17 && green == 51 && blue == 93);
        assert(SDL_ReadSurfacePixel(surface, 4, 0, &red, &green, &blue, &alpha));
        assert(red == 0 && green == 0 && blue == 0);
        window.set_size(40, 32);
        assert((r.output_size() == kin::Vec2i{40, 32}));
        assert((r.window_to_logical({0, 6}) == kin::Vec2f{0, 0}));
        r.set_integer_logical_size(16, 8);
        assert((r.logical_to_window({0, 0}) == kin::Vec2f{4, 8}));
        r.push_viewport({2, 1, 4, 4});
        assert((r.logical_to_window({0, 0}) == kin::Vec2f{8, 10}));
        assert((r.window_to_logical({8, 10}) == kin::Vec2f{0, 0}));
        r.pop_viewport();
        r.clear(kin::colors::black);
        r.fill_rect({0, 0, 16, 8}, kin::colors::white);
        {
            auto native = r.scoped_native_coordinates();
            r.fill_rect({0, 0, 1, 1}, kin::Color::rgb(255, 0, 0));
            assert(pixel(0, 0).r == 255);
        }
        r.present();
    }
    // Texture storage is independent of the renderer/window surface lifetime.
    kin::Renderer2D next{window};
    next.clear(kin::colors::black);
    next.draw_texture(surviving, {0, 0, 2, 1});
    next.present();
}
