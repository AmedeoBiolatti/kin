#include "skin_fixtures.hpp"

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
#include <fstream>
#include <iostream>
#include <iterator>
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
    { ModelBatch3D b{model, copies, {.cell_size = -1}}; check(!b && !b.diagnostics().empty(),"bad cell size accepted"); }
    bad = model; bad.parts[1].skin = 0;
    { ModelBatch3D b{bad, copies}; check(!b && !b.diagnostics().empty(),"skinned batch accepted"); }
    auto bad_copies = copies; bad_copies[1].transform.m[3] = std::numeric_limits<f32>::quiet_NaN();
    { ModelBatch3D b{model, bad_copies}; check(!b && !b.diagnostics().empty(),"nonfinite batch accepted"); }
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

// A .gltf whose image is a base64 data: URI (Blender's "glTF Embedded" export) loads that image. A model under the content
// root or in a mounted pack that names its image outside it (by ".." or an absolute path) does not read it although the file
// is there, and says so; "../textures/" inside it, and ".." from a loose model outside any root, still read.
void embedded_and_outside_images(Renderer3D& r3d) {
    const std::filesystem::path fixtures{KIN_TEST_MODELS};
    const auto bytes_of = [](const std::filesystem::path& path) {
        std::ifstream in{path, std::ios::binary};
        return std::string(std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{});
    };
    const std::string quad = bytes_of(fixtures / "shared-a.gltf"), png = bytes_of(fixtures / "shared pixel.png");
    const std::string named = "\"shared%20pixel.png\"";
    check(quad.find(named) != std::string::npos && !png.empty(), "shared-a.gltf fixture changed");
    Scratch scratch;
    const auto write_model = [&](const std::filesystem::path& file, const std::string& image_uri) {
        std::string json = quad;
        json.replace(json.find(named), named.size(), "\"" + image_uri + "\"");
        std::filesystem::create_directories((scratch.path / file).parent_path());
        std::ofstream{scratch.path / file, std::ios::binary} << json;
    };
    write_model("embedded.gltf", "data:image/png;base64," + skin_fixtures::base64(std::vector<std::uint8_t>(png.begin(), png.end())));
    std::string error;
    const Model embedded = r3d.load_model(scratch.path / "embedded.gltf", &error);
    check(embedded && embedded.parts[0].material.texture && embedded.parts[0].material.normal && embedded.asset_diagnostics.empty(),
          "data: URI image not decoded");
    Assets3D library{r3d, scratch.path};
    const auto shared = library.load_model("embedded.gltf", &error);
    check(shared && shared->parts[0].material.texture, "data: URI image not decoded through the library");

    // scratch/content is the content root, and is mounted again as a pack at scratch/virtual.
    std::filesystem::create_directories(scratch.path / "content" / "textures");
    for (const auto& file : {scratch.path / "shared pixel.png", scratch.path / "content" / "textures" / "shared pixel.png"}) {
        std::ofstream{file, std::ios::binary} << png;
    }
    write_model("content/models/textured.gltf", "../textures/shared%20pixel.png");
    write_model("content/models/escapes.gltf", "../../shared%20pixel.png");
    write_model("content/models/absolute.gltf", (scratch.path / "shared pixel.png").generic_string());
    write_model("loose/loose.gltf", "../shared%20pixel.png");
    const auto packed = write_content_pack(scratch.path / "content", scratch.path / "content.kinpak");
    const auto pack = ContentPack::open(scratch.path / "content.kinpak", &error);
    check(packed.ok && pack, "test models' pack failed");
    mount_content_pack(scratch.path / "virtual", pack);
    char program[] = "model-assets3d-tests";
    std::string root_arg = "--content=" + (scratch.path / "content").string(), none_arg = "--content=" + (scratch.path / "nowhere").string();
    char* root_argv[] = {program, root_arg.data()};
    char* none_argv[] = {program, none_arg.data()};
    check(!find_content_root({}, 2, root_argv).empty(), "test content root not found");
    // Whether the model's image was read; when it was not, it is not listed as a dependency and a diagnostic says so.
    const auto texture_of = [&](const std::filesystem::path& file) {
        const Model model = r3d.load_model(scratch.path / file, &error);
        if (!model) { check(!r3d.diagnostics().empty(),"missing model diagnostic"); return false; }
        const bool read = bool(model.parts[0].material.texture);
        const bool listed = std::ranges::any_of(model.source_files, [](const auto& source) { return source.filename() == "shared pixel.png"; });
        check(listed == read && model.asset_diagnostics.empty() == read, "image dependency and diagnostics disagree with the texture");
        return read;
    };
    Assets3D rooted{r3d, scratch.path / "content"};
    const auto escapes_shared = rooted.load_model("models/escapes.gltf", &error);
    const bool textured = texture_of("content/models/textured.gltf"), packed_textured = texture_of("virtual/models/textured.gltf");
    const bool loose = texture_of("loose/loose.gltf");
    const bool escapes = texture_of("content/models/escapes.gltf"), packed_escapes = texture_of("virtual/models/escapes.gltf");
    const bool absolute = texture_of("content/models/absolute.gltf");
    find_content_root({}, 2, none_argv); // no content root again
    unmount_content_pack(scratch.path / "virtual");
    check(textured && packed_textured && loose, "image up a folder inside the content (or from a loose model) not read");
    check(!escapes && !packed_escapes && !absolute, "image outside the content read");
    check(!escapes_shared && !rooted.diagnostics().empty(), "partial model returned after required texture failed");
}

