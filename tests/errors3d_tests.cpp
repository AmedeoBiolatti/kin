#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <kin/platform/app.hpp>
#include <kin/render3d/assets3d.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/scene/scene_loader3d.hpp>
#include <kin/scene/scene_session3d.hpp>
#include <limits>

using namespace kin;

namespace {
template <class T>
concept ContextualBool = requires(T value) { static_cast<bool>(value); };
static_assert(!ContextualBool<Result3D<bool>>);
static_assert(ContextualBool<Status3D>);
Mesh3D mesh(u32 joints = 0) {
    struct Backend final : IMeshBackend3D {};
    return {std::make_shared<Backend>(), 3, 3, {{0, 0, 0}, {1, 1, 1}}, {}, {}, joints};
}
void submissions() {
    Model3D model;
    model.parts.push_back({"part", mesh()});
    Scene3D scene;
    assert(scene.draw(model, {}));
    const auto revision = scene.draw_revision();
    Material3D wrong[2];
    for (int i = 0; i < 1000; ++i)
        assert(!scene.draw(model, {}, colors::white, 0, wrong));
    assert(scene.diagnostics().size() == 1 && scene.draws().size() == 1 && scene.draw_revision() == revision);
    assert(!scene.draw_skinned(model, {}, {}, colors::white, 0, wrong));
    assert(scene.joint_matrices().empty());
    const auto refused = scene.draw(model, {}, colors::white, 0, wrong);
    const auto saved = refused.diagnostics;
    scene.clear();
    assert(!refused && !saved.empty() && refused.diagnostics == saved);
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
    assert(scene.add_instances(mesh(), std::numeric_limits<std::size_t>::max()).value_or({}).empty());
    assert(scene.draw_revision() == empty_revision && scene.instances().empty() && !scene.diagnostics().empty());
    scene.clear();
    auto invalid_result = InstanceBatch3D::create(mesh(), std::vector<MeshInstance3D>(1), {{1, 1}});
    assert(!invalid_result && !invalid_result.diagnostics.empty());
    InstanceBatch3D invalid;
    assert(!invalid);
    // A rejected factory produces no malformed batch; a default batch is an empty draw.
    assert(scene.draw_instanced(invalid) && scene.draws().empty());
    auto batch = InstanceBatch3D::create(mesh(), std::vector<MeshInstance3D>(1)).value_or({});
    const auto batch_revision = batch.revision();
    MeshInstance3D record;
    assert(!batch.update(1, {&record, 1}));
    assert(batch.revision() == batch_revision && !batch.diagnostics().empty());
    assert(batch.update(0, {&record, 1}) && batch.diagnostics().empty());
    InstanceBatch3D empty;
    assert(!empty.update(0, {}) && !empty && !empty.diagnostics().empty());
    const MeshInstance3D copies[1]{};
    auto bad = ModelBatch3D::create(model, copies, {.cell_size = -1});
    assert(!bad && !bad.value && !bad.diagnostics.empty());
}

void resources() {
    App app{{.mode = AppMode::Headless}};
    auto& window = app.create_window({.width = 64, .height = 64, .hidden = true});
    Renderer2D r2d{window};
    Renderer3D renderer{r2d};
    if (!renderer.capabilities().geometry_pass) {
        const auto refused = renderer.render_geometry({}, {});
        assert(!refused && diagnostics3d_text(refused.diagnostics).find("unavailable") != std::string::npos);
        const auto saved = refused.diagnostics;
        renderer.capabilities();
        assert(refused.diagnostics == saved);
    }
    const auto root = std::filesystem::temp_directory_path() /
                      ("kin-errors3d-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code e;
            std::filesystem::remove_all(path, e);
        }
    } cleanup{root};
    std::filesystem::copy_file(std::filesystem::path(KIN_TEST_MODELS) / "parts.glb", root / "parts.glb");
    Assets3D assets{renderer, root};
    ModelFile3D watcher{renderer, root / "missing.glb"};
    assert(!watcher.model() && watcher.diagnostics().empty()); // construction does no I/O
    const auto first_poll = watcher.poll();
    assert(!first_poll.ok() && !first_poll.value && !first_poll.diagnostics.empty());
    const auto missing_model = assets.load_model("missing.glb");
    assert(!missing_model && !missing_model.value &&
           diagnostics3d_text(missing_model.diagnostics).find("missing.glb") != std::string::npos);
    const auto saved_failure = missing_model.diagnostics;
    PhysicsAssets3D collisions{root};
    assert(!collisions.load_collider("missing.kincollider").value_or({}) && !collisions.diagnostics().empty());
    const auto collision_error = collisions.diagnostics();
    collisions.stats();
    collisions.trim();
    assert(collisions.diagnostics() == collision_error);
    std::ofstream(root / "box.kincollider") << R"({"schema":"kin.collider3d/1","shape":{"kind":"box"}})";
    auto collision = collisions.load_collider("box.kincollider").value_or({});
    assert(collision && collisions.diagnostics().empty());
    assert(!collisions.load_collider("missing.kincollider").value_or({}));
    assert(collisions.load_collider("box.kincollider").value_or({}) &&
           collisions.diagnostics().empty()); // cache hit resets too
    auto valid = assets.load_model("parts.glb").value_or({});
    assert(valid && assets.diagnostics().empty());
    assert(missing_model.diagnostics == saved_failure); // result owns its failure
    const auto missing_mesh = renderer.create_mesh(MeshData3D{});
    assert(!missing_mesh && !missing_mesh.value && !missing_mesh.diagnostics.empty());
    View3D invalid_view;
    const auto resized_missing_view = renderer.resize(invalid_view, {32, 32});
    assert(!resized_missing_view && !resized_missing_view.diagnostics.empty());
    assert(result3d(false).ok() && !*result3d(false)); // false payload is success
    assert(result3d(std::vector<int>{}).ok());         // empty payload is success
    assert(!assets.load_texture("missing.png").value_or({}) && !assets.diagnostics().empty());
    std::ofstream(root / "corrupt.png") << "not an image";
    assert(!assets.load_texture("corrupt.png", false, true).value_or({}) &&
           diagnostics3d_text(assets.diagnostics()).find("corrupt.png") != std::string::npos);
    assert(assets.load_model("parts.glb").value_or({}) && assets.diagnostics().empty()); // cache hit resets errors too
    assert(!assets.load_material_shader("missing.spv").value_or({}) && !assets.diagnostics().empty());
    assert(!assets.load_materials("missing.kinmaterials", valid.shared()).value_or({}) &&
           !assets.diagnostics().empty());
    assert(!renderer.create_mesh(MeshData3D{}).value_or({}) && !renderer.diagnostics().empty());
    const auto error = renderer.diagnostics();
    renderer.capabilities();
    renderer.stats();
    assert(renderer.diagnostics() == error); // observation does not clear failures
    View3D absent;
    assert(!renderer.render(absent, {}) && !renderer.diagnostics().empty());
    auto view = renderer.create_view({32, 32}).value_or({});
    assert(view && renderer.diagnostics().empty());
    assert(renderer.warm_up(view, {}) && renderer.render(view, {})); // SDL fallback succeeds too

