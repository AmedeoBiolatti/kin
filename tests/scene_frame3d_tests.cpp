#include <kin/scene/scene_frame3d.hpp>
#include <kin/scene/scene_loader3d.hpp>
#include <kin/runtime/scene_app.hpp>
#include <kin/runtime/scene_server.hpp>
#include <kin/platform/app.hpp>
#include <kin/ecs/physics3d.hpp>
#include <kin/render3d/renderer3d.hpp>

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

using namespace kin;

namespace {
struct Scratch {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("kin-frame3d-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Scratch() { std::filesystem::create_directories(path); }
    ~Scratch() { std::error_code error; std::filesystem::remove_all(path, error); }
    void write(std::string_view name, std::string_view data) { std::ofstream(path / name) << data; }
};

void loader() {
    App app{{.mode = AppMode::Headless}};
    auto& window = app.create_window({.width = 64, .height = 64, .hidden = true});
    Renderer2D renderer{window};
    Renderer3D r3d{renderer};
    Scratch scratch;
    std::filesystem::copy_file(std::filesystem::path(KIN_TEST_MODELS) / "parts.glb", scratch.path / "parts.glb");
    Assets3D assets{r3d, scratch.path};
    kin::EcsWorld world;
    register_builtin_editor_components(world);
    SceneAsset host; host.name = "Host";
    assert(instantiate_scene_asset(world, host, scratch.path, world.components()).ok());
    const auto existing = world.entities().create({.authored_id = "existing"})
        .set(Transform3D{.position = {4, 5, 6}})
        .set(ModelRenderer3D{.asset = {"deliberately-unbound.glb"}});
    scratch.write("level.kinscene", R"({"name":"Additive","entities":[
        {"id":"prop","components":{"Transform3D":{},"ModelRenderer3D":{"asset":"parts.glb"}}}]})");
    SceneAssetRuntime source;
    assert(source.load(scratch.path / "level.kinscene", scratch.path, world.components()));
    assert(instantiate_scene3d(source, world, assets));
    const auto prop = source.instance().by_authored_id.at("prop");
    assert(prop.get<ModelRenderer3D>()->model && !existing.get<ModelRenderer3D>()->model);
    assert(source.instance().roots.size() == 1 && source.instance().roots[0].id() == prop.id());
    assert(scene_asset_from_world(world).name == "Host");
    assert(!instantiate_scene3d(source, world, assets) && prop.alive());
    scratch.write("bad.kinscene", R"({"entities":[
        {"id":"bad","components":{"Transform3D":{},"ModelRenderer3D":{"asset":"absent.glb"}}}]})");
    SceneAssetRuntime bad;
    assert(bad.load(scratch.path / "bad.kinscene", scratch.path, world.components()));
    assert(!instantiate_scene3d(bad, world, assets, {.update_scene_header = true}));
    assert(scene_asset_from_world(world).name == "Host");
    assert(!world.entities().find_by_authored_id("bad") && prop.alive() && existing.alive());
    assert(bad.instance().by_authored_id.empty());
    scratch.write("empty.kinscene", R"({"entities":[]})");
    SceneAssetRuntime empty;
    assert(empty.load(scratch.path / "empty.kinscene", scratch.path, world.components()));
    assert(instantiate_scene3d(empty, world, assets));
    assert(!existing.get<ModelRenderer3D>()->model);
    SceneAssetRuntime duplicate;
    assert(duplicate.load(scratch.path / "level.kinscene", scratch.path, world.components()));
    assert(!instantiate_scene3d(duplicate, world, assets) && prop.alive());
    source.clear(world);
    assert(!prop.alive() && existing.alive() && existing.get<Transform3D>()->position == Vec3f(4, 5, 6));
}

World3DConfig small_physics() {
    return {.max_bodies = 16, .max_body_pairs = 64, .max_contact_constraints = 64,
        .temp_memory_bytes = 1024 * 1024, .single_threaded = true};
}

