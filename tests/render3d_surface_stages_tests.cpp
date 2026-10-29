#include "surface_stage_shaders.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <kin/platform/app.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/renderer/graph_resources.hpp>
#include <kin/renderer/renderer2d.hpp>
using namespace kin;
namespace {
constexpr Vec2i size{160, 120};
constexpr Rectf rect{0, 0, 160, 120};
MeshData3D quad() {
    MeshData3D m;
    m.vertices = {{{-1, -1, 0}, {0, 0, 1}, {0, 0}},
                  {{1, -1, 0}, {0, 0, 1}, {1, 0}},
                  {{1, 1, 0}, {0, 0, 1}, {1, 1}},
                  {{-1, 1, 0}, {0, 0, 1}, {0, 1}}};
    m.indices = {0, 1, 2, 0, 2, 3};
    return m;
}
struct Fixture {
    Renderer2D& r;
    Renderer3D r3d{r};
    Texture a = r.create_render_attachment(size, TextureFormat::Rgba16Float),
            b = r.create_render_attachment(size, TextureFormat::Rgba16Float);
    Texture depth = r.create_render_attachment(size, TextureFormat::Depth32Float),
            soft_depth = r.create_render_attachment(size, TextureFormat::Depth32Float),
            normals = r.create_render_attachment(size, TextureFormat::Rg16Float);
    Mesh3D mesh = r3d.create_mesh(quad()).value_or({});
    ShaderHandle probe;
    explicit Fixture(Renderer2D& x) : r(x) {
        auto bytes = surface_stage_shaders("depth.frag.spv");
        probe = r.create_shader({.spirv = {bytes.data(), u32(bytes.size())}, .num_samplers = 1});
        assert(a && b && depth && soft_depth && normals && mesh && probe);
    }
    std::vector<u8> read(const Texture& image) {
        r.clear(colors::black);
        r.draw_texture(image, rect);
        std::vector<u8> out;
        Vec2i actual;
        assert(r.read_rgba(rect, out, actual) && actual == size);
        r.present();
        return out;
    }
    RenderTarget depth_picture() {
        auto image = r.create_render_target(size);
        {
            auto scope = r.scoped_render_target(image);
            r.clear(colors::black);
            r.draw_shader_surface(rect, probe, {}, depth);
        }
        return image;
    }
    std::vector<u8> stock(const Scene3D& scene, Color bg = colors::transparent) {
        auto view = r3d.create_view(size, {.samples = 1, .background = bg, .hdr = true}).value_or({});
        assert(view);
        r.clear(colors::black);
        assert(r3d.render(view, scene));
        return read(view.texture());
    }
    std::vector<u8> custom(const Scene3D& scene, SurfaceStage3D stage = SurfaceStage3D::All, bool particles = true,
                           bool out_of_place = false, bool soft = false, Color bg = colors::transparent) {
        r.clear(colors::black);
        const auto p = r3d.prepare(scene).value_or({});
        assert(p);
        if (soft)
            assert(r3d.render_geometry_prepared({soft_depth, normals}, p));
        Texture result = a;
        if (stage == SurfaceStage3D::All) {
            assert(r3d.render_surface(
                {.depth = depth, .linear_colour = a}, p,
                {.background = bg, .particles = particles, .soft_depth = soft ? soft_depth : Texture{}}));
        } else {
            assert(r3d.render_surface({.depth = depth, .linear_colour = a}, p,
                                      {.background = bg, .stage = SurfaceStage3D::Opaque}));
            assert(r3d.debug_stats().particles_drawn == 0 && r3d.debug_stats().transparent_draw_runs == 0);
            auto before_depth = depth_picture();
            const auto opaque_copy = r.create_render_target(size), opaque_after = r.create_render_target(size);
            assert(r3d.render_output(opaque_copy, a));
            result = out_of_place ? b : a;
            assert(r3d.render_surface({.depth = depth, .linear_colour = result}, p,
                                      {.stage = SurfaceStage3D::Transparent,
                                       .particles = particles,
                                       .colour_input = out_of_place ? a : Texture{},
                                       .soft_depth = soft ? soft_depth : Texture{}}));
            assert(r3d.debug_stats().bounds_instance_tests == 0 && r3d.debug_stats().device_waits == 0 &&
                   r3d.debug_stats().surface_colour_copies == u32(out_of_place));
            assert(r3d.debug_stats().shadow_draws == 0 && r3d.debug_stats().local_shadow_draws == 0 &&
                   r3d.debug_stats().taa_dispatches == 0 && r3d.debug_stats().gi_dispatches == 0);
            auto after_depth = depth_picture();
            if (out_of_place) {
                assert(r3d.render_output(opaque_after, a));
                const auto x = read(opaque_copy.texture()), y = read(opaque_after.texture());
                assert(x == y);
            }
            const auto x = read(before_depth.texture()), y = read(after_depth.texture());
            assert(x == y);
        }
        auto output = r.create_render_target(size);
        assert(r3d.render_output(output, result));
        return read(output.texture());
    }
};
void equal(const std::vector<u8>& a, const std::vector<u8>& b, const char* why, int tolerance = 0) {
    assert(a.size() == b.size());
    int largest = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        largest = std::max(largest, std::abs(int(a[i]) - int(b[i])));
    if (largest > tolerance) {
        std::cerr << why << " largest=" << largest << '\n';
        assert(largest <= tolerance);
    }
}
Scene3D mixed(Fixture& f, int variant) {
    Scene3D scene;
    scene.camera = Camera3D::look_at({0, 0, 6}, {});
    Material3D green{.color = Color::rgba(0, 255, 0, 128), .unlit = true, .blend = Blend3D::Alpha};
    auto copies = scene.add_instances(f.mesh, 2, green);
    assert(copies);
    (*copies)[0].transform = Mat4f::translation({0, 0, 1});
    (*copies)[1].transform = Mat4f::translation({0, 0, -2});
    assert(scene.draw(f.mesh, {}, {.color = Color::rgba(0, 0, 255, 128), .unlit = true, .blend = Blend3D::Alpha}));
    assert(scene.draw(f.mesh, Mat4f::translation({1.8f, 0, -3}), {.color = Color::rgb(190, 140, 40), .unlit = true}));
    std::array<ParticleSprite3D, 3> sprites{
        {{{0, 0, 2}, 2, 0, {1, 0, 0}, .5f}, {{0, 0, -1}, 2, 0, {1, 0, 0}, .5f}, {{100, 0, 0}, 2, 0, {1, 0, 0}, .5f}}};
    assert(scene.draw_particles({.softness = 0, .sort = variant != 1}, sprites));
    ParticleSprite3D spark{{0, 0, -3}, 2, 0, {.3f, .1f, .4f}, 1};
    assert(scene.draw_particles({.blend = ParticleBlend3D::Additive, .softness = 0}, {&spark, 1}));
    const u8 pixels[]{255, 0, 128, 128};
    auto image = f.r.create_texture_from_rgba(pixels, {1, 1});
    ParticleSprite3D glow{{.6f, .4f, .5f}, .7f, 0, {.5f, .5f, .5f}, .4f};
    assert(
        scene.draw_particles({.blend = ParticleBlend3D::Premultiplied, .texture = image, .softness = 0}, {&glow, 1}));
    if (variant == 2) {
        auto& overlay = scene.overlay();
        overlay.camera = Camera3D::look_at({0, 0, 6}, {});
        assert(overlay.draw(f.mesh, Mat4f::translation({.8f, -.3f, 2}) * Mat4f::scaling({.4f, .4f, .4f}),
                            {.color = Color::rgb(240, 100, 20), .unlit = true}));
        assert(overlay.draw(f.mesh, Mat4f::translation({-.5f, .3f, 2}) * Mat4f::scaling({.4f, .4f, .4f}),
                            {.color = Color::rgba(20, 180, 240, 150), .unlit = true, .blend = Blend3D::Alpha}));
    }
    if (variant == 3) {
        scene.weather.snow.intensity = .3f;
        scene.weather.snow.max_particles = 120;
        scene.weather.snow.look.soft = false;
        scene.weather.snow.look.lit = .3f;
        scene.weather.time = 1.7;
    }
    if (variant == 4) {
        scene.lights.points.push_back({.position = {-2, 3, 3}, .color = {8, 4, 2}, .radius = 12});
        scene.lights.spots.push_back(
            {.position = {2, 4, 3}, .direction = {-.2f, -.5f, -1}, .color = {4, 8, 10}, .radius = 12});
        ParticleSprite3D lit{{0, 0, 3}, 1, 0, {.4f, .3f, .2f}, .3f};
        assert(
            scene.draw_particles({.softness = 0, .lit = 1, .receives_shadow = true, .casts_shadow = true}, {&lit, 1}));
    }
    return scene;
}
void backgrounds(Fixture& f) {
    Scene3D empty;
    const auto bg = Color::rgba(40, 80, 120, 150);
    equal(f.stock(empty, bg), f.custom(empty, SurfaceStage3D::All, false, false, false, bg), "explicit HDR background");
    std::cout << "surface stages: independent linear HDR background\n";
}
void composition(Fixture& f) {
    for (int working = 0; working < 3; ++working) {
        assert(f.r.set_color_space(working ? ColorSpace::Linear : ColorSpace::Gamma, working == 2));
        for (int variant = 0; variant < 5; ++variant) {
            auto scene = mixed(f, variant);
            const auto bg = Color::rgba(15, 25, 40, 180);
            auto stock = f.stock(scene, bg);
            equal(stock, f.custom(scene, SurfaceStage3D::All, true, false, false, bg), "combined particles");
            equal(stock, f.custom(scene, SurfaceStage3D::Opaque, true, false, false, bg), "in-place split");
            equal(stock, f.custom(scene, SurfaceStage3D::Opaque, true, true, false, bg), "versioned split");
        }
    }
    assert(f.r.set_color_space(ColorSpace::Gamma, false));
    auto scene = mixed(f, 0);
    const auto with = f.custom(scene);
    const auto legacy = f.custom(scene, SurfaceStage3D::All, false);
    assert(with != legacy);
    std::cout << "surface stages: exact stock/combined/in-place/versioned parity, glass/smoke/additive/premultiplied, "
                 "overlays, weather and local lighting\n";
}
void working_targets(Fixture& f) {
    for (int working = 0; working < 3; ++working) {
        assert(f.r.set_color_space(working ? ColorSpace::Linear : ColorSpace::Gamma, working == 2));
        const auto target = f.r.create_render_target(size), composed = f.r.create_render_target(size);
        auto view = f.r3d.create_view(size, {.samples = 1}).value_or({});
        const auto scene = mixed(f, 0);
        f.r.clear(colors::black);
        assert(f.r3d.render(view, scene));
        const auto expected = f.read(view.texture());
        for (bool copy : {false, true}) {
            f.r.clear(colors::black);
            const auto p = f.r3d.prepare(scene).value_or({});
            assert(f.r3d.render_surface({target, f.depth}, p, {.stage = SurfaceStage3D::Opaque}));
            assert(f.r3d.render_surface({copy ? composed : target, f.depth}, p,
                                        {.stage = SurfaceStage3D::Transparent,
                                         .particles = true,
                                         .colour_input = copy ? target.texture() : Texture{}}));
            equal(expected, f.read((copy ? composed : target).texture()), "working colour split");
        }
    }
    assert(f.r.set_color_space(ColorSpace::Gamma, false));
    std::cout << "surface stages: working-colour attachment parity in gamma/linear SDR and HDR\n";
}
void soft(Fixture& f) {
    Scene3D scene;
    scene.camera = Camera3D::look_at({0, 0, 6}, {});
    assert(scene.draw(f.mesh, {}, {.color = Color::rgb(120, 80, 40)}));
    ParticleSprite3D sprite{{0, 0, .05f}, 1.5f, 0, {0, 0, 1}, .8f};
    assert(scene.draw_particles({.softness = 0, .soft = true, .soft_distance = .5f}, {&sprite, 1}));
    const auto stock = f.stock(scene);
    equal(stock, f.custom(scene, SurfaceStage3D::All, true, false, true), "soft combined", 1);
    equal(stock, f.custom(scene, SurfaceStage3D::Opaque, true, true, true), "soft split", 1);
    std::cout << "surface stages: explicit sampled soft depth and intersection fade\n";
}
void mesh_profiles(Fixture& f) {
    auto data = quad();
    data.skin.assign(data.vertices.size(), {});
    const auto skinned = f.r3d.create_mesh(data).value_or({});
    const std::array pose{Mat4f::translation({.2f, 0, 0})};
    const u8 pixels[]{255, 255, 255, 0, 255, 255, 255, 255};
    const auto mask = f.r.create_texture_from_rgba(pixels, {2, 1});
    const auto batch =
        InstanceBatch3D::create(f.mesh, std::vector<MeshInstance3D>{{.transform = Mat4f::translation({-1.3f, 0, -1})},
                                                                    {.transform = Mat4f::translation({1.3f, 0, -1})}})
            .value_or({});
    for (bool clusters : {false, true}) {
        const auto sphere =
            f.r3d.create_mesh(meshes::sphere(1, 24, 12), {.levels = 3, .clusters = clusters}).value_or({});
        for (bool gpu : {false, true}) {
            Scene3D scene;
            scene.camera = Camera3D::look_at({0, 0, 6}, {});
            if (!gpu) {
                scene.camera.projection = Projection3D::Orthographic;
                scene.camera.height = 6;
            }
            scene.lod.enabled = true;
            scene.debug.force_level = 0;
            scene.debug.gpu_culling = gpu;
            assert(scene.draw(sphere, Mat4f::translation({0, 0, -2})));
            assert(scene.draw_skinned(skinned, pose, Mat4f::translation({-.5f, 0, 0}),
                                      {.blend = Blend3D::Mask, .texture = mask}));
            assert(scene.draw_instanced(batch, {.color = Color::rgba(180, 80, 220, 100), .blend = Blend3D::Alpha}));
            assert(scene.draw(f.mesh, Mat4f::translation({0, 0, 1}), {.wireframe = true}));
            ParticleSprite3D sprite{{0, 0, .5f}, 1, 0, {1, .3f, 0}, .5f};
            assert(scene.draw_particles({.softness = 0}, {&sprite, 1}));
            const auto stock = f.stock(scene);
            equal(stock, f.custom(scene, SurfaceStage3D::All), "mesh profile combined");
            equal(stock, f.custom(scene, SurfaceStage3D::Opaque, true, true), "mesh profile split");
        }
    }
    std::cout << "surface stages: masked skinning, retained transparency, wireframe, CPU/GPU clusters and orthographic "
                 "camera\n";
}
void shadow_inputs(Fixture& f) {
    const auto box = f.r3d.create_mesh(meshes::box({2, 1, 2})).value_or({});
    const auto sun_depth = f.r.create_render_attachment({128, 128}, TextureFormat::Depth32Float, 1);
    const auto local_depth = f.r.create_render_attachment({128, 128}, TextureFormat::Depth32Float, 9);
    for (int kind = 0; kind < 3; ++kind) {
        Scene3D scene;
        scene.camera = Camera3D::look_at({0, 0, 6}, {});
        scene.lights.sun = {.direction = {0, 1, .6f}, .color = kind == 0 ? LinearRgb{1, 1, 1} : LinearRgb{}};
        scene.lights.ambient = {.02f, .02f, .02f};
        scene.lights.shadow_area = {{-4, -2, -4}, {4, 5, 4}};
        scene.lights.shadow_resolution = scene.lights.local_shadow_resolution = 128;
        if (kind == 1)
            scene.lights.spots.push_back({.position = {0, 4, 2.4f},
                                          .direction = {0, -1, -.6f},
                                          .color = {30, 30, 30},
                                          .radius = 15,
                                          .shadow_enabled = true});
        if (kind == 2)
            scene.lights.points.push_back(
                {.position = {0, 4, 2.4f}, .color = {30, 30, 30}, .radius = 15, .shadow_enabled = true});
        // The visible mesh is unlit: any colour difference must come from the receiver sprite.
        assert(scene.draw(box, Mat4f::translation({0, 2, 1.2f}), {.unlit = true}));
        ParticleSprite3D sprite{{0, 0, 0}, 3, 0, {.6f, .6f, .6f}, 1};
        assert(scene.draw_particles({.softness = 0, .lit = 1, .receives_shadow = true}, {&sprite, 1}));
        const auto expected = f.stock(scene); // seeds private maps which public calls must never borrow
        const auto draw = [&](bool supplied, bool split) {
            f.r.clear();
            auto p = f.r3d.prepare(scene).value_or({});
            SunShadow3D sun;
            LocalShadows3D local;
            if (kind == 0)
                sun = f.r3d.render_sun_shadow(sun_depth, p, size).value_or({});
            else {
                const std::array requests{LocalShadowRequest3D{
                    .kind = kind == 1 ? LocalShadowKind3D::Spot : LocalShadowKind3D::Point, .first_layer = 2}};
                local = f.r3d.render_local_shadows(local_depth, p, requests, size).value_or({});
            }
            assert(kind == 0 ? bool(sun.depth) : bool(local.depth));
            SurfaceOptions3D options{.sun_shadow = supplied && kind == 0 ? &sun : nullptr,
                                     .local_shadows = supplied && kind != 0 ? &local : nullptr,
                                     .particles = true};
            if (split) {
                auto opaque = options;
                opaque.stage = SurfaceStage3D::Opaque;
                opaque.particles = false;
                assert(f.r3d.render_surface({.depth = f.depth, .linear_colour = f.a}, p, opaque));
                options.stage = SurfaceStage3D::Transparent;
            }
            assert(f.r3d.render_surface({.depth = f.depth, .linear_colour = f.a}, p, options));
            assert(f.r3d.debug_stats().particle_shadow_batches == u32(supplied));
            assert(f.r3d.debug_stats().shadow_draws == 0 && f.r3d.debug_stats().local_shadow_draws == 0);
            const auto target = f.r.create_render_target(size);
            assert(f.r3d.render_output(target, f.a));
            return f.read(target.texture());
        };
        equal(expected, draw(true, false), "explicit sprite shadows");
        equal(expected, draw(true, true), "explicit split sprite shadows");
        assert(expected != draw(false, true));
    }
    std::cout
        << "surface stages: explicit sun/spot/point sprite shadow reception, custom layers and no private fallback\n";
}
void validation(Fixture& f) {
    Scene3D scene;
    scene.camera = Camera3D::look_at({0, 0, 6}, {});
    assert(scene.draw(f.mesh, {}));
    ParticleSprite3D sprite{{0, 0, 1}, 1, 0, {1, 1, 1}, .5f};
    assert(scene.draw_particles({.softness = 0, .soft = true}, {&sprite, 1}));
    f.r.clear(colors::black);
    const auto p = f.r3d.prepare(scene).value_or({});
    assert(p);
    SurfaceTargets3D targets{.depth = f.depth, .linear_colour = f.a};
    assert(!f.r3d.render_surface(targets, p, {.stage = SurfaceStage3D(99)}));
    assert(!f.r3d.render_surface(targets, p, {.stage = SurfaceStage3D::Opaque, .particles = true}));
    assert(!f.r3d.render_surface(targets, p, {.particles = true}));
    assert(!f.r3d.render_surface(targets, p, {.colour_input = f.b}));
    assert(!f.r3d.render_surface(targets, p, {.soft_depth = f.soft_depth}));
    assert(!f.r3d.render_surface(targets, p,
                                 {.stage = SurfaceStage3D::Transparent, .particles = true, .soft_depth = f.depth}));
    assert(!f.r3d.render_surface(targets, p, {.stage = SurfaceStage3D::Transparent, .colour_input = f.a}));
    const auto wrong = f.r.create_render_attachment({80, 60}, TextureFormat::Depth32Float);
    const auto array = f.r.create_render_attachment(size, TextureFormat::Depth32Float, 1);
    assert(!f.r3d.render_surface(targets, p,
                                 {.stage = SurfaceStage3D::Transparent, .particles = true, .soft_depth = wrong}));
    assert(!f.r3d.render_surface(targets, p,
                                 {.stage = SurfaceStage3D::Transparent, .particles = true, .soft_depth = array}));
    assert(f.r3d.render_geometry_prepared({f.soft_depth, f.normals}, p));
    assert(f.r3d.render_surface(targets, p, {.particles = true, .soft_depth = f.soft_depth}));
    assert(f.r3d.diagnostics().empty());
    f.r.present();
    Scene3D feedback;
    assert(feedback.draw_particles({.texture = f.a}, {&sprite, 1}));
    f.r.clear();
    auto bad = f.r3d.prepare(feedback).value_or({});
    assert(bad && !f.r3d.render_surface(targets, bad, {.particles = true}));
    f.r.present();
    std::cout
        << "surface stages: invalid stages, feedback, missing/incompatible soft inputs and capability diagnostics\n";
}
void particles_only_and_lifetime(Fixture& f) {
    const auto output = f.r.create_render_target(size);
    const u8 red[]{255, 0, 0, 255}, blue[]{0, 0, 255, 255};
    auto texture = f.r.create_texture_from_rgba(red, {1, 1});
    Scene3D scene;
    scene.camera = Camera3D::look_at({0, 0, 6}, {});
    ParticleSprite3D p{{0, 0, 0}, 2, 0, {1, 1, 1}, 1};
    assert(scene.draw_particles({.texture = texture, .softness = 0}, {&p, 1}));
    f.r.clear();
    const auto prepared = f.r3d.prepare(scene).value_or({});
    assert(prepared);
    assert(f.r3d.render_surface({.depth = f.depth, .linear_colour = f.a}, prepared, {.stage = SurfaceStage3D::Opaque}));
    // Particles are deliberately live inputs, independent of prepared mesh revisions.
    ParticleSprite3D extra{{2, 0, 0}, 1, 0, {0, 1, 0}, .5f};
    assert(scene.draw_particles({.softness = 0}, {&extra, 1}));
    assert(prepared);
    assert(f.r3d.render_surface({.depth = f.depth, .linear_colour = f.a}, prepared,
                                {.stage = SurfaceStage3D::Transparent, .particles = true}));
    assert(f.r3d.debug_stats().particles_drawn == 2);
    assert(f.r3d.render_output(output, f.a));
    assert(f.r.update_texture(texture, {0, 0}, {1, 1}, blue));
    assert(!prepared);
    auto old = f.read(output.texture());
    assert(old[(60 * 160 + 80) * 4] > 200 && old[(60 * 160 + 80) * 4 + 2] == 0);
    f.r.clear();
    {
        Scene3D temporary;
        temporary.camera = scene.camera;
        auto data = f.r.create_texture_from_rgba(red, {1, 1});
        assert(temporary.draw_particles({.texture = data, .softness = 0}, {&p, 1}));
        const auto q = f.r3d.prepare(temporary).value_or({});
        assert(q && f.r3d.render_surface({.depth = f.depth, .linear_colour = f.a}, q, {.particles = true}));
    }
    Scene3D empty;
    const auto e = f.r3d.prepare(empty).value_or({});
    assert(e && f.r3d.render_surface({.depth = f.soft_depth, .linear_colour = f.b}, e));
    assert(f.r3d.render_output(output, f.a));
    old = f.read(output.texture());
    assert(old[(60 * 160 + 80) * 4] > 200);
    std::cout << "surface stages: particle-only preparation, live particle submissions, queued texture lifetime and "
                 "ordered CPU uploads\n";
}
void history(Fixture& f) {
    auto a = f.r3d.create_view(size, {.samples = 1, .hdr = true}).value_or({}),
         b = f.r3d.create_view(size, {.samples = 1, .hdr = true}).value_or({});
    auto scene = mixed(f, 0);
    scene.taa.enabled = true;
    assert(scene.draw(f.mesh, Mat4f::translation({-1.8f, 0, -1})));
    for (int i = 0; i < 8; ++i) {
        f.r.clear();
        assert(f.r3d.render(a, scene));
        const bool reset = f.r3d.debug_stats().taa_reset;
        assert(f.r3d.debug_stats().taa_dispatches > 0 && reset == (i == 0));
        assert(f.r3d.debug_stats().particle_batches > scene.particle_draws().size());
        auto other = mixed(f, i % 5);
        other.weather.time = i * .1;
        const auto p = f.r3d.prepare(other).value_or({});
        assert(p);
        assert(f.r3d.render_surface({.depth = f.depth, .linear_colour = f.a}, p, {.stage = SurfaceStage3D::Opaque}));
        assert(f.r3d.render_surface({.depth = f.depth, .linear_colour = f.a}, p,
                                    {.stage = SurfaceStage3D::Transparent, .particles = true}));
        assert(f.r3d.render(b, scene) && f.r3d.debug_stats().taa_reset == reset);
        const auto x = f.read(a.texture()), y = f.read(b.texture());
        equal(x, y, "stock particle history isolation");
    }
    const auto tick = [&] {
        f.r.clear();
        const auto p = f.r3d.prepare(scene).value_or({});
        assert(p &&
               f.r3d.render_surface({.depth = f.depth, .linear_colour = f.a}, p, {.stage = SurfaceStage3D::Opaque}));
        assert(f.r3d.render_surface({.depth = f.depth, .linear_colour = f.b}, p,
                                    {.stage = SurfaceStage3D::Transparent, .particles = true, .colour_input = f.a}));
        assert(f.r3d.debug_stats().bounds_instance_tests == 0 && f.r3d.debug_stats().instance_upload_bytes == 0 &&
               f.r3d.debug_stats().device_waits == 0);
        f.r.present();
    };
    for (int i = 0; i < 3; ++i)
        tick();
    const auto before = f.r.backend_stats();
    for (int i = 0; i < 8; ++i)
        tick();
    const auto after = f.r.backend_stats();
    assert(after.gpu_memory.allocations - before.gpu_memory.allocations ==
           after.upload_staging_allocations - before.upload_staging_allocations);
    assert(after.upload_staging_bytes <= before.upload_staging_bytes);
    std::cout << "surface stages: stock particle TAA isolation, shared uploads and warmed resource stability\n";
}
void foreign(Fixture& f, App& app) {
    auto& w = app.create_window({.title = "foreign stages", .width = 64, .height = 64, .hidden = true});
    Renderer2D other{make_render_backend(w, false, true)};
    other.clear();
    other.present();
    const u8 px[]{255, 255, 255, 255};
    auto image = other.create_texture_from_rgba(px, {1, 1});
    auto depth = other.create_render_attachment(size, TextureFormat::Depth32Float),
         colour = other.create_render_attachment(size, TextureFormat::Rgba16Float);
    Scene3D scene;
    ParticleSprite3D sprite{{0, 0, 0}, 1};
    assert(scene.draw_particles({.texture = image}, {&sprite, 1}));
    f.r.clear();
    auto p = f.r3d.prepare(scene).value_or({});
    assert(p && !f.r3d.render_surface({.depth = f.depth, .linear_colour = f.a}, p, {.particles = true}));
    assert(!f.r3d.render_surface({.depth = f.depth, .linear_colour = f.a}, p,
                                 {.stage = SurfaceStage3D::Transparent, .colour_input = colour}));
    assert(
        !f.r3d.render_surface({.depth = f.depth, .linear_colour = f.a}, p, {.particles = true, .soft_depth = depth}));
    f.r.present();
}
} // namespace
int main(int argc, char** argv) {
    App app;
    auto& w = app.create_window({.title = "surface stages", .width = size.x, .height = size.y, .hidden = true});
    Renderer2D r{make_render_backend(w, false, true)};
    Renderer3D r3d{r};
    if (!r3d.capabilities().surface_stages || !r3d.capabilities().surface_particles)
        return std::getenv("KIN_REQUIRE_GPU_TESTS") ? 1 : 77;
    bool foreign_test = argc > 1 && std::string_view(argv[1]) == "--foreign-device";
    if (foreign_test && !r3d.capabilities().software && !std::getenv("KIN_TEST_LIVE_GPU_DEVICES"))
        return 77;
    Fixture f{r};
    if (foreign_test) {
        foreign(f, app);
        return 0;
    }
    backgrounds(f);
    composition(f);
    working_targets(f);
    soft(f);
    mesh_profiles(f);
    shadow_inputs(f);
    validation(f);
    particles_only_and_lifetime(f);
    history(f);
}
