#include "geometry_probe.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <kin/platform/app.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/renderer/graph_resources.hpp>
#include <kin/renderer/renderer2d.hpp>
using namespace kin;
namespace {
constexpr Vec2i extent{160, 120};
constexpr Rectf rect{0, 0, 160, 120};
MeshData3D quad() {
    MeshData3D data;
    data.vertices = {{{-1, -1, 0}, {0, 0, 1}, {0, 0}},
                     {{1, -1, 0}, {0, 0, 1}, {1, 0}},
                     {{1, 1, 0}, {0, 0, 1}, {1, 1}},
                     {{-1, 1, 0}, {0, 0, 1}, {0, 1}}};
    data.indices = {0, 1, 2, 0, 2, 3};
    return data;
}
struct Fixture {
    Renderer2D& renderer;
    Renderer3D r3d{renderer};
    GeometryTargets3D targets{renderer.create_render_attachment(extent, TextureFormat::Depth32Float),
                              renderer.create_render_attachment(extent, TextureFormat::Rg16Float)};
    Mesh3D mesh = r3d.create_mesh(quad()).value_or({});
    ShaderHandle probe;
    Fixture(Renderer2D& r) : renderer(r) {
        const auto bytes = geometry_probe("geometry_probe.frag.spv");
        probe = renderer.create_shader(
            {.spirv = {bytes.data(), static_cast<u32>(bytes.size())}, .num_samplers = 2, .num_uniform_buffers = 1});
        assert(mesh && targets.depth && targets.normals && probe);
    }
    std::vector<u8> draw(const Scene3D& scene, f32 mode = 0, f32 gain = 1, const GeometryTargets3D* output = nullptr) {
        const auto& t = output ? *output : targets;
        renderer.clear(colors::black);
        assert(r3d.render_geometry(t, scene));
        const auto& stats = r3d.debug_stats();
        assert(stats.draws == 0 && stats.shadow_draws == 0 && stats.local_shadow_draws == 0 &&
               stats.ao_dispatches == 0 && stats.taa_dispatches == 0 && stats.gi_dispatches == 0 &&
               stats.particles_drawn == 0 && stats.device_waits == 0 && stats.taa_jitter == Vec2f{});
        ShaderParams params;
        params.uniforms[0] = mode;
        params.uniforms[1] = gain;
        renderer.draw_shader_surface(rect, probe, params, t.depth, t.normals);
        std::vector<u8> pixels;
        Vec2i size;
        assert(renderer.read_rgba(rect, pixels, size) && size == extent);
        renderer.present();
        return pixels;
    }
};
u8 at(const std::vector<u8>& p, u32 x, u32 y, u32 c = 0) {
    return p[(y * 160 + x) * 4 + c];
}
void geometry(Fixture& f) {
    Scene3D scene;
    scene.camera = Camera3D::look_at({0, 0, 5}, {});
    assert(scene.draw(f.mesh, {}));
    auto pixels = f.draw(scene);
    assert(at(pixels, 80, 60) == 255 && at(pixels, 0, 0) == 0);
    const auto coverage = pixels;
    // Exact reverse-Z projection, including finite-far and orthographic cameras.
    for (const auto projection : {Projection3D::Perspective, Projection3D::Orthographic}) {
        scene.camera.projection = projection;
        for (const f32 far : {100.0f, std::numeric_limits<f32>::infinity()}) {
            if (projection == Projection3D::Orthographic && !std::isfinite(far))
                continue;
            scene.camera.z_far = far;
            const auto clip = scene.camera.view_projection(4.f / 3.f) * Vec4f{0, 0, 0, 1};
            const f32 expected = clip.z / clip.w;
            const f32 gain = expected < .25f ? 4.f : 1.f;
            pixels = f.draw(scene, 1, gain);
            assert(std::abs(int(at(pixels, 80, 60)) - std::lround(expected * gain * 255)) <= 1);
        }
    }
    scene.camera = Camera3D::look_at({0, 0, 5}, {});
    pixels = f.draw(scene, 2);
    assert(std::abs(int(at(pixels, 80, 60)) - 128) <= 1 && std::abs(int(at(pixels, 80, 60, 1)) - 128) <= 1 &&
           at(pixels, 80, 60, 2) == 255);
    // Rotated and back-facing normals exercise both signed oct coordinates and the folded hemisphere.
    for (const f32 angle : {-150.f, -30.f, 30.f, 150.f}) {
        scene.clear();
        assert(scene.draw(f.mesh, Mat4f::rotation_y(angle), {.double_sided = true}));
        pixels = f.draw(scene, 2);
        auto n = Quatf::axis_angle({0, 1, 0}, angle).rotate({0, 0, 1});
        if (n.z < 0)
            n = n * -1.f;
        assert(std::abs(int(at(pixels, 80, 60)) - std::lround((n.x * .5f + .5f) * 255)) <= 2);
        assert(std::abs(int(at(pixels, 80, 60, 2)) - std::lround((n.z * .5f + .5f) * 255)) <= 2);
    }
    scene.clear();
    scene.camera = Camera3D::look_at({0, 0, -5}, {});
    assert(scene.draw(f.mesh, {}, {.double_sided = true}));
    pixels = f.draw(scene, 2);
    assert(std::abs(int(at(pixels, 80, 60)) - 128) <= 1 && at(pixels, 80, 60, 2) == 0);
    scene.camera = Camera3D::look_at({0, 0, 5}, {});
    // Flat normals, mirrored placement, posed skinning and retained batches preserve silhouettes.
    auto flat = quad();
    for (auto& v : flat.vertices)
        v.normal = {};
    const auto flat_mesh = f.r3d.create_mesh(flat).value_or({});
    assert(flat_mesh);
    scene.clear();
    assert(scene.draw(flat_mesh, {}));
    assert(f.draw(scene) == coverage);
    pixels = f.draw(scene, 2);
    assert(at(pixels, 80, 60, 2) == 255);
    scene.clear();
    assert(scene.draw(f.mesh, Mat4f::scaling({-1, 1, 1}), {.double_sided = true}));
    assert(f.draw(scene) == coverage);
    auto skin = quad();
    skin.skin.assign(skin.vertices.size(), {});
    const auto skinned = f.r3d.create_mesh(skin).value_or({});
    const std::array pose{Mat4f::translation({.25f, 0, 0})};
    assert(skinned && f.r3d.capabilities().skinning);
    scene.clear();
    assert(scene.draw_skinned(skinned, pose, Mat4f::translation({-.25f, 0, 0})));
    assert(f.draw(scene) == coverage && f.r3d.debug_stats().skin_matrices == 1);
    auto batch = InstanceBatch3D::create(f.mesh, std::vector<MeshInstance3D>(1)).value_or({});
    assert(batch);
    scene.clear();
    assert(scene.draw_instanced(batch));
    assert(f.draw(scene) == coverage);
    assert(f.r3d.debug_stats().retained_instances == 1);
    // Whole-mesh LOD and CPU/GPU cluster cuts use the same complete finest geometry.
    const auto sphere_data = meshes::sphere(1, 32, 16);
    const auto sphere = f.r3d.create_mesh(sphere_data).value_or({});
    const auto levels = f.r3d.create_mesh(sphere_data, {.levels = 3}).value_or({});
    const auto clusters = f.r3d.create_mesh(sphere_data, {.clusters = true}).value_or({});
    assert(sphere && levels && clusters);
    scene.clear();
    assert(scene.draw(sphere, {}));
    const auto sphere_coverage = f.draw(scene);
    scene.lod.enabled = true;
    scene.debug.force_level = 0;
    scene.clear();
    assert(scene.draw(levels, {}));
    assert(f.draw(scene) == sphere_coverage);
    for (const bool gpu : {false, true}) {
        scene.debug.gpu_culling = gpu;
        scene.lod.occlusion.enabled = true;
        scene.clear();
        assert(scene.draw(clusters, {}));
        assert(f.draw(scene) == sphere_coverage);
        assert(!f.r3d.debug_stats().occlusion);
    }
    scene.lod.enabled = false;
    scene.lod.occlusion.enabled = false;
    // Mask coverage includes texture, vertex/instance alpha and imported sampler rules.
    const u8 halves[]{255, 255, 255, 0, 255, 255, 255, 255};
    const auto mask = f.renderer.create_texture_from_rgba(halves, {2, 1});
    assert(mask);
    scene.clear();
    assert(scene.draw(f.mesh, {}, {.blend = Blend3D::Mask, .texture = mask}));
    pixels = f.draw(scene);
    assert(at(pixels, 60, 60) == 0 && at(pixels, 100, 60) == 255);
    scene.clear();
    assert(scene.draw(f.mesh, {}, {.color = Color::rgba(255, 255, 255, 100), .blend = Blend3D::Mask, .texture = mask}));
    pixels = f.draw(scene);
    assert(at(pixels, 100, 60) == 0);
    // Blended/wireframe meshes and overlay are absent. Empty scenes clear previous contents.
    scene.clear();
    assert(scene.draw(f.mesh, {}, {.blend = Blend3D::Alpha}));
    assert(scene.draw(f.mesh, {}, {.wireframe = true}));
    scene.overlay().camera = scene.camera;
    assert(scene.overlay().draw(f.mesh, {}));
    pixels = f.draw(scene);
    for (std::size_t i = 0; i < pixels.size(); i += 4)
        assert(pixels[i] == 0);
    scene.clear();
    pixels = f.draw(scene);
    for (std::size_t i = 0; i < pixels.size(); i += 4)
        assert(pixels[i] == 0);
    // Geometry ignores all stock effects and their jitter, without mutating the scene's requests.
    scene.clear();
    assert(scene.draw(f.mesh, {}));
    scene.taa.enabled = true;
    scene.ambient_occlusion.enabled = true;
    scene.contact_shadows.enabled = true;
    scene.global_illumination.enabled = true;
    scene.sky.enabled = true;
    scene.fog.enabled = true;
    assert(f.draw(scene) == coverage && scene.taa.enabled && scene.global_illumination.enabled);
    // Format/extent/nonattachment validation happens before any commands or allocations.
    auto bad = f.targets;
    bad.depth = {};
    assert(!f.r3d.render_geometry(bad, scene) && !f.r3d.diagnostics().empty());
    bad = f.targets;
    bad.normals = f.targets.depth;
    assert(!f.r3d.render_geometry(bad, scene));
    bad = f.targets;
    bad.normals = f.renderer.create_render_attachment({32, 32}, TextureFormat::Rg16Float);
    assert(!f.r3d.render_geometry(bad, scene));
    bad = f.targets;
    const std::vector<u16> data(extent.x * extent.y * 2, 0);
    bad.normals = f.renderer.create_texture(extent, TextureFormat::Rg16Float, data.data());
    assert(bad.normals && !f.r3d.render_geometry(bad, scene));
    scene.clear();
    assert(scene.draw(f.mesh, {}, {.blend = Blend3D::Mask, .texture = f.targets.normals}));
    assert(!f.r3d.render_geometry(f.targets, scene));
    std::cout << "geometry: projection/depth, signed world normals, flat/mirrored/skinned/retained, masking, "
                 "exclusions, clear and validation\n";
}
void graph(Fixture& f) {
    ResourceGraphDesc desc;
    desc.resources = {{.name = "depth",
                       .kind = GraphResourceKind::RenderAttachment,
                       .format = TextureFormat::Depth32Float,
                       .size = extent,
                       .exported = true},
                      {.name = "normals",
                       .kind = GraphResourceKind::RenderAttachment,
                       .format = TextureFormat::Rg16Float,
                       .size = extent,
                       .exported = true}};
    desc.passes = {{.name = "geometry", .writes = {0, 1}, .required_features = graph_feature::attachments}};
    ResourceGraphCompiler compiler;
    GraphResources resources;
    const auto features = available_graph_features(f.renderer);
    const auto& plan = compiler.compile(desc, features);
    assert(plan && plan.slots.size() == 2 && plan.slot_payload_bytes == 160 * 120 * 8);
    assert(resources.prepare(f.renderer, desc, plan));
    auto old_depth = resources.texture(0), old_normals = resources.texture(1);
    Scene3D scene;
    assert(scene.draw(f.mesh, {}));
    GeometryTargets3D t{old_depth, old_normals};
    const auto original = f.draw(scene, 0, 1, &t);
    const auto allocations = f.renderer.backend_stats().gpu_memory.allocations;
    for (int i = 0; i < 4; ++i) {
        assert(resources.prepare(f.renderer, desc, compiler.compile(desc, features)));
        f.renderer.clear(colors::black);
        assert(f.r3d.render_geometry(t, scene));
        f.renderer.present();
    }
    assert(compiler.stats().compilations == 1 && compiler.stats().cache_hits == 4);
    assert(f.renderer.backend_stats().gpu_memory.allocations == allocations);
    assert(f.draw(scene, 0, 1, &t) == original); // readback outside the allocation measurement
    // HDR changes colour targets only; data handles and bytes remain stable.
    assert(f.renderer.set_color_space(ColorSpace::Linear, true));
    assert(resources.prepare(f.renderer, desc, compiler.compile(desc, features)));
    assert(resources.texture(0) == old_depth && resources.texture(1) == old_normals);
    assert(f.renderer.set_color_space(ColorSpace::Gamma));
    for (auto& r : desc.resources)
        r.size = {80, 60};
    assert(resources.prepare(f.renderer, desc, compiler.compile(desc, features)));
    assert(resources.texture(0) != old_depth && resources.texture(0).size() == (Vec2i{80, 60}));
    // Caller-held old attachments remain valid after a graph resize or clear.
    resources.clear();
    assert(f.draw(scene, 0, 1, &t) == original);
    std::cout << "geometry graph: cached plans, stable GPU allocation, HDR-independent data, resize lifetime\n";
}
void history(Fixture& f) {
    auto a = f.r3d.create_view(extent, {.samples = 1, .hdr = true}).value_or({});
    auto b = f.r3d.create_view(extent, {.samples = 1, .hdr = true}).value_or({});
    assert(a && b);
    Scene3D scene;
    scene.taa.enabled = true;
    scene.ambient_occlusion.enabled = true;
    SurfaceTargets3D surface{f.renderer.create_render_target(extent), f.targets.depth};
    const auto sun = f.renderer.create_render_attachment({128, 128}, TextureFormat::Depth32Float, 4);
    Scene3D unrelated;
    unrelated.lights.sun.direction = {-2, 1, 3};
    unrelated.lights.shadow_cascades = 4;
    assert(unrelated.draw(f.mesh, Mat4f::translation({1, 0, 0})));
    const auto shoot = [&](View3D& view) {
        f.renderer.clear(colors::black);
        assert(f.r3d.render(view, scene));
        f.renderer.draw_texture(view.texture(), rect);
        std::vector<u8> p;
        Vec2i size;
        assert(f.renderer.read_rgba(rect, p, size));
        f.renderer.present();
        return p;
    };
    for (int frame = 0; frame < 8; ++frame) {
        scene.clear();
        assert(scene.draw(f.mesh, Mat4f::translation({f32(frame) * .02f, 0, 0})));
        auto expected = shoot(a);
        f.draw(scene);
        f.renderer.clear(colors::black);
        const auto prepared = f.r3d.prepare(unrelated).value_or({});
        const auto shadow = f.r3d.render_sun_shadow(sun, prepared, extent).value_or({});
        assert(shadow.depth && f.r3d.render_surface(surface, prepared, {.sun_shadow = &shadow}));
        f.renderer.present();
        assert(shoot(b) == expected);
        assert(f.r3d.debug_stats().geometry_graph_reused == (frame > 0));
    }
    std::cout
        << "geometry/shadow/surface interleaving: exact stock pixels, private shadow cache and temporal history\n";
}
std::vector<u8> prepared_picture(Fixture& f, View3D& view, const Scene3D& scene, bool shared) {
    f.renderer.clear(colors::black);
    if (shared) {
        auto prepared = f.r3d.prepare(scene).value_or({});
        assert(prepared);
        assert(f.r3d.render_geometry_prepared(f.targets, prepared));
        assert(f.r3d.debug_stats().bounds_instance_tests == 0);
        assert(f.r3d.render_prepared(view, prepared));
        assert(f.r3d.debug_stats().bounds_instance_tests == 0 && f.r3d.debug_stats().device_waits == 0);
    } else
        assert(f.r3d.render(view, scene));
    f.renderer.draw_texture(view.texture(), rect);
    std::vector<u8> pixels;
    Vec2i size;
    assert(f.renderer.read_rgba(rect, pixels, size));
    f.renderer.present();
    return pixels;
}
void prepared_work(Fixture& f) {
    constexpr u32 count = 10'000;
    Scene3D scene;
    auto rows = scene.add_instances(f.mesh, count).value_or({});
    assert(rows.size() == count);
    for (u32 i = 0; i < count; ++i)
        rows[i].transform = Mat4f::translation({(f32(i % 100) - 50) * .02f, (f32(i / 100) - 50) * .02f, 0}) *
                            Mat4f::scaling({.005f, .005f, .005f});
    const auto gpu_allocations = f.renderer.backend_stats().gpu_memory.allocations;
    auto p = f.r3d.prepare(scene, {.max_cpu_bytes = 2 * 1024 * 1024}).value_or({});
    assert(p && f.r3d.debug_stats().preparation_records == count &&
           f.renderer.backend_stats().gpu_memory.allocations == gpu_allocations);
    assert(f.r3d.render_geometry_prepared(f.targets, p));
    const auto first = f.r3d.debug_stats();
    assert(first.instance_upload_bytes == count * 128 && first.bounds_instance_tests == 0 && first.device_waits == 0);
    const auto allocations = f.renderer.backend_stats().gpu_memory.allocations;
    assert(f.r3d.render_geometry_prepared(f.targets, p));
    const auto second = f.r3d.debug_stats();
    assert(second.instance_upload_bytes == 0 && second.shared_instance_bytes == count * 128 &&
           second.bounds_instance_tests == 0 && f.renderer.backend_stats().gpu_memory.allocations == allocations);
    // A large renderer scratch array must not inflate a later small handle's payload.
    Scene3D small;
    assert(small.draw(f.mesh, {}));
    auto tiny = f.r3d.prepare(small, {.max_cpu_bytes = 2048}).value_or({});
    assert(tiny);
    assert(f.r3d.render_geometry_prepared(f.targets, tiny) && f.r3d.debug_stats().preparation_cpu_bytes <= 2048);
    f.renderer.present();
    std::cout << "shared preparation 10000 copies: 1.28 MB instance upload reused, zero repeated bounds/packing or GPU "
                 "allocations\n";
}
void shared_preparation(Fixture& f) {
    Scene3D scene;
    assert(scene.draw(f.mesh, {}));
    assert(scene.draw(f.mesh, Mat4f::translation({100, 0, 0})));
    auto view = f.r3d.create_view(extent, {.samples = 1, .background = colors::black}).value_or({});
    assert(view && f.r3d.capabilities().shared_preparation);
    const auto ordinary = prepared_picture(f, view, scene, false);
    f.renderer.clear(colors::black);
    auto p = f.r3d.prepare(scene).value_or({});
    assert(p && f.r3d.debug_stats().preparation_records == 2 &&
           f.r3d.debug_stats().preparation_cpu_bytes <= 32 * 1024 * 1024);
    assert(f.r3d.render_geometry_prepared(f.targets, p));
    assert(f.r3d.debug_stats().instance_upload_bytes == 128 && f.r3d.debug_stats().bounds_instance_tests == 0);
    assert(f.r3d.render_geometry_prepared(f.targets, p));
    assert(f.r3d.debug_stats().shared_uploads == 1 && f.r3d.debug_stats().shared_instance_bytes == 128 &&
           f.r3d.debug_stats().instance_upload_bytes == 0 && f.r3d.debug_stats().device_waits == 0);
    assert(f.r3d.render_prepared(view, p));
    assert(f.r3d.debug_stats().shared_uploads == 1 && f.r3d.debug_stats().instance_upload_bytes == 0);
    // A different camera must still cull independently, even with the same world data.
    scene.camera.position = {200, 0, 5};
    assert(p && f.r3d.render_geometry_prepared(f.targets, p));
    assert(f.r3d.debug_stats().geometry_instances == 0 && f.r3d.debug_stats().culled == 2);
    scene.camera.position = {0, 0, 5};
    assert(f.r3d.render_prepared(view, p) && f.r3d.debug_stats().shared_uploads == 1);
    f.renderer.draw_texture(view.texture(), rect);
    std::vector<u8> pixels;
    Vec2i size;
    assert(f.renderer.read_rgba(rect, pixels, size) && pixels == ordinary);
    assert(!p && !f.r3d.render_prepared(view, p)); // readback/submission ended the frame
    f.renderer.present();
    // An intervening ordinary upload must prevent stale reuse, then reuse can resume.
    p = f.r3d.prepare(scene).value_or({});
    assert(p);
    assert(f.r3d.render_geometry_prepared(f.targets, p));
    Scene3D other;
    assert(other.draw(f.mesh, Mat4f::translation({.5f, 0, 0})));
    assert(f.r3d.render_geometry(f.targets, other));
    assert(f.r3d.render_geometry_prepared(f.targets, p));
    assert(f.r3d.debug_stats().shared_uploads == 0 && f.r3d.debug_stats().instance_upload_bytes == 128);
    assert(f.r3d.render_geometry_prepared(f.targets, p) && f.r3d.debug_stats().shared_uploads == 1);
    f.renderer.present();
    assert(!p);
    assert(!f.r3d.prepare(scene, {.max_cpu_bytes = 1}) && !f.r3d.diagnostics().empty());
    // Resize changes visibility/projection, without invalidating world preparation.
    p = f.r3d.prepare(scene).value_or({});
    assert(p && f.r3d.resize(view, {80, 120}));
    assert(f.r3d.render_prepared(view, p) && f.r3d.debug_stats().bounds_instance_tests == 0);
    assert(f.r3d.resize(view, extent));
    scene.clear();
    assert(!p && !f.r3d.render_prepared(view, p));
    auto instances = scene.add_instances(f.mesh, 1).value_or({});
    assert(instances.size() == 1);
    p = f.r3d.prepare(scene).value_or({});
    assert(p);
    instances[0].transform = Mat4f::translation({1, 0, 0});
    scene.modified_instances();
    assert(!p);
    auto batch = InstanceBatch3D::create(f.mesh, std::vector<MeshInstance3D>(1)).value_or({});
    scene.clear();
    assert(scene.draw_instanced(batch));
    p = f.r3d.prepare(scene).value_or({});
    assert(p);
    MeshInstance3D changed;
    changed.transform = Mat4f::translation({1, 0, 0});
    assert(batch.update(0, {&changed, 1}) && !p);
    // Borrowed source lifetime is checked before touching its address, even if a
    // caller keeps the weak identity token locked across destruction or movement.
    auto source = std::make_unique<Scene3D>();
    assert(source->draw(f.mesh, {}));
    auto stamp = source->draw_lifetime().lock();
    p = f.r3d.prepare(*source).value_or({});
    assert(p);
    source.reset();
    assert(stamp && *stamp == 0 && !p && !f.r3d.render_prepared(view, p));
    source = std::make_unique<Scene3D>();
    assert(source->draw(f.mesh, {}));
    p = f.r3d.prepare(*source).value_or({});
    assert(p);
    Scene3D moved = std::move(*source);
    source.reset();
    assert(!p);
    std::cout << "shared preparation: exact pixels, independent cameras/resize, upload generations, budgets, edits and "
                 "lifetimes\n";
}

void public_passes(Fixture& f) {
    assert(f.r3d.capabilities().surface_pass && f.r3d.capabilities().sun_shadow_pass);
    assert(f.renderer.capabilities().render_attachment_arrays);
    SurfaceTargets3D targets{f.renderer.create_render_target(extent), f.targets.depth};
    const auto array = f.renderer.create_render_attachment({128, 128}, TextureFormat::Depth32Float, 4);
    const auto single = f.renderer.create_render_attachment({128, 128}, TextureFormat::Depth32Float, 1);
    assert(targets.colour && array && single && array.array_layers() == 4 && single.array_layers() == 1 &&
           !targets.depth.array_layers());
    ResourceGraphDesc imports;
    imports.resources = {{.name = "sun",
                          .kind = GraphResourceKind::RenderAttachment,
                          .format = TextureFormat::Depth32Float,
                          .size = {128, 128},
                          .imported = true,
                          .array_layers = 1},
                         {.name = "output", .exported = true}};
    imports.passes = {{.name = "sample", .reads = {0}, .writes = {1}}};
    ResourceGraphCompiler import_compiler;
    const auto import_plan = import_compiler.compile(imports, available_graph_features(f.renderer));
    assert(import_plan);
    GraphResources imported;
    const auto plain_depth = f.renderer.create_render_attachment({128, 128}, TextureFormat::Depth32Float);
    for (const auto& t : {plain_depth, array}) {
        const std::array input{GraphImport{.resource = 0, .texture = t}};
        assert(!imported.prepare(f.renderer, imports, import_plan, input));
    }
    const std::array input{GraphImport{.resource = 0, .texture = single}};
    assert(imported.prepare(f.renderer, imports, import_plan, input));
    // 2D upload APIs cannot silently overwrite only layer zero of an array.
    const std::array<u8, 4> pixel{};
    assert(!f.renderer.update_texture(single, {0, 0}, {1, 1}, pixel.data()));
    bool filled = false;
    assert(!f.renderer.write_texture(single, {0, 0}, {1, 1}, [&](auto) { filled = true; }) && !filled);
    const auto box = f.r3d.create_mesh(meshes::box({1, 1, 1})).value_or({});
    const auto ground = f.r3d.create_mesh(meshes::box({8, .1f, 6})).value_or({});
    auto stock = f.r3d.create_view(extent, {.samples = 1, .background = colors::black}).value_or({});
    Scene3D scene;
    scene.camera = Camera3D::look_at({3, 3, 6}, {0, .3f, 0}, 45);
    scene.lights.shadow_area = {{-8, -2, -5}, {8, 6, 5}};
    scene.lights.shadow_resolution = 128;
    scene.lights.sun.direction = {2, 4, 1};
    assert(box && ground && stock);
    assert(scene.draw(ground, Mat4f::translation({0, -.1f, 0})));
    assert(scene.draw(box, Mat4f::translation({0, .5f, 0}), {.color = Color::rgb(200, 100, 50)}));
    const auto read = [&](const Texture& t) {
        f.renderer.draw_texture(t, rect);
        std::vector<u8> pixels;
        Vec2i size;
        assert(f.renderer.read_rgba(rect, pixels, size) && size == extent);
        f.renderer.present();
        return pixels;
    };
    const auto picture = [&](bool custom, bool shadows) {
        f.renderer.clear(colors::black);
        auto p = f.r3d.prepare(scene).value_or({});
        assert(p);
        if (!custom) {
            assert(f.r3d.render_prepared(stock, p));
            return read(stock.texture());
        }
        SunShadow3D shadow;
        if (shadows) {
            const auto result = f.r3d.render_sun_shadow(array, p, extent);
            assert(result && result->projection.count == scene.lights.shadow_cascades);
            shadow = *result;
            assert(f.r3d.debug_stats().shadow_draws > 0 && f.r3d.debug_stats().particle_shadow_draws == 0 &&
                   !f.r3d.debug_stats().shadow_cached && f.r3d.debug_stats().bounds_instance_tests == 0 &&
                   f.r3d.debug_stats().device_waits == 0);
        }
        assert(
            f.r3d.render_surface(targets, p, {.background = colors::black, .sun_shadow = shadows ? &shadow : nullptr}));
        const auto& stats = f.r3d.debug_stats();
        assert(stats.draws > 0 && stats.shadow_draws == 0 && stats.local_shadow_draws == 0 &&
               stats.ao_dispatches == 0 && stats.taa_dispatches == 0 && stats.gi_dispatches == 0 &&
               stats.particles_drawn == 0 && stats.device_waits == 0 && stats.taa_jitter == Vec2f{} &&
               stats.bounds_instance_tests == 0);
        return read(targets.colour.texture());
    };
    for (const u32 count : {1u, 4u}) {
        scene.lights.shadow_cascades = count;
        const auto expected = picture(false, true);
        // Seed stock particle-caster scratch with another scene. Public mesh
        // shadows must neither reuse it nor require its light-view mappings.
        Scene3D particles;
        particles.camera = scene.camera;
        particles.lights = scene.lights;
        const std::array sprites{ParticleSprite3D{.position = {0, 1, 0}, .size = 2}};
        assert(particles.draw_particles({.casts_shadow = true}, sprites));
        f.renderer.clear(colors::black);
        assert(f.r3d.render(stock, particles) && f.r3d.debug_stats().particle_shadow_draws > 0);
        f.renderer.present();
        const auto actual = picture(true, true);
        assert(actual == expected);
    }
    scene.lights.cache_shadows = true;
    for (int i = 0; i < 3; ++i)
        picture(false, true);
    assert(f.r3d.debug_stats().shadow_cached);
    const auto cached = picture(false, true);
    picture(true, true);
    assert(picture(false, true) == cached && f.r3d.debug_stats().shadow_cached);
    scene.lights.cache_shadows = false;
    const auto shadowed = picture(true, true);
    const auto unshadowed = picture(true, false);
    assert(shadowed != unshadowed);
    scene.lights.sun_shadow_strength = 0;
    assert(picture(false, false) == unshadowed);
    scene.lights.sun_shadow_strength = 1;
    // Texture/projection are explicit inputs: a developer can alter the projection.
    f.renderer.clear(colors::black);
    auto p = f.r3d.prepare(scene).value_or({});
    auto shadow = f.r3d.render_sun_shadow(array, p, extent).value_or({});
    assert(shadow.depth);
    for (u32 k = 0; k < shadow.projection.count; ++k)
        shadow.projection.cascade[k].view_projection.m[12] += 1000;
    assert(f.r3d.render_surface(targets, p, {.background = colors::black, .sun_shadow = &shadow}));
    assert(read(targets.colour.texture()) == unshadowed);
    // Depth is stored and can be sampled after surface shading.
    f.renderer.clear(colors::black);
    p = f.r3d.prepare(scene).value_or({});
    assert(f.r3d.render_surface(targets, p));
    f.renderer.draw_shader_surface(rect, f.probe, {}, targets.depth, f.targets.normals);
    std::vector<u8> depth_pixels;
    Vec2i size;
    assert(f.renderer.read_rgba(rect, depth_pixels, size));
    f.renderer.present();
    assert(at(depth_pixels, 80, 60) == 255 && at(depth_pixels, 0, 0) == 0);
    // Unused layers, and every layer of an inactive sun, are initialized to far.
    const auto bytes = geometry_probe("shadow_probe.frag.spv");
    const auto probe = f.renderer.create_shader(
        {.spirv = {bytes.data(), static_cast<u32>(bytes.size())}, .num_samplers = 1, .num_uniform_buffers = 1});
    assert(probe);
    for (const bool active : {true, false}) {
        scene.lights.shadow_cascades = 1;
        scene.lights.sun_shadow_strength = active ? 1 : 0;
        f.renderer.clear(colors::black);
        p = f.r3d.prepare(scene).value_or({});
        const auto result = f.r3d.render_sun_shadow(array, p, extent);
        assert(result && result->projection.count == (active ? 1u : 0u));
        for (u32 layer = active ? 1 : 0; layer < 4; ++layer) {
            ShaderParams params;
            params.uniforms[0] = static_cast<f32>(layer);
            f.renderer.draw_shader_surface(rect, probe, params, array);
            std::vector<u8> pixels;
            assert(f.renderer.read_rgba(rect, pixels, size));
            for (std::size_t i = 0; i < pixels.size(); i += 4)
                assert(pixels[i] == 0);
        }
        f.renderer.present();
    }
    scene.lights.sun_shadow_strength = 1;
    // Single-layer arrays retain their array texture type and work with stock sampling.
    f.renderer.clear(colors::black);
    p = f.r3d.prepare(scene).value_or({});
    auto one = f.r3d.render_sun_shadow(single, p, extent).value_or({});
    assert(one.projection.count == 1 &&
           f.r3d.render_surface(targets, p, {.background = colors::black, .sun_shadow = &one}));
    const auto one_pixels = read(targets.colour.texture());
    assert(one_pixels == picture(false, true));
    scene.lights.sun_shadow_strength = 0;
    f.renderer.clear(colors::black);
    p = f.r3d.prepare(scene).value_or({});
    assert(f.r3d.render_surface(targets, p, {.background = colors::black, .sun_shadow = &one}));
    assert(read(targets.colour.texture()) == unshadowed);
    scene.lights.sun_shadow_strength = .35f;
    assert(picture(true, true) == picture(false, true));
    scene.lights.sun_shadow_strength = 1;

    // Materials and overlay ordering share the existing recorder. Local lights
    // illuminate surfaces, but their private shadow cache is never consumed.
    assert(scene.draw(box, Mat4f::translation({-.5f, .7f, .8f}),
                      {.color = Color::rgba(30, 200, 100, 100), .blend = Blend3D::Alpha}));
    assert(scene.draw(box, Mat4f::translation({.5f, .7f, .5f}),
                      {.color = Color::rgba(100, 20, 10, 60), .blend = Blend3D::Additive}));
    assert(scene.overlay().draw(f.mesh, Mat4f::scaling({.15f, .15f, .15f}), {.unlit = true}));
    scene.lights.points.push_back({.position = {0, 2, 2}, .color = LinearRgb{2, 1, 1}, .radius = 10});
    scene.lights.spots.push_back(
        {.position = {0, 4, 2}, .direction = {0, -1, 0}, .color = LinearRgb{1, 2, 1}, .radius = 10});
    assert(picture(true, true) == picture(false, true));
    // Whole-mesh LOD, CPU/GPU cuts, retained instances, masks, flat geometry
    // and skinning all use the same explicit maps and material recorder.
    for (const bool clustered : {false, true}) {
        const auto mesh =
            f.r3d.create_mesh(meshes::sphere(1, 24, 12), {.levels = 3, .clusters = clustered}).value_or({});
        assert(mesh);
        for (const bool gpu : {false, true}) {
            scene.clear();
            scene.lod.enabled = true;
            scene.debug.gpu_culling = gpu;
            scene.debug.force_level = 0;
            scene.lights.shadow_cascades = 4;
            assert(scene.draw(mesh, Mat4f::scaling({1.5f, .7f, 1})));
            assert(picture(true, true) == picture(false, true));
        }
    }
    scene.lod.enabled = false;
    scene.debug.force_level = -1;
    scene.lights.shadow_cascades = 1;
    auto flat = quad();
    for (auto& vertex : flat.vertices)
        vertex.normal = {};
    auto skin = quad();
    skin.skin.assign(skin.vertices.size(), {});
    const auto flat_mesh = f.r3d.create_mesh(flat).value_or({});
    const auto skin_mesh = f.r3d.create_mesh(skin).value_or({});
    assert(flat_mesh && skin_mesh);
    const std::array<Mat4f, 1> pose{Mat4f::translation({.2f, 0, 0})};
    const auto batch =
        InstanceBatch3D::create(flat_mesh,
                                std::vector<MeshInstance3D>{MeshInstance3D{.transform = Mat4f::translation({-1, 0, 0})},
                                                            MeshInstance3D{.transform = Mat4f::translation({1, 0, 0})}})
            .value_or({});
    scene.clear();
    assert(batch && scene.draw_instanced(batch, {.double_sided = true}));
    assert(scene.draw_skinned(skin_mesh, pose, Mat4f::translation({0, 0, .5f})));
    assert(picture(true, true) == picture(false, true));
    const std::array<u8, 8> mask{255, 255, 255, 0, 255, 255, 255, 255};
    const auto cutout = f.renderer.create_texture_from_rgba(mask.data(), {2, 1});
    scene.clear();
    assert(cutout && scene.draw(f.mesh, {}, {.blend = Blend3D::Mask, .texture = cutout}));
    assert(picture(true, true) == picture(false, true));
    // Surface history/effect requests are opt-in graph operations, with no hidden
    // allocation or work from the corresponding Scene3D switches.
    const auto plain = picture(true, false);
    const std::array ignored_sprites{ParticleSprite3D{.position = {0, 0, 1}, .size = 2}};
    assert(scene.draw_particles({.casts_shadow = true}, ignored_sprites));
    scene.taa.enabled = scene.ambient_occlusion.enabled = scene.contact_shadows.enabled = true;
    scene.sky.enabled = scene.fog.enabled = scene.global_illumination.enabled = true;
    assert(picture(true, false) == plain);
    scene.taa.enabled = scene.ambient_occlusion.enabled = scene.contact_shadows.enabled = false;
    scene.sky.enabled = scene.fog.enabled = scene.global_illumination.enabled = false;
    f.renderer.clear(colors::black);
    p = f.r3d.prepare(scene).value_or({});
    assert(!f.r3d.render_sun_shadow(targets.depth, p, extent));
    assert(!f.r3d.render_sun_shadow(array, p, {0, 1}));
    assert(!f.r3d.render_surface({targets.colour, array}, p));
    auto bad = f.r3d.render_sun_shadow(array, p, extent).value_or({});
    bad.projection.cascade[0].view_projection.m[0] = std::numeric_limits<f32>::quiet_NaN();
    assert(!f.r3d.render_surface(targets, p, {.sun_shadow = &bad}));
    bad.projection.count = 5;
    assert(!f.r3d.render_surface(targets, p, {.sun_shadow = &bad}));
    bad.projection.count = 1;
    bad.depth = targets.depth;
    assert(!f.r3d.render_surface(targets, p, {.sun_shadow = &bad}));
    f.renderer.present();
    assert(!f.r3d.render_surface(targets, p) && !f.r3d.render_sun_shadow(array, p, extent));
    // Feedback is rejected for every material slot, before a GPU pass starts.
    for (u32 slot = 0; slot < 5; ++slot) {
        Scene3D feedback;
        Material3D material;
        const std::array<Texture*, 5> maps{&material.texture, &material.metallic_roughness, &material.normal,
                                           &material.occlusion, &material.emissive_texture};
        *maps[slot] = targets.colour.texture();
        assert(feedback.draw(f.mesh, {}, material));
        f.renderer.clear(colors::black);
        const auto input = f.r3d.prepare(feedback).value_or({});
        assert(input && !f.r3d.render_surface(targets, input));
        f.renderer.present();
    }
    // Queued commands retain every input/output even when the graph's handles
    // are released before 2D is flushed and before GPU submission.
    f.renderer.clear(colors::black);
    p = f.r3d.prepare(scene).value_or({});
    {
        SurfaceTargets3D temporary{f.renderer.create_render_target(extent),
                                   f.renderer.create_render_attachment(extent, TextureFormat::Depth32Float)};
        const auto map = f.renderer.create_render_attachment({128, 128}, TextureFormat::Depth32Float, 1);
        const auto sampled = f.r3d.render_sun_shadow(map, p, extent).value_or({});
        assert(sampled.depth &&
               f.r3d.render_surface(temporary, p, {.background = colors::black, .sun_shadow = &sampled}));
        f.renderer.draw_texture(temporary.colour.texture(), rect);
    }
    std::vector<u8> retained;
    assert(f.renderer.read_rgba(rect, retained, size));
    f.renderer.present();
    assert(std::any_of(retained.begin(), retained.end(), [](u8 x) { return x > 0 && x < 255; }));
    std::cout << "public passes: exact stock shadow/surface pixels, cascades, explicit projection, stored depth, "
                 "array clears, transparency/overlay/local lights, exclusions and validation\n";
}

void prepared_shadows(Fixture& f) {
    const auto ground = f.r3d.create_mesh(meshes::box({8, .1f, 6})).value_or({});
    const auto box = f.r3d.create_mesh(meshes::box({1, 1, 1})).value_or({});
    auto a = f.r3d.create_view(extent, {.samples = 1, .background = colors::black}).value_or({});
    auto b = f.r3d.create_view(extent, {.samples = 1, .background = colors::black}).value_or({});
    Scene3D scene;
    scene.camera = Camera3D::look_at({0, 2.5f, 6}, {0, .3f, 0}, 45);
    scene.lights.sun.direction = {2, 1, 0};
    scene.lights.shadow_area = {{-8, -2, -5}, {8, 6, 5}};
    scene.lights.shadow_resolution = 128;
    assert(ground && box && a && b && scene.draw(ground, Mat4f::translation({0, -.1f, 0})) &&
           scene.draw(box, Mat4f::translation({6, 3, 0})));
    const auto expected = prepared_picture(f, a, scene, false);
    f.renderer.clear(colors::black);
    auto p = f.r3d.prepare(scene).value_or({});
    assert(p);
    assert(f.r3d.render_geometry_prepared(f.targets, p));
    assert(f.r3d.debug_stats().geometry_instances == 1);
    assert(f.r3d.render_prepared(b, p));
    assert(f.r3d.debug_stats().shadow_casters >= 2 && f.r3d.stats().culled == 1 &&
           f.r3d.debug_stats().bounds_instance_tests == 0 && f.r3d.debug_stats().instance_upload_bytes > 0);
    assert(f.r3d.render_prepared(b, p) && f.r3d.debug_stats().shared_uploads == 1);
    f.renderer.draw_texture(b.texture(), rect);
    std::vector<u8> pixels;
    Vec2i size;
    assert(f.renderer.read_rgba(rect, pixels, size) && pixels == expected);
    f.renderer.present();
    std::cout
        << "shared preparation: off-camera sun casters preserved; view-dependent layouts and stock pixels agree\n";
}
void prepared_local_shadows(Fixture& f) {
    const auto ground = f.r3d.create_mesh(meshes::box({8, .1f, 6})).value_or({});
    const auto box = f.r3d.create_mesh(meshes::box({1, 1, 1})).value_or({});
    auto a = f.r3d.create_view(extent, {.samples = 1, .background = colors::black}).value_or({});
    auto b = f.r3d.create_view(extent, {.samples = 1, .background = colors::black}).value_or({});
    Scene3D scene;
    scene.camera = Camera3D::look_at({0, 2.5f, 6}, {0, .3f, 0}, 45);
    scene.lights.sun.color = {};
    scene.lights.local_shadow_resolution = 128;
    scene.lights.points.push_back({.position = {0, 4, 2}, .color = {5, 4, 3}, .radius = 10, .shadow_enabled = true});
    assert(ground && box && a && b && scene.draw(ground, Mat4f::translation({0, -.1f, 0})) &&
           scene.draw(box, Mat4f::translation({0, .5f, 0})));
    const auto expected = prepared_picture(f, a, scene, false);
    f.renderer.clear(colors::black);
    auto p = f.r3d.prepare(scene).value_or({});
    assert(p);
    assert(f.r3d.render_geometry_prepared(f.targets, p));
    assert(f.r3d.render_prepared(b, p));
    assert(f.r3d.debug_stats().local_shadow_casters > 0 && f.r3d.debug_stats().bounds_instance_tests == 0);
    assert(f.r3d.render_prepared(b, p));
    assert(f.r3d.debug_stats().instance_upload_bytes == 0 && f.r3d.debug_stats().shared_instance_bytes > 0);
    f.renderer.draw_texture(b.texture(), rect);
    std::vector<u8> pixels;
    Vec2i size;
    assert(f.renderer.read_rgba(rect, pixels, size) && pixels == expected);
    f.renderer.present();
    const auto frame = [&] {
        f.renderer.clear(colors::black);
        auto shared = f.r3d.prepare(scene).value_or({});
        assert(shared);
        assert(f.r3d.render_geometry_prepared(f.targets, shared) && f.r3d.render_prepared(b, shared));
        f.renderer.present();
    };
    for (int i = 0; i < 3; ++i)
        frame();
    const auto before = f.renderer.backend_stats();
    for (int i = 0; i < 4; ++i)
        frame();
    const auto after = f.renderer.backend_stats();
    // The shared upload staging buffer may shrink at a submission-window
    // boundary inherited from earlier tests. Account for that maintenance
    // separately; the stable geometry/local-shadow resources must not allocate,
    // and this small workload must not grow staging capacity.
    assert(after.gpu_memory.allocations - before.gpu_memory.allocations ==
           after.upload_staging_allocations - before.upload_staging_allocations);
    assert(after.upload_staging_bytes <= before.upload_staging_bytes);
    std::cout
        << "shared preparation: local shadow union parity, shared input buffers and stable allocations across frames\n";
}
void prepared_skin_history(Fixture& f) {
    auto data = quad();
    data.skin.assign(data.vertices.size(), {});
    const auto mesh = f.r3d.create_mesh(data).value_or({});
    auto a = f.r3d.create_view(extent, {.samples = 1, .hdr = true}).value_or({});
    auto b = f.r3d.create_view(extent, {.samples = 1, .hdr = true}).value_or({});
    Scene3D scene;
    scene.taa.enabled = true;
    scene.ambient_occlusion.enabled = true;
    assert(mesh && a && b);
    for (int frame = 0; frame < 8; ++frame) {
        scene.clear();
        const std::array pose{Mat4f::translation({f32(frame) * .02f, 0, 0})};
        assert(scene.draw_skinned(mesh, pose, {}));
        const auto expected = prepared_picture(f, a, scene, false);
        f.renderer.clear(colors::black);
        auto p = f.r3d.prepare(scene).value_or({});
        assert(p && f.r3d.debug_stats().preparation_skin_bounds == 1);
        assert(f.r3d.render_geometry_prepared(f.targets, p));
        assert(f.r3d.debug_stats().joint_upload_bytes == sizeof(Mat4f));
        assert(f.r3d.render_prepared(b, p));
        assert(f.r3d.debug_stats().shared_uploads == 1 && f.r3d.debug_stats().joint_upload_bytes == 0 &&
               f.r3d.debug_stats().shared_joint_uploads == 1 && f.r3d.debug_stats().bounds_instance_tests == 0);
        f.renderer.draw_texture(b.texture(), rect);
        std::vector<u8> pixels;
        Vec2i size;
        assert(f.renderer.read_rgba(rect, pixels, size) && pixels == expected);
        f.renderer.present();
    }
    auto p = f.r3d.prepare(scene).value_or({});
    assert(p);
    const auto revision = scene.draw_revision();
    const std::array pose{Mat4f::translation({1, 0, 0})};
    assert(scene.update_joint_matrices(0, pose));
    assert(scene.draw_revision() == revision && !p && !f.r3d.render_prepared(b, p));
    std::cout << "shared preparation: current joint upload reused, posed bounds built once; exact independent TAA "
                 "histories\n";
}
void prepared_variants(Fixture& f) {
    auto a = f.r3d.create_view(extent, {.samples = 1, .background = colors::black}).value_or({});
    auto b = f.r3d.create_view(extent, {.samples = 1, .background = colors::black}).value_or({});
    const auto data = meshes::sphere(1, 24, 12);
    for (bool clusters : {false, true}) {
        const auto mesh = f.r3d.create_mesh(data, {.levels = 3, .clusters = clusters}).value_or({});
        assert(mesh);
        for (bool gpu : {false, true}) {
            Scene3D scene;
            scene.lod.enabled = true;
            scene.debug.gpu_culling = gpu;
            scene.debug.force_level = 0;
            assert(scene.draw(mesh, Mat4f::scaling({1.5f, .7f, 1})));
            assert(prepared_picture(f, a, scene, false) == prepared_picture(f, b, scene, true));
        }
    }
    Scene3D scene;
    assert(scene.draw(f.mesh, Mat4f::translation({0, 0, -1}), {.color = Color::rgb(50, 100, 200)}));
    assert(scene.draw(f.mesh, {}, {.color = Color::rgba(255, 40, 20, 100), .blend = Blend3D::Alpha}));
    assert(scene.draw(f.mesh, Mat4f::translation({0, 0, -.5f}),
                      {.color = Color::rgba(10, 100, 40, 70), .blend = Blend3D::Additive}));
    assert(scene.overlay().draw(f.mesh, Mat4f::translation({1, 1, 0}) * Mat4f::scaling({.2f, .2f, .2f}),
                                {.color = colors::white}));
    assert(prepared_picture(f, a, scene, false) == prepared_picture(f, b, scene, true));
    auto p = f.r3d.prepare(scene).value_or({});
    assert(p);
    assert(scene.overlay().draw(f.mesh, {}));
    assert(!p);
    std::cout << "shared preparation: LOD/CPU-GPU clusters, transparent composition and overlay parity\n";
}
int prepared_device_lifetime() {
    PreparedScene3D prepared;
    Scene3D source;
    {
        App app;
        auto& window = app.create_window({.title = "preparation lifetime", .width = 64, .height = 64, .hidden = true});
        Renderer2D renderer{make_render_backend(window, false, true)};
        Renderer3D r3d{renderer};
        if (!r3d.capabilities().shared_preparation)
            return std::getenv("KIN_REQUIRE_GPU_TESTS") ? 1 : 77;
        auto mesh = r3d.create_mesh(quad()).value_or({});
        assert(mesh && source.draw(mesh, {}));
        prepared = r3d.prepare(source).value_or({});
        assert(prepared);
    }
    assert(!prepared);
    std::cout << "shared preparation: safe observation/release after renderer and device destruction\n";
    return 0;
}
} // namespace
int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--lifetime")
        return prepared_device_lifetime();
    const bool foreign_device = argc > 1 && std::string_view(argv[1]) == "--foreign-device";
    App app;
    auto& window = app.create_window({.title = "geometry tests", .width = 160, .height = 120, .hidden = true});
    Renderer2D renderer{make_render_backend(window, false, true)};
    Renderer3D r3d{renderer};
    if (!r3d.capabilities().geometry_pass)
        return std::getenv("KIN_REQUIRE_GPU_TESTS") ? 1 : 77;
    // SDL_GPU/NVIDIA can crash releasing one of two live devices' windows,
    // reproducible with plain 2D renderers. Keep that lifecycle probe separate
    // from geometry acceptance; software Vulkan covers live foreign ownership.
    if (foreign_device && !r3d.capabilities().software && !std::getenv("KIN_TEST_LIVE_GPU_DEVICES"))
        return 77;
    Fixture f{renderer};
    if (!foreign_device) {
        geometry(f);
        graph(f);
        history(f);
        prepared_work(f);
        shared_preparation(f);
        public_passes(f);
        prepared_shadows(f);
        prepared_local_shadows(f);
        prepared_skin_history(f);
        prepared_variants(f);
        return 0;
    }
    auto& other_window = app.create_window({.title = "foreign geometry", .width = 160, .height = 120, .hidden = true});
    Renderer2D other{make_render_backend(other_window, false, true)};
    // Exercise a live foreign device, including its swapchain and submission path.
    other.clear(colors::black);
    other.present();
    auto bad = f.targets;
    bad.normals = other.create_render_attachment(extent, TextureFormat::Rg16Float);
    Scene3D scene;
    assert(bad.normals && !f.r3d.render_geometry(bad, scene));
    Renderer3D other3d{other};
    const auto foreign_mesh = other3d.create_mesh(quad()).value_or({});
    assert(foreign_mesh && scene.draw(foreign_mesh, {}));
    assert(!f.r3d.render_geometry(f.targets, scene));
    auto foreign_prepared = other3d.prepare(scene).value_or({});
    assert(foreign_prepared);
    auto own_view = f.r3d.create_view(extent, {.samples = 1}).value_or({});
    assert(own_view);
    assert(!f.r3d.render_prepared(own_view, foreign_prepared));
    assert(!f.r3d.render_geometry_prepared(f.targets, foreign_prepared));
    assert(!other3d.render_prepared(own_view, foreign_prepared)); // view belongs to the other device
    const SurfaceTargets3D surface{f.renderer.create_render_target(extent), f.targets.depth};
    const auto own_array = f.renderer.create_render_attachment({128, 128}, TextureFormat::Depth32Float, 1);
    assert(!f.r3d.render_surface(surface, foreign_prepared));
    assert(!f.r3d.render_sun_shadow(own_array, foreign_prepared, extent));
    assert(!f.r3d.render_local_shadows(own_array, foreign_prepared, {}, extent));
    Scene3D own_scene;
    assert(own_scene.draw(f.mesh, {}));
    const auto own_prepared = f.r3d.prepare(own_scene).value_or({});
    const auto foreign_array = other.create_render_attachment({128, 128}, TextureFormat::Depth32Float, 1);
    const auto foreign_depth = other.create_render_attachment(extent, TextureFormat::Depth32Float);
    const auto foreign_colour = other.create_render_target(extent);
    assert(own_prepared && !f.r3d.render_sun_shadow(foreign_array, own_prepared, extent));
    assert(!f.r3d.render_local_shadows(foreign_array, own_prepared, {}, extent));
    own_scene.lights.spots.push_back({.position = {0, 2, 2}, .direction = {0, -1, 0}});
    LocalShadows3D foreign_local{foreign_array,
                                 {LocalShadowMap3D{.source = {.resolution = 128},
                                                   .projection = {spot_shadow_view(own_scene.lights.spots[0], 128)}}}};
    assert(!f.r3d.render_surface(surface, own_prepared, {.local_shadows = &foreign_local}));
    assert(!f.r3d.render_surface({surface.colour, foreign_depth}, own_prepared));
    assert(!f.r3d.render_surface({foreign_colour, surface.depth}, own_prepared));

    scene.clear();
    const u8 white[]{255, 255, 255, 255};
    const auto foreign_texture = other.create_texture_from_rgba(white, {1, 1});
    assert(foreign_texture && scene.draw(f.mesh, {}, {.blend = Blend3D::Mask, .texture = foreign_texture}));
    assert(!f.r3d.render_geometry(f.targets, scene));
    other.clear(colors::black);
    other.present();
    std::cout << "geometry: foreign-device targets, meshes and cutout textures rejected\n";
}
