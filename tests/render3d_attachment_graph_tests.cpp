#include "attachment_graph_shaders.hpp"

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
constexpr Vec2i initial_size{128, 96};
struct Fixture {
    Renderer2D& r;
    Renderer3D r3d{r};
    Mesh3D box = r3d.create_mesh(meshes::box({2, 2, 2})).value_or({});
    ShaderHandle probe;
    Scene3D scene;
    explicit Fixture(Renderer2D& renderer) : r(renderer) {
        const auto code = attachment_graph_shaders("shadow_probe.frag.spv");
        probe =
            r.create_shader({.spirv = {code.data(), u32(code.size())}, .num_samplers = 1, .num_uniform_buffers = 1});
        assert(box && probe);
        scene.camera = Camera3D::look_at({2, 2, 6}, {});
        assert(scene.draw(box, {}, {.color = Color::rgb(180, 80, 30)}));
        assert(scene.draw(box, Mat4f::translation({0, 0, 1.5f}) * Mat4f::scaling({.7f, .7f, .7f}),
                          {.color = Color::rgba(40, 180, 220, 110), .blend = Blend3D::Alpha}));
        const std::array particles{ParticleSprite3D{{0, 0, 2.5f}, 1.5f, 0, {1, .2f, 0}, .5f},
                                   ParticleSprite3D{{0, 0, -1}, 3, 0, {0, .2f, 1}, .5f}};
        assert(scene.draw_particles({.softness = 0}, particles));
    }
    std::vector<u8> read(const Texture& image, Vec2i size = initial_size) {
        r.clear(colors::black);
        auto shown = image;
        RenderTarget converted;
        if (image.format() == TextureFormat::Rgba16Float) {
            converted = r.create_render_target(size);
            assert(r3d.render_output(converted, image));
            shown = converted.texture();
        }
        r.draw_texture(shown, {0, 0, f32(size.x), f32(size.y)});
        std::vector<u8> out;
        Vec2i actual;
        assert(r.read_rgba({0, 0, f32(size.x), f32(size.y)}, out, actual) && actual == size);
        r.present();
        return out;
    }
};
ResourceGraphDesc definition(Vec2i size = initial_size, bool preserve = false) {
    ResourceGraphDesc g;
    g.resources = {{.name = "opaque",
                    .kind = GraphResourceKind::RenderAttachment,
                    .format = TextureFormat::Rgba16Float,
                    .size = size},
                   {.name = "composed",
                    .kind = GraphResourceKind::RenderAttachment,
                    .format = TextureFormat::Rgba16Float,
                    .size = size},
                   {.name = "depth",
                    .kind = GraphResourceKind::RenderAttachment,
                    .format = TextureFormat::Depth32Float,
                    .size = size},
                   {.name = "output", .kind = GraphResourceKind::ColorTarget, .size = size, .exported = true},
                   {.name = "old_output", .kind = GraphResourceKind::ColorTarget, .size = size, .exported = preserve}};
    g.passes = {{.name = "opaque", .attachments = {{.resource = 0}, {.resource = 2}}},
                {.name = "transparent",
                 .attachments = {{.resource = 1, .previous = 0, .load = GraphAttachmentLoad::Load},
                                 {.resource = 2,
                                  .load = GraphAttachmentLoad::Load,
                                  .access = GraphAttachmentAccess::ReadOnlyDepth}}},
                {.name = "output", .reads = {1}, .writes = {3}},
                {.name = "old", .reads = {0}, .writes = {4}, .after = {"output"}}};
    return g;
}
struct Picture {
    std::vector<u8> final, old;
};
Picture render(Fixture& f, const ResourceGraphDesc& g, ResourceGraphCompiler& compiler, GraphResources& resources,
               bool reference, bool read = true, bool imported = false, std::span<const GraphImport> imports = {}) {
    f.r.clear(colors::black);
    const auto& plan = compiler.compile(g, available_graph_features(f.r), {.reuse_slots = !reference});
    assert(plan && resources.prepare(f.r, g, plan, imports));
    const auto p = f.r3d.prepare(f.scene).value_or({});
    assert(p);
    const std::array<std::function<bool()>, 4> ops{
        [&] {
            return bool(f.r3d.render_surface({.depth = resources.texture(2), .linear_colour = resources.texture(0)}, p,
                                             {.background = Color::rgb(20, 40, 60), .stage = SurfaceStage3D::Opaque}));
        },
        [&] {
            return bool(f.r3d.render_surface({.depth = resources.texture(g.passes[1].attachments.back().resource),
                                              .linear_colour = resources.texture(1)},
                                             p, {.stage = SurfaceStage3D::Transparent, .particles = true}));
        },
        [&] { return bool(f.r3d.render_output(resources.target(3), resources.texture(1))); },
        [&] { return bool(f.r3d.render_output(resources.target(4), resources.texture(0))); }};
    const auto before = f.r.backend_stats();
    assert(resources.execute(plan, ops) && p);
    const auto after = f.r.backend_stats();
    assert(after.texture_copies - before.texture_copies == plan.attachment_copies.size());
    assert(after.texture_copy_bytes - before.texture_copy_bytes == plan.attachment_copy_bytes);
    const bool copied =
        reference || imported || g.resources[0].persistent || g.resources[0].exported || g.resources[4].exported;
    assert(plan.attachment_copies.size() == u32(copied) + u32(reference && g.resources.size() > 5));
    assert((resources.texture(0) == resources.texture(1)) == !copied);
    assert(f.r3d.debug_stats().device_waits == 0);
    if (!read) {
        f.r.present();
        return {};
    }
    auto result = f.read(resources.texture(3), g.resources[3].size);
    const auto old_resource = g.resources[4].exported                                ? 4u
                              : g.resources[0].exported || g.resources[0].persistent ? 0u
                                                                                     : graph_no_slot;
    auto old = old_resource != graph_no_slot ? f.read(resources.texture(old_resource), g.resources[old_resource].size)
                                             : std::vector<u8>{};
    return {std::move(result), std::move(old)};
}
void versions(Fixture& f) {
    ResourceGraphCompiler c;
    GraphResources bindings;
    for (int working = 0; working < 3; ++working) {
        assert(f.r.set_color_space(working ? ColorSpace::Linear : ColorSpace::Gamma, working == 2));
        for (int variant = 0; variant < 5; ++variant) {
            auto g = definition(initial_size, variant == 1);
            g.resources[0].exported = variant == 2;
            g.resources[0].persistent = variant == 3;
            if (variant == 4) {
                g.resources.push_back({.name = "depth_version",
                                       .kind = GraphResourceKind::RenderAttachment,
                                       .format = TextureFormat::Depth32Float,
                                       .size = initial_size});
                g.passes[1].attachments.back() = {.resource = 5, .previous = 2, .load = GraphAttachmentLoad::Load};
            }
            for (auto& r : g.resources)
                if (r.kind == GraphResourceKind::ColorTarget)
                    r.hdr = working == 2;
            const auto optimized = render(f, g, c, bindings, false), reference = render(f, g, c, bindings, true);
            assert(optimized.final == reference.final && optimized.old == reference.old);
            if (variant == 1)
                assert(optimized.final != optimized.old);
        }
    }
    assert(f.r.set_color_space(ColorSpace::Gamma, false));
    auto g = definition();
    {
        GraphResources fresh;
        const auto& plan = c.compile(g, available_graph_features(f.r));
        const auto bytes = f.r.backend_stats().gpu_memory.texture_bytes;
        assert(fresh.prepare(f.r, g, plan));
        assert(f.r.backend_stats().gpu_memory.texture_bytes - bytes == plan.slot_payload_bytes);
    }
    for (int i = 0; i < 3; ++i)
        render(f, g, c, bindings, false, false);
    const auto before = f.r.backend_stats();
    const auto compilations = c.stats().compilations;
    for (int i = 0; i < 8; ++i)
        render(f, g, c, bindings, false, false);
    const auto after = f.r.backend_stats();
    assert(c.stats().compilations == compilations && after.texture_copies == before.texture_copies);
    assert(after.gpu_memory.allocations - before.gpu_memory.allocations ==
           after.upload_staging_allocations - before.upload_staging_allocations);
    assert(after.upload_staging_bytes <= before.upload_staging_bytes);
    const auto old = bindings.texture(3);
    const auto before_resize = f.read(old);
    g = definition({96, 64}, true);
    const auto a = render(f, g, c, bindings, false), b = render(f, g, c, bindings, true);
    assert(a.final == b.final && a.old == b.old && f.read(old) == before_resize);
    const auto plan = c.compile(g, available_graph_features(f.r));
    assert(bindings.prepare(f.r, g, plan));
    u32 calls = 0;
    std::array<std::function<bool()>, 4> ops;
    for (auto& op : ops)
        op = [&] {
            ++calls;
            return true;
        };
    auto other = c.compile(g, available_graph_features(f.r), {.reuse_slots = false});
    assert(!bindings.execute(other, ops) && !calls);
    assert(f.r.set_color_space(ColorSpace::Linear, false));
    assert(!bindings.execute(plan, ops) && !calls);
    assert(f.r.set_color_space(ColorSpace::Gamma, false));
    const auto copies = f.r.backend_stats().texture_copies;
    ops[1] = {};
    assert(!bindings.execute(plan, ops) && !calls && f.r.backend_stats().texture_copies == copies);
    bindings.clear();
    assert(!bindings.execute(plan, ops) && !calls);
    std::cout << "attachment graph: stock surfaces, exact optimized/copy pixels, branching readers, pins, HDR/resize, "
                 "cached plans and warmed handles\n";
}
void imports_and_lifetimes(Fixture& f) {
    auto g = definition();
    auto colour = f.r.create_render_attachment(initial_size, TextureFormat::Rgba16Float);
    auto depth = f.r.create_render_attachment(initial_size, TextureFormat::Depth32Float);
    f.r.clear();
    const auto p = f.r3d.prepare(f.scene).value_or({});
    assert(f.r3d.render_surface({.depth = depth, .linear_colour = colour}, p,
                                {.background = Color::rgb(20, 40, 60), .stage = SurfaceStage3D::Opaque}));
    g.resources[0].imported = g.resources[2].imported = true;
    g.passes[0].enabled = false;
    ResourceGraphCompiler c;
    GraphResources b;
    const std::array imports{GraphImport{.resource = 0, .texture = colour},
                             GraphImport{.resource = 2, .texture = depth}};
    const auto a = render(f, g, c, b, false, true, true, imports);
    const auto reference = render(f, g, c, b, true, true, true, imports);
    assert(a.final == reference.final);
    // Re-import this binding set's previous output under the same cached plan.
    // The imported handle must not be recycled as this frame's destination.
    const auto history = b.texture(1);
    const auto old_history = f.read(history);
    const std::array next_imports{GraphImport{.resource = 0, .texture = history},
                                  GraphImport{.resource = 2, .texture = depth}};
    const auto next = render(f, g, c, b, false, true, true, next_imports);
    assert(b.texture(1) != history && f.read(history) == old_history);
    GraphResources independent;
    const auto next_reference = render(f, g, c, independent, true, true, true, next_imports);
    assert(next.final == next_reference.final);
    const auto output = b.texture(3);
    const auto output_pixels = next.final;
    b.clear();
    colour = {};
    depth = {};
    assert(f.read(output) == output_pixels);
    // A queued raw copy retains its handles, and CPU uploads cannot overtake it.
    const u8 red[]{255, 0, 0, 255}, blue[]{0, 0, 255, 255};
    auto source = f.r.create_texture_from_rgba(red, {1, 1});
    auto target = f.r.create_render_target({1, 1});
    f.r.clear();
    auto prepared = f.r3d.prepare(f.scene).value_or({});
    assert(f.r.copy_texture(source, target.texture()) && prepared);
    assert(f.r.update_texture(source, {0, 0}, {1, 1}, blue) && !prepared);
    source = {};
    auto pixels = f.read(target.texture(), {1, 1});
    assert(pixels[0] == 255 && pixels[1] == 0 && pixels[2] == 0);
    f.r.clear();
    auto survivor = f.r.create_render_target({1, 1});
    assert(f.r.copy_texture(target.texture(), survivor.texture()));
    target = {};
    assert(f.read(survivor.texture(), {1, 1}) == pixels);
    // Copying into a target also publishes pending writes before CPU replacement.
    f.r.clear();
    auto replacement = f.r.create_render_target({1, 1});
    assert(f.r.copy_texture(survivor.texture(), replacement.texture()));
    assert(f.r.copy_texture(replacement.texture(), survivor.texture()));
    assert(f.r.update_texture(replacement.texture(), {0, 0}, {1, 1}, blue));
    assert(f.read(survivor.texture(), {1, 1}) == pixels);
    assert(!f.r.copy_texture(survivor.texture(), survivor.texture()));
    assert(!f.r.copy_texture(survivor.texture(), f.r.create_render_target({2, 1}).texture()));
    assert(!f.r.copy_texture(survivor.texture(), f.r.create_render_attachment({1, 1}, TextureFormat::Rgba16Float)));
    std::cout << "attachment graph: imported history preservation, retained resize outputs, queued release and CPU "
                 "read/write ordering\n";
}
std::vector<u8> probe(Fixture& f, const Texture& texture, u32 layer) {
    f.r.clear(colors::black);
    ShaderParams params;
    params.uniforms[0] = f32(layer);
    f.r.draw_shader_surface({0, 0, 128, 96}, f.probe, params, texture);
    std::vector<u8> out;
    Vec2i size;
    assert(f.r.read_rgba({0, 0, 128, 96}, out, size));
    f.r.present();
    return out;
}
void depth_arrays(Fixture& f) {
    const auto a = f.r.create_render_attachment({128, 128}, TextureFormat::Depth32Float, 3),
               b = f.r.create_render_attachment({128, 128}, TextureFormat::Depth32Float, 3);
    f.scene.lights.shadow_area = {{-10, -4, -10}, {10, 10, 10}};
    f.scene.lights.shadow_cascades = 3;
    f.scene.lights.shadow_distance = 20;
    for (int frame = 0; frame < 3; ++frame) {
        f.r.clear();
        const auto p = f.r3d.prepare(f.scene).value_or({});
        assert(f.r3d.render_sun_shadow(a, p, initial_size));
        const auto bytes = f.r.backend_stats().texture_copy_bytes;
        assert(f.r.copy_texture(a, b) && p);
        assert(f.r.backend_stats().texture_copy_bytes - bytes == 128 * 128 * 4 * 3);
        u64 sum = 0;
        for (u32 layer = 0; layer < 3; ++layer) {
            const auto x = probe(f, a, layer), y = probe(f, b, layer);
            assert(x == y);
            for (std::size_t i = 0; i < x.size(); i += 4)
                sum += x[i];
        }
        assert(sum > 0); // at least one cascade covers the visible caster
    }
    assert(!f.r.copy_texture(a, f.r.create_render_attachment({128, 128}, TextureFormat::Depth32Float)));
    std::cout << "attachment graph: exact D32 array copies across layers and repeated GPU cycles\n";
}
void foreign(Fixture& f, App& app) {
    auto& w = app.create_window({.title = "foreign copies", .width = 64, .height = 64, .hidden = true});
    Renderer2D other{make_render_backend(w, false, true)};
    const auto a = f.r.create_render_target({16, 16}), b = other.create_render_target({16, 16});
    assert(!f.r.copy_texture(b.texture(), a.texture()) && !f.r.copy_texture(a.texture(), b.texture()));
    f.r.clear();
    f.r.present();
    other.clear();
    other.present();
}
} // namespace
int main(int argc, char** argv) {
    App app;
    auto& w = app.create_window(
        {.title = "attachment graph", .width = initial_size.x, .height = initial_size.y, .hidden = true});
    Renderer2D r{make_render_backend(w, false, true)};
    Renderer3D check{r};
    if (!r.capabilities().texture_copies || !check.capabilities().surface_particles ||
        !check.capabilities().sun_shadow_pass)
        return std::getenv("KIN_REQUIRE_GPU_TESTS") ? 1 : 77;
    if (argc > 1 && std::string_view(argv[1]) == "--foreign-device" && !check.capabilities().software &&
        !std::getenv("KIN_TEST_LIVE_GPU_DEVICES"))
        return 77;
    // Creating mesh/shader fixtures is a test assertion only after GPU admission.
    Fixture f{r};
    if (argc > 1 && std::string_view(argv[1]) == "--foreign-device") {
        foreign(f, app);
    } else {
        versions(f);
        imports_and_lifetimes(f);
        depth_arrays(f);
    }
}
