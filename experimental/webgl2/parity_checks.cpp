// Pixel checks of what the browser renderer draws that native kin draws too (docs/web.md, First 3D view): spot
// lights, materials whose shaders the browser can't run, surface shaders and material graphs, HDR views and
// volumetric fog.
#include "render_checks.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <kin/render3d/local_lights3d.hpp>
#include <stdexcept>
#include <string>

using namespace kin;
namespace {
void require(bool value, const std::string& message) {
    if (!value)
        throw std::runtime_error(message);
}
MeshData3D square(f32 half) {
    return {.vertices = {{{-half, -half, 0}, {0, 0, 1}, {0, 1}},
                         {{half, -half, 0}, {0, 0, 1}, {1, 1}},
                         {{half, half, 0}, {0, 0, 1}, {1, 0}},
                         {{-half, half, 0}, {0, 0, 1}, {0, 0}}},
            .indices = {0, 1, 2, 0, 2, 3}};
}
f32 linear(int srgb) {
    return srgb_to_linear(f32(srgb) / 255.0f);
}
struct Bench {
    Renderer2D& r;
    Renderer3D& r3d;
    View3D view;
    RenderTarget target;
    Mesh3D plane;
    Scene3D scene;
    std::vector<u8> pixels;

    Bench(Renderer2D& renderer, Renderer3D& renderer3d, ViewOptions3D options = {.samples = 1})
        : r(renderer), r3d(renderer3d) {
        view = r3d.create_view({64, 64}, options).value_or({});
        target = r.create_render_target({64, 64}, ScaleMode::Nearest);
        plane = r3d.create_mesh(square(2)).value_or({});
        require(view && target && plane, "Parity reference allocation failed");
        // Looking down -z at the plane, 16 pixels a unit: pixel (32, 32) is the origin.
        scene.camera = Camera3D::look_at({0, 0, 50}, {});
        scene.camera.projection = Projection3D::Orthographic;
        scene.camera.height = 4;
        scene.camera.z_far = 100;
        scene.lights.sun.color = {0, 0, 0};
        scene.lights.fill.color = {0, 0, 0};
        scene.lights.ambient = {0, 0, 0};
    }
    void render(const char* what) {
        require(bool(r3d.render(view, scene)), std::string(what) + ": the render was refused");
        r.push_render_target(target);
        r.clear(colors::black);
        r.draw_texture(view.texture(), {0, 0, 64, 64});
        Vec2i size;
        require(r.read_rgba({0, 0, 64, 64}, pixels, size), std::string(what) + ": readback failed");
        r.pop_render_target();
    }
    std::array<int, 4> at(i32 x, i32 y) const {
        const auto* p = &pixels[std::size_t(y * 64 + x) * 4];
        return {p[0], p[1], p[2], p[3]};
    }
};
void near(std::array<int, 4> value, std::array<int, 4> expected, int tolerance, const std::string& message) {
    for (u32 i = 0; i < 4; ++i)
        if (std::abs(value[i] - expected[i]) > tolerance) {
            std::cerr << message << ": channel " << i << " got " << value[i] << " expected " << expected[i] << '\n';
            throw std::runtime_error(message);
        }
}
struct OpaqueShader final : IMaterialShaderBackend3D {};

void check_spot_lights(Renderer2D& r, Renderer3D& r3d) {
    Bench b(r, r3d);
    SpotLightSource3D spot{.position = {0, 0, 1}, .direction = {0, 0, -1}, .color = {0.6f, 0.6f, 0.6f}, .radius = 10};
    spot.inner_angle = 20;
    spot.outer_angle = 30;
    b.scene.lights.spots.push_back(spot);
    b.scene.draw(b.plane, {}, {.color = colors::white, .metallic = 0, .roughness = 1});
    b.render("Spot light");
    const auto centre = b.at(32, 32), edge = b.at(39, 32), outside = b.at(52, 32);
    require(centre[0] > 60 && centre[0] < 250, "A spot light must light what is inside its cone (unclipped)");
    near(outside, {0, 0, 0, 255}, 1, "A spot light must not light what is outside its outer angle");
    // In the penumbra, as the CPU twin of native mesh.frag's falloff has it (times the cosine at the surface).
    const auto lit = [&](f32 x) {
        const Vec3f p{x, 0, 0};
        return spot_attenuation(spot, p) / std::sqrt(1 + x * x);
    };
    const f32 expected = lit((39.5f - 32) / 16) / lit(0.5f / 16), got = linear(edge[0]) / linear(centre[0]);
    if (std::abs(got - expected) > 0.1f * expected + 0.01f)
        std::cerr << "spot penumbra: got " << got << " expected " << expected << " (centre " << centre[0] << ")\n";
    require(std::abs(got - expected) <= 0.1f * expected + 0.01f, "A spot light's penumbra must follow native falloff");
    // Turned away, it lights nothing.
    b.scene.lights.spots[0].direction = {1, 0, 0};
    b.render("Turned spot light");
    near(b.at(32, 32), {0, 0, 0, 255}, 1, "A spot light turned away must not light the plane");
    std::cout << "web parity: spot lights passed\n";
}

void check_unrunnable_shaders(Renderer2D& r, Renderer3D& r3d) {
    Bench b(r, r3d);
    Material3D material{.color = Color::rgb(200, 30, 40), .unlit = true};
    material.shader = MaterialShader3D{std::make_shared<OpaqueShader>()};
    b.scene.draw(b.plane, {}, material);
    b.render("Custom-shader material");
    near(b.at(32, 32), {200, 30, 40, 255}, 2, "A custom-shader material must draw as kin's stock material");
    std::cout << "web parity: custom-shader fallback passed\n";
}
} // namespace

void check_webgl_parity(Renderer2D& r, Renderer3D& r3d) {
    check_spot_lights(r, r3d);
    check_unrunnable_shaders(r, r3d);
}
