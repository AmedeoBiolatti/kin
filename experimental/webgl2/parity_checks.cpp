// Pixel checks of what the browser renderer draws that native kin draws too (docs/web.md, First 3D view): spot
// lights, materials whose shaders the browser can't run, surface shaders and material graphs, HDR views,
// volumetric fog and volumetric clouds.
#include "render_checks.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <kin/render3d/local_lights3d.hpp>
#include <kin/render3d/material_graph3d.hpp>
#include <kin/render3d/particles3d.hpp>
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
// Native fog_volume.comp (cells lit at their middles, no history) and fog_volume_integrate.comp on the CPU: the sum of
// the light a point light's air sends along column (x, y) to the volume's far edge.
f32 fog_column(const Scene3D& scene, std::array<u32, 3> cells, f32 x, f32 y) {
    const auto& v = scene.fog.volumetric;
    const auto& light = scene.lights.points[0];
    const Mat4f to_world = scene.camera.view_projection(1.0f).inverse();
    const Vec3f eye = scene.camera.position, forward = scene.camera.forward();
    const auto ray = [&](f32 u, f32 w) { // GL rows up, as the browser's volume
        const Vec4f p = to_world * Vec4f{u * 2 - 1, w * 2 - 1, 0.5f, 1.0f};
        const Vec3f r = Vec3f{p.x / p.w, p.y / p.w, p.z / p.w} - eye;
        return r * (1.0f / dot(r, forward));
    };
    const Vec3f r = ray((x + 0.5f) / cells[0], (y + 0.5f) / cells[1]);
    const f32 stretch = length(r), sigma = scene.fog.density, g = v.anisotropy;
    f32 sum = 0, through = 1, before = 0;
    for (u32 z = 0; z < cells[2]; ++z) {
        const f32 mid = (z + 0.5f) / cells[2], depth = v.distance * mid * mid;
        const Vec3f p = eye + r * depth, to = light.position - p, dir = r * (1.0f / stretch);
        const f32 d2 = dot(to, to), r2 = d2 / (light.radius * light.radius);
        const f32 window = std::clamp(1 - r2 * r2, 0.0f, 1.0f);
        const f32 k = window * window / std::max(d2, light.size * light.size);
        const f32 mu = dot(dir, to * (1.0f / std::sqrt(d2))), x2 = std::max(1 + g * g - 2 * g * mu, 1e-4f);
        const f32 lit = light.color.r * v.intensity * k * (1 - g * g) / (4 * 3.14159265f * x2 * std::sqrt(x2)) * sigma;
        const f32 w = (z + 1.0f) / cells[2], after = v.distance * w * w, len = (after - before) * stretch;
        before = after;
        const f32 kept = std::exp(-sigma * len);
        sum += through * lit * (1 - kept) / sigma;
        through *= kept;
    }
    return sum;
}

