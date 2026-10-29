#include "postprocess_shaders.hpp"

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
#include <limits>
using namespace kin;
namespace {
constexpr Vec2i extent{160, 120}, half{80, 60};
constexpr Rectf rect{0, 0, 160, 120};
float aces(float x) {
    return std::clamp(x * (2.51f * x + .03f) / (x * (2.43f * x + .59f) + .14f), 0.f, 1.f);
}
float encode(float x) {
    return x < .0031308f ? 12.92f * x : 1.055f * std::pow(x, 1.f / 2.4f) - .055f;
}
struct Fixture {
    Renderer2D& r;
    Renderer3D r3d{r};
    Texture source = r.create_storage_texture(extent, TextureFormat::Rgba16Float);
    BloomTargets3D bloom{r.create_render_attachment(half, TextureFormat::Rgba16Float),
                         r.create_render_attachment(half, TextureFormat::Rgba16Float)};
    Texture colour = r.create_render_attachment(extent, TextureFormat::Rgba16Float);
    Texture depth = r.create_render_attachment(extent, TextureFormat::Depth32Float);
    Mesh3D cube = r3d.create_mesh(meshes::box({2, 2, 2})).value_or({});
    ComputeShaderHandle fill;
    MaterialShader3D material;
    ShaderHandle probe;
    explicit Fixture(Renderer2D& renderer) : r(renderer) {
        const auto bytes = postprocess_shaders("fill.comp.spv");
        fill = r.create_compute_shader({bytes.data(), u32(bytes.size())});
        material = r3d.create_material_shader(postprocess_shaders("input.frag.spv")).value_or({});
        const auto p = postprocess_shaders("probe.frag.spv");
        probe = r.create_shader({.spirv = {p.data(), u32(p.size())}, .num_samplers = 1});
        assert(source && bloom.result && bloom.scratch && colour && depth && cube && fill && material && probe);
    }
    void produce(Vec4f value, bool spot = false) {
        ShaderParams p;
        p.uniforms[0] = value.x;
        p.uniforms[1] = value.y;
        p.uniforms[2] = value.z;
        p.uniforms[3] = value.w;
        p.uniforms[4] = spot ? 1.f : 0.f;
        const std::array outputs{source};
        assert(r.dispatch_compute(fill, extent, {.outputs = outputs, .params = &p}));
    }
    std::vector<u8> read(const Texture& t) {
        r.clear(colors::black);
        r.draw_texture(t, rect);
        std::vector<u8> p;
        Vec2i size;
        assert(r.read_rgba(rect, p, size) && size == extent);
        r.present();
        return p;
    }
    std::vector<u8> raw(const RenderTarget& target) {
        std::vector<u8> p;
        Vec2i size;
        {
            auto scope = r.scoped_render_target(target);
            assert(r.read_rgba(rect, p, size) && size == extent);
        }
        r.present();
        return p;
    }
    void surface(const Scene3D& scene) {
        const auto p = r3d.prepare(scene).value_or({});
        assert(p && r3d.render_surface({.depth = depth, .linear_colour = colour}, p));
    }
};
u8 centre(const std::vector<u8>& p, int c = 0) {
    return p[(60 * 160 + 80) * 4 + c];
}
void analytic(Fixture& f) {
    auto output = f.r.create_render_target(extent);
    f.r.clear(colors::black);
    f.produce({.125f, .25f, 1, .5f});
    assert(f.r3d.render_output(output, f.source, {.exposure = 1.5f}));
    assert(f.r3d.debug_stats().output_draws == 1 && f.r3d.stats().draws == 0 && f.r3d.debug_stats().device_waits == 0);
    auto p = f.raw(output);
    for (int i = 0; i < 3; ++i) {
        const std::array<float, 3> unpremult{.25f, .5f, 2};
        assert(std::abs(int(centre(p, i)) - std::lround(encode(aces(unpremult[i] * 1.5f)) * .5f * 255)) <= 1);
    }
    assert(std::abs(int(centre(p, 3)) - 128) <= 1);
    f.r.clear(colors::black);
    f.produce({.125f, .25f, 1, .5f});
    assert(f.r3d.render_output(output, f.source, {.exposure = 8, .tone_map_sdr = false}));
    p = f.raw(output);
    assert(std::abs(int(centre(p)) - std::lround(encode(.25f) * .5f * 255)) <= 1);
    // Stable dithering perturbs gamma output by at most one stored 8-bit step.
    const auto dithered = f.r.create_render_target(extent);
    f.r.clear(colors::black);
    f.produce({.04f, .04f, .04f, 1});
    assert(f.r3d.render_output(output, f.source));
    assert(f.r3d.render_output(dithered, f.source, {.dither = true}));
    const auto flat = f.raw(output), noise = f.raw(dithered);
    std::size_t changed = 0;
    for (std::size_t i = 0; i < flat.size(); ++i) {
        assert(std::abs(int(flat[i]) - noise[i]) <= 1);
        changed += flat[i] != noise[i];
    }
    assert(changed > 100);
    f.r.clear(colors::black);
    assert(f.r3d.render_output(dithered, f.source, {.dither = true}));
    assert(f.raw(dithered) == noise);
    // Pure additive radiance has no coverage but must survive the output stage.
    f.r.clear(colors::black);
    f.produce({2, 0, 0, 0});
    assert(f.r3d.render_output(output, f.source));
    p = f.raw(output);
    assert(centre(p) > 240 && centre(p, 3) == 0);
    // Bloom alpha extends glow over empty background; extraction is before exposure.
    f.r.clear(colors::black);
    f.produce({8, 4, 2, 1}, true);
    assert(f.r3d.render_bloom(f.bloom, f.source, {.threshold = .5f}));
    assert(f.r3d.debug_stats().bloom_draws == 5 && f.r3d.stats().draws == 0);
    assert(f.r3d.render_output(output, f.source, {.bloom = f.bloom.result, .bloom_strength = .6f}));
    p = f.raw(output);
    const auto halo = (60 * 160 + 96) * 4;
    assert(p[halo] > 0 && p[halo + 3] > 0);
    f.r.clear(colors::black);
    assert(f.r3d.render_bloom(f.bloom, f.source, {.threshold = 100}));
    assert(f.r3d.render_output(output, f.source, {.bloom = f.bloom.result, .bloom_strength = .6f}));
    p = f.raw(output);
    assert(p[halo] == 0 && p[halo + 3] == 0);
    std::cout << "postprocess: analytic ACES/exposure/premultiplication, additive coverage, bloom halo and threshold\n";
}
void parity(Fixture& f) {
    for (int working = 0; working < 3; ++working) {
        assert(f.r.set_color_space(working ? ColorSpace::Linear : ColorSpace::Gamma, working == 2));
        for (float strength : {0.f, .4f})
            for (float exposure : {.5f, 1.7f}) {
                auto view =
                    f.r3d
                        .create_view(
                            extent,
                            {.samples = 1, .hdr = true, .exposure = exposure, .bloom = strength, .bloom_threshold = 1})
                        .value_or({});
                const auto target = f.r.create_render_target(extent);
                Scene3D scene;
                scene.camera = Camera3D::look_at({0, 0, 5}, {});
                assert(scene.draw(
                    f.cube, Mat4f::translation({-.4f, 0, 0}),
                    {.color = Color::rgba(220, 70, 30, 180), .emissive = 5, .unlit = true, .blend = Blend3D::Alpha}));
                assert(scene.draw(f.cube, Mat4f::translation({.8f, .4f, -2}),
                                  {.color = Color::rgb(30, 80, 220), .emissive = 2, .unlit = true}));
                f.r.clear(colors::black);
                assert(f.r3d.render(view, scene));
                assert(f.r3d.debug_stats().output_draws == 1 &&
                       f.r3d.debug_stats().bloom_draws == (strength > 0 ? 5u : 0u));
                const auto stock = f.read(view.texture());
                f.r.clear(colors::black);
                f.surface(scene);
                if (strength > 0)
                    assert(f.r3d.render_bloom(f.bloom, f.colour));
                assert(f.r3d.render_output(target, f.colour,
                                           {.bloom = strength > 0 ? f.bloom.result : Texture{},
                                            .bloom_strength = strength,
                                            .exposure = exposure}));
                const auto explicit_pixels = f.read(target.texture());
                if (stock != explicit_pixels) {
                    int largest = 0;
                    for (std::size_t i = 0; i < stock.size(); ++i)
                        largest = std::max(largest, std::abs(int(stock[i]) - explicit_pixels[i]));
                    std::cerr << "stock/public mismatch working=" << working << " bloom=" << strength
                              << " exposure=" << exposure << " largest=" << largest << '\n';
                    assert(stock == explicit_pixels);
                }
            }
    }
    // Inspect HDR working output after switching back to gamma: it retains
    // linear light above one and leaves exposure to the frame's output stage.
    assert(f.r.set_color_space(ColorSpace::Linear, true));
    const auto hdr = f.r.create_render_target(extent);
    f.r.clear(colors::black);
    f.produce({2, 1, .5f, 1});
    assert(f.r3d.render_output(hdr, f.source, {.exposure = .05f, .dither = true}));
    f.r.present();
    assert(f.r.set_color_space(ColorSpace::Gamma, false));
    f.r.clear(colors::black);
    f.r.draw_shader_surface(rect, f.probe, {}, hdr.texture());
    std::vector<u8> numeric;
    Vec2i actual;
    assert(f.r.read_rgba(rect, numeric, actual));
    f.r.present();
    assert(std::abs(int(centre(numeric)) - 64) <= 1 && std::abs(int(centre(numeric, 1)) - 32) <= 1 &&
           std::abs(int(centre(numeric, 2)) - 16) <= 1);

    std::cout << "postprocess: exact stock/public surface+bloom+output parity in gamma, sRGB and HDR working formats\n";
}
void validation(Fixture& f) {
    const auto output = f.r.create_render_target(extent);
    const std::vector<u16> zero_half(80 * 60 * 4), zero_full(160 * 120 * 4);
    const auto wrong = f.r.create_texture(half, TextureFormat::Rgba16Float, zero_half.data());
    const auto sampler_only = f.r.create_texture(extent, TextureFormat::Rgba16Float, zero_full.data());
    const auto array = f.r.create_render_attachment(half, TextureFormat::Rgba16Float, 1);
    const auto odd = f.r.create_render_attachment({81, 60}, TextureFormat::Rgba16Float);
    const float inf = std::numeric_limits<float>::infinity(), nan = std::numeric_limits<float>::quiet_NaN();
    f.r.clear(colors::black);
    f.produce({2, 1, .5f, 1});
    assert(!f.r3d.render_bloom({}, f.source));
    assert(!f.r3d.render_bloom({wrong, f.bloom.scratch}, f.source));
    assert(!f.r3d.render_bloom({array, f.bloom.scratch}, f.source));
    assert(!f.r3d.render_bloom({odd, f.bloom.scratch}, f.source));
    assert(!f.r3d.render_bloom({f.bloom.result, f.bloom.result}, f.source));
    assert(!f.r3d.render_bloom(f.bloom, f.bloom.result));
    assert(!f.r3d.render_bloom(f.bloom, f.depth));
    for (float value : {-1.f, inf, nan}) {
        assert(!f.r3d.render_bloom(f.bloom, f.source, {.threshold = value}));
        assert(!f.r3d.render_output(output, f.source, {.exposure = value}));
        assert(!f.r3d.render_output(output, f.source, {.bloom_strength = value}));
    }
    assert(!f.r3d.render_output(output, f.source, {.bloom_strength = 1}));
    assert(!f.r3d.render_output(output, f.source, {.bloom = f.depth}));
    assert(!f.r3d.render_output({}, f.source));
    assert(!f.r3d.render_output(output, output.texture()));
    assert(f.r3d.render_output(output, f.source));
    assert(f.r3d.diagnostics().empty());
    f.r.present();
    // Exactly one surface colour destination; sampler-only float textures cannot be attachments.
    Scene3D scene;
    assert(scene.draw(f.cube, {}));
    f.r.clear(colors::black);
    const auto prepared = f.r3d.prepare(scene).value_or({});
    assert(prepared);
    assert(!f.r3d.render_surface({.colour = output, .depth = f.depth, .linear_colour = f.colour}, prepared));
    assert(!f.r3d.render_surface({.depth = f.depth, .linear_colour = sampler_only}, prepared));
    assert(!f.r3d.render_surface({.depth = f.depth, .linear_colour = f.bloom.result}, prepared));
    assert(f.r3d.render_surface({.depth = f.depth, .linear_colour = f.colour}, prepared));
    f.r.present();
    // Native-format feedback validation also applies to HDR working RenderTargets.
    assert(f.r.set_color_space(ColorSpace::Linear, true));
    const auto hdr = f.r.create_render_target(extent);
    assert(!f.r3d.render_output(hdr, hdr.texture()));
    assert(!f.r3d.render_output(hdr, f.source, {.bloom = hdr.texture()}));
    assert(f.r.set_color_space(ColorSpace::Gamma, false));
    std::cout
        << "postprocess: shapes/usages/formats/numeric options, output feedback and explicit HDR surface validation\n";
}
void ordering_and_lifetime(Fixture& f) {
    const std::array<u16, 4> white{0x3c00, 0x3c00, 0x3c00, 0x3c00}, blue{0, 0, 0x3c00, 0x3c00};
    auto input = f.r.create_texture({1, 1}, TextureFormat::Rgba16Float, white.data());
    const auto a = f.r.create_render_target(extent), b = f.r.create_render_target(extent);
    Scene3D scene;
    assert(scene.draw(f.cube, {}));
    f.r.clear(colors::black);
    const auto prepared = f.r3d.prepare(scene).value_or({});
    assert(prepared);
    assert(f.r3d.render_output(a, input));
    assert(f.r.update_texture(input, {0, 0}, {1, 1}, reinterpret_cast<const u8*>(blue.data())));
    assert(!prepared);
    assert(f.r3d.render_output(b, input));
    input = {};
    auto old = f.raw(a), next = f.raw(b);
    assert(centre(old) > 200 && centre(old, 2) > 200 && centre(next) == 0 && centre(next, 2) > 200);
    // Destroy bloom's application handles before recording's submission.
    f.r.clear(colors::black);
    f.produce({4, 2, 1, 1}, true);
    {
        BloomTargets3D temporary{f.r.create_render_attachment(half, TextureFormat::Rgba16Float),
                                 f.r.create_render_attachment(half, TextureFormat::Rgba16Float)};
        assert(f.r3d.render_bloom(temporary, f.source));
        assert(f.r3d.render_output(a, f.source, {.bloom = temporary.result, .bloom_strength = .5f}));
    }
    old = f.raw(a);
    assert(centre(old) > 200);
    // A CPU upload after a queued output write must replace that write, too.
    f.r.clear(colors::black);
    assert(f.r3d.render_output(a, f.source));
    std::vector<u8> red(160 * 120 * 4);
    for (std::size_t i = 0; i < red.size(); i += 4) {
        red[i] = 255;
        red[i + 3] = 255;
    }
    assert(f.r.update_texture(a.texture(), {0, 0}, extent, red.data()));
    old = f.raw(a);
    assert(centre(old) == 255 && centre(old, 1) == 0 && centre(old, 2) == 0);
    // Explicit HDR surface writes have the same CPU-upload ordering boundary.
    f.r.clear(colors::black);
    const auto surface_prepared = f.r3d.prepare(scene).value_or({});
    assert(surface_prepared);
    assert(f.r3d.render_surface({.depth = f.depth, .linear_colour = f.colour}, surface_prepared));
    std::vector<u16> blue_hdr(160 * 120 * 4);
    for (std::size_t i = 0; i < blue_hdr.size(); i += 4) {
        blue_hdr[i + 2] = 0x3c00;
        blue_hdr[i + 3] = 0x3c00;
    }
    assert(f.r.update_texture(f.colour, {0, 0}, extent, reinterpret_cast<const u8*>(blue_hdr.data())));
    assert(!surface_prepared);
    assert(f.r3d.render_output(b, f.colour));
    next = f.raw(b);
    assert(centre(next) == 0 && centre(next, 2) > 200);
    std::cout << "postprocess: ordered CPU uploads and queued resource retention\n";
}
void history_and_reuse(Fixture& f) {
    auto a = f.r3d.create_view(extent, {.samples = 1, .hdr = true, .bloom = .3f}).value_or({});
    auto b = f.r3d.create_view(extent, {.samples = 1, .hdr = true, .bloom = .3f}).value_or({});
    const auto output = f.r.create_render_target(extent);
    Scene3D scene;
    scene.taa.enabled = true;
    assert(scene.draw(f.cube, {}, {.emissive = 2}));
    for (int i = 0; i < 8; ++i) {
        f.r.clear(colors::black);
        assert(f.r3d.render(a, scene));
        const bool reset = f.r3d.debug_stats().taa_reset;
        f.produce({4, 1, 2, 1}, true);
        assert(f.r3d.render_bloom(f.bloom, f.source));
        assert(f.r3d.render_output(output, f.source, {.bloom = f.bloom.result, .bloom_strength = .7f}));
        assert(f.r3d.render(b, scene) && f.r3d.debug_stats().taa_reset == reset);
        const auto pa = f.read(a.texture()), pb = f.read(b.texture());
        assert(pa == pb);
    }
    // New rendered input revisions invalidate stock TAA through the existing material bindings.
    const std::array textures{f.bloom.result};
    const auto inputs = f.r3d.create_material_inputs(f.material, textures).value_or({});
    assert(inputs);
    scene.clear();
    assert(scene.draw(f.cube, {}, {.shader = f.material, .inputs = inputs}));
    const auto shot = [&] {
        f.r.clear(colors::black);
        assert(f.r3d.render(a, scene));
        const auto reset = f.r3d.debug_stats().taa_reset;
        f.r.present();
        return reset;
    };
    assert(shot());
    assert(!shot());
    f.r.clear(colors::black);
    assert(f.r3d.render_bloom(f.bloom, f.source, {.threshold = 2}));
    f.r.present();
    assert(shot());
    assert(!shot());
    const auto tick = [&] {
        f.r.clear(colors::black);
        f.produce({2, 1, .5f, 1}, true);
        assert(f.r3d.render_bloom(f.bloom, f.source));
        assert(f.r3d.render_output(output, f.source, {.bloom = f.bloom.result, .bloom_strength = .3f, .dither = true}));
        assert(f.r3d.debug_stats().device_waits == 0);
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
    std::cout << "postprocess: stock TAA isolation, input revision invalidation and warmed GPU handle reuse\n";
}
void foreign(Fixture& f, App& app) {
    auto& window = app.create_window({.title = "foreign postprocess", .width = 64, .height = 64, .hidden = true});
    Renderer2D other{make_render_backend(window, false, true)};
    const auto tex = other.create_render_attachment(extent, TextureFormat::Rgba16Float);
    const auto small = other.create_render_attachment(half, TextureFormat::Rgba16Float);
    const auto target = other.create_render_target(extent);
    other.clear();
    other.present();
    assert(!f.r3d.render_bloom(f.bloom, tex));
    assert(!f.r3d.render_bloom({small, f.bloom.scratch}, f.source));
    assert(!f.r3d.render_output(target, f.source));
    assert(!f.r3d.render_output(f.r.create_render_target(extent), tex));
    assert(!f.r3d.render_output(f.r.create_render_target(extent), f.source, {.bloom = small, .bloom_strength = 1}));
    Scene3D scene;
    assert(scene.draw(f.cube, {}));
    f.r.clear();
    const auto p = f.r3d.prepare(scene).value_or({});
    assert(p);
    assert(!f.r3d.render_surface({.depth = f.depth, .linear_colour = tex}, p));
    f.r.present();
    std::cout << "postprocess: foreign inputs, targets, scratch and HDR surface attachments rejected\n";
}
} // namespace
int main(int argc, char** argv) {
    App app;
    auto& window =
        app.create_window({.title = "postprocess graph", .width = extent.x, .height = extent.y, .hidden = true});
    Renderer2D r{make_render_backend(window, false, true)};
    Renderer3D r3d{r};
    if (!r3d.capabilities().bloom_pass || !r3d.capabilities().output_pass)
        return std::getenv("KIN_REQUIRE_GPU_TESTS") ? 1 : 77;
    const bool foreign_test = argc > 1 && std::string_view(argv[1]) == "--foreign-device";
    if (foreign_test && !r3d.capabilities().software && !std::getenv("KIN_TEST_LIVE_GPU_DEVICES"))
        return 77;
    Fixture f{r};
    if (foreign_test) {
        foreign(f, app);
        return 0;
    }
    analytic(f);
    parity(f);
    validation(f);
    ordering_and_lifetime(f);
    history_and_reuse(f);
}
