#include <array>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <kin/core/json.hpp>
#include <kin/core/json_value.hpp>
#include <kin/platform/app.hpp>
#include <kin/render3d/material_asset3d.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <stdexcept>

namespace {
using namespace kin;
void cpu() {
    const auto parsed = parse_materials3d_asset(R"({"schema":"kin.materials3d/1","material":{
        "emissive":3,"emission_color":[0.6,0.2,0.05],"texture":{"asset":"a.png","sampler":{
        "wrap_u":"mirror","wrap_v":"clamp","min":"linear","mag":"nearest","mip":"nearest"}},
        "normal":{"asset":"n.png","sampler":"inherit"},"occlusion":{"asset":"ao.png","sampler":null}}})");
    assert(parsed);
    const auto& m = parsed->base.material;
    assert(m.emission_mode == EmissionMode3D::Independent && m.emission_color == (LinearRgb{.6f, .2f, .05f}));
    assert(m.texture->sampler == (TextureSampler3D{Wrap3D::Mirror, Wrap3D::Clamp, TextureFilter3D::Linear,
                                                   TextureFilter3D::Nearest, MipFilter3D::Nearest}));
    assert(!m.normal->reset_sampler && m.occlusion->reset_sampler && !m.occlusion->sampler);
    const auto legacy = parse_materials3d_asset(R"({"schema":"kin.materials3d/1","material":{"emissive":2}})");
    assert(legacy && legacy->base.material.emission_mode == EmissionMode3D::BaseRelative);
    for (const auto* bad : {R"({"emission_color":[1,0]})", R"({"emission_color":[-1,0,0]})",
                            R"({"emission_color":[1,0,0],"emission_mode":"base_relative"})",
                            R"({"texture":{"asset":"a.png","filter":"nearest","sampler":null}})",
                            R"({"texture":{"asset":"a.png","sampler":{"min":"cubic"}}})"}) {
        assert(!parse_materials3d_asset(std::string("{\"schema\":\"kin.materials3d/1\",\"material\":") + bad + "}"));
    }
    Material3D material;
    material.set_emission({0, 0, 0}, 10);
    assert(!material.emits_light());
    material.set_emission({1, 0, 0}, 0);
    assert(!material.emits_light());
    material.emissive = 2;
    assert(material.emits_light());
}
struct Scratch {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("kin-fidelity-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Scratch() { std::filesystem::create_directories(path); }
    ~Scratch() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};
std::vector<u8> shoot(Renderer2D& renderer, Renderer3D& r3d, View3D& view, const Scene3D& scene) {
    renderer.clear(colors::black);
    assert(r3d.render(view, scene));
    renderer.draw_texture(view.texture(), {0, 0, 160, 120});
    std::vector<u8> pixels;
    Vec2i size;
    assert(renderer.read_rgba({0, 0, 160, 120}, pixels, size) && size == (Vec2i{160, 120}));
    renderer.present();
    return pixels;
}
void check_gpu(Renderer2D& renderer) {
    Renderer3D r3d{renderer};
    auto made = r3d.create_view({160, 120}, {.samples = 1, .background = colors::black});
    assert(made);
    View3D view = *made;
    MeshData3D data;
    data.vertices = {{{-1, -1, 0}, {0, 0, 1}, {0, 0}, Color::rgb(0, 0, 255)},
                     {{1, -1, 0}, {0, 0, 1}, {1, 0}, Color::rgb(0, 0, 255)},
                     {{1, 1, 0}, {0, 0, 1}, {1, 1}, Color::rgb(0, 0, 255)},
                     {{-1, 1, 0}, {0, 0, 1}, {0, 1}, Color::rgb(0, 0, 255)}};
    data.indices = {0, 1, 2, 0, 2, 3};
    const auto mesh = r3d.create_mesh(data).value_or({});
    assert(mesh);
    Scene3D scene;
    scene.camera = Camera3D::look_at({0, 0, 5}, {});
    scene.lights.sun.color = {};
    scene.lights.ambient = {};
    Material3D material{.color = Color::rgb(0, 0, 255)};
    material.set_emission({.6f, .2f, .05f}, 1);
    const auto picture = [&](const Material3D& m) {
        scene.clear();
        assert(scene.draw(mesh, Mat4f{}, m, Color::rgb(0, 0, 255)));
        return shoot(renderer, r3d, view, scene);
    };
    auto pixels = picture(material);
    const u32 at = (60 * 160 + 80) * 4;
    const Color expected = to_srgb(LinearRgb{.6f, .2f, .05f}.with_alpha());
    assert(std::abs(int(pixels[at]) - expected.r) <= 2 && std::abs(int(pixels[at + 1]) - expected.g) <= 2 &&
           std::abs(int(pixels[at + 2]) - expected.b) <= 2);
    material.emission_mode = EmissionMode3D::BaseRelative;
    pixels = picture(material);
    assert(pixels[at] < 3 && pixels[at + 1] < 3 && pixels[at + 2] > 250);
    material.set_emission({.6f, .2f, .05f}, 2);
    pixels = picture(material);
    assert(pixels[at] > 250 && pixels[at + 1] > expected.g && pixels[at + 2] > expected.b);
    material = {.color = Color::rgb(0, 0, 80), .emissive = 2, .unlit = true};
    pixels = picture(material);
    const auto legacy_blue = to_srgb(LinearRgb{0, 0, 3 * srgb_to_linear(80.f / 255)}.with_alpha());
    assert(pixels[at] < 3 && std::abs(int(pixels[at + 2]) - legacy_blue.b) <= 2);
    material.set_emission({.25f, .02f, .1f}, 1);
    pixels = picture(material);
    const auto independent_unlit = to_srgb(LinearRgb{.25f, .02f, .1f + srgb_to_linear(80.f / 255)}.with_alpha());
    assert(std::abs(int(pixels[at]) - independent_unlit.r) <= 2 &&
           std::abs(int(pixels[at + 1]) - independent_unlit.g) <= 2 &&
           std::abs(int(pixels[at + 2]) - independent_unlit.b) <= 2);
    material.set_emission({0, 0, 0}, 1);
    pixels = picture(material);
    assert(pixels[at] < 3 && pixels[at + 1] < 3 && std::abs(int(pixels[at + 2]) - 80) <= 2);

    for (const bool linear : {false, true}) {
        assert(renderer.set_color_space(linear ? ColorSpace::Linear : ColorSpace::Gamma));
        view = r3d.create_view({160, 120}, {.samples = 1, .background = colors::black}).value_or({});
        assert(view);
        // A zero normal requests flat reconstruction; a usable UV frame must still apply the normal map.
        auto smooth_data = data;
        constexpr std::array<Vec2f, 4> normal_uv{{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
        for (u32 i = 0; i < 4; ++i)
            smooth_data.vertices[i].uv = normal_uv[i];
        for (auto& v : smooth_data.vertices)
            v.color = colors::white;
        auto flat_data = smooth_data;
        for (auto& v : flat_data.vertices)
            v.normal = {};
        const auto smooth_mesh = r3d.create_mesh(smooth_data).value_or({});
        const auto flat_mesh = r3d.create_mesh(flat_data).value_or({});
        const u8 bent[]{230, 128, 204, 255};
        const auto normal = renderer.create_texture({1, 1}, TextureFormat::Rgba8, bent);
        assert(normal && smooth_mesh && flat_mesh);
        scene.clear();
        scene.lights.sun = {.direction = {1, 0, 0}, .color = {1, 1, 1}};
        assert(scene.draw(smooth_mesh, {}, {.double_sided = true, .normal = normal}));
        const auto mapped = shoot(renderer, r3d, view, scene);
        scene.clear();
        assert(scene.draw(flat_mesh, {}, {.double_sided = true, .normal = normal}));
        const auto reconstructed = shoot(renderer, r3d, view, scene);
        for (std::size_t i = 0; i < mapped.size(); ++i)
            assert(std::abs(int(mapped[i]) - reconstructed[i]) <= 1);
        assert(mapped[at] > 100);
        scene.lights.sun.color = {};
        for (auto& v : data.vertices)
            v.color = colors::white;
        const auto white = r3d.create_mesh(data).value_or({});
        assert(white);
        const Material3D red{.color = Color::rgba(255, 0, 0, 128), .unlit = true, .blend = Blend3D::Alpha};
        const Material3D green{.color = Color::rgba(0, 255, 0, 128), .unlit = true, .blend = Blend3D::Alpha};
        const auto near = Mat4f::translation({0, 0, 1}), far = Mat4f::translation({0, 0, -2});
        const std::array<MeshInstance3D, 2> copies{{{near}, {far}}};
        scene.clear();
        assert(scene.draw_instanced(white, copies, red));
        assert(scene.draw(white, {}, green));
        const auto batched = shoot(renderer, r3d, view, scene);
        scene.clear();
        assert(scene.draw(white, far, red));
        assert(scene.draw(white, {}, green));
        assert(scene.draw(white, near, red));
        assert(shoot(renderer, r3d, view, scene) == batched);
        const f32 alpha = 128.f / 255;
        const auto expect_center = [&](const std::vector<u8>& picture, LinearRgb light) {
            // Gamma targets preserve the existing display-encoded blend contract; linear targets blend radiance.
            const auto color = linear ? to_srgb(light.with_alpha())
                                      : Color::rgb(u8(std::lround(light.r * 255)), u8(std::lround(light.g * 255)),
                                                   u8(std::lround(light.b * 255)));
            std::cerr << "composition pixel " << int(picture[at]) << ',' << int(picture[at + 1]) << ','
                      << int(picture[at + 2]) << " expected " << int(color.r) << ',' << int(color.g) << ','
                      << int(color.b) << '\n';
            assert(std::abs(int(picture[at]) - color.r) <= 3 && std::abs(int(picture[at + 1]) - color.g) <= 3 &&
                   std::abs(int(picture[at + 2]) - color.b) <= 3);
        };
        expect_center(batched, {alpha + alpha * (1 - alpha) * (1 - alpha), alpha * (1 - alpha), 0});
        if (r3d.capabilities().particles) {
            const std::array<ParticleSprite3D, 2> sprites{
                {{{0, 0, 1}, 2, 0, {1, 0, 0}, alpha}, {{0, 0, -2}, 2, 0, {1, 0, 0}, alpha}}};
            scene.clear();
            assert(scene.draw(white, {}, green));
            assert(scene.draw_particles({.softness = 0}, sprites));
            expect_center(shoot(renderer, r3d, view, scene),
                          {alpha + alpha * (1 - alpha) * (1 - alpha), alpha * (1 - alpha), 0});
            scene.clear();
            assert(scene.draw(white, {}, green));
            assert(scene.draw(white, near, red));
            const ParticleSprite3D spark{{0, 0, -2}, 2, 0, {0, 0, 1}, 1};
            assert(scene.draw_particles({.blend = ParticleBlend3D::Additive, .softness = 0}, {&spark, 1}));
            expect_center(shoot(renderer, r3d, view, scene), {alpha, alpha * (1 - alpha), (1 - alpha) * (1 - alpha)});
        }
        const u8 texels[]{255, 0, 0, 255, 0, 255, 0, 255};
        const auto image = renderer.create_texture_from_rgba(texels, {2, 1});
        assert(image);
        renderer.set_scale_mode(image, ScaleMode::Mipmapped);
        for (auto& v : data.vertices)
            v.uv = {1.25f, .5f};
        const auto sampling_mesh = r3d.create_mesh(data).value_or({});
        material = {};
        material.set_emission({1, 1, 1});
        material.emissive_texture = image;
        material.texture_samplers[4] = TextureSampler3D{Wrap3D::Repeat, Wrap3D::Clamp, TextureFilter3D::Nearest,
                                                        TextureFilter3D::Nearest, MipFilter3D::None};
        scene.clear();
        const auto left = Mat4f::translation({-1.1f, 0, 0}) * Mat4f::scaling({.7f, .7f, .7f});
        const auto right = Mat4f::translation({1.1f, 0, 0}) * Mat4f::scaling({.7f, .7f, .7f});
        assert(scene.draw(sampling_mesh, left, material));
        material.texture_samplers[4]->wrap_u = Wrap3D::Clamp;
        assert(scene.draw(sampling_mesh, right, material));
        pixels = shoot(renderer, r3d, view, scene);
        for (bool is_right : {false, true}) {
            const auto projected = scene.camera.project({is_right ? 1.1f : -1.1f, 0, 0}, {0, 0, 160, 120});
            assert(projected);
            const auto i = (u32(projected->pos.y) * 160 + u32(projected->pos.x)) * 4;
            assert(pixels[i + (is_right ? 1 : 0)] > 250 && pixels[i + (is_right ? 0 : 1)] < 3);
        }

        const auto sphere = r3d.create_mesh(meshes::sphere(1, 32, 16), {.clusters = true}).value_or({});
        assert(sphere && sphere.cluster_count() > 1);
        for (const bool gpu_cut : {false, true}) {
            scene.clear();
            scene.debug.gpu_culling = gpu_cut;
            assert(scene.draw(sphere, {}, {.unlit = true}));
            scene.lod.enabled = false;
            const auto complete = shoot(renderer, r3d, view, scene);
            scene.lod.enabled = true;
            for (const u32 budget : {0u, 1u}) {
                scene.lod.max_cluster_triangles = budget;
                assert(shoot(renderer, r3d, view, scene) == complete);
                assert(r3d.debug_stats().cluster_budget_fallbacks == 1 && r3d.debug_stats().cluster_overflow == 0);
            }
            scene.lod.max_cluster_triangles = sphere.leaf_triangles() - 1;
            (void)shoot(renderer, r3d, view, scene);
            assert(r3d.debug_stats().cluster_budget_fallbacks == 1);
            scene.lod.max_cluster_triangles = sphere.leaf_triangles();
            (void)shoot(renderer, r3d, view, scene);
            assert(r3d.debug_stats().cluster_budget_fallbacks == 0);
        }
        if (linear && r3d.capabilities().global_illumination) {
            scene.clear();
            scene.lod.enabled = false;
            scene.global_illumination = {.enabled = true,
                                         .area = {{-2, -2, -2}, {2, 2, 2}},
                                         .cell = .25f,
                                         .levels = 1,
                                         .side = 32,
                                         .height = 32,
                                         .probes = {.enabled = false}};
            material = {.color = Color::rgb(0, 0, 255), .double_sided = true};
            material.set_emission({.6f, .2f, .05f}, 2);
            const auto record = [&] {
                scene.clear();
                assert(scene.draw(mesh, {}, material, Color::rgb(0, 0, 255)));
                assert(r3d.render(view, scene));
                renderer.present();
                return r3d.debug_stats().gi_built;
            };
            const auto emitted = [&](LinearRgb expected) {
                GiScene3D voxels;
                assert(r3d.read_gi_scene(view, voxels).value_or(false));
                std::array<f32, 3> peak{};
                for (std::size_t i = 0; i < voxels.emission.size(); ++i)
                    peak[i % 3] = std::max(peak[i % 3], voxels.emission[i]);
                assert(std::abs(peak[0] - expected.r) < .01f && std::abs(peak[1] - expected.g) < .01f &&
                       std::abs(peak[2] - expected.b) < .01f);
            };
            assert(record());
            emitted({1.2f, .4f, .1f});
            material.emissive = 4;
            assert(!record()); // intensity-only changes retain voxel attributes
            emitted({2.4f, .8f, .2f});
            const u8 green_pixel[]{0, 255, 0, 255}, red_pixel[]{255, 0, 0, 255};
            material.emissive_texture = renderer.create_texture_from_rgba(green_pixel, {1, 1});
            assert(material.emissive_texture);
            const auto settle = [&] {
                bool changed = false;
                for (int i = 0; i < 8; ++i)
                    changed = record() || changed;
                return changed;
            };
            assert(settle());
            emitted({0, .8f, 0});
            assert(renderer.update_texture(material.emissive_texture, {}, {1, 1}, red_pixel));
            assert(settle()); // same image handle, new pixels: voxel attributes must be refreshed
            emitted({2.4f, 0, 0});
            material.emission_color = {0, 0, 0};
            bool rebuilt = false;
            for (int i = 0; i < 8; ++i)
                rebuilt = record() || rebuilt;
            assert(rebuilt);
            emitted({0, 0, 0});
        }
        std::cout << "composition: cross-batch meshes, particles and additive behind alpha; complete cluster budget "
                     "fallback\n";
    }
#if KIN_ENABLE_IMAGE_IO
    Scratch scratch;
    std::filesystem::copy_file(std::filesystem::path{KIN_TEST_MODELS} / "shared pixel.png",
                               scratch.path / "shared pixel.png");
    std::ifstream input(std::filesystem::path{KIN_TEST_MODELS} / "shared-a.gltf");
    const std::string text{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    const auto parsed = parse_json(text);
    assert(parsed.ok());
    auto root = parsed.value->members();
    root["materials"] = *parse_json(R"([{"pbrMetallicRoughness":{"baseColorFactor":[0,0,1,1],
        "baseColorTexture":{"index":0},"metallicRoughnessTexture":{"index":0}},
        "emissiveFactor":[0.6,0.2,0.05],"emissiveTexture":{"index":1},"occlusionTexture":{"index":1},
        "extensions":{"KHR_materials_emissive_strength":{"emissiveStrength":3}}}])")
                             .value;
    root["extensionsUsed"] = *parse_json(R"(["KHR_materials_emissive_strength"])").value;
    root["textures"] = *parse_json(R"([{"source":0,"sampler":0},{"source":0,"sampler":1}])").value;
    u32 cases = 0;
    for (const int min : {9728, 9729, 9984, 9985, 9986, 9987})
        for (const int mag : {9728, 9729})
            for (const int u : {33071, 10497, 33648})
                for (const int v : {33071, 10497, 33648}) {
                    root["samplers"] =
                        *parse_json("[{\"minFilter\":" + std::to_string(min) + ",\"magFilter\":" + std::to_string(mag) +
                                    ",\"wrapS\":" + std::to_string(u) + ",\"wrapT\":" + std::to_string(v) +
                                    "},{\"wrapS\":33071,\"wrapT\":33071}]")
                             .value;
                    const auto path = scratch.path / "sample.gltf";
                    {
                        std::ofstream stream(path);
                        JsonWriter json{stream};
                        write_json(json, JsonValue{root});
                    }
                    auto model = r3d.load_model(path);
                    if (!model)
                        throw std::runtime_error(diagnostics3d_text(r3d.diagnostics()));
                    const auto& m = model->parts[0].material;
                    assert(m.emission_mode == EmissionMode3D::Independent && m.emissive == 3 &&
                           m.emission_color == (LinearRgb{.6f, .2f, .05f}));
                    assert(m.texture == m.emissive_texture && m.metallic_roughness == m.occlusion &&
                           m.texture != m.occlusion);
                    const auto s = *m.texture_samplers[0];
                    assert(s.mag == (mag == 9728 ? TextureFilter3D::Nearest : TextureFilter3D::Linear));
                    assert(s.min == (min == 9728 || min == 9984 || min == 9986 ? TextureFilter3D::Nearest
                                                                               : TextureFilter3D::Linear));
                    assert(s.mip == (min == 9728 || min == 9729   ? MipFilter3D::None
                                     : min == 9984 || min == 9985 ? MipFilter3D::Nearest
                                                                  : MipFilter3D::Linear));
                    const auto wrap = [](int w) {
                        return w == 33071 ? Wrap3D::Clamp : w == 33648 ? Wrap3D::Mirror : Wrap3D::Repeat;
                    };
                    assert(s.wrap_u == wrap(u) && s.wrap_v == wrap(v));
                    assert(m.texture_samplers[4]->wrap_u == Wrap3D::Clamp &&
                           m.texture_samplers[4]->mip == MipFilter3D::Linear);
                    ++cases;
                }
    std::cout << "material fidelity: " << cases << " imported sampler combinations, RGB emission and shared images\n";
    root["materials"] = *parse_json(R"([{"pbrMetallicRoughness":{"baseColorFactor":[0.2,0.4,0.6,1]},
        "emissiveFactor":[1,0,0],"extensions":{"KHR_materials_unlit":{}}}])")
                             .value;
    root["extensionsUsed"] = *parse_json(R"(["KHR_materials_unlit"])").value;
    const auto path = scratch.path / "unlit.gltf";
    {
        std::ofstream stream(path);
        JsonWriter json{stream};
        write_json(json, JsonValue{root});
    }
    const auto unlit_model = r3d.load_model(path);
    assert(unlit_model && unlit_model->parts[0].material.unlit && unlit_model->parts[0].material.emissive == 0);
#endif
}
} // namespace
int main() {
    cpu();
    bool ready = false;
    const char* require = std::getenv("KIN_REQUIRE_GPU_TESTS");
    try {
        App app;
        auto& window = app.create_window({.title = "material fidelity", .width = 160, .height = 120, .hidden = true});
        auto backend = make_render_backend(window, false, true);
        if (!backend || backend->name() != "SDL_GPU")
            throw std::runtime_error("SDL_GPU unavailable");
        Renderer2D renderer{std::move(backend)};
        ready = true;
        check_gpu(renderer);
        return 0;
    } catch (const std::exception& e) {
        const bool fail = ready || (require && require[0] && require[0] != '0');
        std::cerr << (fail ? "[FAIL] " : "[SKIP] ") << e.what() << '\n';
        return fail ? 1 : 77;
    }
}