    Scene3D invalid;
    Material3D wrong[1];
    Model3D model;
    assert(!invalid.draw(model, {}, colors::white, 0, wrong));
    assert(!renderer.render(view, invalid) && renderer.diagnostics() == invalid.diagnostics());
    invalid.clear();
    assert(!invalid.overlay().draw(model, {}, colors::white, 0, wrong));
    assert(!renderer.render(view, invalid) && renderer.diagnostics() == invalid.overlay().diagnostics());
    assert(!renderer.load_environment(root / "missing.hdr").value_or({}) && !renderer.diagnostics().empty());
    assert(!renderer.compile_material_shader(root / "missing.frag").value_or({}) && !renderer.diagnostics().empty());
    assert(!renderer.load_model(root / "missing.glb").value_or({}) &&
           diagnostics3d_text(renderer.diagnostics()).find("missing.glb") != std::string::npos);
    std::ofstream(root / "bad.kinscene")
        << R"({"entities":[{"id":"bad","components":{"Transform3D":{},"ModelRenderer3D":{"asset":"missing.glb"}}}]})";
    kin::EcsWorld world;
    register_builtin_editor_components(world);
    SceneAssetRuntime source;
    assert(source.load(root / "bad.kinscene", root, world.components()));
    assert(!instantiate_scene3d(source, world, assets) && !source.diagnostics().empty());
    assert(source.instance().by_authored_id.empty());
    std::ofstream(root / "empty.kinscene") << R"({"entities":[]})";
    bool reject = false;
    SceneSession3DOptions options;
    options.compose = [&](auto&, Scene3D& destination) {
        if (reject)
            (void)destination.draw(model, {}, colors::white, 0, wrong);
    };
    SceneSession3D session{renderer, assets, options};
    assert(session.load("empty.kinscene"));
    reject = true;
    assert(!session.collect() && !session.diagnostics().empty());
    reject = false;
    assert(session.collect() && session.diagnostics().empty());
}
} // namespace

int main() {
    submissions();
    resources();
}
