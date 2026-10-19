#include <kin/render3d/assets3d.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/platform/app.hpp>
#include <kin/scene/scene_session3d.hpp>
#include <kin/scene/scene_loader3d.hpp>

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>

using namespace kin;

namespace {
Mesh mesh(u32 joints = 0) {
    struct Backend final : IMeshBackend {};
    return {std::make_shared<Backend>(), 3, 3, {{0, 0, 0}, {1, 1, 1}}, {}, {}, joints};
}
void submissions() {
    Model model; model.parts.push_back({"part", mesh()});
    Scene3D scene;
    assert(scene.draw(model, {}));
    const auto revision = scene.draw_revision();
    Material3D wrong[2];
    for (int i = 0; i < 1000; ++i) assert(!scene.draw(model, {}, colors::white, 0, wrong));
    assert(scene.diagnostics().size() == 1 && scene.draws().size() == 1 && scene.draw_revision() == revision);
    assert(!scene.draw_skinned(model, {}, {}, colors::white, 0, wrong));
    assert(scene.joint_matrices().empty());
    scene.clear();
    const Mat4f pose[2]{};
    assert(!scene.draw_skinned(mesh(3), pose, {}));
    assert(scene.joint_matrices().empty() && scene.draws().empty());
    scene.clear();
    assert(scene.draw_skinned(mesh(2), pose, {}));
    const auto count = scene.draws().size();
    const auto before = scene.joint_matrices()[0];
    assert(!scene.update_joint_matrices(2, pose));
    assert(scene.draws().size() == count && scene.joint_matrices()[0].m[0] == before.m[0]);
    scene.clear_diagnostics();
    assert(scene.update_joint_matrices(0, pose) && scene.diagnostics().empty());
    scene.clear();
    const auto empty_revision = scene.draw_revision();
    assert(scene.add_instances(mesh(), std::numeric_limits<std::size_t>::max()).empty());
    assert(scene.draw_revision() == empty_revision && scene.instances().empty() && !scene.diagnostics().empty());
    scene.clear();
    InstanceBatch3D invalid{mesh(), std::vector<MeshInstance>(1), {{1, 1}}};
    assert(!invalid && !invalid.diagnostics().empty());
    assert(!scene.draw_instanced(invalid) && scene.draws().empty());
    InstanceBatch3D batch{mesh(), std::vector<MeshInstance>(1)};
    const auto batch_revision = batch.revision();
    MeshInstance record;
    assert(!batch.update(1, {&record, 1}));
    assert(batch.revision() == batch_revision && !batch.diagnostics().empty());
    assert(batch.update(0, {&record, 1}) && batch.diagnostics().empty());
    InstanceBatch3D empty;
    assert(!empty.update(0, {}) && !empty && !empty.diagnostics().empty());
    const MeshInstance copies[1]{};
    ModelBatch3D bad{model, copies, {.cell_size = -1}};
    assert(!bad && bad.parts().empty() && !bad.diagnostics().empty());
}

void resources() {
    App app{{.mode = AppMode::Headless}};
    auto& window = app.create_window({.width = 64, .height = 64, .hidden = true});
    Renderer2D r2d{window}; Renderer3D renderer{r2d};
    const auto root = std::filesystem::temp_directory_path() /
        ("kin-errors3d-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code e; std::filesystem::remove_all(path, e); } } cleanup{root};
    std::filesystem::copy_file(std::filesystem::path(KIN_TEST_MODELS) / "parts.glb", root / "parts.glb");
    Assets3D assets{renderer, root};
    assert(!assets.load_model("missing.glb") && diagnostics3d_text(assets.diagnostics()).find("missing.glb") != std::string::npos);
    std::string legacy;
    assert(!assets.load_model("missing.glb", &legacy) && legacy == diagnostics3d_text(assets.diagnostics()));
    auto valid = assets.load_model("parts.glb");
    assert(valid && assets.diagnostics().empty());
    assert(!assets.load_texture("missing.png") && !assets.diagnostics().empty());
    std::ofstream(root / "corrupt.png") << "not an image";
    assert(!assets.load_texture("corrupt.png", false, true, &legacy) &&
           legacy == diagnostics3d_text(assets.diagnostics()));
    assert(assets.load_model("parts.glb") && assets.diagnostics().empty()); // cache hit resets errors too
    assert(!assets.load_material_shader("missing.spv") && !assets.diagnostics().empty());
    assert(!assets.load_materials("missing.kinmaterials", valid.shared()) && !assets.diagnostics().empty());
    assert(!renderer.create_mesh(MeshData{}) && !renderer.diagnostics().empty());
    const auto error = renderer.diagnostics();
    renderer.capabilities(); renderer.stats();
    assert(renderer.diagnostics() == error); // observation does not clear failures
    View3D absent;
    assert(!renderer.render(absent, {}) && !renderer.diagnostics().empty());
    auto view = renderer.create_view({32, 32});
    assert(view && renderer.diagnostics().empty());
    Scene3D invalid;
    Material3D wrong[1];
    Model model;
    assert(!invalid.draw(model, {}, colors::white, 0, wrong));
    assert(!renderer.render(view, invalid) && renderer.diagnostics() == invalid.diagnostics());
    invalid.clear(); invalid.overlay().draw(model, {}, colors::white, 0, wrong);
    assert(!renderer.render(view, invalid) && renderer.diagnostics() == invalid.overlay().diagnostics());
    assert(!renderer.load_environment(root / "missing.hdr") && !renderer.diagnostics().empty());
    assert(!renderer.compile_material_shader(root / "missing.frag") && !renderer.diagnostics().empty());
    assert(!renderer.load_model(root / "missing.glb", &legacy) && legacy == diagnostics3d_text(renderer.diagnostics()));
    std::ofstream(root / "bad.kinscene") << R"({"entities":[{"id":"bad","components":{"Transform3D":{},"ModelRenderer3D":{"asset":"missing.glb"}}}]})";
    kin::EcsWorld world; register_builtin_editor_components(world);
    SceneAssetRuntime source;
    assert(source.load(root / "bad.kinscene", root, world.components()));
    assert(!instantiate_scene3d(source, world, assets) && !source.diagnostics().empty());
    assert(source.instance().by_authored_id.empty());
    std::ofstream(root / "empty.kinscene") << R"({"entities":[]})";
    bool reject = false;
    SceneSession3DOptions options;
    options.compose = [&](auto&, Scene3D& destination) { if (reject) destination.draw(model, {}, colors::white, 0, wrong); };
    SceneSession3D session{renderer, assets, options};
    assert(session.load("empty.kinscene"));
    reject = true;
    assert(!session.collect() && !session.diagnostics().empty());
    reject = false;
    assert(session.collect() && session.diagnostics().empty());
}
} // namespace

int main() { submissions(); resources(); }