void check_volumetric_fog(Renderer2D& r, Renderer3D& r3d) {
    Bench b(r, r3d);
    auto& scene = b.scene;
    scene.camera = Camera3D::look_at({0, 0, 0}, {0, 0, -1});
    scene.camera.fov = 60;
    scene.fog.enabled = true;
    scene.fog.density = 0.08f;
    scene.fog.colour = {0, 0, 0};
    scene.fog.volumetric = {
        .enabled = true, .intensity = 1, .anisotropy = 0.4f, .distance = 20, .tile = 4, .slices = 64};
    scene.lights.points.push_back({.position = {0.5f, 0.3f, -6}, .color = {3, 3, 3}, .radius = 8, .size = 0.6f});
    b.render("Volumetric fog");
    // Pixel (32, 32), its middle at the volume's (8.125, 7.875) in cells (rows up): between four columns' sums.
    const std::array<u32, 3> cells{16, 16, 64};
    const f32 fx = 32.5f / 64 * 16 - 0.5f, fy = (64 - 32.5f) / 64 * 16 - 0.5f;
    const u32 x0 = u32(fx), y0 = u32(fy);
    const f32 tx = fx - x0, ty = fy - y0;
    const f32 expected =
        (fog_column(scene, cells, x0, y0) * (1 - tx) + fog_column(scene, cells, x0 + 1, y0) * tx) * (1 - ty) +
        (fog_column(scene, cells, x0, y0 + 1) * (1 - tx) + fog_column(scene, cells, x0 + 1, y0 + 1) * tx) * ty;
    const f32 got = linear(b.at(32, 32)[0]);
    if (std::abs(got - expected) > 0.04f * expected + 0.004f)
        std::cerr << "volumetric fog: got " << got << " expected " << expected << '\n';
    require(expected > 0.05f && expected < 0.9f, "The volumetric fog check's light must be visible and unclipped");
    require(std::abs(got - expected) <= 0.04f * expected + 0.004f,
            "The fog volume's light must be native's (cells lit at their middles, summed front to back)");
    const auto lit = b.at(32, 32);
    scene.fog.volumetric.enabled = false;
    b.render("Fog without its volume");
    near(b.at(32, 32), {0, 0, 0, 255}, 1, "Without the volume a light lights no air");
    // A plane in front of the light: its pixels take the air's light in front of it only (less).
    scene.fog.volumetric.enabled = true;
    require(bool(scene.draw(b.plane, Mat4f::translation({0, 0, -4}), {.color = colors::black})),
            "Parity submission failed");
    b.render("Fog volume in front of a surface");
    require(b.at(32, 32)[0] < lit[0] - 10 && b.at(32, 32)[0] > 0,
            "A surface in front of a light takes only the air's light before it");
    std::cout << "web parity: volumetric fog passed\n";
}
void check_particles(Renderer2D& r, Renderer3D& r3d) {
    Bench b(r, r3d);
    auto& scene = b.scene;
    const auto sprite = [](Vec3f at, LinearRgb colour, f32 alpha = 1, f32 size = 1) {
        return ParticleSprite3D{.position = at, .size = size, .colour = colour, .alpha = alpha};
    };
    const auto submit = [&](const ParticleLook3D& look, std::vector<ParticleSprite3D> sprites) {
        require(bool(scene.draw_particles(look, sprites)), "Particle submission failed");
    };
    // A hard procedural disc, its colour as it is (unlit): inside, the colour; outside its radius, nothing.
    ParticleLook3D disc{.softness = 0};
    submit(disc, {sprite({0, 0, 0}, {0.5f, 0.2f, 0.1f})});
    b.render("Particle");
    near(b.at(32, 32), srgb8(0.5f, 0.2f, 0.1f), 2, "A particle must draw its colour");
    near(b.at(32 + 10, 32), {0, 0, 0, 255}, 0, "A particle's disc must end at its radius");
    require(r3d.debug_stats().particles_drawn == 1 && r3d.debug_stats().particle_batches == 1,
            "The browser renderer must count the particles it draws");
    // Additive sprites add.
    scene.clear();
    ParticleLook3D glow{.blend = ParticleBlend3D::Additive, .softness = 0};
    submit(glow, {sprite({0, 0, 0}, {0.2f, 0.1f, 0}), sprite({0, 0, 0.1f}, {0.2f, 0.1f, 0})});
    b.render("Additive particles");
    // (An 8-bit view adds them encoded, as native's does.)
    const auto one = srgb8(0.2f, 0.1f, 0);
    near(b.at(32, 32), {2 * one[0], 2 * one[1], 0, 255}, 2, "Additive particles must add");
    // Sorted with transparent meshes: a half-transparent red sprite in front of a half-transparent blue plane,
    // submitted first, still lies over it.
    scene.clear();
    submit(disc, {sprite({0, 0, 1}, {1, 0, 0}, 0.5f)});
    require(bool(scene.draw(b.plane, {}, {.color = {0, 0, 255, 128}, .unlit = true, .blend = Blend3D::Alpha})),
            "Parity submission failed");
    b.render("Sorted particle");
    near(b.at(32, 32), {128, 0, 64, 255}, 3, "A sprite in front of a blended mesh must be drawn over it");
    // Behind an opaque surface: hidden. Soft, just in front of one: faded to almost nothing.
    scene.clear();
    require(bool(scene.draw(b.plane, {}, {.color = colors::black, .unlit = true})), "Parity submission failed");
    submit(disc, {sprite({0, 0, -0.5f}, {1, 1, 1})});
    b.render("Hidden particle");
    near(b.at(32, 32), {0, 0, 0, 255}, 0, "A particle behind an opaque surface must be hidden");
    for (const bool soft : {false, true}) {
        scene.clear();
        require(bool(scene.draw(b.plane, {}, {.color = colors::black, .unlit = true})), "Parity submission failed");
        ParticleLook3D look{.softness = 0, .soft = soft, .soft_distance = 0.5f};
        submit(look, {sprite({0, 0, 0.05f}, {1, 1, 1})});
        b.render("Soft particle");
        const int at = b.at(32, 32)[0];
        // Its coverage times smoothstep(0.05 / 0.5) = 0.028: white over black is 0.028 * 255 (the fade is alpha).
        require(soft ? at >= 5 && at <= 9 : at > 250, "A soft particle must fade near the surface behind it");
    }
    // A flipbook: a 2 x 1 atlas (red, green); frame 1 shows the second.
    scene.clear();
    const std::array<u8, 8> atlas{255, 0, 0, 255, 0, 255, 0, 255};
    ParticleLook3D book{.texture = r.create_texture_from_rgba(atlas.data(), Vec2i{2, 1}), .atlas_columns = 2};
    auto frame = sprite({0, 0, 0}, {1, 1, 1});
    frame.frame = 1;
    submit(book, {frame});
    b.render("Flipbook particle");
    near(b.at(32, 32), {0, 255, 0, 255}, 1, "A flipbook particle must show its frame");
    // Lit: the colour times the scene's tint (ambient, half the sun and the fill).
    scene.clear();
    scene.lights.ambient = {0.4f, 0.4f, 0.4f};
    ParticleLook3D lit{.softness = 0, .lit = 1};
    submit(lit, {sprite({0, 0, 0}, {1, 1, 1})});
    b.render("Lit particle");
    const LinearRgb tint = particle_light_tint(scene.lights);
    near(b.at(32, 32), srgb8(tint.r, tint.g, tint.b), 2, "A lit particle must take the scene's light tint");
    // Shadows. Cast: an invisible (additive black) sprite above a lit plane shadows it where the sun is blocked.
    scene.clear();
    scene.lights.ambient = {0, 0, 0};
    scene.lights.sun = {.direction = {0.5f, 0, 1}, .color = {1, 1, 1}};
    scene.lights.shadow_area = {{-3, -3, -1}, {3, 3, 3}}; // (empty: no sun shadows)
    require(bool(scene.draw(b.plane, {}, {.color = colors::white})), "Parity submission failed");
    ParticleLook3D caster{.blend = ParticleBlend3D::Additive, .softness = 0, .casts_shadow = true};
    submit(caster, {sprite({0, 0, 1}, {0, 0, 0})});
    b.render("Particle casting");
    // The shadow lies half a unit (8 pixels) away from the sun: centred on pixel 24.
    require(b.at(24, 32)[0] + 60 < b.at(48, 32)[0], "A particle that casts must shadow the surface under it");
    require(r3d.debug_stats().particle_shadow_draws > 0, "The browser renderer must count particle shadow draws");
    // Receive: a lit sprite whose way to the sun a mesh blocks takes none of the sun's half of its tint.
    std::array<int, 2> received{};
    for (const bool receives : {false, true}) {
        scene.clear();
        scene.lights.sun = {.direction = {1, 0, 1}, .color = {1, 1, 1}};
        require(bool(scene.draw(b.plane, Mat4f::translation({1.6f, 0, 1.6f}) * Mat4f::scaling({0.3f, 0.3f, 1}),
                                {.color = colors::black, .unlit = true})),
                "Parity submission failed");
        submit({.softness = 0, .lit = 1, .receives_shadow = receives}, {sprite({0, 0, 0}, {1, 1, 1})});
        b.render("Particle receiving");
        received[receives] = b.at(30, 32)[0];
    }
    if (!(received[0] > 150 && received[1] < 40))
        std::cerr << "particle shadow: unshadowed " << received[0] << " shadowed " << received[1] << '\n';
    require(received[0] > 150 && received[1] < 40, "A particle that receives shadows must lose the blocked sun");
    scene.lights.sun = {};
    // Weather: rain's streaks round the camera.
    Bench w(r, r3d);
    w.scene.camera = Camera3D::look_at({0, 2, 0}, {0, 2, -10});
    w.scene.weather.rain.intensity = 1;
    w.render("Weather");
    u32 drops = 0;
    for (std::size_t i = 0; i < w.pixels.size(); i += 4)
        drops += w.pixels[i + 2] > 10;
    require(r3d.debug_stats().weather_particles > 0 && drops > 20, "Rain must be drawn in the browser");
    std::cout << "web parity: particles (disc, additive, sorting, depth, soft, lit, shadows, weather) passed\n";
}

