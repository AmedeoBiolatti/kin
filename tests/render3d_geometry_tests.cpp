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
        assert(shoot(b) == expected);
        assert(f.r3d.debug_stats().geometry_graph_reused == (frame > 0));
    }
    std::cout << "geometry interleaving: exact stock pixels and unchanged temporal history\n";
}
} // namespace
int main(int argc, char** argv) {
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
    scene.clear();
    const u8 white[]{255, 255, 255, 255};
    const auto foreign_texture = other.create_texture_from_rgba(white, {1, 1});
    assert(foreign_texture && scene.draw(f.mesh, {}, {.blend = Blend3D::Mask, .texture = foreign_texture}));
    assert(!f.r3d.render_geometry(f.targets, scene));
    other.clear(colors::black);
    other.present();
    std::cout << "geometry: foreign-device targets, meshes and cutout textures rejected\n";
}