void external_frame() {
    kin::EcsWorld world;
    register_builtin_editor_components(world);
    World3D physics{small_physics()};
    const auto native = physics.create_body({.shape = Shape3D::box({0.5f, 0.5f, 0.5f}), .position = {10, 5, 0}});
    const auto crate = world.raw().entity().set(Transform3D{.position = {0, 5, 0}})
        .set(Collider3D{}).set(RigidBody3D{});
    WorldRenderState3D rendering{world};
    WorldPhysicsState3D bodies{world, physics};
    bodies.sync_bodies();
    assert(physics.body_count() == 2 && bodies.body_count() == 1);
    SceneFrame3D frame{world, rendering, {.bodies = &bodies}};
    int boundaries = 0;
    SystemPhaseHooks hooks;
    hooks.after = [&](SystemPhase phase, std::string&) {
        ++boundaries;
        if (phase == SystemPhase::PostPhysics) assert(crate.get<Transform3D>()->position.y < 5);
        return true;
    };
    assert(frame.step(1.f / 60, hooks) && boundaries == system_phase_count);
    assert(physics.body_count() == 2 && physics.contains(native));
    assert(!frame.step(std::numeric_limits<float>::quiet_NaN()));
    int native_steps = 0;
    SceneFrame3D raw{world, rendering, {.physics_step = [&](f32 dt) { ++native_steps; physics.step(dt); }}};
    assert(raw.step(1.f / 60) && native_steps == 1);
    SystemPhaseHooks stop;
    stop.after = [](SystemPhase phase, std::string& error) {
        if (phase == SystemPhase::Physics) { error = "stop before native physics"; return false; }
        return true;
    };
    assert(!raw.step(1.f / 60, stop) && native_steps == 1);
    assert(!raw.diagnostics().empty());
    bool rejected = false;
    try { SceneFrame3D invalid{world, rendering, {.bodies = &bodies, .physics_step = [](f32) {}}}; }
    catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
    world.raw().entity().set(Transform3D{.position = {0, 3, 8}}).set(SceneCamera3D{});
    world.raw().entity().set(SceneEnvironment3D{.view = {.samples = 1}});
    Scene3D scene;
    View3DOptions view;
    scene.debug = {.force_level = 2, .gpu_culling = false, .stats_readback = true, .freeze_jitter = true};
    assert(frame.collect(scene, &view) && scene.camera.position == Vec3f(0, 3, 8) && view.samples == 1);
    assert(scene.debug.force_level == -1 && scene.debug.gpu_culling && !scene.debug.stats_readback && !scene.debug.freeze_jitter);
}

struct RuntimeState { Renderer3D* renderer = nullptr; int enters = 0, updates = 0, exits = 0; bool enabled = false; };
class RuntimeScene final : public Scene {
public:
    explicit RuntimeScene(RuntimeState& state) : _state(state) {}
    void on_enter(SceneContext& context) override {
        assert(bool(context.renderer3d) == _state.enabled);
        _state.renderer = context.renderer3d; ++_state.enters;
    }
    void update(SceneContext& context) override { assert(context.renderer3d == _state.renderer); ++_state.updates; }
    void on_exit(SceneContext& context) override { assert(context.renderer3d == _state.renderer); ++_state.exits; }
private:
    RuntimeState& _state;
};

void runtime() {
    for (bool enabled : {false, true}) {
        RuntimeState state; state.enabled = enabled;
        SceneManager scenes; scenes.push(std::make_unique<RuntimeScene>(state));
        assert(run_scene_app({.window = {.title = "frame3d-runtime", .width = 64, .height = 64},
            .headless = {.enabled = true, .frames = 2}, .rendering_3d = enabled}, scenes) == 0);
        assert(state.enters == 1 && state.updates == 1 && state.exits == 1);
        RuntimeState server_state; server_state.enabled = enabled;
        SceneManager server_scenes; server_scenes.push(std::make_unique<RuntimeScene>(server_state));
        std::istringstream in{R"({"id":1,"method":"sim.tick","params":{"count":2}})" "\n"};
        std::ostringstream out;
        assert(run_scene_server({.window = {.title = "frame3d-server", .width = 64, .height = 64},
            .in = &in, .out = &out, .rendering_3d = enabled}, server_scenes) == 0);
        assert(server_state.enters == 1 && server_state.updates == 2 && server_state.exits == 1);
    }
}
} // namespace

int main() { loader(); external_frame(); runtime(); }
