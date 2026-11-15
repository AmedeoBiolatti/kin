// Pixel checks of what the browser renderer draws that native kin draws too (docs/web.md, First 3D view): spot
// lights, materials whose shaders the browser can't run, surface shaders and material graphs, HDR views and
// volumetric fog.
#include "render_checks.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <kin/render3d/local_lights3d.hpp>
#include <kin/render3d/material_graph3d.hpp>
#include <map>
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
    require(bool(b.scene.draw(b.plane, {}, {.color = colors::white, .metallic = 0, .roughness = 1})),
            "Parity submission failed");
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
    require(bool(b.scene.draw(b.plane, {}, material)), "Parity submission failed");
    b.render("Custom-shader material");
    near(b.at(32, 32), {200, 30, 40, 255}, 2, "A custom-shader material must draw as kin's stock material");
    std::cout << "web parity: custom-shader fallback passed\n";
}
// Native kin_view_output.inc.glsl's tonemap of linear light, then the sRGB encode, as an 8-bit value.
int tonemapped(f32 x) {
    const f32 a = std::clamp((x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f), 0.0f, 1.0f);
    return int(std::lround(linear_to_srgb(a) * 255.0f));
}

void check_hdr_views(Renderer2D& r, Renderer3D& r3d) {
    for (const f32 exposure : {1.0f, 0.5f}) {
        Bench b(r, r3d, {.samples = 1, .hdr = true, .exposure = exposure});
        Material3D glow{.color = colors::black, .unlit = true};
        glow.set_emission({2.0f, 1.0f, 0.25f}, 1);
        require(bool(b.scene.draw(b.plane, Mat4f::scaling({0.5f, 0.5f, 1}), glow)), "Parity submission failed");
        b.render("HDR view");
        near(b.at(32, 32),
             {tonemapped(2.0f * exposure), tonemapped(1.0f * exposure), tonemapped(0.25f * exposure), 255}, 3,
             "An HDR view must expose and tonemap its linear light as native's output does");
        near(b.at(4, 4), {0, 0, 0, 255}, 0, "An HDR view's empty pixels must show what is under the view");
    }
    // Bloom: a glow round what is brighter than the threshold, and none round what is not.
    const auto halo = [&](f32 strength, f32 light) {
        Bench b(r, r3d, {.samples = 1, .hdr = true, .bloom = strength, .bloom_threshold = 1});
        Material3D glow{.color = colors::black, .unlit = true};
        glow.set_emission({light, light, light}, 1);
        require(bool(b.scene.draw(b.plane, Mat4f::scaling({0.1f, 0.1f, 1}), glow)), "Parity submission failed");
        b.render("Bloom view");
        return b.at(32 + 6, 32)[0]; // a few pixels off the bright square's edge
    };
    require(halo(0, 8) == 0, "Without bloom nothing glows past an edge");
    require(halo(1, 8) > 20, "Bloom must spread what is brighter than its threshold");
    require(halo(1, 0.5f) == 0, "Bloom must leave what is under its threshold");
    // Auto exposure brings a dim and a bright scene to the key (mid-grey) alike.
    std::array<int, 2> metered{};
    for (u32 i = 0; i < 2; ++i) {
        ViewOptions3D options{.samples = 1, .hdr = true};
        options.auto_exposure.enabled = true;
        Bench b(r, r3d, options);
        Material3D grey{.color = colors::black, .unlit = true};
        const f32 light = i == 0 ? 0.02f : 3.0f;
        grey.set_emission({light, light, light}, 1);
        b.scene.camera.height = 3; // the plane fills the view
        require(bool(b.scene.draw(b.plane, {}, grey)), "Parity submission failed");
        b.render("Auto exposure view");
        metered[i] = b.at(32, 32)[0];
    }
    const int key = tonemapped(0.18f);
    if (std::abs(metered[0] - key) > 8 || std::abs(metered[1] - key) > 8)
        std::cerr << "auto exposure: dim " << metered[0] << " bright " << metered[1] << " key " << key << '\n';
    require(std::abs(metered[0] - key) <= 8 && std::abs(metered[1] - key) <= 8,
            "Auto exposure must bring a dim and a bright view to its key on its first frame");
    std::cout << "web parity: HDR exposure/tonemap, bloom and auto exposure passed\n";
}
// A material graph compiled on the CPU, made into a surface shader by the browser renderer (its GLSL, as GLSL ES).
MaterialGraphShader3D graph(Renderer3D& r3d, const std::string& parameters, const std::string& nodes,
                            const std::string& outputs) {
    const std::string text = R"({"schema": "kin.material-graph/1", "name": "check", "parameters": [)" + parameters +
                             R"(], "nodes": [)" + nodes + R"(], "outputs": {)" + outputs + "}}";
    CookedMaterialGraph3D compiled{compile_material_graph3d(text), {}};
    for (const auto& e : compiled.graph.errors)
        std::cerr << "graph: " << e.node << ": " << e.message << '\n';
    require(compiled.graph.ok(), "A check's material graph must compile");
    auto made = r3d.create_material_graph(compiled);
    for (const auto& d : made.diagnostics)
        std::cerr << "graph: " << d << '\n';
    require(bool(made) && made.value->surface, "The browser renderer must make a material graph's surface shader");
    return std::move(*made.value);
}
std::array<int, 4> srgb8(f32 r, f32 g, f32 b) {
    return {int(std::lround(linear_to_srgb(r) * 255)), int(std::lround(linear_to_srgb(g) * 255)),
            int(std::lround(linear_to_srgb(b) * 255)), 255};
}