// A glTF whose animation turns a plain node: a red square ("base", at x -1) carries "arm" (half up, at x 0), which carries a
// green square ("tip", 1 along it); "turn" swings the arm 90 degrees about Z in a second, so the tip goes from (1, 0.5) to (0, 1.5).
// Drawn with its node pose at rest the picture is the static draw's; turned, the tip is where the pose puts it.
void node_pose(Renderer2D& renderer, Renderer3D& r3d) {
    std::vector<std::uint8_t> buffer;
    std::vector<std::pair<std::size_t, std::size_t>> views;
    const auto add = [&](const auto& values) {
        while (buffer.size() % 4 != 0) buffer.push_back(0);
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(values.data());
        views.emplace_back(buffer.size(), values.size() * sizeof(values[0]));
        buffer.insert(buffer.end(), bytes, bytes + views.back().second);
    };
    const Quatf turn = Quatf::axis_angle({0, 0, 1}, 90);
    add(std::vector<float>{-.4f, -.4f, 0, .4f, -.4f, 0, .4f, .4f, 0, -.4f, .4f, 0});
    add(std::vector<float>{0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1});
    add(std::vector<std::uint16_t>{0, 1, 2, 0, 2, 3});
    add(std::vector<float>{0, 1});
    add(std::vector<float>{0, 0, 0, 1, turn.x, turn.y, turn.z, turn.w});
    std::string json = "{\"asset\":{\"version\":\"2.0\"},\"extensionsUsed\":[\"KHR_materials_unlit\"],\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"name\":\"base\",\"mesh\":0,\"translation\":[-1,0,0],\"children\":[1]},"
        "{\"name\":\"arm\",\"translation\":[1,0.5,0],\"children\":[2]},{\"name\":\"tip\",\"mesh\":1,\"translation\":[1,0,0]}],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorFactor\":[1,0,0,1]},\"extensions\":{\"KHR_materials_unlit\":{}}},"
        "{\"pbrMetallicRoughness\":{\"baseColorFactor\":[0,1,0,1]},\"extensions\":{\"KHR_materials_unlit\":{}}}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1},\"indices\":2,\"material\":0}]},"
        "{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1},\"indices\":2,\"material\":1}]}],"
        "\"animations\":[{\"name\":\"turn\",\"samplers\":[{\"input\":3,\"output\":4}],"
        "\"channels\":[{\"sampler\":0,\"target\":{\"node\":1,\"path\":\"rotation\"}}]}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\",\"min\":[-0.4,-0.4,0],\"max\":[0.4,0.4,0]},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\"},"
        "{\"bufferView\":2,\"componentType\":5123,\"count\":6,\"type\":\"SCALAR\"},"
        "{\"bufferView\":3,\"componentType\":5126,\"count\":2,\"type\":\"SCALAR\",\"min\":[0],\"max\":[1]},"
        "{\"bufferView\":4,\"componentType\":5126,\"count\":2,\"type\":\"VEC4\"}],"
        "\"buffers\":[{\"byteLength\":" + std::to_string(buffer.size()) + ",\"uri\":\"data:application/octet-stream;base64," +
        skin_fixtures::base64(buffer) + "\"}],\"bufferViews\":[";
    for (std::size_t i = 0; i < views.size(); ++i) {
        json += (i ? "," : "") + std::string{"{\"buffer\":0,\"byteOffset\":"} + std::to_string(views[i].first) + ",\"byteLength\":" +
                std::to_string(views[i].second) + "}";
    }
    json += "]}";
    Scratch scratch;
    std::ofstream{scratch.path / "arm.gltf", std::ios::binary} << json;
    std::string error;
    const Model model = r3d.load_model(scratch.path / "arm.gltf", &error);
    check(model && model.parts.size() == 2 && model.node_skeleton == 0 && model.skeletons.size() == 1, "node-animated model not loaded");
    check(model.parts[0].node == 0 && model.parts[1].node == 1, "parts do not hang from their animated nodes");
    const AnimClip3D* clip = model.find_clip("turn", static_cast<u32>(model.node_skeleton));
    check(clip && clip->skeleton == 0, "node animation clip missing");
    const Skeleton& skeleton = model.skeletons[0];
    std::vector<Mat4f> nodes;
    Pose pose;
    Scene3D scene;
    scene.camera = Camera3D::look_at({0, .5f, 5}, {0, .5f, 0}, 60);
    auto view = r3d.create_view({160, 120}, {.samples = 1});
    scene.draw(model, Mat4f{});
    const auto still = image(renderer, r3d, view, scene);
    sample_clip(*clip, skeleton, 0, false, pose);
    compute_world_matrices(skeleton, pose, nodes);
    scene.clear();
    scene.draw(model, Mat4f{}, nodes);
    const auto rest = image(renderer, r3d, view, scene);
    int coloured = 0, differing = 0;
    for (std::size_t i = 0; i < still.size(); ++i) {
        check(std::abs(int(still[i]) - int(rest[i])) <= 1, "a node pose at rest changed the picture");
        differing += still[i] != rest[i] ? 1 : 0;
        if (i % 4 != 3 && still[i] > 10) ++coloured;
    }
    check(coloured > 300, "blank node pose picture");
    sample_clip(*clip, skeleton, 1, false, pose);
    compute_world_matrices(skeleton, pose, nodes);
    scene.clear();
    scene.draw(model, Mat4f{}, nodes);
    const auto turned = image(renderer, r3d, view, scene);
    const auto pixel = [&](const std::vector<u8>& rgba, Vec3f world) {
        const auto at = scene.camera.project(world, {0, 0, 160, 120});
        check(at.has_value(), "test point behind the camera");
        const std::size_t x = static_cast<std::size_t>(at->pos.x), y = static_cast<std::size_t>(at->pos.y);
        return std::array<int, 3>{rgba[(y * 160 + x) * 4], rgba[(y * 160 + x) * 4 + 1], rgba[(y * 160 + x) * 4 + 2]};
    };
    const auto green = [](std::array<int, 3> c) { return c[1] > 150 && c[0] < 80 && c[2] < 80; };
    const auto red = [](std::array<int, 3> c) { return c[0] > 150 && c[1] < 80 && c[2] < 80; };
    check(green(pixel(still, {1, .5f, 0})) && !green(pixel(still, {0, 1.5f, 0})), "the tip is not where its rest puts it");
    check(green(pixel(turned, {0, 1.5f, 0})) && !green(pixel(turned, {1, .5f, 0})), "the turned tip is not where the pose puts it");
    check(red(pixel(turned, {-1, 0, 0})), "the base moved with a pose that does not move it");
    std::cout << "node pose at rest: " << differing << " of " << still.size() << " bytes differ by one\n";
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
    ModelFile packed_file{r3d, mount / "shared-a.gltf"};
    check(packed_file.model().valid() && !packed_file.poll() && packed_file.diagnostics().empty(),
          "polling immutable packed content reported a filesystem error");
    unmount_content_pack(mount);
    check(!packed_file.poll() && !packed_file.diagnostics().empty() && packed_file.model().valid(),
          "missing model poll did not retain the last good model and diagnose failure");
    check(pa && pb && pa->parts[0].material.texture == pb->parts[0].material.texture, "pack dependencies not loaded/shared");
    embedded_and_outside_images(r3d);
    node_pose(renderer, r3d);
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
