#include <kin/renderer/backend.hpp>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <filesystem>
#include <kin/assets/asset_server.hpp>
#include <kin/ecs/assets2d.hpp>
#include <kin/ecs/lighting.hpp>
#include <kin/ecs/skeleton2d.hpp>
#include <kin/platform/app.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/scene/scene_asset.hpp>
#include <limits>

using namespace kin;
namespace {
bool near(f32 a, f32 b) {
    return std::abs(a - b) < .001f;
}
struct Blob {
    u64 bytes = 10;
};
void memory() {
    AssetManager manager;
    manager.register_loader<Blob>([](const auto&) { return Blob{}; });
    manager.register_memory_sizer<Blob>([](const Blob& b) { return b.bytes; });
    {
        auto a = manager.load_handle<Blob>("a");
        auto b = manager.load_handle<Blob>("b");
    }
    { auto a = manager.load_handle<Blob>("a"); }
    manager.set_memory_budget(20);
    auto c = manager.load_handle<Blob>("c");
    assert(manager.loaded<Blob>("a") && !manager.loaded<Blob>("b") && manager.loaded<Blob>("c"));
    assert(manager.memory_report().resident_bytes == 20 && manager.memory_report().pinned_bytes == 10);
    auto a = manager.load_handle<Blob>("a");
    manager.set_memory_budget(1);
    auto report = manager.memory_report();
    assert(report.over_budget && report.pinned_bytes == 20 && report.resident_bytes == 20);
    auto rows = manager.residency();
    assert(rows.size() == 2 && !rows[0].size_estimated && rows[0].external_references == 1 &&
           !rows[0].retained_by.empty());
    a = {};
    assert(manager.trim_memory().evicted == 1 && c->bytes == 10);
    c = {};
    assert(manager.trim_memory().resident_bytes == 0);
    manager.set_memory_budget(0);
    manager.register_loader<int>([](const auto&) { return 42; });
    { auto value = manager.load_handle<int>("estimate"); }
    assert(manager.residency()[0].size_estimated);
    manager.clear();
    JobSystem jobs{{.single_threaded = true}};
    AssetServer server{manager, {.jobs = &jobs}};
    server.register_async_loader<Blob>([](const auto& path, LoadContext& context) {
        context.require<Blob>(path.filename() == "root" ? "leaf" : "root");
        return Blob{};
    });
    auto root = server.load_async<Blob>("root");
    server.drain();
    manager.set_memory_budget(1);
    report = manager.memory_report();
    assert(report.over_budget && report.pinned_bytes == 20);
    for (const auto& row : manager.residency())
        if (row.path == "leaf")
            assert(row.retained_by == std::vector<std::string>({"root", "leaf"}));
    root = {};
    server.drain();
    assert(manager.memory_report().resident_bytes == 0 && !manager.loaded<Blob>("root") &&
           !manager.loaded<Blob>("leaf"));
}
SkeletonAsset2D rig() {
    SkeletonAsset2D a;
    a.bones = {{"body", -1, {}, 0}, {"arm", 0, {.position = {1, 0}}, 0}};
    auto clip = [&](const char* name, f32 duration, f32 x, f32 angle) {
        a.clips[name] = {
            duration,
            {{0, {{0, {.position = {x, 0}, .rotation = angle}}}}, {1, {{0, {.position = {1, 0}, .rotation = angle}}}}}};
    };
    clip("idle", 1, 0, 0);
    clip("right", 2, 10, 90);
    clip("up", 1, 20, 180);
    clip("wrap", 1, 0, -170);
    a.blend_spaces["move"] = {{{{0, 0}, "idle"}, {{1, 0}, "right"}, {{0, 1}, "up"}}, {{0, 1, 2}}};
    a.blend_spaces["speed"] = {{{{0, 0}, "idle"}, {{1, 0}, "right"}}, {}};
    return a;
}
void skeletons() {
    auto a = rig();
    std::string error;
    assert(validate_skeleton2d(a, error));
    SkeletonPose2D pose;
    assert(sample_skeleton_blend_space2d(a, "move", {.25f, .25f}, .5f, true, pose));
    assert(near(pose.local[0].position.x, 7.5f) && near(pose.local[0].rotation, 67.5f));
    assert(sample_skeleton_blend_space2d(a, "move", {3, 0}, 0, false, pose) && near(pose.local[0].position.x, 10));
    assert(sample_skeleton_blend_space2d(a, "speed", {.5f, 5}, 0, false, pose) && near(pose.local[0].position.x, 5));
    // Different clip durations follow the same normalized phase.
    a.clips["right"].tracks[0].keys.push_back({2, {.position = {30, 0}, .rotation = 90}});
    assert(sample_skeleton_blend_space2d(a, "speed", {1, 0}, .5f, false, pose) && near(pose.local[0].position.x, 20));
    SkeletonPose2D from, to;
    from.reset(a);
    to.reset(a);
    from.local[0].rotation = 170;
    to.local[0].rotation = -170;
    assert(blend_skeleton_poses2d(a, from, to, .5f, pose) && near(std::abs(pose.local[0].rotation), 180));
    assert(!blend_skeleton_poses2d(a, from, to, std::numeric_limits<f32>::quiet_NaN(), pose));
    from.reset(a);
    to.reset(a);
    to.local[0].rotation = 90;
    to.local[1].rotation = 60;
    const std::array<u8, 2> mask{0, 1};
    assert(layer_skeleton2d(a, to, .5f, mask, true, from) && near(from.local[0].rotation, 0) &&
           near(from.local[1].rotation, 30));
    auto bad = a.blend_spaces["move"];
    bad.triangles = {{0, 0, 1}};
    assert(!validate_skeleton_blend_space2d(a, bad, error));
    bad = a.blend_spaces["move"];
    bad.points[0].clip = "missing";
    assert(!validate_skeleton_blend_space2d(a, bad, error));
    // A prepared rig poses as one that isn't: preparing only works out its inverse binds once.
    SkeletonAsset2D prepared = rig();
    assert(prepare_skeleton2d(prepared, error) && prepared.inverse_bind.size() == prepared.bones.size());
    SkeletonPose2D slow, fast;
    assert(sample_skeleton_blend_space2d(rig(), "move", {.25f, .25f}, .5f, true, slow));
    assert(sample_skeleton_blend_space2d(prepared, "move", {.25f, .25f}, .5f, true, fast));
    for (std::size_t i = 0; i < slow.skin.size(); ++i) {
        assert(near(slow.skin[i].tx, fast.skin[i].tx) && near(slow.skin[i].a, fast.skin[i].a));
    }
    kin::EcsWorld world;
    register_builtin_editor_components(world);
    auto actor = world.entities().create();
    auto shared = std::make_shared<SkeletonAsset2D>(rig());
    SkeletonRenderer2D renderer;
    renderer.skeleton = shared;
    renderer.pose.reset(*shared);
    actor.set<SkeletonRenderer2D>(renderer);
    actor.set(SkeletonAnimator2D{.animation = "idle", .fade_seconds = 1});
    actor.set(SkeletonLayer2D{.animation = "right", .bone = "arm", .weight = .5f, .playing = false, .additive = true});
    update_skeletons2d(world, 0);
    assert(near(actor.get<SkeletonRenderer2D>()->pose.local[1].rotation, 45));
    actor.get_mut<SkeletonAnimator2D>()->animation = "right";
    update_skeletons2d(world, .25f);
    assert(near(actor.get<SkeletonRenderer2D>()->pose.local[0].position.x, 2.5f));
    assert(near(actor.get<SkeletonRenderer2D>()->pose.local[1].rotation, 67.5f));
    actor.get_mut<SkeletonAnimator2D>()->animation = "up";
    update_skeletons2d(world, 0);
    assert(near(actor.get<SkeletonRenderer2D>()->pose.local[0].position.x, 2.5f));
    update_skeletons2d(world, .5f);
    assert(near(actor.get<SkeletonRenderer2D>()->pose.local[0].position.x, 11.25f));
    auto other = world.entities().create();
    other.set<SkeletonRenderer2D>(renderer);
    other.set(SkeletonAnimator2D{.animation = "idle"});
    update_skeletons2d(world, 1);
    assert(near(other.get<SkeletonRenderer2D>()->pose.local[0].position.x, 0));
    assert(near(actor.get<SkeletonRenderer2D>()->pose.local[0].position.x, 20));
}
void textures() {
    App app{{.mode = AppMode::Headless}};
    auto& window = app.create_window({.width = 16, .height = 16, .hidden = true});
    Renderer2D renderer{make_render_backend(window, false, false)};
    Assets2D assets{renderer, {}};
    const Image pixels{{2, 2}, std::vector<u8>(16, 255)};
    assert(assets.prime_texture("a.png", pixels) && assets.prime_texture("b.png", pixels));
    auto live = assets.texture("a.png");
    assets.set_texture_budget(16);
    auto report = assets.texture_memory_report();
    assert(report.resident_bytes == 16 && report.pinned_bytes == 16);
    assets.set_texture_budget(1);
    assert(assets.texture_memory_report().over_budget && live);
    live = {};
    assert(assets.trim_texture_memory().evicted == 1 && assets.texture_residency().empty());
    // Colour and data textures share the budget: the least recently used goes first.
    assets.set_texture_budget(0);
    assert(assets.prime_texture("c.png", pixels));
    auto data = assets.data_texture("c.png");
    auto colour = assets.texture("c.png");
    data = {};
    colour = {};
    assets.set_texture_budget(16);
    const auto rows = assets.texture_residency();
    assert(!assets.over_texture_budget() && rows.size() == 1 && rows[0].key == "color:c.png");
}
// An image or skeleton that cannot be read is a reported problem, not an
// exception out of bind(); it is not read again until its file changes.
void unreadable_assets() {
    App app{{.mode = AppMode::Headless}};
    auto& window = app.create_window({.width = 16, .height = 16, .hidden = true});
    Renderer2D renderer{make_render_backend(window, false, false)};
    const auto root = std::filesystem::temp_directory_path() / "kin-advanced-workflow2d-unreadable";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    Assets2D assets{renderer, root};
    assert(!assets.image("shape.png") && assets.problems().contains("shape.png"));
    assert(!assets.image("shape.png") && !assets.data_texture("shape.png"));
    assert(!assets.skeleton2d("rig.kinskel") && assets.problems().contains("rig.kinskel"));
    kin::EcsWorld world;
    auto light = world.entities().create();
    light.set(LightComponent{.shape_asset = {"shape.png"}});
    assets.bind(world); // must not throw
    assert(!light.get<LightComponent>()->shape && !light.get<LightComponent>()->shape_image);
#if KIN_ENABLE_IMAGE_IO // the file, once readable, is picked up
    assert(save_image({{2, 2}, std::vector<u8>(16, 255)}, root / "shape.png"));
    const auto changed = assets.reload_changed();
    assert(std::ranges::find(changed, std::string{"shape.png"}) != changed.end());
    assets.bind(world);
    assert(assets.image("shape.png") && light.get<LightComponent>()->shape);
    assert(!assets.problems().contains("shape.png"));
#endif
    std::filesystem::remove_all(root);
}
} // namespace
int main() {
    memory();
    skeletons();
    textures();
    unreadable_assets();
}