void check_decals(Renderer2D& r, Renderer3D& r3d) {
    Bench b(r, r3d);
    auto& scene = b.scene;
    scene.lights.ambient = {0.8f, 0.8f, 0.8f};
    // The box's +Y (where it is projected from) toward the camera: rotated a quarter turn about X.
    const Mat4f box = Mat4f::rotation_x(90);
    const Color red = Color::rgb(200, 40, 30);
    // The reference: the plane itself red, lit as the decal is (roughness 0.6, not metal).
    require(bool(scene.draw(b.plane, {}, {.color = red, .metallic = 0, .roughness = 0.6f})),
            "Parity submission failed");
    b.render("Decal reference");
    const auto expected = b.at(32, 32);
    scene.clear();
    require(bool(scene.draw(b.plane, {}, {.color = Color::rgb(90, 90, 90)})), "Parity submission failed");
    require(bool(scene.draw_decal({.transform = box, .color = red})), "Decal submission failed");
    b.render("Decal");
    near(b.at(32, 32), expected, 3, "A decal must be lit as a material of its colour on the surface under it");
    require(b.at(32 + 12, 32) != expected, "A decal must end at its box");
    require(r3d.debug_stats().decals_drawn == 1, "The browser renderer must count the decals it draws");
    // Half its alpha: halfway (in the 8-bit picture, as the blend is) between the surface and the decal.
    const auto grey = b.at(32 + 12, 32);
    scene.clear();
    require(bool(scene.draw(b.plane, {}, {.color = Color::rgb(90, 90, 90)})), "Parity submission failed");
    require(bool(scene.draw_decal({.transform = box, .color = Color{200, 40, 30, 128}})), "Decal submission failed");
    b.render("Half decal");
    for (u32 c = 0; c < 3; ++c)
        require(std::abs(b.at(32, 32)[c] - (expected[c] * 128 + grey[c] * 127) / 255) <= 3,
                "A decal's alpha must blend it over the surface");
    // A surface that takes no decals is drawn over them.
    scene.clear();
    require(bool(scene.draw(b.plane, {}, {.color = Color::rgb(90, 90, 90), .receives_decals = false})),
            "Parity submission failed");
    require(bool(scene.draw_decal({.transform = box, .color = red})), "Decal submission failed");
    b.render("Decal on a non-receiver");
    near(b.at(32, 32), grey, 1, "A surface that takes no decals must not show them");
    // Batches: three decals of one picture, each its own colour, are one draw; a decal of a higher order lies over a
    // lower one whatever the submission order.
    scene.clear();
    scene.lights.ambient = {1, 1, 1};
    require(bool(scene.draw(b.plane, {}, {.color = colors::black, .unlit = true})), "Parity submission failed");
    const std::array<Color, 3> colours{Color::rgb(200, 0, 0), Color::rgb(0, 200, 0), Color::rgb(0, 0, 200)};
    for (int i = 0; i < 3; ++i)
        require(bool(scene.draw_decal(
                    {.transform = Mat4f::translation({f32(i - 1), 0, 0}) * box * Mat4f::scaling({0.8f, 1, 0.8f}),
                     .color = colours[u32(i)]})),
                "Decal submission failed");
    b.render("Decal batch");
    require(r3d.debug_stats().decals_drawn == 3 && r3d.debug_stats().decal_draws == 1,
            "Decals sharing a picture must be one draw");
    for (int i = 0; i < 3; ++i) {
        const auto at = b.at(32 + 16 * (i - 1), 32);
        const int strongest = int(std::max_element(at.begin(), at.begin() + 3) - at.begin());
        require(strongest == i && at[u32(i)] > 100, "Each decal of a batch must keep its own colour");
    }
    scene.clear();
    require(bool(scene.draw(b.plane, {}, {.color = colors::black, .unlit = true})), "Parity submission failed");
    const std::array<u8, 4> white_pixel{255, 255, 255, 255};
    const Texture other = r.create_texture_from_rgba(white_pixel.data(), Vec2i{1, 1});
    require(bool(scene.draw_decal({.transform = box, .color = Color::rgb(200, 0, 0), .order = 1})),
            "Decal submission failed");
    require(bool(scene.draw_decal({.transform = box, .texture = other, .color = Color::rgb(0, 200, 0), .order = 0})),
            "Decal submission failed");
    b.render("Decal order");
    require(r3d.debug_stats().decal_draws == 2, "Decals of different pictures must be different draws");
    // (Red over green: the green and blue left are the red decal's own ambient reflection, about 47 each.)
    require(b.at(32, 32)[0] > 150 && b.at(32, 32)[1] < 80 && b.at(32, 32)[1] == b.at(32, 32)[2],
            "A decal of a higher order must lie over a lower one");
    std::cout << "web parity: decals (lighting, box, alpha, receivers, batches, order) passed\n";
}
void check_local_shadows(Renderer2D& r, Renderer3D& r3d) {
    Bench b(r, r3d);
    auto& scene = b.scene;
    // A light up and to the right of the plane's middle, a small caster halfway: its shadow falls on the middle (pixel
    // 32, 32), while the camera sees the caster at pixel 48.
    const Vec3f at{2, 0, 3};
    const auto shade = [&](auto light_setup) {
        scene.clear();
        scene.lights.points.clear();
        scene.lights.spots.clear();
        scene.lights.fill.color = {0, 0, 0};
        scene.lights.fill_shadow = {};
        scene.lights.shadow_area = {{-3, -3, -1}, {3, 3, 4}};
        light_setup();
        require(bool(scene.draw(b.plane, {}, {.color = colors::white})), "Parity submission failed");
        require(bool(scene.draw(b.plane, Mat4f::translation(at * 0.5f) * Mat4f::scaling({0.08f, 0.08f, 1}),
                                {.color = colors::black, .unlit = true, .double_sided = true})),
                "Parity submission failed");
        b.render("Local shadow");
        return std::array<int, 2>{b.at(32, 32)[0], b.at(32, 20)[0]}; // the shadow, and lit plane above it
    };
    for (const bool shadowed : {false, true}) {
        const auto spot = shade([&] {
            SpotLightSource3D l{.position = at, .direction = -at, .color = {6, 6, 6}, .radius = 12};
            l.inner_angle = 40;
            l.outer_angle = 50;
            l.shadow_enabled = shadowed;
            scene.lights.spots.push_back(l);
        });
        const auto point = shade([&] {
            PointLightSource3D l{.position = at, .color = {6, 6, 6}, .radius = 12};
            l.shadow_enabled = shadowed;
            scene.lights.points.push_back(l);
        });
        const auto moon = shade([&] {
            scene.lights.fill = {.direction = at, .color = {0.8f, 0.8f, 0.8f}};
            scene.lights.fill_shadow = {.enabled = shadowed, .cascades = 1};
        });
        for (const auto& [name, v] : {std::pair{"spot", spot}, {"point", point}, {"moon", moon}}) {
            if (shadowed ? !(v[0] + 40 < v[1]) : std::abs(v[0] - v[1]) > 30)
                std::cerr << name << (shadowed ? " shadowed" : " unshadowed") << ": " << v[0] << " vs " << v[1] << '\n';
            require(shadowed ? v[0] + 40 < v[1] : std::abs(v[0] - v[1]) <= 30,
                    std::string("A ") + name + " light's shadow must fall where (and only when) it is asked for");
        }
    }
    require(r3d.debug_stats().moon_cascades == 1, "The browser renderer must count the moon's cascades");
    // In volumetric fog: a spot's beam below a caster is darker with its shadow.
    std::array<int, 2> beam{};
    for (const bool shadowed : {false, true}) {
        Bench f(r, r3d);
        f.scene.camera = Camera3D::look_at({0, 0, 0}, {0, 0, -1});
        f.scene.fog.enabled = true;
        f.scene.fog.density = 0.08f;
        f.scene.fog.colour = {0, 0, 0};
        f.scene.fog.volumetric = {.enabled = true, .distance = 20, .tile = 4, .slices = 64};
        SpotLightSource3D l{.position = {0, 3, -6}, .direction = {0, -1, 0}, .color = {8, 8, 8}, .radius = 10};
        l.inner_angle = 25;
        l.outer_angle = 35;
        l.shadow_enabled = shadowed;
        f.scene.lights.spots.push_back(l);
        require(
            bool(f.scene.draw(
                f.plane, Mat4f::translation({0, 1.8f, -6}) * Mat4f::rotation_x(-90) * Mat4f::scaling({0.3f, 0.3f, 1}),
                {.color = colors::black, .unlit = true, .double_sided = true})),
            "Parity submission failed");
        f.render("Shadowed beam");
        beam[shadowed] = f.at(32, 30)[0];
    }
    if (!(beam[1] + 10 < beam[0]))
        std::cerr << "fog beam: unshadowed " << beam[0] << " shadowed " << beam[1] << '\n';
    require(beam[1] + 10 < beam[0], "A spot's shadow must darken its beam in the fog");
    std::cout << "web parity: spot, point and moon shadows (surfaces and fog) passed\n";
}
} // namespace

