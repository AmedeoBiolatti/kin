#include <kin/assets/content.hpp>
#include <kin/platform/app.hpp>
#include <kin/render3d/assets3d.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/renderer/renderer2d.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>

namespace {
using namespace kin;
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void invalid(F f) {
    bool caught = false;
    try { f(); } catch (const std::invalid_argument&) { caught = true; }
    check(caught, "invalid model placement/batch accepted");
}
bool near(Vec3f a, Vec3f b) { return length(a - b) < .0001f; }

void placement_and_batches() {
    struct MeshBackend final : IMeshBackend {};
    Mesh mesh{std::make_shared<MeshBackend>(), 3, 3, {{-1, -2, -1}, {1, 2, 1}}};
    Model model;
    model.bounds = {{1, 2, 3}, {3, 6, 5}};
    model.parts.push_back({"first", mesh, {.color = Color::rgb(255, 0, 0)}, Mat4f::translation({1, 0, 0})});
    model.parts.push_back({"second", mesh, {.color = Color::rgb(0, 255, 0)}, Mat4f::translation({0, 3, 0})});
    const auto matrix = model.placement({.position = {7, 2, -4}, .rotation = Quatf::axis_angle({0, 1, 0}, 90),
                                         .pivot = ModelPivot3D::BottomCenter, .size = 8});
    check(near(matrix.apply({2, 2, 4}), {7, 2, -4}), "bottom pivot does not land at position");
    check(std::abs(length(matrix.apply({2, 6, 4}) - matrix.apply({2, 2, 4})) - 8) < .0001f, "fit uses wrong extent");
    check(near(model.placement({.pivot = ModelPivot3D::Center}).apply(model.bounds.center()), {}), "center pivot");
    invalid([&] { model.placement({.size = -1}); });
    invalid([&] { model.placement({.size = std::numeric_limits<f32>::infinity()}); });
    invalid([&] { model.placement({.position = {std::numeric_limits<f32>::quiet_NaN(), 0, 0}}); });
    invalid([&] { Model{}.placement({.size = 1}); });
    auto bad = model; bad.bounds.min.y = std::numeric_limits<f32>::quiet_NaN();
    invalid([&] { bad.placement({.size = 1}); });

    std::array<MeshInstance, 3> copies{{{Mat4f::translation({16, 0, 0}), colors::white, 101},
                                      {Mat4f::translation({-.1f, 0, 0}), Color::rgb(0, 0, 255), 202},
                                      {Mat4f::translation({.1f, 0, 0}), Color::rgb(255, 0, 0), 303}}};
    ModelBatch3D batch{model, copies};
    check(batch.instance_count() == 3 && batch.parts().size() == 2 && batch.triangle_count() == 6, "whole model batch accounting");
    check(batch.record_bytes() == 6 * sizeof(MeshInstance), "batch CPU storage accounting");
    for (std::size_t p = 0; p < 2; ++p) {
        const auto& part = batch.parts()[p];
        check(part.instances.groups().size() == 3, "negative/cell-edge positions grouped incorrectly");
        check(part.material.color == model.parts[p].material.color, "part material lost");
        for (const auto& record : part.instances.instances()) {
            const auto original = std::find_if(copies.begin(), copies.end(), [&](auto& copy) { return copy.id == record.id; });
            check(original != copies.end() && record.tint == original->tint, "copy tint or stable ID lost");
            check(near(record.transform.apply({.5f, 1, 2}), original->transform.apply(model.parts[p].transform.apply({.5f, 1, 2}))),
                  "hierarchical part transform lost");
        }
    }
    ModelBatch3D replacement{model, copies, {.cell_size = 0, .material = Material3D{.color = Color::rgb(0, 0, 255)}}};
    check(replacement.parts()[0].material.color == Color::rgb(0, 0, 255) && replacement.parts()[1].material.color == Color::rgb(0, 0, 255),
          "material replacement not applied to all parts");
    Scene3D scene;
    scene.draw_instanced(batch);
    batch = {}; // Scene must retain the data, not borrow the source object's storage.
    check(scene.draws().size() == 2, "scene did not retain full model");
    invalid([&] { ModelBatch3D b{model, copies, {.cell_size = -1}}; });
    bad = model; bad.parts[1].skin = 0;
    invalid([&] { ModelBatch3D b{bad, copies}; });
    auto bad_copies = copies; bad_copies[1].transform.m[3] = std::numeric_limits<f32>::quiet_NaN();
    invalid([&] { ModelBatch3D b{model, bad_copies}; });
}

struct Scratch {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("kin-model-assets-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Scratch() { std::filesystem::create_directories(path); }
    ~Scratch() { std::error_code error; std::filesystem::remove_all(path, error); }
};

std::vector<u8> image(Renderer2D& renderer, Renderer3D& r3d, View3D& view, Scene3D& scene) {
    renderer.clear();
    check(r3d.render(view, scene), "model render failed");
    renderer.draw_texture(view.texture(), {0, 0, 160, 120});
    std::vector<u8> rgba; Vec2i size;
    check(renderer.read_rgba({0, 0, 160, 120}, rgba, size) && size == Vec2i{160, 120}, "model readback failed");
    renderer.present();
    return rgba;
}

void gpu_assets(Renderer2D& renderer) {
    Renderer3D r3d{renderer};
    const std::filesystem::path fixtures{KIN_TEST_MODELS};
    Assets3D assets{r3d, fixtures};
    std::string error = "stale";
    auto a = assets.load_model("shared-a.gltf", &error);
    check(a && error.empty(), "shared model A failed");
    auto alias = assets.load_model("./shared-a.gltf", &error);
    check(a.shared() == alias.shared() && assets.stats().model_hits == 1, "path alias not reused");
    auto b = assets.load_model("shared-b.gltf");
    auto filtered = assets.load_model("shared-filtered.gltf");
    check(b && filtered, "shared model B/filter failed");
    const auto& mat = a->parts[0].material;
    check(mat.texture && mat.normal && mat.texture == b->parts[0].material.texture, "cross-model external texture sharing failed");
    check(mat.texture != mat.normal, "sRGB and linear data textures aliased");
    check(mat.normal == mat.metallic_roughness && mat.normal == mat.occlusion, "same data texture not shared");
    check(mat.texture != filtered->parts[0].material.texture, "different filtering modes aliased");
    check(assets.stats().texture_loads == 3, "unnecessary dependent texture uploads");

    auto parts = assets.load_model("parts.glb");
    auto changed_options = assets.load_model("parts.glb", {.levels = 2});
    auto other_embedded = assets.load_model("pbr.glb");
    check(parts && changed_options && other_embedded && parts.shared() != changed_options.shared(), "mesh options missing from model key");
    const auto texture = parts->parts[1].material.texture;
    check(texture == changed_options->parts[1].material.texture, "same embedded image duplicated across mesh options");
    check(texture != other_embedded->parts[0].material.texture, "embedded image identities collided across files");
    Assets3D separate{r3d, fixtures};
    auto independent = separate.load_model("parts.glb");
    check(independent && independent->parts[1].material.texture != texture, "separate library unexpectedly shared resources");

    const std::array<MeshInstance, 2> copies{{{Mat4f::translation({-4, 0, 0}), colors::white, 71},
                                             {Mat4f::translation({3, 0, 0}), Color::rgb(170, 220, 240), 82}}};
    Scene3D scene;
    scene.camera = Camera3D::look_at({0, 2, 12}, {0, 1, 0}, 60);
    for (const auto& copy : copies) scene.draw(*parts, copy.transform, copy.tint, copy.id);
    auto view = r3d.create_view({160, 120}, {.samples = 1});
    const auto reference = image(renderer, r3d, view, scene);
    scene.clear();
    scene.draw_instanced(ModelBatch3D{*parts, copies});
    const auto batched = image(renderer, r3d, view, scene);
    check(reference.size() == batched.size(), "image size mismatch");
    int colored = 0;
    for (std::size_t i = 0; i < reference.size(); ++i) {
        check(std::abs(int(reference[i]) - int(batched[i])) <= 1, "whole-model batching changed pixels");
        if (i % 4 != 3 && reference[i] > 10) ++colored;
    }
    check(colored > 1000 && r3d.stats().instances == 6, "blank or incomplete batch parity check");

    Assets3D limited{r3d, fixtures, {.models = 1, .textures = 1}};
    auto first = limited.load_model("parts.glb");
    auto second = limited.load_model("shared-a.gltf");
    check(first && second && limited.stats().model_entries == 1 && limited.stats().texture_entries <= 1, "cache metadata limits exceeded");
    auto again = limited.load_model("parts.glb");
    check(again && again.shared() != first.shared() && first->parts.size() == 3, "lookup eviction damaged live handles");
    limited.clear();
    check(first && second && limited.stats().model_entries == 0 && limited.stats().texture_entries == 0, "clear invalidated consumers");
    Assets3D no_cache{r3d, fixtures, {.models = 0, .textures = 0}};
    auto uncached = no_cache.load_model("parts.glb");
    check(uncached && no_cache.stats().model_entries == 0 && no_cache.stats().texture_entries == 0, "disabled lookup cache retained metadata");
    Assets3D lifetime{r3d, fixtures};
    std::weak_ptr<const Model> weak;
    { auto temporary = lifetime.load_model("parts.glb"); weak = temporary.shared(); }
    lifetime.trim();
    check(weak.expired() && lifetime.stats().model_entries == 0 && lifetime.stats().texture_entries == 0, "weak cache pinned unloaded resources");

    Scratch scratch;
    Assets3D retry{r3d, scratch.path};
    check(!retry.load_model("late.glb", &error) && !error.empty(), "missing asset not diagnosed");
    std::filesystem::copy_file(fixtures / "parts.glb", scratch.path / "late.glb");
    auto repaired = retry.load_model("late.glb", &error);
    check(repaired && error.empty(), "failed load was cached or error stayed stale");
    // Existing consumers keep their old version after explicit invalidation.
    auto before_clear = repaired.shared(); retry.clear();
    auto after_clear = retry.load_model("late.glb");
    check(after_clear && after_clear.shared() != before_clear && before_clear->valid(), "explicit invalidation broke handle lifetime");
    const auto packed = write_content_pack(fixtures, scratch.path / "models.kinpak");
    check(packed.ok, "fixture pack failed");
    auto pack = ContentPack::open(scratch.path / "models.kinpak", &error);
    check(bool(pack), "fixture pack read failed");
    const auto mount = scratch.path / "virtual";
    mount_content_pack(mount, pack);
    Assets3D packed_assets{r3d, mount};
    auto pa = packed_assets.load_model("shared-a.gltf");
    auto pb = packed_assets.load_model("shared-b.gltf");
    unmount_content_pack(mount);
    check(pa && pb && pa->parts[0].material.texture == pb->parts[0].material.texture, "pack dependencies not loaded/shared");
}
}

int main() {
    try { placement_and_batches(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    bool gpu_ready = false;
    try {
        App app{};
        auto& window = app.create_window({.title = "model-assets3d-tests", .width = 160, .height = 120, .hidden = true});
        auto backend = make_render_backend(window, false, true);
        if (!backend || backend->name() != "SDL_GPU") throw std::runtime_error("GPU backend required for 3D asset integration checks");
        gpu_ready = true;
        Renderer2D renderer{std::move(backend)};
        gpu_assets(renderer);
        std::cout << "3D assets, ownership, placement and model batching checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        const char* required = std::getenv("KIN_REQUIRE_GPU_TESTS");
        return gpu_ready || (required && std::string_view(required) != "0") ? 1 : 77;
    }
}
