#include "render_checks.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <emscripten.h>
#include <kin/render3d/renderer3d.hpp>
#include <kin/runtime/scene_app.hpp>
#include <stdexcept>

using namespace kin;
namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
MeshData3D quad(f32 size) {
    const f32 h = size * 0.5f;
    return {.vertices = {{{-h, -h, 0}, {0, 0, 1}, {0, 1}},
                         {{h, -h, 0}, {0, 0, 1}, {1, 1}},
                         {{h, h, 0}, {0, 0, 1}, {1, 0}},
                         {{-h, h, 0}, {0, 0, 1}, {0, 0}}},
            .indices = {0, 1, 2, 0, 2, 3}};
}
class Fixture final : public Scene {
    Mesh3D _quad, _box;
    Texture _texture;
    View3D _view, _preview;
    RenderTarget _target, _nested;
    bool _checked = false;
    f32 _angle = 25;
    int _frames = 0, _input = 0;
    int _resizes = 0;

public:
    std::string_view name() const override { return "webgl2-3d-fixture"; }
    void on_enter(SceneContext& ctx) override {
        require(ctx.renderer3d && ctx.renderer3d->available(), "Real WebGL2 3D provider is required");
        auto& r3d = *ctx.renderer3d;
        require(ctx.renderer.capabilities().render_targets && ctx.renderer.capabilities().rendering_3d,
                "2D render targets and 3D capability required");
        require(r3d.capabilities().depth_bits >= 24 && r3d.capabilities().skinning,
                "3D capabilities must match implementation");
        const auto result = r3d.create_mesh(quad(3));
        require(bool(result), "Quad mesh upload failed");
        _quad = *result;
        _box = r3d.create_mesh(meshes::box({1.5f, 1.5f, 1.5f})).value_or({});
        require(bool(_box), "Box mesh upload failed");
        _view = r3d.create_view({256, 192}, {.samples = 1}).value_or({});
        _preview = r3d.create_view({256, 192}, {.samples = 1, .background = Color::rgb(25, 35, 55)}).value_or({});
        require(_view && _preview, "RGBA8 depth views failed");
        constexpr std::array<u8, 16> texels{240, 40, 30, 255, 240, 40, 30, 255, 25, 70, 240, 255, 25, 70, 240, 255};
        _texture = ctx.renderer.create_texture_from_rgba(texels.data(), {2, 2});
        _target = ctx.renderer.create_render_target({256, 192}, ScaleMode::Nearest);
        _nested = ctx.renderer.create_render_target({16, 16}, ScaleMode::Nearest);
        require(_texture && _target && _nested, "Texture/target allocation failed");
        // Unsupported requests must fail visibly instead of returning a background-only view.
        require(!r3d.create_view({8, 8}, {.samples = 1, .hdr = true}), "Initial core profile must refuse HDR");
        require(!r3d.create_view({1 << 20, 1}, {.samples = 1}), "Oversized views must be refused without GL errors");
    }
    void update(SceneContext& ctx) override {
        if (ctx.input.pressed(Key::Escape))
            ctx.app.quit();
        if (ctx.input.pressed(Key::Right)) {
            _angle += 15;
            ++_input;
        }
        if (ctx.input.pressed(Key::Left)) {
            _angle -= 15;
            ++_input;
        }
    }
    void render(SceneContext& ctx) override {
        auto& r = ctx.renderer;
        auto& r3d = *ctx.renderer3d;
        const Vec2i output = r.output_size();
        const f32 scale = std::min(f32(output.x) / 640, f32(output.y) / 360);
        const Vec2i preview_size{std::max(1, int(288 * scale)), std::max(1, int(216 * scale))};
        if (_preview.size() != preview_size) {
            require(bool(r3d.resize(_preview, preview_size)), "3D view resize failed");
            ++_resizes;
        }
        r.clear(Color::rgb(12, 18, 30));
        // Queue 2D work on the default framebuffer before entering 3D.
        r.fill_rect({12, 12, 616, 6}, Color::rgb(240, 180, 55));
        Scene3D scene;
        scene.camera = Camera3D::look_at({0, 0, 5}, {});
        scene.camera.projection = Projection3D::Orthographic;
        scene.camera.height = 4;
        scene.camera.z_far = 10;
        const std::array<MeshInstance3D, 5> copies{
            {{.transform = Mat4f::translation({0, 0, 1}) * Mat4f::scaling({0.333333f, 0.333333f, 1})},
             {.transform = Mat4f::translation({0.9f, 0.7f, 1}) * Mat4f::scaling({0.167f, 0.167f, 1})},
             {.transform = Mat4f::translation({100, 0, 1})},
             {.transform = Mat4f::translation({0, 0, 4.99f})},
             {.transform = Mat4f::translation({0, 0, -20})}}};
        require(bool(scene.draw_instanced(_quad, copies, {.color = Color::rgb(30, 230, 60), .unlit = true})),
                "Near draw failed");
        // Far geometry submitted last must not paint over the nearer green square.
        require(bool(scene.draw(_quad, {}, {.unlit = true, .texture = _texture})), "Far draw failed");
        require(bool(r3d.render(_view, scene)), "Depth/texture view render failed");
        require(r3d.stats().draws == 2 && r3d.stats().instances == 3 && r3d.stats().culled == 3,
                "CPU cull/instance counters are wrong");
        r.push_render_target(_target);
        r.clear(Color::rgb(10, 20, 30));
        r.draw_texture(_view.texture(), {0, 0, 256, 192});
        // Queue target draws around another framebuffer, to test nesting and restoration.
        r.push_render_target(_nested);
        r.clear(Color::rgb(255, 255, 255));
        r.fill_rect({0, 0, 16, 8}, Color::rgb(240, 180, 55));
        r.pop_render_target();
        r.draw_texture(_nested.texture(), {4, 4, 16, 16});
        if (!_checked) {
            std::vector<u8> pixels;
            Vec2i size;
            require(r.read_rgba({0, 0, 256, 192}, pixels, size) && size == Vec2i{256, 192}, "RGBA readback failed");
            const auto check = [&](i32 x, i32 y, std::array<int, 4> expected) {
                for (int c = 0; c < 4; ++c)
                    require(std::abs(int(pixels[(y * size.x + x) * 4 + c]) - expected[c]) <= 2,
                            "3D pixel oracle failed (depth/UV/alpha/target nesting)");
            };
            check(128, 96, {30, 230, 60, 255}); // Near depth wins.
            check(171, 62, {30, 230, 60, 255}); // Another visible copy has its own instance transform.
            check(80, 45, {240, 40, 30, 255});  // Texture top.
            check(80, 145, {25, 70, 240, 255}); // Texture bottom, view and target V flips.
            check(240, 170, {10, 20, 30, 255}); // Transparent view keeps 2D background.
            check(8, 8, {240, 180, 55, 255});   // Nested target's top.
            check(8, 16, {255, 255, 255, 255}); // Nested target's bottom.

            auto alpha = r3d.create_view({64, 64}, {.samples = 1}).value_or({});
            require(bool(alpha), "Alpha reference view allocation failed");
            Scene3D blend;
            blend.camera = scene.camera;
            // Submitted near-first across different draws: transparent copies must be sorted far-first.
            require(bool(blend.draw(_quad, Mat4f::translation({0, 0, 1}),
                                    {.color = {255, 0, 0, 128}, .unlit = true, .blend = Blend3D::Alpha})),
                    "Near alpha draw failed");
            require(bool(blend.draw(_quad, {}, {.color = {0, 0, 255, 128}, .unlit = true, .blend = Blend3D::Alpha})),
                    "Far alpha draw failed");
            require(bool(r3d.render(alpha, blend)), "Alpha view render failed");
            r.push_render_target(_nested);
            r.clear(Color::rgb(10, 20, 30));
            r.draw_texture(alpha.texture(), {0, 0, 16, 16});
            require(r.read_rgba({0, 0, 16, 16}, pixels, size), "Alpha reference readback failed");
            check(8, 8, {131, 5, 71, 255}); // Premultiplied red over blue, then the 2D background.
            r.pop_render_target();
            Scene3D feedback;
            require(bool(feedback.draw(_quad, {}, {.unlit = true, .texture = _view.texture()})),
                    "Feedback reference submission failed");
            require(!r3d.render(_view, feedback), "A view must refuse sampling its own render target");
            check_webgl_geometry2d(r);
            check_webgl_materials(r, r3d);
            check_webgl_instances(r, r3d);
            _checked = true;
        }
        r.pop_render_target();
        // Render a second, lit view after 2D target work. HUD state must survive.
        Scene3D preview;
        preview.camera = Camera3D::orbit({}, _angle, 20, 4.5f);
        require(bool(preview.draw(_box, {}, {.color = Color::rgb(220, 160, 70), .texture = _texture})),
                "Lit mesh draw failed");
        require(bool(r3d.render(_preview, preview)), "Lit preview render failed");
        r.draw_texture(_target.texture(), {24, 55, 288, 216});
        r.draw_texture(_preview.texture(), {328, 55, 288, 216});
        r.push_clip({24, 290, 592, 30});
        r.fill_rect({0, 285, 640, 45}, Color::rgb(25, 150, 190));
        r.pop_clip();
        r.fill_rect({24, 332, 90, 8}, Color::rgb(30, 230, 60));
        ++_frames;
        EM_ASM(
            {
                Module.kin3dFixture = ({
                    passed : !!$0,
                    materials : !!$0,
                    skinning : !!$0,
                    sky : !!$0,
                    fog : !!$0,
                    shadows : !!$0,
                    frames : $1,
                    input : $2,
                    draws : 2,
                    instances : 3,
                    culled : 3,
                    resizes : $3
                });
            },
            _checked, _frames, _input, _resizes);
    }
    void on_exit(SceneContext&) override {
        EM_ASM({ Module.kin3dFixture.stopped = true; });
    }
};
} // namespace
int main() {
    auto scenes = std::make_unique<SceneManager>();
    scenes->push(std::make_unique<Fixture>());
    return run_scene_app({.window = {.title = "Kin WebGL2: 2D and 3D",
                                     .width = 960,
                                     .height = 600,
                                     .logical_width = 640,
                                     .logical_height = 360},
                          .pipeline_record_path = std::filesystem::path{},
                          .rendering_3d = true},
                         std::move(scenes));
}
