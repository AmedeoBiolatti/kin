#include <cassert>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <kin/core/instrumentation.hpp>
#include <kin/platform/app.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/scene/camera_rig3d.hpp>
#include <limits>
#include <thread>

namespace {
using namespace kin;
void check(const auto& value, const char* why) {
    if (!value)
        throw std::runtime_error(why);
}
bool close(float a, float b) {
    return std::abs(a - b) < .001f;
}
Vec3f position(const Mat4f& m) {
    return {m.m[12], m.m[13], m.m[14]};
}
void spring_arm() {
    World3DConfig config;
    config.max_bodies = 8;
    config.temp_memory_bytes = 1024 * 1024;
    config.single_threaded = true;
    World3D world{config};
    SpringArm3D arm{{.distance = 4, .shoulder = 0, .probe_radius = .2f, .margin = .1f, .ease_out = 2}};
    auto free = arm.frame(world, {0, 1, 0}, {}, .1f);
    check(free && close(free->length, 4) && !free->obstructed, "clear arm failed");
    auto wall =
        world.create_body({.shape = Shape3D::box({2, 2, .1f}), .motion = Body3DMotion::Static, .position = {0, 1, 2}});
    auto short_arm = arm.frame(world, {0, 1, 0}, {}, .1f);
    check(short_arm && short_arm->obstructed && short_arm->length > 1.4f && short_arm->length < 1.7f,
          "arm did not compress immediately to swept-sphere clearance");
    world.destroy_body(wall);
    auto recovering = arm.frame(world, {0, 1, 0}, {}, .1f);
    check(recovering && close(recovering->length, short_arm->length + .2f), "arm did not recover at configured rate");
    arm.reset();
    check(close(arm.frame(world, {0, 1, 0}, {}, 0)->length, 4), "reset retained obstruction history");
    auto settings = arm.settings();
    settings.shoulder = 1;
    check(arm.configure(settings), "shoulder configure failed");
    wall = world.create_body(
        {.shape = Shape3D::box({.1f, 2, 2}), .motion = Body3DMotion::Static, .position = {.7f, 1, 0}});
    auto side = arm.frame(world, {0, 1, 0}, {}, 0);
    check(side && side->shoulder_obstructed && side->pivot.x < .45f, "shoulder sweep missed wall");
    world.destroy_body(wall);
    wall = world.create_body({.shape = Shape3D::box({1, 1, 1}), .motion = Body3DMotion::Static, .position = {0, 1, 0}});
    auto inside = arm.frame(world, {0, 1, 0}, {}, 0);
    check(inside && inside->initial_overlap && inside->length == 0, "initial overlap not reported/collapsed");
    world.destroy_body(wall);
    settings.distance = -1;
    check(!arm.configure(settings) && arm.settings().distance == 4, "invalid configuration changed arm");
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    check(!arm.frame(world, {nan, 0, 0}, {}, 0) && !arm.frame(world, {}, {}, -1) &&
              !arm.frame(world, {}, Quatf{0, 0, 0, 0}, 0),
          "invalid frame accepted");
    arm.frame(world, {0, 1, 0}, {}, .1f);
    AllocationScope allocations;
    for (int i = 0; i < 1000; ++i)
        check(bool(arm.frame(world, {0, 1, 0}, {}, .01f)), "warm arm failed");
    check(allocations.snapshot().count == 0, "warm arm allocated through operator new");
}
struct Scratch {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("kin-camera3d-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Scratch() { std::filesystem::create_directories(path); }
    ~Scratch() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};
void scene_camera(Renderer3D& renderer) {
    Scratch scratch;
    std::ofstream(scratch.path / "level.kinscene") << R"({"entities":[
      {"id":"parent","components":{"Transform3D":{"position":[10,0,0]}}},
      {"id":"target","parent":"parent","components":{"Transform3D":{"position":[2,1,0]},"Collider3D":{"half_extents":[0.2,0.2,0.2]}}}]})";
    std::ofstream(scratch.path / "box.kinprefab")
        << R"({"id":"box","root":"box","entities":[{"id":"box","components":{"Transform3D":{},"Collider3D":{"half_extents":[0.2,0.2,0.2]}}}]})";
    Assets3D assets{renderer, scratch.path};
    SceneSession3DOptions options;
    options.viewport = {160, 120};
    options.warm_up = false;
    options.physics = World3DConfig{.max_bodies = 16,
                                    .max_body_pairs = 32,
                                    .max_contact_constraints = 32,
                                    .temp_memory_bytes = 1024 * 1024,
                                    .single_threaded = true};
    SceneSession3D session{renderer, assets, options};
    check(session.load("level.kinscene"), "camera scene load failed");
    auto query = session.queries();
    auto target = session.entity("target");
    const auto handle = session.handle(target);
    CameraRig3DSettings settings;
    settings.target_offset = {};
    settings.arm.distance = 4;
    settings.arm.shoulder = 0;
    settings.initial_pitch = 0;
    CameraRig3D rig{settings};
    check(!rig.frame(query), "unobserved rig returned frame");
    rig.follow(handle);
    check(rig.observe(*session.world()), "observe failed");
    auto first = rig.frame(query, 0, 0);
    check(first && close(first->camera.position.x, 12) && close(first->camera.position.z, 4) && !first->arm.obstructed,
          "parent world pose/self ignore incorrect");
    assert(rig.set_angles(179, 0));
    rig.observe(*session.world());
    target.get_mut<Transform3D>()->position.x = 4;
    session.rendering()->propagate_transforms();
    assert(rig.set_angles(-179, 0));
    rig.observe(*session.world());
    const auto previous = rig.frame(query, 0, 0), middle = rig.frame(query, .5f, 0), latest = rig.frame(query, 1, 0);
    check(previous && middle && latest && close(position(previous->target_transform).x, 12) &&
              close(position(middle->target_transform).x, 13) && close(position(latest->target_transform).x, 14),
          "target interpolation failed");
    check(middle->camera.forward().z > .99f, "yaw interpolation took long path");
    const auto local = target.get<Transform3D>()->matrix(), world = target.get<WorldTransform3D>()->matrix;
    const auto hash = session.physics()->state_hash();
    rig.frame(query, .5f, .01f);
    {
        AllocationScope guard{true};
        for (int i = 0; i < 1000; ++i)
            check(bool(rig.frame(query, .5f, .01f)), "warm scene camera failed");
    }
    check(local == target.get<Transform3D>()->matrix() && world == target.get<WorldTransform3D>()->matrix &&
              hash == session.physics()->state_hash(),
          "presentation changed simulation");
    check(!rig.frame(query, std::numeric_limits<float>::quiet_NaN(), 0) && !rig.frame(query, 1, -1),
          "invalid presentation time accepted");
    auto invalid = settings;
    invalid.z_near = 0;
    check(!rig.configure(invalid) && rig.settings().z_near == settings.z_near, "failed settings changed rig");
    // Rotation changes must not look like scale changes due to basis-length
    // rounding, or every turn silently disables translation interpolation.
    for (int i = 0; i < 30; ++i) {
        const auto before = target.get<WorldTransform3D>()->position().x;
        auto* transform = target.get_mut<Transform3D>();
        transform->position.x += .1f;
        transform->rotation = Quatf::axis_angle({0, 1, 0}, float(i) * 7.3f);
        session.rendering()->propagate_transforms();
        check(rig.observe(*session.world()), "rotating target observation failed");
        check(close(position(rig.frame(query, 0, 0)->target_transform).x, before),
              "rotation rounding snapped interpolation");
    }
    target.get_mut<Transform3D>()->scale = {2, 2, 2};
    target.get_mut<Transform3D>()->position.x = 5;
    session.rendering()->propagate_transforms();
    rig.observe(*session.world());
    check(close(position(rig.frame(query, 0, 0)->target_transform).x, 15), "real scale change did not snap history");
    target.get_mut<Transform3D>()->position.x = 4;
    target.get_mut<Transform3D>()->scale = {1, 1, 1};
    target.get_mut<Transform3D>()->rotation = {};
    session.rendering()->propagate_transforms();
    assert(rig.set_angles(0, 0));
    rig.reset();
    rig.observe(*session.world());
    CameraRig3D sparse = rig;
    const auto wall = session.physics()->create_body(
        {.shape = Shape3D::box({2, 2, .1f}), .motion = Body3DMotion::Static, .position = {14, 1, 2}});
    check(rig.fixed_frame(query, .1f)->arm.obstructed && sparse.fixed_frame(query, .1f)->arm.obstructed,
          "fixed camera collision failed");
    for (int i = 0; i < 10; ++i)
        rig.frame(query, .5f, .01f);
    session.physics()->destroy_body(wall);
    check(close(rig.fixed_frame(query, .1f)->arm.length, sparse.fixed_frame(query, .1f)->arm.length),
          "render frequency changed gameplay arm");
    target.get_mut<Transform3D>()->position.x = 40;
    session.rendering()->propagate_transforms();
    rig.observe(*session.world());
    check(close(position(rig.frame(query, 0, 0)->target_transform).x, 50), "teleport interpolated across scene");
    auto* matrix = target.get_mut<WorldTransform3D>();
    matrix->matrix.m[0] = 0;
    matrix->matrix.m[1] = 0;
    matrix->matrix.m[2] = 0;
    check(!rig.observe(*session.world()) && !rig.frame(query), "degenerate target accepted");
    session.rendering()->propagate_transforms();
    auto prepared = session.preload("box.kinprefab").value_or({});
    auto pool = session.create_pool(prepared, {.capacity = 1}).value_or({});
    auto lease = session.spawn(pool).value_or({});
    check(bool(lease), "camera pool failed");
    rig.follow(lease.root);
    check(rig.observe(*session.world()), "pooled target observe failed");
    session.resolve(lease.root).set(RenderTransform3D{Mat4f::translation({99, 0, 0}), true});
    check(session.despawn(lease) && !rig.frame(query), "released target retained frame");
    auto renewed = session.spawn(pool).value_or({});
    check(bool(renewed) && !session.resolve(renewed.root).has<RenderTransform3D>() && !rig.observe(*session.world()),
          "new pool lease inherited camera/override");
    rig.follow(renewed.root);
    check(rig.observe(*session.world()), "new checked target rejected");
    auto foreign = handle;
    ++foreign.session;
    check(!query.resolve(foreign), "foreign checked target resolved");
    check(!rig.orbit({std::numeric_limits<float>::infinity(), 0}), "nonfinite orbit accepted");
    assert(rig.set_angles(720, 500));
    check(rig.yaw() == 0 && rig.pitch() == settings.pitch_max, "angle wrapping/pitch clamp failed");
    rig.reset();
    check(!rig.frame(query) && rig.observe(*session.world()), "explicit reset retained interpolation state");
    bool worker_rejected = false;
    std::thread worker([&] { worker_rejected = !query.resolve(handle); });
    worker.join();
    check(worker_rejected, "worker resolved scene target");
    check(session.reload() && !rig.frame(query) && !query.resolve(handle), "reload retained old camera endpoint");
}
void render_placement(Renderer3D& renderer) {
    Assets3D assets{renderer, KIN_TEST_MODELS};
    auto model = assets.load_model("parts.glb").value_or({});
    check(bool(model) && !model->parts.empty(), "placement fixture failed");
    kin::EcsWorld world;
    WorldRenderState3D rendering{world};
    auto entity = world.entity()
                      .set(Transform3D{.position = {2, 0, 0}})
                      .set(MeshRenderer3D{.mesh = model->parts[0].mesh})
                      .set(PointLight3D{})
                      .set(SpotLight3D{});
    entity.set(RenderTransform3D{Mat4f::translation({7, 0, 0}), true});
    auto child = world.entity().child_of(entity).set(Transform3D{.position = {1, 0, 0}});
    rendering.propagate_transforms();
    Scene3D scene;
    rendering.collect(scene);
    check(scene.instances().size() == 1 && close(position(scene.instances()[0].transform).x, 7) &&
              close(scene.lights.points[0].position.x, 7) && close(scene.lights.spots[0].position.x, 7),
          "mesh/light placement override ignored");
    check(close(entity.get<WorldTransform3D>()->position().x, 2) &&
              close(child.get<WorldTransform3D>()->position().x, 3),
          "render placement propagated to simulation hierarchy");
    entity.get_mut<RenderTransform3D>()->enabled = false;
    scene.clear();
    rendering.collect(scene);
    check(close(position(scene.instances()[0].transform).x, 2), "disabled placement override applied");
    entity.remove<RenderTransform3D>();
    scene.clear();
    rendering.collect(scene);
    check(scene.instances().size() == 1, "optional placement query lost normal entity");
    entity.remove<MeshRenderer3D>();
    entity.set(ModelRenderer3D{.model = model.shared()}).set(RenderTransform3D{Mat4f::translation({9, 0, 0}), true});
    scene.clear();
    rendering.collect(scene);
    check(scene.instances().size() == model->parts.size(), "model placement lost parts");
    for (size_t i = 0; i < scene.instances().size(); ++i)
        check(scene.instances()[i].transform == Mat4f::translation({9, 0, 0}) * model->parts[i].transform,
              "model placement override ignored");
    auto data = meshes::box({1, 1, 1});
    data.skin.resize(data.vertices.size());
    auto skinned = std::make_shared<Model3D>();
    skinned->parts.push_back({.mesh = renderer.create_mesh(data).value_or({}), .skin = 0});
    Skeleton3D skeleton;
    skeleton.names = {"root"};
    skeleton.parents = {-1};
    skeleton.rest.resize(1);
    skeleton.inverse_bind.resize(1);
    skinned->skeletons.push_back(std::move(skeleton));
    Animator3D animator;
    entity.remove<ModelRenderer3D>();
    entity.set(SkinnedMeshRenderer3D{.model = skinned}).set(std::move(animator));
    rendering.animate(.01f);
    rendering.propagate_transforms();
    scene.clear();
    rendering.collect(scene);
    check(scene.instances().size() == 1 && close(position(scene.instances()[0].transform).x, 9) &&
              scene.draws()[0].skinned,
          "skinned root override ignored");
    check(close(entity.get<Transform3D>()->position.x, 2) && close(entity.get<WorldTransform3D>()->position().x, 2),
          "skinned override moved simulation");
}
} // namespace
int main() {
    bool ready = false;
    try {
        spring_arm();
        App app;
        auto& window = app.create_window({.title = "camera-tests", .width = 160, .height = 120, .hidden = true});
        auto backend = make_render_backend(window, false, true);
        if (!backend || backend->name() != "SDL_GPU")
            throw std::runtime_error("GPU backend required");
        ready = true;
        Renderer2D renderer{std::move(backend)};
        Renderer3D r3d{renderer};
        scene_camera(r3d);
        render_placement(r3d);
        std::cout << "spring arm collision/recovery, checked follow camera, render independence, lifecycle and "
                     "placement passed; rig bytes="
                  << sizeof(CameraRig3D) << ", arm bytes=" << sizeof(SpringArm3D) << "\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        const auto* required = std::getenv("KIN_REQUIRE_GPU_TESTS");
        return ready || (required && std::string_view(required) != "0") ? 1 : 77;
    }
}
