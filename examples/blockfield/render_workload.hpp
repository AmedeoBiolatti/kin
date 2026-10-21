#pragma once
// Small shared material/light rig for the rendering benchmark. No asset downloads or extra terrain copies.
#include <array>
#include <cmath>
#include <kin/render3d/renderer3d.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <stdexcept>
#include <string>

namespace blockfield::compare {
using namespace kin;
struct Features {
    std::string material = "unlit", camera = "auto";
    u32 resolution_scale = 1;
    u32 samples = 1, points = 0, spots = 0, shadow_resolution = 1024;
    bool hdr = false, taa = false, shadows = false, local_shadows = false;
    bool point_shadows = true;
    f32 shadow_near = 0.05f;
    bool normal_map = true, ao = false, contact = false, environment = false;
    void validate() const {
        if (material != "unlit" && material != "lit" && material != "pbr")
            throw std::runtime_error("material must be unlit, lit or pbr");
        if (camera != "auto" && camera != "field" && camera != "detail" && camera != "ground")
            throw std::runtime_error("camera must be auto, field, detail or ground");
        if (samples != 1 && samples != 2 && samples != 4 && samples != 8)
            throw std::runtime_error("samples must be 1, 2, 4 or 8");
        if (resolution_scale != 1 && resolution_scale != 2)
            throw std::runtime_error("resolution scale must be 1 or 2");
        if (points > 16 || spots > 16)
            throw std::runtime_error("point/spot light counts must be 0..16");
        if (shadow_resolution != 512 && shadow_resolution != 1024 && shadow_resolution != 2048)
            throw std::runtime_error("shadow resolution must be 512, 1024 or 2048");
        if (taa && samples != 1)
            throw std::runtime_error("choose TAA or MSAA, not both");
        if (!std::isfinite(shadow_near) || shadow_near < 0.001f || shadow_near > 1.0f)
            throw std::runtime_error("shadow near must be 0.001..1");
        if (local_shadows && !(point_shadows && points) && !spots)
            throw std::runtime_error("local shadows need an eligible point or spot light");
        if (material == "unlit" && (points || spots || shadows || local_shadows || ao || contact))
            throw std::runtime_error("lighting features need a lit or pbr material");
    }
};
inline Environment3D make_environment(Renderer3D& renderer) {
    constexpr int width = 128, height = 64;
    std::vector<f32> rgb(width * height * 3);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const f32 t = f32(y) / (height - 1);
            const f32 bright = x >= 76 && x < 96 && y >= 12 && y < 24 ? 4.0f : 0.0f;
            const int at = (y * width + x) * 3;
            rgb[at] = 0.35f * (1 - t) + 0.05f * t + bright;
            rgb[at + 1] = 0.5f * (1 - t) + 0.04f * t + bright;
            rgb[at + 2] = 0.8f * (1 - t) + 0.03f * t + bright;
        }
    auto environment = renderer.create_environment({width, height}, rgb).value_or({});
    if (!environment)
        throw std::runtime_error("image-based environment creation failed");
    return environment;
}
struct Materials {
    std::array<Texture, 4> maps;
    std::array<Material3D, 3> values;
    u64 texture_bytes = 0;
    void create(Renderer2D& renderer, const Features& f) {
        if (f.material == "pbr") {
            constexpr int side = 64;
            std::array<std::vector<u8>, 4> pixels;
            for (auto& p : pixels)
                p.resize(side * side * 4);
            for (int y = 0; y < side; ++y)
                for (int x = 0; x < side; ++x) {
                    const int at = (y * side + x) * 4;
                    const int checker = ((x / 4) ^ (y / 4)) & 1;
                    const std::array<u8, 4> base{u8(180 + checker * 55), u8(170 + checker * 55), u8(155 + checker * 55),
                                                 255};
                    const std::array<u8, 4> orm{u8(180 + checker * 75), u8(96 + (x % 16) * 10), 255, 255};
                    const f32 nx = f32(x % 16 - 8) * 0.02f, ny = f32(y % 16 - 8) * 0.02f;
                    const f32 nz = std::sqrt(1 - nx * nx - ny * ny);
                    const auto channel = [](f32 n) { return u8(std::lround((n + 1) * 127.5f)); };
                    const std::array<u8, 4> normal{channel(nx), channel(ny), channel(nz), 255};
                    const bool glow = (x + y) % 32 < 2;
                    const std::array<u8, 4> emission{u8(glow ? 255 : 0), u8(glow ? 60 : 0), u8(glow ? 10 : 0), 255};
                    for (int c = 0; c < 4; ++c) {
                        pixels[0][at + c] = base[c];
                        pixels[1][at + c] = orm[c];
                        pixels[2][at + c] = normal[c];
                        pixels[3][at + c] = emission[c];
                    }
                }
            maps[0] = renderer.create_texture_from_rgba(pixels[0].data(), {side, side});
            maps[1] = renderer.create_texture({side, side}, TextureFormat::Rgba8, pixels[1].data());
            maps[2] = renderer.create_texture({side, side}, TextureFormat::Rgba8, pixels[2].data());
            maps[3] = renderer.create_texture_from_rgba(pixels[3].data(), {side, side});
            for (const auto& texture : maps) {
                if (!texture)
                    throw std::runtime_error("material texture creation failed");
                renderer.set_scale_mode(texture, ScaleMode::Linear);
            }
            texture_bytes = 4 * side * side * 4;
        }
        constexpr std::array<f32, 3> metallic{0.0f, 0.65f, 1.0f}, roughness{0.9f, 0.45f, 0.16f};
        for (u32 i = 0; i < values.size(); ++i) {
            auto& m = values[i];
            m.unlit = f.material == "unlit";
            m.casts_shadow = f.shadows || f.local_shadows;
            m.receives_shadow = m.casts_shadow;
            if (f.material == "pbr") {
                m.texture = maps[0];
                m.metallic_roughness = maps[1];
                m.occlusion = maps[1];
                m.normal = f.normal_map ? maps[2] : Texture{};
                m.normal_scale = 0.7f;
                m.emissive_texture = maps[3];
                m.emissive = 0.15f;
                m.metallic = metallic[i];
                m.roughness = roughness[i];
                m.wrap_u = m.wrap_v = Wrap3D::Repeat;
            }
        }
    }
};
inline void configure(Scene3D& scene, const Features& f, f32 time, f32 extent) {
    scene.lights.sun = {.direction = {-0.5f, 1.0f, 0.35f}, .color = {0.7f, 0.65f, 0.55f}};
    scene.lights.ambient = {0.12f, 0.14f, 0.18f};
    scene.lights.ground_ambient = {true, kin::LinearRgb{0.04f, 0.035f, 0.03f}};
    scene.lights.shadow_resolution = f.shadow_resolution;
    scene.lights.shadow_cascades = 1;
    scene.lights.shadow_area = f.shadows ? Bounds3D{{-extent, -1, -extent}, {extent, 100, extent}} : Bounds3D{};
    scene.lights.cache_shadows = false;
    scene.lights.local_shadow_resolution = f.shadow_resolution;
    scene.lights.max_shadowed_points = f.point_shadows ? 1 : 0;
    scene.lights.max_shadowed_spots = 2;
    scene.taa.enabled = f.taa;
    scene.ambient_occlusion = {.enabled = f.ao, .radius = 2, .half_resolution = true};
    scene.contact_shadows = {.enabled = f.contact, .distance = 2, .steps = 16};
    constexpr std::array<LinearRgb, 4> colours{
        {{2400, 1200, 600}, {600, 1200, 2400}, {1200, 2400, 900}, {2400, 700, 1800}}};
    for (u32 i = 0; i < f.points; ++i) {
        const f32 a = f32(i) * 6.28318530718f / f32(f.points) + time * 0.18f;
        const f32 radius = std::min(extent * 0.45f, 200.0f);
        PointLightSource3D light{.position = {radius * std::cos(a), 80, radius * std::sin(a)},
                                 .color = colours[i % 4],
                                 .radius = extent * 2,
                                 .size = 1};
        light.shadow_enabled = f.local_shadows && f.point_shadows && i == 0;
        light.shadow_near_plane = f.shadow_near;
        light.id = i + 1;
        scene.lights.points.push_back(light);
    }
    for (u32 i = 0; i < f.spots; ++i) {
        const f32 a = f32(i) * 6.28318530718f / f32(f.spots) - time * 0.13f;
        const f32 radius = std::min(extent * 0.25f, 100.0f);
        SpotLightSource3D light{.position = {radius * std::cos(a), 100, radius * std::sin(a)},
                                .direction = {0, -1, 0},
                                .color = colours[(i + 1) % 4] * 2,
                                .radius = extent * 2,
                                .size = 1,
                                .inner_angle = 30,
                                .outer_angle = 45};
        light.shadow_enabled = f.local_shadows && i < 2;
        light.shadow_near_plane = f.shadow_near;
        light.id = i + 1;
        scene.lights.spots.push_back(light);
    }
}
} // namespace blockfield::compare
