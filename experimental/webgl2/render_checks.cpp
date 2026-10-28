#include "render_checks.hpp"

#include <array>
#include <cmath>
#include <stdexcept>
#include <string>

using namespace kin;
namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
MeshData3D quad() {
    return {.vertices = {{{-0.5f, -0.5f, 0}, {0, 0, 1}, {0, 1}},
                         {{0.5f, -0.5f, 0}, {0, 0, 1}, {1, 1}},
                         {{0.5f, 0.5f, 0}, {0, 0, 1}, {1, 0}},
                         {{-0.5f, 0.5f, 0}, {0, 0, 1}, {0, 0}}},
            .indices = {0, 1, 2, 0, 2, 3}};
}
} // namespace
void check_webgl_geometry2d(Renderer2D& r) {
    const auto target = r.create_render_target({64, 64}, ScaleMode::Nearest);
    require(bool(target), "Indexed 2D reference target allocation failed");
    r.push_render_target(target);
    r.clear(colors::black);
    r.fill_rect({0, 0, 8, 8}, Color::rgb(255, 0, 0));
    ShapeMesh dense;
    dense.vertices.resize(8);
    dense.vertices[4] = {{16, 8}, Color::rgb(0, 255, 0)};
    dense.vertices[5] = {{32, 8}, Color::rgb(0, 255, 0)};
    dense.vertices[6] = {{32, 24}, Color::rgb(0, 255, 0)};
    dense.vertices[7] = {{16, 24}, Color::rgb(0, 255, 0)};
    dense.indices = {4, 5, 6, 4, 6, 7};
    dense.runs = {{.count = 6, .first_vertex = 4, .vertex_count = 4}};
    r.draw_shape(dense);
    ShapeMesh sparse;
    sparse.vertices.resize(8);
    sparse.vertices[0] = {{40, 8}, Color::rgb(0, 0, 255)};
    sparse.vertices[7] = {{56, 8}, Color::rgb(0, 0, 255)};
    sparse.vertices[4] = {{48, 24}, Color::rgb(0, 0, 255)};
    sparse.indices = {0, 7, 4};
    r.draw_shape(sparse);
    const std::array<u8, 16> checker{255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
    const auto texture = r.create_texture_from_rgba(checker.data(), {2, 2});
    require(bool(texture), "Indexed 2D reference texture allocation failed");
    r.draw_texture(texture, {0, 48, 16, 16});
    r.draw_texture(texture, {0, 0, 2, 2}, {40, 40, 16, 16}, colors::white, 90, {0.5f, 0.5f});
    std::vector<u8> pixels;
    Vec2i size;
    require(r.read_rgba({0, 0, 64, 64}, pixels, size), "Indexed 2D reference readback failed");
    const auto check = [&](i32 x, i32 y, Color color, const char* message) {
        const std::array<int, 4> expected{color.r, color.g, color.b, color.a};
        for (u32 i = 0; i < 4; ++i)
            require(std::abs(int(pixels[(y * 64 + x) * 4 + i]) - expected[i]) <= 2, message);
    };
    check(4, 4, Color::rgb(255, 0, 0), "A quad before an indexed mesh must keep its indices");
    check(24, 16, Color::rgb(0, 255, 0), "Dense shape runs must honor their nonzero vertex base");
    check(48, 12, Color::rgb(0, 0, 255), "Sparse meshes must retain their referenced vertices");
    check(4, 52, Color::rgb(255, 0, 0), "Textured quads must preserve their top-left UV");
    check(12, 60, colors::white, "Textured quads must preserve their bottom-right UV");
    check(44, 44, Color::rgb(0, 0, 255), "Rotated quads must preserve their index/UV order");
    check(52, 44, Color::rgb(255, 0, 0), "Rotated quads must preserve their pivot");
    r.clear(colors::black);
    ShapeMesh rounded;
    rounded.add({.half_size = {10, 8},
                 .radius = 4,
                 .fill = Color::rgb(255, 0, 0),
                 .stroke = Color::rgb(0, 255, 0),
                 .stroke_width = 2,
                 .transform = Affine2::translation({16, 16})});
    r.draw_shape(rounded);
    r.draw_texture(texture, {40, 8, 16, 16});
    ShapeMesh ellipse;
    ellipse.add({.kind = PathPrimitive::Kind::Ellipse,
                 .half_size = {8, 4},
                 .fill = Color::rgb(0, 0, 255),
                 .transform = Affine2::trs({48, 12}, 90)});
    r.push_transform(Affine2::translation({0, 4}));
    r.draw_shape(ellipse);
    r.pop_transform();
    r.fill_rect({47, 15, 2, 2}, colors::white);
    r.push_viewport({32, 32, 32, 32});
    r.push_clip({0, 0, 12, 12});
    r.fill_rounded_rect({0, 0, 16, 16}, 2, colors::white);
    r.pop_clip();
    r.pop_viewport();
    require(r.read_rgba({0, 0, 64, 64}, pixels, size), "Analytic 2D reference readback failed");
    check(16, 16, Color::rgb(255, 0, 0), "Analytic rounded rectangles must preserve the fill");
    check(26, 16, Color::rgb(0, 255, 0), "Analytic strokes must be centered on the outline");
    check(5, 5, colors::black, "Analytic rounded corners must discard outside fragments");
    const int edge = pixels[(16 * 64 + 27) * 4 + 1];
    require(edge > 50 && edge < 220, "Analytic strokes must retain a soft one-pixel edge");
    check(48, 20, Color::rgb(0, 0, 255), "Analytic ellipses must combine primitive and renderer transforms");
    check(48, 16, colors::white, "Textured geometry after analytic shapes must preserve submission order");
    check(40, 9, Color::rgb(255, 0, 0), "Analytic shapes after textures must preserve submission order");
    check(40, 40, colors::white, "Analytic shapes must include the viewport offset");
    check(46, 40, colors::black, "Analytic shapes must obey clipping");
    r.clear(colors::black);
    r.set_logical_size({32, 32});
    r.fill_rounded_rect({4, 4, 8, 8}, 2, Color::rgb(255, 0, 0));
    require(r.read_rgba({0, 0, 64, 64}, pixels, size), "Scaled analytic 2D reference readback failed");
    check(16, 16, Color::rgb(255, 0, 0), "Analytic shapes must apply logical scaling");
    check(4, 4, colors::black, "Analytic shapes must preserve their scaled bounds");
    r.set_logical_size({0, 0});
    r.clear(colors::transparent);
    r.fill_rounded_rect({8, 8, 16, 16}, 4, {255, 0, 0, 128});
    require(r.read_rgba({0, 0, 64, 64}, pixels, size), "Translucent analytic 2D reference readback failed");
    check(16, 16, {128, 0, 0, 128}, "Analytic shapes must preserve premultiplied target storage");
    r.pop_render_target();
    const auto composite = r.create_render_target({64, 64}, ScaleMode::Nearest);
    require(bool(composite), "Analytic 2D composite target allocation failed");
    r.push_render_target(composite);
    r.clear(colors::black);
    r.draw_texture(target.texture(), {0, 0, 64, 64});
    require(r.read_rgba({0, 0, 64, 64}, pixels, size), "Analytic 2D composite readback failed");
    check(16, 16, {128, 0, 0, 255}, "Translucent analytic shapes must composite without a second alpha multiply");
    r.pop_render_target();
}

void check_webgl_instances(Renderer2D& r, Renderer3D& r3d) {
    require(!r3d.capabilities().geometry_pass && !r.capabilities().render_attachments,
            "WebGL must report unsupported standalone geometry attachments");
    require(!r3d.capabilities().shared_preparation && !r3d.prepare({}),
            "Unsupported shared preparation must return diagnostics");
    require(!r3d.capabilities().sun_shadow_pass && !r3d.capabilities().surface_pass &&
                !r.capabilities().render_attachment_arrays && !r3d.render_sun_shadow({}, {}, {64, 64}) &&
                !r3d.render_surface({}, {}),
            "Unsupported public shadow/surface operations must fail explicitly");
    require(!r3d.capabilities().material_inputs && !r3d.create_material_inputs({}),
            "Unsupported material inputs must fail explicitly");
    require(!r3d.capabilities().local_shadow_pass && !r3d.render_local_shadows({}, {}, {}, {64, 64}),
            "Unsupported local shadow operation must fail explicitly");
    require(!r3d.capabilities().bloom_pass && !r3d.capabilities().output_pass && !r3d.render_bloom({}, {}) &&
                !r3d.render_output({}, {}),
            "Unsupported bloom/output operations must fail explicitly");
    const auto unsupported = r3d.render_geometry({}, {});
    require(!unsupported && !unsupported.diagnostics.empty(), "Unsupported geometry must return diagnostics");
    require(!r.create_render_attachment({64, 64}, TextureFormat::Depth32Float),
            "Unsupported depth attachment allocation must fail explicitly");
    auto view = r3d.create_view({64, 64}, {.samples = 1, .background = colors::black}).value_or({});
    const auto target = r.create_render_target({64, 64}, ScaleMode::Nearest);
    const auto mesh = r3d.create_mesh(quad()).value_or({});
    require(view && target && mesh, "Instance retention reference allocation failed");
    std::vector<MeshInstance3D> copies{{.transform = Mat4f::translation({-1, 0, 0}), .tint = Color::rgb(255, 0, 0)},
                                       {.transform = Mat4f::translation({1, 0, 0}), .tint = Color::rgb(0, 255, 0)},
                                       {.transform = Mat4f::translation({100, 0, 0}), .tint = Color::rgb(255, 255, 0)}};
    auto batch = InstanceBatch3D::create(mesh, copies).value_or({});
    require(bool(batch), "Instance retention batch allocation failed");
    Scene3D scene;
    scene.camera = Camera3D::look_at({0, 0, 5}, {});
    scene.camera.projection = Projection3D::Orthographic;
    scene.camera.height = 4;
    const auto submit = [&] {
        scene.clear();
        require(bool(scene.draw_instanced(batch, {.unlit = true})), "Instance retention submission failed");
    };
    const auto render = [&] { require(bool(r3d.render(view, scene)), "Instance retention render failed"); };
    const auto pixel = [&](const View3D& image, i32 x, Color expected) {
        r.push_render_target(target);
        r.clear(colors::black);
        r.draw_texture(image.texture(), {0, 0, 64, 64});
        std::vector<u8> pixels;
        Vec2i size;
        require(r.read_rgba({0, 0, 64, 64}, pixels, size), "Instance retention readback failed");
        r.pop_render_target();
        const std::array<int, 4> color{expected.r, expected.g, expected.b, expected.a};
        for (u32 c = 0; c < 4; ++c)
            require(std::abs(int(pixels[(32 * 64 + x) * 4 + c]) - color[c]) <= 3,
                    "Retained instances must follow source edits, visibility and view ownership");
    };
    submit();
    render();
    require(r3d.debug_stats().retained_upload_bytes > 0, "First visible instance list must upload");
    pixel(view, 16, Color::rgb(255, 0, 0));
    pixel(view, 48, Color::rgb(0, 255, 0));
    render();
    require(r3d.debug_stats().retained_upload_bytes == 0 && r3d.debug_stats().instance_upload_bytes == 0 &&
                r3d.debug_stats().prepared_reused == 1,
            "Unchanged instances must reuse prepared transforms and GPU storage");
    copies[0].transform = {};
    copies[0].tint = Color::rgb(0, 0, 255);
    copies[2].transform = Mat4f::translation({-1, 0, 0});
    require(bool(batch.update(0, copies)), "Instance retention edit failed");
    submit();
    render();
    pixel(view, 16, Color::rgb(255, 255, 0));
    pixel(view, 32, Color::rgb(0, 0, 255));
    require(r3d.stats().instances == 3, "Newly visible edited instances must enter the retained list");
    scene.camera = Camera3D::look_at({1, 0, 5}, {1, 0, 0});
    scene.camera.projection = Projection3D::Orthographic;
    scene.camera.height = 2;
    render();
    pixel(view, 32, Color::rgb(0, 255, 0));
    require(r3d.stats().instances == 2, "Camera changes must rebuild visibility independently of batch revision");
    scene.camera = Camera3D::look_at({0, 0, 5}, {});
    scene.camera.projection = Projection3D::Orthographic;
    scene.camera.height = 4;
    render();
    const auto first = view;
    view = r3d.create_view({64, 64}, {.samples = 1, .background = colors::black}).value_or({});
    copies[0].tint = Color::rgb(255, 0, 255);
    require(bool(batch.update(0, {copies.data(), 1})), "Interleaved retained instance edit failed");
    render();
    pixel(view, 32, Color::rgb(255, 0, 255));
    pixel(first, 32, Color::rgb(0, 0, 255));
    view = first;
    render();
    pixel(view, 32, Color::rgb(255, 0, 255));
    require(bool(r3d.resize(view, {32, 32})) && bool(r3d.resize(view, {64, 64})), "Retained view resize failed");
    render();
    require(r3d.debug_stats().retained_upload_bytes > 0, "Resized views must recreate their retained lists");
    pixel(view, 32, Color::rgb(255, 0, 255));
    scene.clear();
    require(bool(scene.draw(mesh, {}, {.color = Color::rgb(0, 255, 255), .unlit = true})),
            "Transient replacement submission failed");
    render();
    pixel(view, 32, Color::rgb(0, 255, 255));
    scene.clear();
    require(bool(scene.draw(mesh, Mat4f::translation({-1, 0, 0}), {.color = Color::rgb(255, 0, 0), .unlit = true})),
            "Transient transform edit failed");
    render();
    pixel(view, 16, Color::rgb(255, 0, 0));
    pixel(view, 32, colors::black);
    auto wide_data = quad();
    for (auto& v : wide_data.vertices)
        v.position.x *= 2;
    const auto wide = r3d.create_mesh(wide_data).value_or({});
    require(bool(wide), "Replacement mesh allocation failed");
    scene.clear();
    require(bool(scene.draw(wide, {}, {.color = Color::rgb(255, 255, 0), .unlit = true})),
            "Replacement mesh submission failed");
    render();
    pixel(view, 44, Color::rgb(255, 255, 0));
    auto& overlay = scene.overlay();
    overlay.camera = scene.camera;
    require(bool(overlay.draw(mesh, {}, {.color = Color::rgb(255, 0, 0), .unlit = true})),
            "Retained overlay submission failed");
    render();
    pixel(view, 32, Color::rgb(255, 0, 0));
    pixel(view, 44, Color::rgb(255, 255, 0));
    // Refused cache allocations still draw through the original streaming path.
    copies.assign(36000, {.transform = Mat4f::translation({100, 0, 0})});
    copies.back().transform = {};
    scene.clear();
    require(bool(scene.draw_instanced(mesh, copies, {.unlit = true})), "Instance cache pressure submission failed");
    render();
    require(r3d.debug_stats().retained_refused > 0 && r3d.debug_stats().instance_upload_bytes > 0,
            "Over-budget instance lists must use the streaming fallback");
    pixel(view, 32, colors::white);
}

void check_webgl_materials(Renderer2D& r, Renderer3D& r3d) {
    auto view = r3d.create_view({64, 64}, {.samples = 1}).value_or({});
    auto target = r.create_render_target({64, 64}, ScaleMode::Nearest);
    auto mesh = r3d.create_mesh(quad()).value_or({});
    require(view && target && mesh, "Material reference allocation failed");
    const auto tex = [&](std::array<u8, 4> color) { return r.create_texture_from_rgba(color.data(), {1, 1}); };
    Scene3D scene;
    scene.camera = Camera3D::look_at({0, 0, 1000}, {});
    scene.camera.projection = Projection3D::Orthographic;
    scene.camera.height = 4;
    scene.camera.z_far = 2000;
    scene.lights.sun.color = {0, 0, 0};
    scene.lights.ambient = {0, 0, 0};
    std::vector<u8> pixels;
    Vec2i size;
    const auto read = [&](i32 x = 32, i32 y = 32) {
        require(bool(r3d.render(view, scene)), "Material reference render failed");
        r.push_render_target(target);
        r.clear(colors::black);
        r.draw_texture(view.texture(), {0, 0, 64, 64});
        require(r.read_rgba({0, 0, 64, 64}, pixels, size), "Material reference readback failed");
        r.pop_render_target();
        return std::array<int, 4>{pixels[(y * 64 + x) * 4], pixels[(y * 64 + x) * 4 + 1], pixels[(y * 64 + x) * 4 + 2],
                                  pixels[(y * 64 + x) * 4 + 3]};
    };
    const auto submit = [&](Material3D material, Mat4f transform = Mat4f{}) {
        scene.clear();
        require(bool(scene.draw(mesh, transform, material)), "Material reference submission failed");
        return read();
    };
    const auto near = [&](std::array<int, 4> value, std::array<int, 4> expected, const char* message) {
        for (u32 i = 0; i < 4; ++i)
            require(std::abs(value[i] - expected[i]) <= 3, message);
    };
    scene.lights.ambient = {1, 1, 1};
    near(submit({.occlusion = tex({128, 0, 0, 255})}), {188, 188, 188, 255},
         "AO data must be sampled in linear space, from R");
    near(submit({.occlusion = tex({0, 255, 255, 255}), .occlusion_strength = 0.5f}), {188, 188, 188, 255},
         "AO strength must interpolate linearly");
    scene.lights.ambient = {0, 0, 0};
    near(submit({.color = colors::black, .emissive = 1, .emissive_texture = tex({128, 0, 0, 255})}), {128, 0, 0, 255},
         "Emissive maps must decode sRGB and emit independently of base color");
    scene.lights.sun.direction = {0, 0, 1};
    scene.lights.sun.color = {0.2f, 0.2f, 0.2f};
    Material3D material{.color = Color::rgb(100, 100, 100), .metallic = 1};
    material.metallic_roughness = tex({0, 0, 255, 255});
    const auto smooth = submit(material);
    material.metallic_roughness = tex({255, 255, 255, 255});
    const auto rough = submit(material);
    require(smooth[0] > rough[0] + 30, "Roughness must come from the map's G channel");
    material.metallic_roughness = tex({0, 255, 0, 255});
    const auto dielectric = submit(material);
    require(std::abs(dielectric[0] - rough[0]) > 8, "Metallic must come from the map's B channel");
    material = {.normal = tex({128, 128, 255, 255})};
    const auto neutral = submit(material);
    material.normal = tex({255, 128, 128, 255});
    const auto tilted = submit(material);
    require(neutral[0] > tilted[0] + 40, "Normal maps must change the light-facing direction");
    material.normal_scale = 0;
    const auto flattened = submit(material);
    require(std::abs(flattened[0] - neutral[0]) <= 3, "Zero normal scale must recover the geometric normal");
    scene.lights.sun.direction = {0, 1, 0};
    material = {.normal = tex({128, 255, 128, 255})};
    const auto up = submit(material);
    material.normal = tex({128, 0, 128, 255});
    require(up[0] > submit(material)[0] + 40, "OpenGL normal maps must interpret green as up");
    material = {
        .color = {240, 40, 30, 255}, .unlit = true, .blend = Blend3D::Mask, .texture = tex({255, 255, 255, 100})};
    near(submit(material), {0, 0, 0, 255}, "Masked fragments below the cutoff must preserve the background");
    material.alpha_cutoff = 0.2f;
    near(submit(material), {240, 40, 30, 255}, "Masked fragments above the cutoff must write opaque color/depth");

    // Material uniforms survive other programs, but their values must follow
    // edits. Uploaded images and render targets have opposite texture origins.
    const std::array<u8, 16> stripes{255, 0, 0, 255, 255, 0, 0, 255, 0, 0, 255, 255, 0, 0, 255, 255};
    const auto uploaded = r.create_texture_from_rgba(stripes.data(), {2, 2});
    const auto surface = r.create_render_target({2, 2}, ScaleMode::Nearest);
    require(uploaded && surface, "Material orientation reference allocation failed");
    r.push_render_target(surface);
    r.clear(Color::rgb(0, 0, 255));
    r.fill_rect({0, 0, 2, 1}, Color::rgb(255, 0, 0));
    r.pop_render_target();
    submit({.unlit = true, .texture = surface.texture()});
    near(read(32, 28), {255, 0, 0, 255}, "Render-target material must preserve its top row");
    near(read(32, 36), {0, 0, 255, 255}, "Render-target material must preserve its bottom row");
    submit({.unlit = true, .texture = uploaded});
    near(read(32, 28), {255, 0, 0, 255},
         "Changing from a target to an uploaded image must refresh material UV orientation");
    near(read(32, 36), {0, 0, 255, 255}, "Uploaded material must preserve its bottom row after a target draw");

    // One texture can have different material wrapping in the same pass. A 2D
    // readback between renders also changes unit 0 and must invalidate bindings.
    const std::array<u8, 16> checker{255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
    const auto pattern = r.create_texture_from_rgba(checker.data(), {2, 2});
    const auto constant_uv = [&](Vec2f uv) {
        auto data = quad();
        for (auto& vertex : data.vertices)
            vertex.uv = uv;
        auto value = r3d.create_mesh(data).value_or({});
        require(bool(value), "Sampler reference mesh allocation failed");
        return value;
    };
    const auto wrapped = constant_uv({1.25f, 0.25f});
    scene.clear();
    require(bool(scene.draw(wrapped, Mat4f::translation({-0.75f, 0, 0}), {.unlit = true, .texture = pattern})),
            "Clamped sampler submission failed");
    require(bool(scene.draw(wrapped, Mat4f::translation({0.75f, 0, 0}),
                            {.unlit = true, .texture = pattern, .wrap_u = Wrap3D::Repeat})),
            "Repeated sampler submission failed");
    near(read(20, 32), {0, 255, 0, 255}, "Clamped and repeated materials must not change each other's sampler");
    near(read(44, 32), {255, 0, 0, 255}, "Repeat must survive an interleaved 2D pass");
    const auto sample = [&](const Mesh3D& value, Wrap3D u = Wrap3D::Clamp, Wrap3D v = Wrap3D::Clamp) {
        scene.clear();
        require(bool(scene.draw(value, {}, {.unlit = true, .texture = pattern, .wrap_u = u, .wrap_v = v})),
                "Sampler reference submission failed");
        return read();
    };
    near(sample(wrapped, Wrap3D::Mirror), {0, 255, 0, 255}, "Mirror must flip the second horizontal tile");
    near(sample(constant_uv({2.25f, 0.25f}), Wrap3D::Mirror), {255, 0, 0, 255},
         "Mirror must restore the third horizontal tile");
    const auto vertical = constant_uv({0.25f, 1.25f});
    near(sample(vertical), {0, 0, 255, 255}, "Vertical clamp must use the bottom edge");
    near(sample(vertical, Wrap3D::Clamp, Wrap3D::Repeat), {255, 0, 0, 255}, "Vertical repeat must tile independently");
    near(sample(vertical, Wrap3D::Clamp, Wrap3D::Mirror), {0, 0, 255, 255},
         "Vertical mirror must flip the second tile");
    const auto filtered = constant_uv({0.5f, 0.25f});
    near(sample(filtered), {0, 255, 0, 255}, "Nearest sampling must retain sharp texels");
    r.set_scale_mode(pattern, ScaleMode::Linear);
    near(sample(filtered), {128, 128, 0, 255}, "Changing texture filtering must reach the next 3D render");
    r.set_scale_mode(pattern, ScaleMode::Mipmapped);
    near(sample(filtered), {128, 128, 0, 255}, "Mipmapped textures must keep linear magnification");
    r.set_scale_mode(pattern, ScaleMode::Nearest);
    near(sample(filtered), {0, 255, 0, 255}, "Returning to nearest must invalidate the material sampler");

    // Three copies share one skinned draw but retain independent palette offsets.
    auto data = quad();
    data.skin.resize(data.vertices.size(), {{0, 1, 255, 255}, {0.5f, 0.5f, 0, 0}});
    auto skin = r3d.create_mesh(data).value_or({});
    require(bool(skin), "Skinned mesh upload failed");
    scene.clear();
    scene.camera.position.z = 5;
    scene.camera.z_far = 10;
    const auto pose = [](Vec3f offset) {
        const Mat4f m = Mat4f::translation(offset);
        return std::array{m, m};
    };
    auto left = pose({-0.9f, 0, 0}), right = pose({0.9f, 0, 0}), outside = pose({-100, 0, 0});
    const Material3D unlit{.unlit = true};
    require(bool(scene.draw_skinned(skin, left, {}, unlit, Color::rgb(255, 0, 0))), "Left skinned submission failed");
    require(bool(scene.draw_skinned(skin, right, {}, unlit, Color::rgb(0, 255, 0))), "Right skinned submission failed");
    require(bool(scene.draw_skinned(skin, outside, Mat4f::translation({100, 0, 0}), unlit, Color::rgb(0, 0, 255))),
            "Posed visibility submission failed");
    require(scene.draws().size() == 1, "Independent skin palettes must preserve draw merging");
    near(read(18, 32), {255, 0, 0, 255}, "First palette must move the left skinned instance");
    near(read(46, 32), {0, 255, 0, 255}, "Second palette must move the right skinned instance independently");
    near(read(32, 32), {0, 0, 255, 255}, "A posed mesh must not be culled by its unposed bounds");
    require(r3d.stats().skinned_copies == 3 && r3d.stats().skinned_draws == 1 && r3d.debug_stats().skin_matrices == 6,
            "Skin upload and draw statistics must reflect the actual work");
    left = pose({-0.9f, 1.2f, 0});
    require(bool(scene.update_joint_matrices(0, left)), "Updating an existing pose failed");
    near(read(18, 32), {0, 0, 0, 255}, "Old pose must disappear after a joint update");
    near(read(18, 13), {255, 0, 0, 255}, "Joint updates must reach the next GPU frame");

    scene.clear();
    scene.sky = {.enabled = true};
    scene.camera = Camera3D::look_at({0, 0, 0}, {0, 1, -1});
    scene.lights.sun.direction = normalize({0.4f, 1, 0.3f});
    const Ray3 sky_ray = scene.camera.ray({32.5f, 32.5f}, {0, 0, 64, 64});
    const LinearRgb sky_reference = sky_color(scene.sky, scene.lights.sun.direction, sky_ray.direction);
    const auto expected = [](LinearRgb c) {
        const Color value = to_srgb({c.r, c.g, c.b, 1});
        return std::array<int, 4>{value.r, value.g, value.b, value.a};
    };
    near(read(), expected(sky_reference), "Browser sky rays/scattering must agree with the CPU atmosphere");
    require(r3d.debug_stats().sky_drawn, "Sky pass must report actual rendering");
    scene.sky.enabled = false;
    scene.camera = Camera3D::look_at({0, 0, 5}, {});
    scene.camera.projection = Projection3D::Orthographic;
    scene.camera.height = 4;
    scene.fog = {.enabled = true, .density = 0.25f, .colour = {0, 0, 1}};
    const LinearRgb fog_reference = fog_apply(scene.fog, scene.sky, scene.lights.sun.direction, scene.lights.sun.color,
                                              scene.camera.position, {0, 0, -1}, 5, {1, 0, 0});
    near(submit({.color = Color::rgb(255, 0, 0), .unlit = true}), expected(fog_reference),
         "Browser distance fog must agree with CPU opacity/color");
    scene.fog.colour_from_sky = true;
    const LinearRgb sky_fog = fog_apply(scene.fog, scene.sky, scene.lights.sun.direction, scene.lights.sun.color,
                                        scene.camera.position, {0, 0, -1}, 5, {1, 0, 0});
    near(submit({.color = Color::rgb(255, 0, 0), .unlit = true}), expected(sky_fog),
         "Texture-backed fog table must preserve sky-colored fog");
    require(r3d.debug_stats().fog_applied, "Fog must report actual application");

    scene.fog.enabled = false;
    scene.lights.sun.direction = normalize({1, 0, 2});
    scene.lights.sun.color = {0.7f, 0.7f, 0.7f};
    scene.lights.ambient = {0, 0, 0};
    scene.lights.shadow_bias = 0.1f;
    scene.lights.shadow_normal_offset = 0;
    scene.lights.shadow_resolution = 128;
    scene.lights.shadow_cascades = 2;
    scene.lights.shadow_distance = 10;
    const Material3D floor{.color = Color::rgb(128, 128, 128)};
    auto box = r3d.create_mesh(meshes::box({0.5f, 0.5f, 0.5f})).value_or({});
    const auto populate = [&](bool receives, bool casts) {
        scene.clear();
        auto ground = floor;
        ground.receives_shadow = receives;
        require(bool(scene.draw(mesh, Mat4f::scaling({4, 4, 1}), ground)), "Shadow floor submission failed");
        require(bool(scene.draw(box, Mat4f::translation({0.8f, 0, 1}), {.casts_shadow = casts})),
                "Shadow box submission failed");
    };
    populate(true, true);
    const auto unshadowed = read(37, 32);
    scene.lights.shadow_area = {{-2, -2, -0.1f}, {2, 2, 2}};
    const auto shadowed = read(37, 32);
    require(shadowed[0] + 20 < unshadowed[0], "Sun shadow must darken an occluded receiver");
    require(r3d.stats().shadow_cascades == 2 && r3d.stats().shadow_draws >= 2,
            "Cascade statistics must describe real depth passes");
    populate(false, true);
    near(read(37, 32), unshadowed, "receives_shadow=false must leave the receiver lit");
    populate(true, false);
    near(read(37, 32), unshadowed, "casts_shadow=false must remove the occluder from the maps");

    const auto mask = tex({255, 255, 255, 0});
    scene.clear();
    require(bool(scene.draw(mesh, Mat4f::scaling({4, 4, 1}), floor)), "Cutout shadow floor failed");
    require(bool(scene.draw(mesh, Mat4f::translation({0.8f, 0, 1}) * Mat4f::scaling({0.5f, 0.5f, 1}),
                            {.blend = Blend3D::Mask, .texture = mask})),
            "Cutout caster submission failed");
    near(read(37, 32), unshadowed, "Transparent mask texels must not cast shadows");
    const std::array<u8, 4> opaque{255, 255, 255, 255};
    require(r.update_texture(mask, {0, 0}, {1, 1}, opaque.data()), "Updating the cutout texture failed");
    require(read(37, 32)[0] + 20 < unshadowed[0], "Opaque mask texels must cast shadows after a texture update");

    data = quad();
    data.skin.resize(data.vertices.size(), {{0, 0, 0, 0}, {1, 0, 0, 0}});
    auto caster = r3d.create_mesh(data).value_or({});
    scene.clear();
    require(bool(scene.draw(mesh, Mat4f::scaling({4, 4, 1}), floor)), "Skinned shadow floor failed");
    const std::array<Mat4f, 1> caster_pose{Mat4f::translation({0.8f, 0, 1}) * Mat4f::scaling({0.5f, 0.5f, 1})};
    require(bool(scene.draw_skinned(caster, caster_pose, {})), "Skinned shadow submission failed");
    require(read(37, 32)[0] + 20 < unshadowed[0], "Skinned poses must be applied in shadow passes");
    const std::array<Mat4f, 1> moved_pose{Mat4f::translation({-1, 0, 1}) * Mat4f::scaling({0.5f, 0.5f, 1})};
    require(bool(scene.update_joint_matrices(0, moved_pose)), "Updating the shadow pose failed");
    near(read(37, 32), unshadowed, "A moved skinned caster must not leave a stale shadow");

    // Each view owns its diffuse-sky cache. Alternating views must not replace
    // each other's lighting, and time-of-day edits respect the update budget.
    scene = Scene3D{};
    scene.camera = Camera3D::look_at({0, 0, 5}, {});
    scene.camera.projection = Projection3D::Orthographic;
    scene.camera.height = 4;
    scene.sky = {.enabled = true, .intensity = 0.2f, .light_threshold = 0.001f, .light_interval = 4};
    scene.lights.sky_lights_scene = true;
    scene.lights.sun.color = {0, 0, 0};
    require(bool(scene.draw(mesh, {}, {.roughness = 1})), "Sky-lit mesh submission failed");
    const auto low = read();
    const auto first_view = view;
    view = r3d.create_view({64, 64}, {.samples = 1}).value_or({});
    require(bool(view), "Second sky-lit view allocation failed");
    scene.sky.intensity = 0.8f;
    const auto high = read();
    require(high[0] > low[0] + 8, "Sky intensity must affect the actual lit mesh");
    view = first_view;
    scene.sky.intensity = 0.2f;
    near(read(), low, "Interleaved views must retain independent sky lighting");
    scene.sky.intensity = 0.8f;
    near(read(), low, "Sky lighting must reuse its cached result before the update interval");
    near(read(), low, "Sky lighting must retain its update budget across renders");
    near(read(), high, "Sky lighting must refresh after the configured interval");
}
