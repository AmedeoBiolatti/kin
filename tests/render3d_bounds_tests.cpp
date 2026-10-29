#include <array>
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <kin/platform/app.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <stdexcept>
#include <vector>

namespace {
using namespace kin;

MeshData3D grid() {
    MeshData3D mesh;
    constexpr u32 side = 17;
    for (u32 y = 0; y < side; ++y)
        for (u32 x = 0; x < side; ++x)
            mesh.vertices.push_back({{-1 + f32(x) / 8, -1 + f32(y) / 8, 0}, {0, 0, 1}});
    for (u32 y = 0; y + 1 < side; ++y)
        for (u32 x = 0; x + 1 < side; ++x) {
            const u32 a = y * side + x;
            mesh.indices.insert(mesh.indices.end(), {a, a + 1, a + side, a + 1, a + side + 1, a + side});
        }
    return mesh;
}

// A sheared mesh's origin is outside the camera, while its left tip occupies visible pixels. Comparing culling on
// and off catches both per-copy rejection and rejection of a spatial group. The same scene exercises shadow reach,
// picking, retained sphere caches, whole-mesh LOD, CPU/GPU cluster cuts and posed bounds without Jolt or Lua.
void check_bounds(Renderer2D& renderer) {
    Renderer3D r3d{renderer};
    const auto caps = r3d.capabilities();
    const MeshData3D data = grid();
    const Mesh3D plain = r3d.create_mesh(data).value_or({});
    const Mesh3D levels = r3d.create_mesh(data, {.levels = 4}).value_or({});
    const Mesh3D clusters = r3d.create_mesh(data, {.clusters = true}).value_or({});
    auto skin_data = data;
    skin_data.skin.resize(skin_data.vertices.size());
    const Mesh3D skinned = r3d.create_mesh(skin_data).value_or({});
    const Mesh3D receiver = r3d.create_mesh(meshes::box({40, 8, .1f})).value_or({});
    assert(plain && levels && clusters && skinned && receiver);
    assert(levels.level_count() > 1 && clusters.cluster_count() > 1);
    auto made_view = r3d.create_view({160, 120}, {.samples = 1, .background = colors::black, .pick = true});
    assert(made_view);
    View3D view = *made_view;
    const Material3D material{.color = Color::rgb(220, 35, 70), .double_sided = true};
    u32 cases = 0;
    for (bool mirrored : {false, true})
        for (bool shadows : {false, true})
            for (u32 mode = 0; mode < 8; ++mode) {
                if ((mode == 5 && !caps.gpu_driven_clusters) || (mode == 6 && !caps.skinning) ||
                    (mode == 7 && !caps.ambient_occlusion))
                    continue;
                const Mat4f transform = Mat4f::translation({13.1f, 0, 0}) *
                                        Mat4f::scaling({mirrored ? -8.0f : 8.0f, 1, .5f}) * Mat4f::rotation_z(45);
                const Mesh3D& mesh = mode == 3 ? levels : mode == 4 || mode == 5 ? clusters : plain;
                const std::vector<MeshInstance3D> records(mode == 2 ? 256 : 1,
                                                          MeshInstance3D{transform, colors::white, 17});
                auto batch = InstanceBatch3D::create(mesh, records, {{0, u32(records.size())}}).value_or({});
                Scene3D scene;
                scene.camera = Camera3D::look_at({0, 0, 5}, {});
                scene.camera.projection = Projection3D::Orthographic;
                scene.camera.height = 4;
                scene.camera.z_far = 30;
                scene.lights.sun.direction = {0, 0, 1};
                scene.lights.ambient = {.1f, .1f, .1f};
                if (shadows) {
                    scene.lights.shadow_area = {{-30, -4, -3}, {30, 4, 1}};
                    scene.lights.shadow_resolution = 512;
                    assert(scene.draw(receiver, Mat4f::translation({0, 0, -2}), {}, colors::white, 1));
                }
                scene.lod.enabled = mode >= 3 && mode <= 5;
                scene.debug.gpu_culling = mode == 5;
                scene.ambient_occlusion.enabled = mode == 7;
                if (mode == 1)
                    assert(scene.draw_instanced_group(mesh, records, instance_bounds(mesh, records), material));
                else if (mode == 2)
                    assert(scene.draw_instanced(batch, material));
                else if (mode == 6) {
                    const std::array<Mat4f, 1> joints{Mat4f::identity()};
                    assert(scene.draw_skinned(skinned, joints, transform, material, colors::white, 17));
                } else
                    assert(scene.draw(mesh, transform, material, colors::white, 17));

                const auto shoot = [&](bool cull) {
                    scene.cull = cull;
                    renderer.clear(colors::black);
                    assert(r3d.render(view, scene));
                    renderer.draw_texture(view.texture(), {0, 0, 160, 120});
                    std::vector<u8> pixels;
                    Vec2i size;
                    assert(renderer.read_rgba({0, 0, 160, 120}, pixels, size) && size == (Vec2i{160, 120}));
                    renderer.present();
                    return pixels;
                };
                const auto expected = shoot(false);
                const auto actual = shoot(true);
                if (actual != expected) {
                    std::cerr << "affine culling changed pixels: mode=" << mode << " mirrored=" << mirrored
                              << " shadows=" << shadows << '\n';
                    throw std::runtime_error("affine culling removed visible geometry or its shadow");
                }
                Vec2i visible{-1, -1};
                u32 red = 0;
                for (u32 p = 0; p < actual.size() / 4; ++p)
                    if (actual[4 * p] > actual[4 * p + 1] + 40) {
                        ++red;
                        visible = {i32(p % 160), i32(p / 160)};
                    }
                assert(red >= 8); // the reference must contain visible geometry, not merely match a blank picture
                const auto hit = r3d.pick(view, visible);
                assert(hit && hit->hit && hit->id == 17);
                if (mode == 2)
                    assert(r3d.debug_stats().retained_instances == records.size());
                if (mode == 4 || mode == 5)
                    assert(r3d.debug_stats().cluster_gpu == (mode == 5));
                if (mode == 7)
                    assert(r3d.debug_stats().ao_instances > 0);
                ++cases;
            }
    std::cout << "affine bounds: " << cases << " GPU comparisons of culling on/off, including shadows and picking\n";
}
} // namespace

int main() {
    const char* required = std::getenv("KIN_REQUIRE_GPU_TESTS");
    const bool require_gpu = required && required[0] && required[0] != '0';
    bool ready = false;
    try {
        kin::App app;
        auto& window = app.create_window({.title = "affine bounds", .width = 160, .height = 120, .hidden = true});
        auto backend = kin::make_render_backend(window, false, true);
        if (!backend || backend->name() != "SDL_GPU")
            throw std::runtime_error("SDL_GPU unavailable");
        kin::Renderer2D renderer{std::move(backend)};
        ready = true;
        check_bounds(renderer);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << (ready || require_gpu ? "[FAIL] " : "[SKIP] ") << error.what() << '\n';
        return ready || require_gpu ? 1 : 77;
    }
}
