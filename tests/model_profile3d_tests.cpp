#include "skin_fixtures.hpp"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <kin/platform/app.hpp>
#include <kin/render3d/assets3d.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <stdexcept>

namespace {
using namespace kin;
void check(bool value, const char* why) {
    if (!value)
        throw std::runtime_error(why);
}
void replace(std::string& text, std::string_view from, std::string_view to) {
    const auto at = text.find(from);
    check(at != std::string::npos, "profile fixture changed");
    text.replace(at, from.size(), to);
}
std::string two_skins(bool distinct) {
    auto json = skin_fixtures::make_gltf().json;
    const auto start = json.find("\"skins\":[") + 9, end = json.find("}],", start) + 1;
    auto second = json.substr(start, end - start);
    if (distinct)
        replace(second, "[1,2]", "[2,1]");
    json.insert(end, "," + second);
    replace(json, "\"nodes\":[0,3]", "\"nodes\":[0,3,4]");
    replace(json, "],\"meshes\":[", ",{\"name\":\"second_strip\",\"mesh\":0,\"skin\":1}],\"meshes\":[");
    return json;
}
struct Scratch {
    std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("kin-model-profile-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Scratch() {
        std::filesystem::create_directories(root);
        std::filesystem::copy(KIN_TEST_MODELS, root,
                              std::filesystem::copy_options::recursive |
                                  std::filesystem::copy_options::overwrite_existing);
    }
    ~Scratch() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
    void write(std::string_view name, std::string_view bytes) const {
        const auto path = root / name;
        std::ofstream(path, std::ios::binary) << bytes;
        static int revision = 0;
        std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now() +
                                                   std::chrono::seconds(++revision));
    }
};
void profile(Renderer3D& renderer, bool gpu) {
    Scratch source;
    Assets3D assets{renderer, source.root};
    std::string error;
    for (const auto* name : {"parts.glb", "pbr.glb", "shared-a.gltf"})
        check(bool(assets.load_model(name)), "static glTF/GLB profile fixture rejected");
    for (const auto joints : {skin_fixtures::GltfOptions::Joints::U8, skin_fixtures::GltfOptions::Joints::U16})
        for (const auto weights : {skin_fixtures::GltfOptions::Weights::Float, skin_fixtures::GltfOptions::Weights::U8,
                                   skin_fixtures::GltfOptions::Weights::U16}) {
            skin_fixtures::GltfOptions options;
            options.joints = joints;
            options.weights = weights;
            source.write("attributes.gltf", skin_fixtures::make_gltf(options).json);
            assets.invalidate("attributes.gltf");
            auto model = assets.load_model("attributes.gltf");
            error = kin::diagnostics3d_text(assets.diagnostics());
            check(model && model->skeletons.size() == 1 && model->find_clip("bend"), "skin attribute profile rejected");
        }
    source.write("equivalent.gltf", two_skins(false));
    auto equivalent = assets.load_model("equivalent.gltf");
    error = kin::diagnostics3d_text(assets.diagnostics());
    check(equivalent && equivalent->parts.size() == 2 && equivalent->skeletons.size() == 1,
          "identical skin palettes did not share a whole-model pose");
    if (gpu) {
        Pose3D pose;
        sample_clip(*equivalent->find_clip("bend"), equivalent->skeletons[0], .5f, false, pose);
        std::vector<Mat4f> matrices;
        compute_joint_matrices(equivalent->skeletons[0], pose, matrices);
        Scene3D scene;
        check(scene.draw_skinned(*equivalent, matrices, Mat4f{}) && scene.draws().size() == 2,
              "a supported two-part character was only drawn partially");
    }
    const auto rejected = [&](std::string_view name, const std::string& json, std::string_view diagnostic) {
        source.write(name, json);
        auto model = assets.load_model(name);
        error = kin::diagnostics3d_text(assets.diagnostics());
        check(!model && error.find(diagnostic) != std::string::npos && !assets.diagnostics().empty(),
              "unsupported asset accepted or missing actionable diagnostic");
        if (gpu) {
            const auto direct = renderer.load_model(source.root / name, {});
            check(!direct && direct.parts.empty() &&
                      diagnostics3d_text(renderer.diagnostics()).find(diagnostic) != std::string::npos,
                  "direct loader retained drawable fragments of a rejected model");
        }
    };
    rejected("multi.gltf", two_skins(true), "multiple distinct skin palettes");
    ModelData3D inspected;
    check(read_model_data(two_skins(true), source.root / "multi.gltf", inspected, &error) &&
              inspected.skeletons.size() == 2 && !inspected.asset_diagnostics.empty(),
          "tooling parse lost unsupported-content diagnostics");
    skin_fixtures::GltfOptions options;
    options.joint_count = 257;
    rejected("large.gltf", skin_fixtures::make_gltf(options).json, "257 joints");
    options = {};
    options.bad_index = true;
    rejected("index.gltf", skin_fixtures::make_gltf(options).json, "joints past the skin");
    options = {};
    options.morph_channel = true;
    rejected("morph.gltf", skin_fixtures::make_gltf(options).json, "morph target");
    options = {};
    options.node_animation = true;
    rejected("mixed.gltf", skin_fixtures::make_gltf(options).json, "mixed skin and plain-node animation");
    std::ifstream input(source.root / "shared-a.gltf", std::ios::binary);
    const std::string quad{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>{}};
    auto jpeg = quad;
    // Generated 2x2 white JPEG: embedded test data, no decoder/tool dependency in the test.
    replace(jpeg, "shared%20pixel.png",
            "data:image/jpeg;base64,/9j/4AAQSkZJRgABAQAAAQABAAD/"
            "2wBDAAgGBgcGBQgHBwcJCQgKDBQNDAsLDBkSEw8UHRofHh0aHBwgJC4nICIsIxwcKDcpLDAxNDQ0Hyc5PTgyPC4zNDL/"
            "2wBDAQkJCQwLDBgNDRgyIRwhMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjL/"
            "wAARCAACAAIDASIAAhEBAxEB/8QAFQABAQAAAAAAAAAAAAAAAAAAAAf/xAAUEAEAAAAAAAAAAAAAAAAAAAAA/"
            "8QAFAEBAAAAAAAAAAAAAAAAAAAAAP/EABQRAQAAAAAAAAAAAAAAAAAAAAD/2gAMAwEAAhEDEQA/AL+AD//Z");
    source.write("jpeg.gltf", jpeg);
    auto jpeg_model = assets.load_model("jpeg.gltf");
    error = kin::diagnostics3d_text(assets.diagnostics());
    check(jpeg_model && (!gpu || bool(jpeg_model->parts[0].material.texture)), "JPEG material profile rejected");
    auto transformed = quad;
    const std::string placement =
        R"("index":0,"extensions":{"KHR_texture_transform":{"offset":[0.25,0.5],"scale":[2,3]}})";
    for (std::size_t at = 0; (at = transformed.find("\"index\": 0", at)) != std::string::npos; at += placement.size())
        transformed.replace(at, 10, placement);
    source.write("transformed.gltf", transformed);
    check(bool(assets.load_model("transformed.gltf")), "shared texture placement rejected");
    auto different = quad;
    replace(different, "\"index\": 0", placement);
    rejected("uv-conflict.gltf", different, "textures place themselves differently");
    auto missing = quad;
    replace(missing, "\"TEXCOORD_0\": 1", "\"_UNUSED_UV\": 1");
    rejected("uv-missing.gltf", missing, "TEXCOORD");
    // A failed lookup must be repairable without clearing a cached partial model.
    source.write("multi.gltf", skin_fixtures::make_gltf().json);
    check(bool(assets.load_model("multi.gltf")), "repaired model remained rejected in cache");
    if (gpu) {
        ModelFile3D file{renderer, source.root / "multi.gltf"};
        check(file.model().valid(), "good polling model rejected");
        source.write("multi.gltf", two_skins(true));
        check(!file.poll() && file.model().valid() && !file.diagnostics().empty(),
              "unsupported model reload replaced the last good asset");
    }
}
} // namespace
int main(int argc, char** argv) {
    const bool gpu = argc > 1 && std::string_view(argv[1]) == "gpu";
    try {
        App app{{.mode = gpu ? AppMode::Windowed : AppMode::Headless}};
        auto& window = app.create_window({.title = "model-profile", .width = 160, .height = 120, .hidden = true});
        auto renderer = gpu ? std::make_unique<Renderer2D>(make_render_backend(window, false, true))
                            : std::make_unique<Renderer2D>(window);
        if (gpu && !renderer->capabilities().rendering_3d) {
            const auto* required = std::getenv("KIN_REQUIRE_GPU_TESTS");
            if (required && std::string_view(required) != "0")
                throw std::runtime_error("GPU required");
            return 77;
        }
        Renderer3D rendering{*renderer};
        profile(rendering, gpu);
        std::cout << "glTF asset profile passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