void check_material_graphs(Renderer2D& r, Renderer3D& r3d) {
    Bench b(r, r3d);
    const auto draw = [&](const MaterialGraphShader3D& g, Material3D m,
                          const std::map<std::string, std::array<f32, 4>>& values = {}) {
        m.surface = g.surface;
        m.shader_params = material_graph_params3d(g.graph, values);
        b.scene.clear();
        require(bool(b.scene.draw(b.plane, {}, m)), "Parity submission failed");
        b.render("Material graph");
    };
    // Parameters reach the surface (Material3D::shader_params).
    const auto tinted = graph(r3d, R"({"name": "tint", "type": "color", "default": [1, 0.5, 0.2, 1]})", "",
                              R"("base_color": "tint.rgb")");
    draw(tinted, {.unlit = true}, {{"tint", {0.2f, 0.6f, 0.1f, 1}}});
    near(b.at(32, 32), srgb8(0.2f, 0.6f, 0.1f), 2, "A graph's parameters must reach its surface");
    // Built-in inputs: the UV, the screen position (rows down, as native's) and the local position.
    const auto uv = graph(r3d, "", "", R"("base_color": "uv")");
    draw(uv, {.unlit = true});
    near(b.at(8, 56), srgb8(8.5f / 64, 1 - 7.5f / 64, 0), 3, "A graph's uv must be the mesh's");
    const auto screen = graph(r3d, "", "", R"("base_color": "screen")");
    draw(screen, {.unlit = true});
    near(b.at(8, 56), srgb8(8.5f / 64, 56.5f / 64, 0), 3, "A graph's screen position must run down, as native's");
    const auto local =
        graph(r3d, "", R"({"id": "p", "type": "multiply", "inputs": {"a": "local_position", "b": 0.25}})",
              R"("base_color": "p")");
    b.scene.clear();
    Material3D plain{.unlit = true};
    plain.surface = local.surface;
    plain.shader_params = material_graph_params3d(local.graph);
    require(bool(b.scene.draw(b.plane, Mat4f::translation({0.5f, 0, 0}) * Mat4f::scaling({0.5f, 0.5f, 1}), plain)),
            "Parity submission failed");
    b.render("Local position graph");
    // Pixel (40, 24) is world (0.531, 0.469): local (0.0625, 0.9375) (the copy moved right and halved), times 0.25.
    near(b.at(40, 24), srgb8(0.015625f, 0.234375f, 0), 3, "A graph's local position must be in the copy's own space");
    // Lit: a graph's roughness and metallic as the stock material's same values.
    b.scene.lights.sun = {.direction = {0.3f, 0.4f, 1}, .color = {1, 1, 1}};
    b.scene.lights.ambient = {0.1f, 0.1f, 0.1f};
    const auto rough = graph(r3d, "", "", R"("roughness": 0.3, "metallic": 0.2)");
    draw(rough, {.color = Color::rgb(180, 120, 90)});
    const auto graphed = b.at(32, 32);
    b.scene.clear();
    require(bool(b.scene.draw(b.plane, {}, {.color = Color::rgb(180, 120, 90), .metallic = 0.2f, .roughness = 0.3f})),
            "Parity submission failed");
    b.render("Stock reference");
    near(graphed, b.at(32, 32), 2, "A graph's surface must be lit as kin's stock material with the same values");
    // A clear coat and noise compile and light (native's coat and graph functions in GLSL ES).
    const auto coat = graph(r3d, "", R"({"id": "n", "type": "fbm", "inputs": {"p": "position", "scale": 3}})",
                            R"("base_color": "n", "clearcoat": 1, "clearcoat_roughness": 0.1)");
    draw(coat, {});
    require(b.at(10, 10) != b.at(50, 50), "A noise graph must vary over the plane");
    std::cout << "web parity: material graphs (parameters, inputs, lighting, coat, noise) passed\n";
}
} // namespace

void check_webgl_parity(Renderer2D& r, Renderer3D& r3d) {
    check_spot_lights(r, r3d);
    check_unrunnable_shaders(r, r3d);
    check_hdr_views(r, r3d);
    check_material_graphs(r, r3d);
}
