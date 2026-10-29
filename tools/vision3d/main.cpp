#include "vision.hpp"
#include "vision3d_shader.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <kin/platform/app.hpp>
#include <thread>
using namespace kin;
using vision3d::Graph;
using vision3d::Request;
namespace {
void check(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}
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
    Renderer3D& r3d;
    ComputeShaderHandle reduce;
    ShaderHandle pack, show;
    Mesh3D cube, plane, skinned;
    Fixture(Renderer2D& renderer, Renderer3D& renderer3d) : r(renderer), r3d(renderer3d) {
        const auto compute = vision3d_shader("reduce.comp.spv"), fragment = vision3d_shader("pack.frag.spv");
        reduce = r.create_compute_shader({compute.data(), static_cast<u32>(compute.size())});
        pack = r.create_shader({.spirv = {fragment.data(), static_cast<u32>(fragment.size())},
                                .num_samplers = 0,
                                .num_uniform_buffers = 0,
                                .num_storage_buffers = 1});
        const auto display = vision3d_shader("show.frag.spv");
        show = r.create_shader(
            {.spirv = {display.data(), static_cast<u32>(display.size())}, .num_samplers = 1, .num_uniform_buffers = 0});
        cube = r3d.create_mesh(meshes::box({1, 2, 1})).value_or({});
        plane = r3d.create_mesh(quad()).value_or({});
        auto skin = meshes::box({1, 2, 1});
        skin.skin.assign(skin.vertices.size(), {});
        skinned = r3d.create_mesh(skin).value_or({});
        check(reduce && pack && show && cube && plane && skinned, "visibility resources unavailable");
    }
    Scene3D world(bool wall = true, f32 wall_x = 0, const Material3D& wall_material = {}) {
        Scene3D s;
        s.camera = Camera3D::look_at({0, 1, 5}, {0, 1, 0}, 60);
        s.camera.z_far = 30;
        check(bool(s.draw(cube, Mat4f::translation({0, 1, 0}), {.color = Color::rgb(220, 70, 40)}, colors::white, 1)),
              "target draw failed");
        if (wall)
            check(bool(s.draw(plane, Mat4f::translation({wall_x, 1, 2}) * Mat4f::scaling({1.4f, 1.4f, 1}),
                              wall_material, colors::white, 2)),
                  "occluder draw failed");
        return s;
    }
    std::vector<u8> image(const Texture& texture) {
        const auto size = texture.size();
        auto output = r.create_render_target(size);
        check(bool(output), "image target failed");
        auto target = r.scoped_render_target(output);
        r.clear(colors::transparent);
        const Rectf rect{0, 0, static_cast<f32>(size.x), static_cast<f32>(size.y)};
        r.draw_shader_surface(rect, show, {}, texture);
        std::vector<u8> bytes;
        Vec2i actual;
        check(r.read_rgba(rect, bytes, actual) && actual == size, "image readback failed");
        return bytes;
    }
    u32 mask_count(Scene3D& s, const VisibilityOptions3D& options = {}, Vec2i size = {64, 64}) {
        VisibilityTargets3D targets{r.create_render_attachment(size, TextureFormat::Depth32Float),
                                    r.create_render_attachment(size, TextureFormat::Rgba16Float)};
        auto p = r3d.prepare(s).value_or({});
        check(p && bool(r3d.render_visibility(targets, p, options)), "visibility pass failed");
        const auto stats = r3d.debug_stats();
        check(stats.draws == 0 && stats.shadow_draws == 0 && stats.gi_dispatches == 0 && stats.ao_dispatches == 0 &&
                  stats.taa_dispatches == 0 && stats.device_waits == 0 && stats.taa_jitter == Vec2f{},
              "visibility ran unrelated work or waited");
        const auto pixels = image(targets.mask);
        check(!p, "readback must expire preparation");
        r.present();
        u32 count = 0;
        for (std::size_t i = 0; i < pixels.size(); i += 4)
            count += pixels[i] > 128;
        return count;
    }
    std::vector<vision3d::Result> drain(Graph& g) {
        std::vector<vision3d::Result> result;
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (g.pending() && std::chrono::steady_clock::now() < end) {
            auto ready = g.poll();
            result.insert(result.end(), ready.begin(), ready.end());
            if (g.pending()) {
                r.present();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        check(g.pending() == 0, "async visibility did not complete");
        for (const auto& x : result)
            check(x.valid, "invalid visibility readback");
        return result;
    }
};
void visibility_tests(Fixture& f) {
    auto open = f.world(false), blocked = f.world();
    const u32 visible = f.mask_count(open);
    check(visible > 100 && f.mask_count(blocked) == 0, "occluder/target depth test failed");
    check(f.mask_count(blocked, {.target_id = 1, .ignored_id = 2}) == visible, "ignored observer body wrote depth");
    blocked.camera = Camera3D::look_at({4, 1, 3}, {0, 1, 0}, 60);
    check(f.mask_count(blocked) > 0, "second camera inherited first camera's culling");
    open.camera = Camera3D::look_at({0, 1, 5}, {0, 1, 10}, 60);
    // No draws is valid for an empty frustum: inspect through the graph below instead of the mask-count stats helper.
    Graph out_of_view(f.r, f.r3d, f.reduce, f.pack);
    const std::array request{Request{1, open.camera, {}}};
    auto prepared = f.r3d.prepare(open).value_or({});
    check(out_of_view.record(open, prepared, request, 1) == 1 && out_of_view.submit(), "off-frustum query failed");
    auto outside = f.drain(out_of_view);
    check(outside.size() == 1 && outside[0].visible_pixels == 0, "behind-camera target reported visible");

    std::array<u8, 8 * 8 * 4> texels{};
    for (std::size_t i = 0; i < texels.size(); i += 4)
        texels[i] = texels[i + 1] = texels[i + 2] = 255;
    auto transparent = f.r.create_texture_from_rgba(texels.data(), 8, 8);
    Material3D fence{.blend = Blend3D::Mask, .texture = transparent};
    auto holes = f.world(true, 0, fence);
    check(f.mask_count(holes) == visible, "transparent cutout texels occluded target");
    for (u32 y = 0; y < 8; ++y)
        for (u32 x = 0; x < 8; ++x)
            texels[(y * 8 + x) * 4 + 3] = x < 4 ? 255 : 0;
    fence.texture = f.r.create_texture_from_rgba(texels.data(), 8, 8);
    auto half = f.world(true, 0, fence);
    const auto partial = f.mask_count(half);
    check(partial > visible / 4 && partial < visible * 3 / 4, "partial cover did not preserve cutout coverage");
    auto glass = f.world(true, 0, {.color = Color::rgba(180, 200, 255, 128), .blend = Blend3D::Alpha});
    check(f.mask_count(glass) == visible, "blended surfaces unexpectedly occluded vision");
    Scene3D large_id;
    large_id.camera = Camera3D::look_at({0, 1, 5}, {0, 1, 0});
    check(bool(large_id.draw(f.cube, Mat4f::translation({0, 1, 0}), {}, colors::white, 0xfedcba98u)),
          "large ID setup failed");
    check(f.mask_count(large_id, {.target_id = 0xfedcba98u}) == visible, "integer target ID lost precision");

    Scene3D skin;
    skin.camera = Camera3D::look_at({0, 1, 5}, {0, 1, 0});
    const std::array pose{Mat4f::identity()};
    check(bool(skin.draw_skinned(f.skinned, pose, Mat4f::translation({0, 1, 0}), {}, colors::white, 1)),
          "skin setup failed");
    check(f.mask_count(skin) == visible, "skinned target IDs/pose differ from static mesh");
    skin.clear();
    const std::array moved{Mat4f::translation({4, 0, 0})};
    check(bool(skin.draw_skinned(f.skinned, moved, Mat4f::translation({0, 1, 0}), {}, colors::white, 1)),
          "skin move failed");
    check(f.mask_count(skin) == 0, "visibility ignored animated pose");

    auto s = f.world();
    auto p = f.r3d.prepare(s).value_or({});
    VisibilityTargets3D target{f.r.create_render_attachment({64, 64}, TextureFormat::Depth32Float),
                               f.r.create_render_attachment({64, 64}, TextureFormat::Rgba16Float)};
    check(!f.r3d.render_visibility(target, p, {.target_id = ~0u}) &&
              !f.r3d.render_visibility(target, p, {.target_id = 1, .ignored_id = 1}),
          "invalid IDs admitted");
    const std::vector<u16> zero_texels(64 * 64 * 4);
    const auto sampled_only = f.r.create_texture({64, 64}, TextureFormat::Rgba16Float, zero_texels.data());
    p = f.r3d.prepare(s).value_or({});
    check(p && !f.r3d.render_visibility({target.depth, sampled_only}, p),
          "sampled-only texture admitted as attachment");
    struct ForeignTexture final : ITextureBackend {
        Vec2i size() const override { return {1, 1}; }
    };
    auto wrong_source = f.world(false);
    const Material3D bad_mask{.blend = Blend3D::Mask, .texture = Texture{std::make_shared<ForeignTexture>()}};
    const auto queued = wrong_source.draw(f.plane, Mat4f::translation({0, 1, 2}), bad_mask);
    const auto wrong_prepared = f.r3d.prepare(wrong_source);
    check(!queued || !wrong_prepared || !f.r3d.render_visibility(target, *wrong_prepared),
          "foreign masked texture silently changed visibility");
    s.camera.z_near = 0;
    check(!f.r3d.render_visibility(target, p), "invalid camera projection admitted");
    s.camera.z_near = .1f;
    s.clear();
    check(!p && !f.r3d.render_visibility(target, p), "edited preparation was accepted");
    std::cout << "visibility: independent cameras, occlusion, cutouts, partial cover, glass policy, self exclusion, "
                 "skinning and validation\n";
}
void async_tests(Fixture& f) {
    auto s = f.world();
    const std::array requests{Request{100, s.camera, {}}, Request{200, Camera3D::look_at({4, 1, 3}, {0, 1, 0}), {}},
                              Request{300, s.camera, {.target_id = 1, .ignored_id = 2}}};
    Graph g(f.r, f.r3d, f.reduce, f.pack, {.views_per_batch = 1, .max_pending = 1});
    const auto original = s.camera;
    for (u32 i = 0; i < 3; ++i) {
        auto p = f.r3d.prepare(s).value_or({});
        check(g.record(s, p, requests, 40 + i) == 1 && p, "budgeted query recording failed");
        check(s.camera.position == original.position && s.camera.rotation == original.rotation,
              "camera was not restored");
        check(g.submit() && !p, "readback did not end preparation");
        auto next = f.r3d.prepare(s).value_or({});
        check(g.record(s, next, requests, 999) == 0, "pending-read budget ignored");
        const auto results = f.drain(g);
        check(results.size() == 1 && results[0].token == requests[i].token && results[0].frame == 40 + i &&
                  (i == 0 ? results[0].visible_pixels == 0 : results[0].visible_pixels > 0),
              "async result identity/order/visibility is wrong");
    }
    const auto compilations = g.compilations();
    auto p = f.r3d.prepare(s).value_or({});
    g.record(s, p, requests, 77);
    g.submit();
    f.drain(g);
    check(g.compilations() == compilations, "camera changes recompiled the graph");
    const auto held_mask = g.mask();
    const auto before = f.r.backend_stats().gpu_memory;
    for (u32 frame = 0; frame < 24; ++frame) {
        auto prep = f.r3d.prepare(s).value_or({});
        check(g.record(s, prep, requests, 100 + frame) == 1 && g.submit(), "steady replay failed");
        f.drain(g);
    }
    const auto after = f.r.backend_stats().gpu_memory;
    check(g.mask() == held_mask && g.compilations() == compilations &&
              after.texture_bytes + after.buffer_bytes <= before.texture_bytes + before.buffer_bytes,
          "steady camera replay grew texture/buffer storage or recompiled");
    std::cout
        << "visibility: steady targets and owned texture/buffer plateau; readback transfers are released separately\n";
    Graph snapshots(f.r, f.r3d, f.reduce, f.pack);
    std::array historical{Request{1234, Camera3D::look_at({0, 1, 5}, {0, 1, 0}), {}}};
    const auto original_camera = historical[0].camera;
    {
        auto ephemeral = f.world(false);
        auto prep = f.r3d.prepare(ephemeral).value_or({});
        check(snapshots.record(ephemeral, prep, historical, 31337) == 1, "historical query failed");
    } // geometry/source identity can disappear before asynchronous completion
    historical[0].token = 9999;
    historical[0].camera.position = {100, 100, 100};
    historical[0].query.target_id = 99;
    check(snapshots.submit(), "historical query submission failed");
    const auto historical_result = f.drain(snapshots);
    check(historical_result.size() == 1 && historical_result[0].token == 1234 && historical_result[0].frame == 31337 &&
              historical_result[0].target_id == 1 && historical_result[0].camera.position == original_camera.position &&
              historical_result[0].visible_pixels > 0,
          "late result borrowed a replaced request/entity");
    // Exact byte packing across colour spaces and HDR. Full coverage is 65,536, exercising byte 2.
    for (const auto mode : {0, 1, 2}) {
        check(f.r.set_color_space(mode ? ColorSpace::Linear : ColorSpace::Gamma, mode == 2),
              "colour-space change failed");
        Scene3D full;
        full.camera = Camera3D::look_at({0, 0, 5}, {});
        full.camera.projection = Projection3D::Orthographic;
        full.camera.height = 2;
        full.camera.z_far = 20;
        check(bool(full.draw(f.plane, Mat4f::scaling({10, 10, 1}), {}, colors::white, 1)), "full mask setup failed");
        Graph packed(f.r, f.r3d, f.reduce, f.pack, {.size = {256, 256}, .views_per_batch = 1});
        const std::array query{Request{u64(mode), full.camera, {}}};
        auto prep = f.r3d.prepare(full).value_or({});
        check(packed.record(full, prep, query, 0) == 1 && packed.submit(), "packed query failed");
        const auto result = f.drain(packed);
        check(result.size() == 1 && result[0].visible_pixels == 65536, "count bytes changed with colour space/HDR");
    }
    check(f.r.set_color_space(ColorSpace::Gamma), "gamma restore failed");
    std::cout << "visibility: fair scheduling/backpressure, delayed identities, cached graph, exact gamma/linear/HDR "
                 "counts\n";
}
void history_test(Fixture& f) {
    auto s = f.world();
    s.camera = Camera3D::look_at({4, 3, 6}, {0, 1, 0});
    s.lights.sun.color = {};
    s.lights.ambient = {.4f, .4f, .4f};
    s.taa.enabled = true;
    s.global_illumination = {.enabled = true,
                             .area = {{-3, -1, -3}, {3, 4, 4}},
                             .cell = .2f,
                             .levels = 1,
                             .side = 32,
                             .height = 32,
                             .probes = {.rays = 16, .per_render = 0}};
    auto control = f.r3d.create_view({160, 120}, {.samples = 1}).value_or({});
    for (u32 frame = 0; frame < 16; ++frame) {
        check(bool(f.r3d.render(control, s)), "standalone main baseline failed");
        f.r.present();
    }
    const auto control_pixels = f.image(control.texture());
    f.r.present();
    auto a = f.r3d.create_view({160, 120}, {.samples = 1}).value_or({});
    auto b = f.r3d.create_view({160, 120}, {.samples = 1}).value_or({});
    Graph vision(f.r, f.r3d, f.reduce, f.pack);
    const std::array requests{Request{11, Camera3D::look_at({0, 1, 5}, {0, 1, 0}), {}},
                              Request{22, Camera3D::look_at({4, 1, 3}, {0, 1, 0}), {}}};
    for (u32 frame = 0; frame < 16; ++frame) {
        (void)vision.poll();
        auto p = f.r3d.prepare(s).value_or({});
        check(bool(f.r3d.render_prepared(a, p)), "reference main render failed");
        const auto jitter = f.r3d.debug_stats().taa_jitter;
        vision.record(s, p, requests, frame);
        check(p && bool(f.r3d.render_prepared(b, p)) && f.r3d.debug_stats().taa_jitter == jitter &&
                  f.r3d.debug_stats().gi_lit && f.r3d.debug_stats().gi_resolved,
              "observer graph changed main camera/history or disabled GI");
        vision.submit();
        f.r.present();
    }
    f.drain(vision);
    const auto left = f.image(a.texture()), right = f.image(b.texture());
    check(left == right && right == control_pixels, "observer graph/readback altered standalone stock GI/TAA pixels");
    f.r.present();
    // Destruction with in-flight data is bounded and safe, even without polling.
    {
        Graph temporary(f.r, f.r3d, f.reduce, f.pack);
        auto p = f.r3d.prepare(s).value_or({});
        temporary.record(s, p, requests, 100);
        temporary.submit();
    }
    std::cout << "visibility: two main views retain exact GI/TAA pixels and jitter across observer graphs; pending "
                 "teardown safe\n";
}
} // namespace
int main(int argc, char** argv) {
    bool test = false;
    int frames = 0, resolution = 64, budget = 2;
    std::string capture;
    try {
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg = argv[i];
            const auto value = [&]() -> std::string {
                if (++i >= argc)
                    throw std::runtime_error("missing option value");
                return argv[i];
            };
            if (arg == "--test")
                test = true;
            else if (arg == "--frames")
                frames = std::stoi(value());
            else if (arg == "--resolution")
                resolution = std::stoi(value());
            else if (arg == "--budget")
                budget = std::stoi(value());
            else if (arg == "--capture")
                capture = value();
            else if (arg == "--help") {
                std::cout
                    << "kin_vision3d [--test] [--frames N] [--resolution 1..256] [--budget 1..64] [--capture "
                       "PNG]\nSPACE toggles the wall; ESC quits. Top: normal view; bottom: enemy masks/count bars.\n";
                return 0;
            } else
                throw std::runtime_error("unknown option");
        }
        check(frames >= 0 && resolution >= 1 && resolution <= 256 && budget >= 1 && budget <= 64,
              "invalid frame/resolution/budget value");
        App app{{.max_fps = test ? 0.f : 60.f}};
        auto& window = app.create_window(
            {.title = "Kin multi-camera visibility: SPACE toggles cover", .width = 960, .height = 640, .hidden = test});
        Renderer2D r{make_render_backend(window, false, true)};
        Renderer3D r3d{r};
        const auto features = available_graph_features(r);
        if (!r3d.capabilities().visibility_pass || !r3d.capabilities().shared_preparation ||
            !(features & graph_feature::compute) || !(features & graph_feature::buffers)) {
            std::cerr << "Vision graph requires native visibility, shared preparation, compute and storage buffers\n";
            return std::getenv("KIN_REQUIRE_GPU_TESTS") ? 1 : 77;
        }
        Fixture f{r, r3d};
        if (test) {
            visibility_tests(f);
            async_tests(f);
            history_test(f);
            return 0;
        }
        Graph vision(r, r3d, f.reduce, f.pack,
                     {.size = {resolution, resolution}, .views_per_batch = static_cast<u32>(budget)});
        auto view = r3d.create_view({960, 440}, {.samples = 1}).value_or({});
        bool wall = true;
        u32 frame = 0;
        std::array<u32, 3> counts{};
        std::array<u64, 3> latest{};
        std::array<bool, 3> known{};
        const std::array previews{r.create_render_target({180, 180}), r.create_render_target({180, 180}),
                                  r.create_render_target({180, 180})};
        for (const auto& preview : previews) {
            auto target = r.scoped_render_target(preview);
            r.clear(colors::black);
        }
        const auto draw = [&] {
            for (const auto& result : vision.poll())
                if (result.token < 3 && result.frame >= latest[result.token]) {
                    counts[result.token] = result.visible_pixels;
                    known[result.token] = result.valid;
                    latest[result.token] = result.frame;
                }
            auto s = f.world(wall);
            s.camera = Camera3D::look_at({5, 4, 7}, {0, 1, 0});
            s.lights.sun = {.direction = {.4f, 1, .3f}, .color = {2, 1.9f, 1.8f}};
            s.lights.ambient = {.25f, .3f, .4f};
            s.lights.shadow_area = {{-4, -1, -4}, {4, 5, 6}};
            s.taa.enabled = true;
            check(bool(s.draw(f.cube, Mat4f::translation({0, -.1f, 0}) * Mat4f::scaling({8, .05f, 8}),
                              {.color = Color::rgb(100, 110, 125)})),
                  "ground draw failed");
            const std::array requests{Request{0, Camera3D::look_at({0, 1, 5}, {0, 1, 0}), {}},
                                      Request{1, Camera3D::look_at({4, 1, 3}, {0, 1, 0}), {}},
                                      Request{2, Camera3D::look_at({0, 1, -4}, {0, 1, 0}), {}}};
            r.clear(Color::rgb(15, 20, 28));
            auto p = r3d.prepare(s).value_or({});
            check(p && bool(r3d.render_prepared(view, p)), "main view failed");
            r.draw_texture(view.texture(), {0, 0, 960, 440});
            vision.record(s, p, requests, frame, [&](u64 token, const Texture& mask) {
                auto target = r.scoped_render_target(previews[token]);
                r.clear(colors::black);
                r.draw_shader_surface({0, 0, 180, 180}, f.show, {}, mask);
            });
            for (u32 i = 0; i < 3; ++i) {
                r.draw_texture(previews[i].texture(), {f32(i * 320 + 12), 450, 180, 180});
                const bool visible = counts[i] > 0;
                r.fill_rect({f32(i * 320 + 204), 470, 90, 30},
                            known[i] ? (visible ? Color::rgb(60, 210, 100) : Color::rgb(220, 70, 70))
                                     : Color::rgb(100, 110, 120));
                r.fill_rect(
                    {f32(i * 320 + 204), 510, std::min(90.f, f32(counts[i]) / f32(resolution * resolution) * 90), 12},
                    colors::white);
            }
            vision.submit();
            if (frame % 60 == 0)
                std::cout << "vision frame " << frame << ": front " << (known[0] ? i32(counts[0]) : -1) << ", flank "
                          << (known[1] ? i32(counts[1]) : -1) << ", rear " << (known[2] ? i32(counts[2]) : -1)
                          << " pixels; pending " << vision.pending() << '\n';
            if (!capture.empty() && ((frames && frame + 1 == u32(frames)) || (!frames && frame == 60)))
                check(r.save_png(capture), "capture failed");
            r.present();
            if (frames && ++frame >= u32(frames))
                app.quit();
            else if (!frames)
                ++frame;
        };
        app.run(
            [&](f32) {
                if (app.input().pressed(Key::Escape))
                    app.quit();
                if (app.input().pressed(Key::Space))
                    wall = !wall;
            },
            [&](f32) { draw(); });
        f.drain(vision);
    } catch (const std::exception& e) {
        std::cerr << "vision3d: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
