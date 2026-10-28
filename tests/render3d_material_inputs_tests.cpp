#include "material_input_shaders.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <kin/platform/app.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/renderer/shader_reflect.hpp>
using namespace kin;
namespace {
constexpr Vec2i extent{160, 120};
constexpr Rectf rect{0, 0, 160, 120};
struct Fixture {
    Renderer2D& r;
    Renderer3D r3d{r};
    SurfaceTargets3D target{r.create_render_target(extent),
                            r.create_render_attachment(extent, TextureFormat::Depth32Float)};
    Mesh3D box = r3d.create_mesh(meshes::box({2, 2, 2})).value_or({});
    MaterialShader3D buffer, texture_buffer;
    ComputeShaderHandle producer;
    Fixture(Renderer2D& renderer) : r(renderer) {
        buffer = shader("buffer.frag.spv");
        texture_buffer = shader("texture_buffer.frag.spv");
        const auto bytes = material_input_shaders("producer.comp.spv");
        producer = r.create_compute_shader({bytes.data(), static_cast<u32>(bytes.size())});
        assert(target.colour && target.depth && box && producer);
    }
    MaterialShader3D shader(const char* name) {
        const auto bytes = material_input_shaders(name);
        return r3d.create_material_shader(bytes).value_or({});
    }
    std::vector<u8> read(const Texture& texture) {
        r.draw_texture(texture, rect);
        std::vector<u8> p;
        Vec2i size;
        assert(r.read_rgba(rect, p, size) && size == extent);
        r.present();
        return p;
    }
    std::vector<u8> picture(Material3D material) {
        Scene3D scene;
        assert(scene.draw(box, {}, material));
        r.clear(colors::black);
        const auto p = r3d.prepare(scene).value_or({});
        assert(p && r3d.render_surface(target, p, {.background = colors::black}));
        return read(target.colour.texture());
    }
    DataBuffer data(Vec4f value) { return r.create_data_buffer(sizeof(value), &value); }
};
u8 centre(const std::vector<u8>& p, u32 c = 0) {
    return p[(60 * 160 + 80) * 4 + c];
}
void basics(Fixture& f) {
    const auto data = f.data({1, 0, 0, 1});
    const std::array buffers{data};
    const auto inputs = f.r3d.create_material_inputs(f.buffer, {}, buffers).value_or({});
    assert(inputs);
    Material3D m{.shader = f.buffer, .shader_params = {1, 1, 1}, .inputs = inputs};
    auto red = f.picture(m);
    assert(centre(red) > 240 && centre(red, 1) == 0 && centre(red, 2) == 0);
    Vec4f green{0, 1, 0, 1};
    assert(f.r.update_data_buffer(data, 0, sizeof(green), &green));
    auto pixels = f.picture(m);
    assert(centre(pixels) == 0 && centre(pixels, 1) > 240);
    assert(f.r.write_data_buffer(data, 0, sizeof(green), [](auto out) {
        const Vec4f blue{0, 0, 1, 1};
        std::memcpy(out.data(), &blue, sizeof(blue));
    }));
    pixels = f.picture(m);
    assert(centre(pixels, 2) > 240 && centre(pixels) == 0);
    // Same input packet can be reused by independently prepared consumers.
    Scene3D scene;
    assert(scene.draw(f.box, {}, m));
    f.r.clear(colors::black);
    const auto prepared = f.r3d.prepare(scene).value_or({});
    assert(prepared && f.r3d.render_surface(f.target, prepared));
    assert(f.r3d.render_surface(f.target, prepared) && f.r3d.debug_stats().bounds_instance_tests == 0);
    f.r.present();
    // CPU writes after a queued material read preserve that read and expire preparation.
    f.r.clear(colors::black);
    const auto p = f.r3d.prepare(scene).value_or({});
    const auto a = f.r.create_render_target(extent);
    assert(f.r3d.render_surface({a, f.target.depth}, p));
    const Vec4f yellow{1, 1, 0, 1};
    assert(f.r.update_data_buffer(data, 0, sizeof(yellow), &yellow));
    assert(!p && !f.r3d.render_surface(f.target, p));
    const auto next = f.r3d.prepare(scene).value_or({});
    assert(next && f.r3d.render_surface(f.target, next));
    auto old = f.read(a.texture());
    assert(centre(old, 2) > 240 && centre(old) == 0);
    pixels = f.read(f.target.colour.texture());
    assert(centre(pixels) > 240 && centre(pixels, 1) > 240 && centre(pixels, 2) == 0);
    // A GPU producer updates an input in-frame without rebuilding world data.
    f.r.clear(colors::black);
    const auto gpu_prepared = f.r3d.prepare(scene).value_or({});
    ShaderParams params;
    params.uniforms[0] = 0;
    params.uniforms[1] = 1;
    params.uniforms[2] = 1;
    params.uniforms[3] = 1;
    assert(f.r.dispatch_compute(f.producer, {1, 1}, {.output_buffers = buffers, .params = &params}));
    assert(gpu_prepared && f.r3d.render_surface(f.target, gpu_prepared));
    pixels = f.read(f.target.colour.texture());
    assert(centre(pixels) == 0 && centre(pixels, 1) > 240 && centre(pixels, 2) > 240);
    // A temporary 2D target must remain owned by its stack across the upload's submission boundary.
    f.r.clear(colors::black);
    {
        auto scope = f.r.scoped_render_target(f.r.create_render_target(extent));
        const auto p = f.r3d.prepare(scene).value_or({});
        assert(p && f.r3d.render_surface(f.target, p));
        assert(f.r.update_data_buffer(data, 0, sizeof(green), &green));
        f.r.clear(Color::rgb(30, 40, 50));
        std::vector<u8> pixels;
        Vec2i actual;
        assert(f.r.read_rgba(rect, pixels, actual));
        assert(centre(pixels) == 30 && centre(pixels, 1) == 40 && centre(pixels, 2) == 50);
    }
    f.r.present();
    std::cout << "material inputs: immutable bindings, CPU/GPU revisions, ordered writes and shared preparation\n";
}
void types_and_feedback(Fixture& f) {
    const auto data = f.data({1, 1, 1, 1});
    const std::array buffers{data};
    const std::array<u8, 4> white{255, 255, 255, 255};
    const auto tex = f.r.create_texture_from_rgba(white.data(), {1, 1});
    const std::array textures{tex};
    auto inputs = f.r3d.create_material_inputs(f.texture_buffer, textures, buffers).value_or({});
    assert(inputs);
    auto pixels = f.picture({.shader = f.texture_buffer, .inputs = inputs});
    assert(centre(pixels) > 240);
    const std::array<u8, 4> blue{0, 0, 255, 255};
    assert(f.r.update_texture(tex, {0, 0}, {1, 1}, blue.data()));
    pixels = f.picture({.shader = f.texture_buffer, .inputs = inputs});
    assert(centre(pixels) == 0 && centre(pixels, 2) > 240);
    f.r.clear(colors::black);
    Scene3D queued;
    assert(queued.draw(f.box, {}, {.shader = f.texture_buffer, .inputs = inputs}));
    const auto prepared = f.r3d.prepare(queued).value_or({});
    const auto previous = f.r.create_render_target(extent);
    assert(prepared && f.r3d.render_surface({previous, f.target.depth}, prepared));
    assert(f.r.update_texture(tex, {0, 0}, {1, 1}, white.data()));
    assert(!prepared);
    const auto old = f.read(previous.texture());
    assert(centre(old) == 0 && centre(old, 2) > 240);
    pixels = f.picture({.shader = f.texture_buffer, .inputs = inputs});
    assert(centre(pixels) > 240 && centre(pixels, 2) > 240);
    assert(!f.r3d.create_material_inputs(f.texture_buffer, {}, buffers));
    assert(!f.r3d.create_material_inputs(f.texture_buffer, textures, {}));
    const std::array short_buffer{f.r.create_data_buffer(4)};
    assert(!f.r3d.create_material_inputs(f.buffer, {}, short_buffer));
    Scene3D scene;
    assert(scene.draw(f.box, {}, {.shader = f.buffer, .inputs = inputs}));
    assert(!f.r3d.prepare(scene)); // input set belongs to another shader
    scene.clear();
    assert(scene.draw(f.box, {}, {.shader = f.buffer}));
    assert(!f.r3d.prepare(scene));
    const auto integer = f.shader("uint.frag.spv");
    assert(integer);
    const u16 value = 128;
    const auto uint_tex = f.r.create_texture({1, 1}, TextureFormat::R16Uint, &value);
    const std::array ints{uint_tex};
    const auto int_inputs = f.r3d.create_material_inputs(integer, ints).value_or({});
    assert(int_inputs);
    assert(!f.r3d.create_material_inputs(integer, textures));
    assert(!f.r3d.create_material_inputs(f.texture_buffer, ints, buffers));
    pixels = f.picture({.shader = integer, .inputs = int_inputs});
    assert(centre(pixels) > 100 && centre(pixels) < 230);
    // Output feedback is rejected in both independent and stock rendering.
    const std::array feedback{f.target.colour.texture()};
    const auto bad = f.r3d.create_material_inputs(f.texture_buffer, feedback, buffers).value_or({});
    assert(bad);
    scene.clear();
    assert(scene.draw(f.box, {}, {.shader = f.texture_buffer, .inputs = bad}));
    f.r.clear(colors::black);
    auto p = f.r3d.prepare(scene).value_or({});
    assert(p && !f.r3d.render_surface(f.target, p));
    f.r.present();
    auto view = f.r3d.create_view(extent, {.samples = 1}).value_or({});
    assert(view);
    const std::array view_texture{view.texture()};
    const auto self = f.r3d.create_material_inputs(f.texture_buffer, view_texture, buffers).value_or({});
    assert(self);
    scene.clear();
    assert(scene.draw(f.box, {}, {.shader = f.texture_buffer, .inputs = self}));
    assert(!f.r3d.render(view, scene));
    Scene3D missing;
    assert(missing.draw(f.box, {}, {.shader = f.buffer}));
    assert(!f.r3d.warm_up(view, missing));
    for (const char* name :
         {"write.frag.spv", "gap.frag.spv", "cube.frag.spv", "buffer_gap.frag.spv", "limit.frag.spv"})
        assert(!f.shader(name));
    const auto budget = f.shader("budget.frag.spv");
    assert(budget);
    const std::array<Texture, 4> four{tex, tex, tex, tex};
    const std::array<DataBuffer, 8> eight{data, data, data, data, data, data, data, data};
    const auto full = f.r3d.create_material_inputs(budget, four, eight).value_or({});
    assert(full);
    pixels = f.picture({.shader = budget, .inputs = full});
    assert(centre(pixels, 2) > 240);
    const std::vector<Texture> excessive_textures(5, tex);
    const std::vector<DataBuffer> excessive_buffers(9, data);
    assert(!f.r3d.create_material_inputs(budget, excessive_textures, eight) &&
           !f.r3d.create_material_inputs(budget, four, excessive_buffers));
    const auto matrix = f.shader("matrix.frag.spv");
    assert(matrix);
    const Vec4f mat{1, 0, 0, 1};
    const std::array matrix_buffers{f.data(mat)};
    const auto matrix_input = f.r3d.create_material_inputs(matrix, {}, matrix_buffers).value_or({});
    assert(matrix_input);
    pixels = f.picture({.shader = matrix, .inputs = matrix_input});
    assert(centre(pixels) > 240 && centre(pixels, 1) == 0);
    std::cout << "material inputs: reflected numeric/layout/minimum-size validation, shader matching and feedback "
                 "rejection\n";
}
void arrays_and_lifetime(Fixture& f) {
    const auto raw = f.shader("array.frag.spv"), compare = f.shader("compare.frag.spv");
    assert(raw && compare);
    const auto array = f.r.create_render_attachment({128, 128}, TextureFormat::Depth32Float, 2);
    const std::array textures{array};
    const auto raw_inputs = f.r3d.create_material_inputs(raw, textures).value_or({});
    const auto compare_inputs = f.r3d.create_material_inputs(compare, textures).value_or({});
    assert(raw_inputs && compare_inputs);
    const std::array not_array{f.target.depth};
    assert(!f.r3d.create_material_inputs(raw, not_array));
    Scene3D scene;
    assert(scene.draw(f.box, {}));
    scene.lights.shadow_cascades = 1;
    f.r.clear(colors::black);
    auto p = f.r3d.prepare(scene).value_or({});
    assert(p && f.r3d.render_sun_shadow(array, p, extent));
    f.r.present();
    auto pixels = f.picture({.shader = raw, .shader_params = {1}, .inputs = raw_inputs});
    assert(centre(pixels) == 0); // unused layer
    pixels = f.picture({.shader = compare, .shader_params = {1, 1}, .inputs = compare_inputs});
    assert(centre(pixels) > 240);
    pixels = f.picture({.shader = compare, .shader_params = {1, -1}, .inputs = compare_inputs});
    assert(centre(pixels) == 0);
    // Drop packet, material and source allocations before queued GPU work submits.
    f.r.clear(colors::black);
    {
        const auto data = f.data({.2f, .4f, .7f, 1});
        const std::array b{data};
        const auto input = f.r3d.create_material_inputs(f.buffer, {}, b).value_or({});
        Scene3D temporary;
        assert(input && temporary.draw(f.box, {}, {.shader = f.buffer, .shader_params = {1, 1, 1}, .inputs = input}));
        const auto prepared = f.r3d.prepare(temporary).value_or({});
        assert(prepared && f.r3d.render_surface(f.target, prepared));
    }
    pixels = f.read(f.target.colour.texture());
    assert(centre(pixels, 2) > centre(pixels, 0));
    std::cout << "material inputs: raw/compared array sampling and queued resource lifetime\n";
}
void foreign(Fixture& f, App& app) {
    auto& window = app.create_window({.title = "foreign inputs", .width = 64, .height = 64, .hidden = true});
    Renderer2D other{make_render_backend(window, false, true)};
    Renderer3D other3d{other};
    other.clear(colors::black);
    other.present();
    const auto bytes = material_input_shaders("buffer.frag.spv");
    const auto shader = other3d.create_material_shader(bytes).value_or({});
    assert(shader);
    const Vec4f white{1, 1, 1, 1};
    const auto data = other.create_data_buffer(sizeof(white), &white);
    const std::array b{data};
    const auto packet = other3d.create_material_inputs(shader, {}, b).value_or({});
    assert(packet);
    assert(!f.r3d.create_material_inputs(f.buffer, {}, b));
    assert(!f.r3d.create_material_inputs(shader, {}, b));
    Scene3D scene;
    assert(scene.draw(f.box, {}, {.shader = shader, .shader_params = {1, 1, 1}, .inputs = packet}));
    assert(!f.r3d.prepare(scene));
    const std::array<u8, 4> rgba{255, 255, 255, 255};
    const auto texture = other.create_texture_from_rgba(rgba.data(), {1, 1});
    const std::array t{texture};
    const std::array own{f.data(white)};
    assert(!f.r3d.create_material_inputs(f.texture_buffer, t, own));
    other.clear(colors::black);
    other.present();
    std::cout << "material inputs: live foreign shaders, packets, textures and buffers rejected\n";
}
int lifetime() {
    MaterialShader3D shader;
    MaterialInputs3D inputs;
    DataBuffer data;
    {
        App app;
        auto& window = app.create_window({.title = "input lifetime", .width = 64, .height = 64, .hidden = true});
        Renderer2D r{make_render_backend(window, false, true)};
        Renderer3D r3d{r};
        if (!r3d.capabilities().material_inputs)
            return std::getenv("KIN_REQUIRE_GPU_TESTS") ? 1 : 77;
        const auto bytes = material_input_shaders("buffer.frag.spv");
        shader = r3d.create_material_shader(bytes).value_or({});
        const Vec4f white{1, 1, 1, 1};
        data = r.create_data_buffer(sizeof(white), &white);
        const std::array b{data};
        inputs = r3d.create_material_inputs(shader, {}, b).value_or({});
        assert(shader && inputs);
        r.clear(colors::black);
        r.present();
    }
    assert(!shader && !inputs);
    inputs = {};
    shader = {};
    data = {};
    std::cout << "material inputs: shader/packet observation and destruction after renderer lifetime are safe\n";
    return 0;
}

void temporal(Fixture& f) {
    auto view = f.r3d.create_view(extent, {.samples = 1, .background = colors::black, .hdr = true}).value_or({});
    assert(view);
    const auto data = f.data({.2f, .3f, .4f, 1});
    const std::array b{data};
    auto inputs = f.r3d.create_material_inputs(f.buffer, {}, b).value_or({});
    assert(inputs);
    Scene3D scene;
    scene.taa.enabled = true;
    const auto shot = [&] {
        f.r.clear(colors::black);
        assert(f.r3d.render(view, scene));
        const bool reset = f.r3d.debug_stats().taa_reset;
        f.r.present();
        return reset;
    };
    assert(scene.draw(f.box, {}, {.shader = f.buffer, .shader_params = {1, 1, 1}, .inputs = inputs}));
    assert(shot());
    assert(!shot());
    const Vec4f value{.7f, .3f, .4f, 1};
    assert(f.r.update_data_buffer(data, 0, sizeof(value), &value));
    assert(shot());
    assert(!shot());
    const auto before = f.r.backend_stats();
    for (int i = 0; i < 8; ++i)
        assert(!shot());
    const auto after = f.r.backend_stats();
    assert(after.gpu_memory.allocations - before.gpu_memory.allocations ==
           after.upload_staging_allocations - before.upload_staging_allocations);
    assert(after.upload_staging_bytes <= before.upload_staging_bytes);
    // A still-open 2D target batch must publish its write before TAA samples
    // its revision, rather than causing a one-frame-late history reset.
    const auto pattern = f.r.create_render_target(extent);
    const std::array textures{pattern.texture()};
    const auto textured = f.r3d.create_material_inputs(f.texture_buffer, textures, b).value_or({});
    assert(textured);
    scene.clear();
    assert(scene.draw(f.box, {}, {.shader = f.texture_buffer, .inputs = textured}));
    assert(shot());
    assert(!shot());
    f.r.clear(colors::black);
    {
        auto scope = f.r.scoped_render_target(pattern);
        f.r.clear(colors::white);
        assert(f.r3d.render(view, scene) && f.r3d.debug_stats().taa_reset);
    }
    f.r.present();
    assert(!shot());
    std::cout << "material inputs: stock opaque TAA invalidation on content changes and stable warmed resources\n";
}
} // namespace
int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--lifetime")
        return lifetime();
    App app;
    auto& window = app.create_window({.title = "material inputs", .width = 160, .height = 120, .hidden = true});
    Renderer2D r{make_render_backend(window, false, true)};
    Renderer3D r3d{r};
    if (!r3d.capabilities().material_inputs)
        return std::getenv("KIN_REQUIRE_GPU_TESTS") ? 1 : 77;
    if (argc > 1 && std::string_view(argv[1]) == "--foreign-device" && !r3d.capabilities().software &&
        !std::getenv("KIN_TEST_LIVE_GPU_DEVICES"))
        return 77;
    Fixture f{r};
    if (argc > 1 && std::string_view(argv[1]) == "--foreign-device") {
        foreign(f, app);
        return 0;
    }
    basics(f);
    types_and_feedback(f);
    arrays_and_lifetime(f);
    temporal(f);
}