// Volumetric clouds: an overcast greys a clear blue sky (a camera on the ground looking up); a plane in front of them
// keeps its colour (their depth test); the frames blend after the first; with no coverage the sky is as without them.
void check_clouds(Renderer2D& r, Renderer3D& r3d) {
    Bench b(r, r3d);
    auto& scene = b.scene;
    scene.camera = Camera3D::look_at({0, 2, 0}, {3, 7.77f, -10});
    scene.camera.fov = 60;
    const f32 e = 40.0f * 3.14159265f / 180.0f;
    scene.lights.sun = {.direction = {0, std::sin(e), -std::cos(e)}, .color = {1, 1, 1}};
    scene.sky = {.enabled = true, .turbidity = 3, .sun_intensity = 1, .sun_size = 0.01f};
    b.render("A clear sky");
    const auto clear = b.at(32, 32);
    scene.clouds = {.enabled = true, .coverage = 0.0f, .shadows = false};
    b.render("Clouds with no coverage");
    near(b.at(32, 32), clear, 1, "Clouds with no coverage leave the sky as it was");
    scene.clouds.coverage = 1.0f;
    for (int frame = 0; frame < 3; ++frame)
        b.render("An overcast");
    const auto overcast = b.at(32, 32);
    require(r3d.debug_stats().clouds && r3d.debug_stats().clouds_history && r3d.debug_stats().clouds_depth_bounded,
            "The clouds were walked (to the meshes' depth) and blended");
    require(clear[2] - clear[0] > 15, "The clouds check's clear sky must be blue");
    if (overcast[2] - overcast[0] >= (clear[2] - clear[0]) / 2)
        std::cerr << "clouds: clear " << clear[0] << ' ' << clear[1] << ' ' << clear[2] << ", overcast " << overcast[0]
                  << ' ' << overcast[1] << ' ' << overcast[2] << '\n';
    require(overcast[2] - overcast[0] < (clear[2] - clear[0]) / 2, "An overcast greys the sky");
    const Vec3f at = scene.camera.position + scene.camera.forward() * 20.0f;
    const Mesh3D box = r3d.create_mesh(meshes::box({4, 4, 4})).value_or({});
    require(bool(box) &&
                bool(scene.draw(box, Mat4f::translation(at), {.color = Color::rgb(200, 40, 40), .unlit = true})),
            "Parity submission failed");
    b.render("A plane in front of the clouds");
    near(b.at(32, 32), {200, 40, 40, 255}, 2, "The clouds stay behind a plane in front of them");
    std::cout << "web parity: volumetric clouds passed\n";
}
void check_webgl_parity(Renderer2D& r, Renderer3D& r3d) {
    check_spot_lights(r, r3d);
    check_unrunnable_shaders(r, r3d);
    check_hdr_views(r, r3d);
    check_material_graphs(r, r3d);
    check_volumetric_fog(r, r3d);
    check_particles(r, r3d);
    check_decals(r, r3d);
    check_local_shadows(r, r3d);
    check_clouds(r, r3d);
}
