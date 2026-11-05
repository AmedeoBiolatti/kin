#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>
#include <kin/platform/app.hpp>
#include <kin/renderer/build.hpp>
#include <kin/renderer/normal_lighting2d.hpp>

using namespace kin;
int main() {
    App app{{.mode = AppMode::Headless}};
    auto& window = app.create_window({.width = 32, .height = 32, .hidden = true});
    Renderer2D renderer{make_render_backend(window, false, false)};
    if (!normal_lighting2d_gpu_supported(renderer.capabilities()))
        return 77;
    Image albedo{{8, 8}, std::vector<u8>(8 * 8 * 4)}, normal = albedo, specular = albedo, cookie = albedo;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            const auto p = (y * 8 + x) * 4;
            albedo.rgba[p] = static_cast<u8>(70 + x * 16);
            albedo.rgba[p + 1] = static_cast<u8>(30 + y * 20);
            albedo.rgba[p + 2] = 90;
            albedo.rgba[p + 3] = 255;
            normal.rgba[p] = static_cast<u8>(64 + x * 16);
            normal.rgba[p + 1] = static_cast<u8>(72 + y * 16);
            normal.rgba[p + 2] = 220;
            normal.rgba[p + 3] = 255;
            specular.rgba[p] = static_cast<u8>(x * 32);
            specular.rgba[p + 3] = 255;
            cookie.rgba[p + 3] = static_cast<u8>(y * 32);
        }
    SurfaceMaterial2D material{.albedo = std::make_shared<Image>(albedo),
                               .normal = std::make_shared<Image>(normal),
                               .specular = std::make_shared<Image>(specular),
                               .normal_strength = .8f,
                               .specular_strength = .2f,
                               .shininess = 12,
                               .tint = {210, 180, 250, 255}};
    const std::array<LightOccluder2D, 1> blockers{{{.points = {{7, 7}, {8, 7}, {8, 21}, {7, 21}}}}};
    const std::array<SurfaceLight2D, 3> lights{{{.position = {3, 14},
                                                 .height = 12,
                                                 .radius = 32,
                                                 .intensity = 1.2f,
                                                 .color = {255, 160, 90, 255},
                                                 .mask = 1,
                                                 .shadows = true,
                                                 .shadow_softness = 2},
                                                {.height = 1,
                                                 .intensity = .4f,
                                                 .color = {140, 190, 255, 255},
                                                 .mask = static_cast<i32>(0x80000000u),
                                                 .directional = true,
                                                 .direction = {.4f, -.3f}},
                                                {.position = {16, 10},
                                                 .height = 8,
                                                 .radius = 12,
                                                 .intensity = .6f,
                                                 .shape = std::make_shared<Image>(cookie),
                                                 .rotation = 30}}};
    for (auto space : {ColorSpace::Gamma, ColorSpace::Linear}) {
        assert(renderer.set_color_space(space));
        for (int variant = 0; variant < 9; ++variant) {
            SurfaceGeometry2D geometry{.source = {1, 1, 6, 6},
                                       .dest = {12, 12, 6, 6},
                                       .rotation = variant == 4 ? 90.0f : 0.0f,
                                       .flip = variant == 1   ? Flip::X
                                               : variant == 2 ? Flip::Y
                                                              : Flip::None};
            material.light_mask = variant == 3 ? static_cast<i32>(0x80000000u) : -1;
            material.normal = variant == 6 ? nullptr : std::make_shared<Image>(normal);
            material.specular = variant == 6 ? nullptr : std::make_shared<Image>(specular);
            material.tint.a = variant == 7 ? 128 : 255;
            const auto shadows =
                variant == 5 ? std::span<const LightOccluder2D>{} : std::span<const LightOccluder2D>{blockers};
            // Variant 8: a light that can't be used, skipped by both.
            std::vector<SurfaceLight2D> list(lights.begin(), lights.end());
            if (variant == 8) {
                list.insert(list.begin(), SurfaceLight2D{.height = std::numeric_limits<f32>::quiet_NaN()});
            }
            const auto image = shade_normal_sprite2d(material, geometry, {40, 50, 70, 255}, list, shadows);
            assert(image.valid());
            auto texture = renderer.create_texture_from_rgba(image.rgba.data(), image.size);
            renderer.clear(colors::black);
            renderer.draw_texture(texture, {}, geometry.dest, colors::white, geometry.rotation, geometry.pivot,
                                  geometry.flip);
            std::vector<u8> expected, actual;
            Vec2i size;
            assert(renderer.read_rgba({0, 0, 32, 32}, expected, size));
            renderer.clear(colors::black);
            NormalLightingGpuCache2D cache;
            const SurfaceLightBuffer2D packed = pack_surface_lights2d(renderer, list, shadows);
            assert(packed.buffer.valid() && packed.lights == lights.size());
            assert(draw_normal_sprite2d_gpu(renderer, material, geometry, {40, 50, 70, 255}, packed, cache) &&
                   cache.used);
            assert(renderer.read_rgba({0, 0, 32, 32}, actual, size));
            int maximum = 0;
            for (std::size_t i = 0; i < actual.size(); ++i)
                maximum = std::max(maximum, std::abs(int(actual[i]) - int(expected[i])));
            if (maximum > 3)
                std::fprintf(stderr, "GPU/CPU mismatch space=%d variant=%d error=%d\n", int(space), variant, maximum);
            assert(maximum <= 3);
        }
    }
}
