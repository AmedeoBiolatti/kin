// Baked lighting drawn (Scene3D::baked_lighting; docs/rendering_3d.md, "Baked lighting"): a lightmapped copy shows its
// lightmap in place of the ambient light, a copy without one the probes', Baked lights are not drawn while Mixed and
// Realtime ones are, and the temporally anti-aliased variants read them too.
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <kin/platform/app.hpp>
#include <kin/render3d/baked_lighting3d.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/renderer/backend.hpp>
#include <kin/renderer/renderer2d.hpp>

using namespace kin;

namespace {

constexpr Vec2i extent{160, 120};
constexpr Rectf rect{0, 0, 160, 120};

struct Rgb {
    int r, g, b;
};
std::ostream& operator<<(std::ostream& out, Rgb c) {
    return out << c.r << " " << c.g << " " << c.b;
}

// A floor of 4 x 4, its lightmap UVs its UVs inset.
MeshData3D floor_data() {
    MeshData3D data = meshes::plane({4, 4});
    for (const MeshVertex3D& v : data.vertices)
        data.lightmap_uv.push_back(
            {0.05f + 0.9f * std::clamp(v.uv.x, 0.0f, 1.0f), 0.05f + 0.9f * std::clamp(v.uv.y, 0.0f, 1.0f)});
    return data;
}

Lights3D dark() {
    Lights3D l;
    l.sun.color = {};
    l.fill.color = {};
    l.ambient = {};
    return l;
}

// The floor baked under a green sky, its probes round it.
BakedLightingData3D green_bake(const MeshData3D& floor) {
    const BakeSurface3D surfaces[] = {{.mesh = &floor}};
    BakeSky3D sky;
    sky.above = sky.below = {0.0f, 0.5f, 0.0f};
    LightBakeSettings3D settings;
    settings.texels_per_unit = 4;
    settings.samples = 16;
    settings.bounces = 1;
    settings.probe_area = {{-3, -1, -3}, {3, 3, 3}};
    settings.probe_samples = 64;
    auto baked = bake_lighting(surfaces, dark(), sky, settings);
    assert(baked.ok());
    return std::move(*baked);
}

Rgb shoot(Renderer2D& r, Renderer3D& r3d, View3D& view, const Scene3D& scene, Vec3f at) {
    r.clear(colors::black);
    assert(r3d.render(view, scene));
    r.draw_texture(view.texture(), rect);
    std::vector<u8> pixels;
    Vec2i size;
    assert(r.read_rgba(rect, pixels, size) && size == extent);
    r.present();
    const auto p = scene.camera.project(at, rect);
    assert(p);
    const u8* c = &pixels[(std::size_t(p->pos.y) * std::size_t(extent.x) + std::size_t(p->pos.x)) * 4];
    return Rgb{c[0], c[1], c[2]};
}

// Under a red ambient light: the lightmapped floor green from its lightmap, the same floor without a placement green
// from the probes, a sphere without lightmap UVs green from the probes; without the bake, all red.
void test_lightmap_and_probes(Renderer2D& r, Renderer3D& r3d, bool taa) {
    View3D view = r3d.create_view(extent, {.samples = 1, .background = colors::black}).value_or({});
    const MeshData3D floor = floor_data();
    const Mesh3D floor_mesh = r3d.create_mesh(floor).value_or({});
    const Mesh3D ball = r3d.create_mesh(meshes::sphere(0.5f)).value_or({});
    assert(view && floor_mesh && ball);
    BakedLighting3D baked = r3d.create_baked_lighting(green_bake(floor)).value_or({});
    if (!baked) {
        std::cout << "baked lighting: not drawn on this device (skipped)\n";
        return;
    }
    assert(baked.placements() == 1 && baked.has_probes());
    const auto scene_with = [&](bool bake, u32 lightmap) {
        Scene3D scene;
        scene.lights = dark();
        scene.lights.ambient = {0.5f, 0.0f, 0.0f};
        scene.taa.enabled = taa;
        scene.camera = Camera3D::look_at({0, 5, 3}, {0, 0, 0}, 50);
        if (bake)
            scene.baked_lighting = baked;
        const MeshInstance3D copy{.transform = Mat4f::identity(), .lightmap = lightmap};
        assert(scene.draw_instanced(floor_mesh, std::span{&copy, 1}, {.color = colors::white, .roughness = 1.0f}));
        assert(scene.draw(ball, Mat4f::translation({1.2f, 0.6f, 0.8f}), {.color = colors::white, .roughness = 1.0f}));
        return scene;
    };
    const auto shots = [&](const Scene3D& scene) {
        Rgb a{}, b{};
        // With TAA, a few frames for the history to settle.
        for (int i = 0; i < (taa ? 8 : 1); ++i) {
            a = shoot(r, r3d, view, scene, {-1.0f, 0.0f, -1.0f});
            b = shoot(r, r3d, view, scene, {1.2f, 1.1f, 0.8f});
        }
        return std::pair{a, b};
    };
    const auto [lit_floor, lit_ball] = shots(scene_with(true, 1));
    const auto [probed_floor, probed_ball] = shots(scene_with(true, 0));
    const auto [plain_floor, plain_ball] = shots(scene_with(false, 1));
    std::cerr << (taa ? "taa " : "") << "floor lightmapped: " << lit_floor << " / by the probes: " << probed_floor
              << " / unbaked: " << plain_floor << "; ball: " << lit_ball << " / unbaked: " << plain_ball << "\n";
    assert(lit_floor.g > 100 && lit_floor.r < 30);
    assert(probed_floor.g > 100 && probed_floor.r < 30);
    assert(std::abs(lit_floor.g - probed_floor.g) < 40); // (the probes hold the same sky, from above)
    assert(lit_ball.g > 60 && lit_ball.r < 30);
    assert(plain_floor.r > 100 && plain_floor.g < 30);
    assert(plain_ball.r > 60 && plain_ball.g < 30);
    std::cout << "baked lighting: lightmap and probes" << (taa ? " (taa)" : "") << "\n";
}

// A Baked lamp over the floor is not drawn with the bake (whose lightmap does not have it: the floor stays as baked);
// Mixed, it is drawn; without the bake, Baked or not, it is drawn. The sun alike.
void test_baked_lights_not_drawn(Renderer2D& r, Renderer3D& r3d) {
    View3D view = r3d.create_view(extent, {.samples = 1, .background = colors::black}).value_or({});
    const MeshData3D floor = floor_data();
    const Mesh3D floor_mesh = r3d.create_mesh(floor).value_or({});
    BakedLighting3D baked = r3d.create_baked_lighting(green_bake(floor)).value_or({});
    if (!baked)
        return;
    const auto shot = [&](bool bake, LightBake3D lamp_mode, LightBake3D sun_mode, bool lamp, bool sun) {
        Scene3D scene;
        scene.lights = dark();
        if (lamp) {
            PointLightSource3D light;
            light.position = {0, 1, 0};
            light.color = {0, 0, 8};
            light.radius = 6;
            light.bake = lamp_mode;
            scene.lights.points.push_back(light);
        }
        if (sun) {
            scene.lights.sun.direction = {0, 1, 0};
            scene.lights.sun.color = {0, 0, 2};
            scene.lights.sun.bake = sun_mode;
        }
        scene.camera = Camera3D::look_at({0, 5, 3}, {0, 0, 0}, 50);
        if (bake)
            scene.baked_lighting = baked;
        const MeshInstance3D copy{.transform = Mat4f::identity(), .lightmap = 1};
        assert(scene.draw_instanced(floor_mesh, std::span{&copy, 1}, {.color = colors::white, .roughness = 1.0f}));
        return shoot(r, r3d, view, scene, {0, 0, 0});
    };
    const Rgb none = shot(true, LightBake3D::Baked, LightBake3D::Baked, false, false);
    const Rgb baked_lamp = shot(true, LightBake3D::Baked, LightBake3D::Baked, true, false);
    const Rgb mixed_lamp = shot(true, LightBake3D::Mixed, LightBake3D::Baked, true, false);
    const Rgb realtime_lamp = shot(true, LightBake3D::Realtime, LightBake3D::Baked, true, false);
    const Rgb unbaked_lamp = shot(false, LightBake3D::Baked, LightBake3D::Baked, true, false);
    const Rgb baked_sun = shot(true, LightBake3D::Baked, LightBake3D::Baked, false, true);
    const Rgb mixed_sun = shot(true, LightBake3D::Baked, LightBake3D::Mixed, false, true);
    std::cerr << "lamp: none " << none << " / baked " << baked_lamp << " / mixed " << mixed_lamp << " / realtime "
              << realtime_lamp << " / unbaked " << unbaked_lamp << "; sun: baked " << baked_sun << " / mixed "
              << mixed_sun << "\n";
    assert(std::abs(baked_lamp.b - none.b) < 4 && std::abs(baked_lamp.g - none.g) < 4);
    assert(mixed_lamp.b > none.b + 60 && realtime_lamp.b > none.b + 60 && unbaked_lamp.b > 60);
    assert(std::abs(baked_sun.b - none.b) < 4);
    assert(mixed_sun.b > none.b + 60);
    std::cout << "baked lighting: Baked lights not drawn, Mixed and Realtime drawn\n";
}

// A bake made by hand: its page red on the left half, blue on the right, its probes green. The lightmapped floor shows
// the page where its lightmap UVs fall (one side red, the other blue), the same floor without a placement the probes'
// green; the second of two pages (another layer) is read for a placement on it.
void test_lightmap_texels(Renderer2D& r, Renderer3D& r3d) {
    View3D view = r3d.create_view(extent, {.samples = 1, .background = colors::black}).value_or({});
    const Mesh3D floor_mesh = r3d.create_mesh(floor_data()).value_or({});
    BakedLightingData3D data;
    for (int p = 0; p < 2; ++p) {
        LightmapPage3D page;
        page.size = {16, p == 0 ? 16 : 8};
        for (int y = 0; y < page.size.y; ++y)
            for (int x = 0; x < page.size.x; ++x) {
                const bool left = x < page.size.x / 2;
                const LinearRgb c = p == 1 ? LinearRgb{0.5f, 0.5f, 0.5f}
                                    : left ? LinearRgb{0.5f, 0, 0}
                                           : LinearRgb{0, 0, 0.5f};
                page.rgb.insert(page.rgb.end(), {c.r, c.g, c.b});
            }
        data.pages.push_back(std::move(page));
    }
    data.placements.push_back({0, {1, 1, 0, 0}});
    data.placements.push_back({1, {1, 1, 0, 0}});
    data.probes.area = {{-3, -1, -3}, {3, 3, 3}};
    data.probes.count = {2, 2, 2};
    for (int i = 0; i < 8; ++i)
        data.probes.sh.insert(data.probes.sh.end(), {0, 0, 0, 0, 0.5f, 0, 0, 0, 0, 0, 0, 0});
    BakedLighting3D baked = r3d.create_baked_lighting(data).value_or({});
    if (!baked)
        return;
    // A surface shader that changes nothing (Material3D::surface): its own baked variants read the same.
    const std::filesystem::path source = std::filesystem::temp_directory_path() / "kin_baked_surface.surface.glsl";
    {
        std::ofstream out(source);
        out << "void kin_surface(inout KinSurface s) {}\n";
    }
    const SurfaceShader3D surface = r3d.compile_surface_shader(source).value_or({});
    std::filesystem::remove(source);
    Material3D material{.color = colors::white, .roughness = 1.0f};
    const auto shot = [&](u32 lightmap, Vec3f at) {
        Scene3D scene;
        scene.lights = dark();
        scene.camera = Camera3D::look_at({0, 6, 0.01f}, {0, 0, 0}, 50);
        scene.baked_lighting = baked;
        const MeshInstance3D copy{.transform = Mat4f::identity(), .lightmap = lightmap};
        assert(scene.draw_instanced(floor_mesh, std::span{&copy, 1}, material));
        return shoot(r, r3d, view, scene, at);
    };
    const Rgb a = shot(1, {-1.5f, 0, 0}), b = shot(1, {1.5f, 0, 0});
    const Rgb probes = shot(0, {0, 0, 0}), grey = shot(2, {1.5f, 0, 0});
    std::cerr << "page halves: " << a << " / " << b << "; probes: " << probes << "; second page: " << grey << "\n";
    const auto red = [](Rgb c) { return c.r > 120 && c.g < 20 && c.b < 20; };
    const auto blue = [](Rgb c) { return c.b > 120 && c.g < 20 && c.r < 20; };
    assert((red(a) && blue(b)) || (blue(a) && red(b)));
    assert(probes.g > 120 && probes.r < 20 && probes.b < 20);
    assert(grey.r > 120 && std::abs(grey.r - grey.g) < 8 && std::abs(grey.g - grey.b) < 8);
    std::cout << "baked lighting: the page's texels where the lightmap UVs fall\n";
    if (!surface) {
        std::cout << "baked lighting: surface shaders skipped (no glslc)\n";
        return;
    }
    material.surface = surface;
    const Rgb sa = shot(1, {-1.5f, 0, 0}), sb = shot(1, {1.5f, 0, 0}), sp = shot(0, {0, 0, 0});
    std::cerr << "surface: page halves " << sa << " / " << sb << "; probes " << sp << "\n";
    assert(sa.r == a.r && sa.b == a.b && sb.r == b.r && sb.b == b.b && sp.g == probes.g);
    std::cout << "baked lighting: surface shaders read the bake\n";

    // Custom shaders made to read it (KIN_BAKED_LIGHT): kin_lit's diffuse light is the bake's, kin_baked_light gives
    // it; their own extra inputs start at 14.
    const auto custom = [&](const char* body) {
        const std::filesystem::path file = std::filesystem::temp_directory_path() / "kin_baked_custom.frag.glsl";
        {
            std::ofstream out(file);
            out << "#version 450 core\n#define KIN_BAKED_LIGHT\n#include \"kin_material.inc.glsl\"\n" << body;
        }
        auto made = r3d.compile_material_shader(file);
        std::filesystem::remove(file);
        assert(made.ok());
        return *made;
    };
    material.surface = {};
    material.shader = custom("void main() { vec4 b = kin_base_color();\n"
                             "FragColor = kin_output(kin_lit(b.rgb, kin_normal(), 0.0, 1.0, 1.0), b.a); }\n");
    const Rgb ca = shot(1, {-1.5f, 0, 0}), cb = shot(1, {1.5f, 0, 0}), cp = shot(0, {0, 0, 0});
    material.shader = custom("void main() { vec3 light; float share;\n"
                             "kin_baked_light(kin_normal(), light, share); FragColor = kin_output(light, 1.0); }\n");
    const Rgb da = shot(1, {-1.5f, 0, 0}), dp = shot(0, {0, 0, 0});
    // With an input of its own at 14: a half-grey picture times the bake.
    material.shader = custom("layout(set = 2, binding = 14) uniform sampler2D tint;\n"
                             "void main() { vec3 light; float share; kin_baked_light(kin_normal(), light, share);\n"
                             "FragColor = kin_output(light * texture(tint, vec2(0.5)).rgb, 1.0); }\n");
    const u8 half_grey[4] = {128, 128, 128, 255};
    const Texture tint = r.create_texture_from_rgba(half_grey, 1, 1);
    const std::array textures{tint};
    auto inputs = r3d.create_material_inputs(material.shader, textures, {});
    assert(inputs.ok());
    material.inputs = *inputs;
    const Rgb ea = shot(1, {-1.5f, 0, 0});
    std::cerr << "custom: kin_lit " << ca << " / " << cb << " / probes " << cp << "; kin_baked_light " << da << " / "
              << dp << "; with an input " << ea << "\n";
    assert(std::abs(ca.r - a.r) <= 2 && ca.b == a.b && std::abs(cb.b - b.b) <= 2 && cb.r == b.r && cp.g > 150);
    assert(std::abs(da.r - std::max(a.r, a.b)) < 12 || std::abs(da.b - std::max(a.r, a.b)) < 12);
    assert(dp.g > 150 && dp.r < 10);
    assert((ea.r > 60 && ea.r < da.r - 30) || (ea.b > 60 && ea.b < da.b - 30));
    std::cout << "baked lighting: custom shaders read the bake\n";
}

// The probes' visibility on the GPU: a closed dark room under a white sky, the probe grid straddling its walls (the
// walls baked, not drawn). A floor inside, lit by the probes, stays dark by the wall; with the probes' visibility left
// out it is lit through it.
void test_probes_through_walls(Renderer2D& r, Renderer3D& r3d) {
    View3D view = r3d.create_view(extent, {.samples = 1, .background = colors::black}).value_or({});
    const Mesh3D floor_mesh = r3d.create_mesh(meshes::plane({3.6f, 3.6f})).value_or({});
    std::vector<MeshData3D> walls;
    std::vector<BakeSurface3D> surfaces;
    const Vec3f normals[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (const Vec3f& n : normals) {
        MeshData3D wall = meshes::plane({4, 4});
        // The plane faces +y: turned to face -n (into the room), moved out along n.
        const Vec3f in = -n;
        const Vec3f axis = std::fabs(in.y) > 0.9f ? Vec3f{1, 0, 0} : normalize(cross({0, 1, 0}, in));
        const f32 angle = std::fabs(in.y) > 0.9f ? (in.y > 0 ? 0.0f : 180.0f)
                                                 : std::acos(std::clamp(in.y, -1.0f, 1.0f)) * 180.0f / 3.14159265f;
        walls.push_back(wall);
        surfaces.push_back({.transform = Mat4f::translation(n * 2.0f) * Mat4f::rotation(Quatf::axis_angle(axis, angle)),
                            .albedo = {0, 0, 0},
                            .lightmapped = false,
                            .double_sided = true});
    }
    for (std::size_t i = 0; i < 6; ++i)
        surfaces[i].mesh = &walls[i];
    BakeSky3D sky;
    sky.above = sky.below = {1, 1, 1};
    LightBakeSettings3D settings;
    settings.probe_area = {{-3.5f, -3.5f, -3.5f}, {3.5f, 3.5f, 3.5f}};
    settings.probe_spacing = 1.0f;
    settings.probe_samples = 512;
    auto data = bake_lighting(surfaces, dark(), sky, settings);
    assert(data.ok());
    const auto shot = [&](const BakedLightingData3D& bake) {
        BakedLighting3D baked = r3d.create_baked_lighting(bake).value_or({});
        assert(baked);
        Scene3D scene;
        scene.lights = dark();
        scene.camera = Camera3D::look_at({-1.0f, 1.9f, 0.3f}, {-1.7f, 0.0f, 0.3f}, 60);
        scene.baked_lighting = baked;
        assert(scene.draw(floor_mesh, Mat4f::translation({0, 0.3f, 0}), {.color = colors::white, .roughness = 1.0f}));
        return shoot(r, r3d, view, scene, {-1.7f, 0.3f, 0.3f});
    };
    const Rgb seen = shot(*data);
    BakedLightingData3D blind = *data;
    blind.probes.visibility.clear();
    const Rgb leaked = shot(blind);
    std::cerr << "by the wall: " << seen << " / without the probes' visibility: " << leaked << "\n";
    assert(seen.r < 40 && leaked.r > seen.r + 40);
    std::cout << "baked lighting: no probe light through walls\n";
}

// Refusals: a placement on a page the bake has not, a probe grid whose values do not match.
void test_refusals(Renderer3D& r3d) {
    BakedLightingData3D bad;
    bad.placements.push_back({3, {0.5f, 0.5f, 0, 0}});
    assert(!r3d.create_baked_lighting(bad).ok());
    BakedLightingData3D probes;
    probes.probes.count = {2, 2, 2};
    probes.probes.sh.resize(5);
    assert(!r3d.create_baked_lighting(probes).ok());
    std::cout << "baked lighting: refusals\n";
}

} // namespace

int main() {
    App app;
    auto& window = app.create_window({.title = "baked lighting", .width = 160, .height = 120, .hidden = true});
    Renderer2D r{make_render_backend(window, false, true)};
    Renderer3D r3d{r};
    const bool require = std::getenv("KIN_REQUIRE_GPU_TESTS") != nullptr;
    if (!r3d.available())
        return require ? 1 : 77;
    test_refusals(r3d);
    test_lightmap_texels(r, r3d);
    test_lightmap_and_probes(r, r3d, false);
    test_lightmap_and_probes(r, r3d, true);
    test_baked_lights_not_drawn(r, r3d);
    test_probes_through_walls(r, r3d);
}
