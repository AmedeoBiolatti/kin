#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <kin/ecs/lifecycle.hpp>
#include <kin/ecs/render.hpp>
#include <kin/platform/app.hpp>
#include <kin/prefab/prefab_pool2d.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/scene/scene_preload2d.hpp>
#include <thread>
#if KIN_ENABLE_SCRIPTING
#include <kin/scripting/entity_scripts.hpp>
#endif

using namespace kin;
struct Health {
    i32 value = 100;
};
struct RuntimeOnly {
    i32 value = 9;
};
void pool_test() {
    kin::EcsWorld world;
    register_builtin_editor_components(world);
    world.components().native<Health>("Health").field("value", &Health::value);
    auto parsed = parse_prefab_asset(R"({"schema":"kin.prefab/1","id":"bullet","root":"root","entities":[
      {"id":"root","name":"Bullet","components":{"Transform2D":{},"Health":{"value":25}}},
      {"id":"tail","parent":"root","components":{"Transform2D":{"pos":{"x":-2,"y":0}}}}]})",
                                     world.components());
    assert(parsed.ok());
    int removed = 0, released = 0;
    auto observer = world.raw().observer<Health>().event(flecs::OnRemove).each([&](Health&) { ++removed; });
    {
        PrefabPool2D pool{
            world, *parsed.prefab, {.capacity = 2, .on_release = [&](auto&, const auto&) { ++released; }}};
        auto first = pool.acquire({.pos = {10, 20}});
        auto second = pool.acquire();
        assert(first && second && !pool.acquire() && pool.active() == 2);
        auto root = pool.resolve(*first)->root;
        auto child = pool.resolve(*first)->by_authored_id.at("tail");
        const auto id = root.id();
        assert(root.get<Transform2D>()->pos.x == 10 && child.parent().id() == id);
        root.get_mut<Health>()->value = 1;
        root.set(RuntimeOnly{});
        auto extra = world.entities().create({.name = "runtime child", .parent = root});
        child.child_of(extra); // runtime hierarchy edits must not delete authored nodes
        const int before = removed;
        assert(pool.release(*first) && removed == before + 1 && released == 1);
        assert(!pool.resolve(*first) && !extra && !world.raw().entity(id).enabled());
        auto next = pool.acquire({.pos = {30, 40}});
        assert(next && next->generation != first->generation && pool.resolve(*next)->root.id() == id);
        root = pool.resolve(*next)->root;
        assert(child && child.parent().id() == root.id());
        assert(root.get<Health>()->value == 25 && !root.has<RuntimeOnly>());
        assert(root.get<Transform2D>()->pos.x == 30 && !pool.release(*first));
        assert(root.get<EntityLifetime>()->generation == next->generation);
    }
    assert(world.count<Health>() == 0); // idle/live slots are destroyed; prefab templates are excluded
    observer.destruct();
}
void memory_content_test() {
    auto content = MemoryContent::create({{"folder/data.txt", "bytes"}});
    auto root = content->root();
    assert(content_file_exists(root / "folder/data.txt") && content_directory_exists(root / "folder"));
    assert(list_content_files(root, true).size() == 1 && list_content_files(root, false).empty());
    auto view = view_content_file(root / "folder/data.txt");
    content.reset();
    assert(view && view->bytes.size() == 5);
    view.reset();
    assert(!content_file_exists(root / "folder/data.txt"));
}
void preload_test() {
    App app{{.mode = AppMode::Headless}};
    auto& window = app.create_window({.title = "preload", .width = 32, .height = 32, .hidden = true});
    Renderer2D renderer{window};
    JobSystem jobs{{.single_threaded = true}};
    MemoryContent::Files files{{"scene.kinscene", R"({"schema":"kin.scene/1","name":"Loaded","entities":[
        {"id":"hero","components":{"Transform2D":{}}}]})"}};
#if KIN_ENABLE_IMAGE_IO
    const auto png = std::filesystem::temp_directory_path() / "kin-preload-pixel.png";
    assert(save_image(Image{{1, 1}, {255, 0, 0, 255}}, png));
    auto pixels = read_content_file(png);
    std::filesystem::remove(png);
    assert(pixels);
    files["a.png"] = files["b.png"] = *pixels;
    files["sprites.kinsprites"] = "texture atlas a.png\nsprite one atlas 0 0 1 1\n";
    files["scene.kinscene"] = R"({"schema":"kin.scene/1","name":"Loaded","entities":[
        {"id":"a","components":{"Transform2D":{},"TextureRenderer":{"asset":"a.png"}}},
        {"id":"b","components":{"Transform2D":{},"TextureRenderer":{"asset":"b.png"}}},
        {"id":"sprite","components":{"Transform2D":{},"SpriteRenderer":{"asset":"sprites.kinsprites","sprite":"one"}}}]})";
#endif
    auto source = MemoryContent::create(files);
    bool custom_bound = false;
    const auto main_thread = std::this_thread::get_id();
    ScenePreloadOptions2D options;
    options.asset_binders.emplace_back("game", [&](kin::EcsWorld&, Assets2D&) {
        assert(std::this_thread::get_id() == main_thread);
        custom_bound = true;
    });
    ScenePreload2D request{renderer, jobs, source->root(), "scene.kinscene", std::move(options)};
    request.pump(0);
#if KIN_ENABLE_IMAGE_IO
    assert(request.state() == SceneLoadState2D::Uploading);
    request.pump(1);
    assert(request.state() == SceneLoadState2D::Uploading);
#endif
    request.pump(1);
    assert(request.state() == SceneLoadState2D::Ready && request.progress() == 1 && custom_bound);
    auto loaded = request.take();
    assert(loaded && loaded->name() == "Loaded" && !request.take());
    assert(loaded->assets() && !loaded->instance().by_authored_id.empty());
#if KIN_ENABLE_IMAGE_IO
    const auto* sprite = loaded->instance().by_authored_id.at("sprite").get<SpriteRenderer>();
    assert(sprite && sprite->sprite.catalog);
    assert(sprite->sprite.catalog->textures().at("atlas").texture ==
           loaded->assets()->texture("a.png")); // no hidden catalog re-upload
#endif
    source.reset(); // prepared scene owns a full content snapshot
    assert(content_file_exists(loaded->content_root() / "scene.kinscene"));
    ScenePreload2D cancelled{renderer, jobs, loaded->content_root(), "scene.kinscene"};
    cancelled.cancel();
    cancelled.pump();
    assert(cancelled.state() == SceneLoadState2D::Cancelled);
    ScenePreload2D failed{renderer, jobs, loaded->content_root(), "missing.kinscene"};
    failed.pump();
    assert(failed.state() == SceneLoadState2D::Failed && !failed.error().empty());
    assert(!loaded->instance().by_authored_id.empty()); // failure never replaces the active scene
    JobSystem workers;
    ScenePreload2D threaded{renderer, workers, loaded->content_root(), "scene.kinscene"};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (threaded.state() == SceneLoadState2D::Loading && std::chrono::steady_clock::now() < deadline) {
        threaded.pump(0);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    while (threaded.state() == SceneLoadState2D::Uploading)
        threaded.pump(1);
    assert(threaded.state() == SceneLoadState2D::Ready && threaded.take());
}
#if KIN_ENABLE_SCRIPTING
void pooled_script_test() {
    kin::EcsWorld world;
    register_builtin_editor_components(world);
    world.components().native<Health>("Health").field("value", &Health::value);
    auto content = MemoryContent::create({{"pool.lua", R"(
        local first=nil
        local starts=0
        function on_start(self)
            starts=starts+1
            if first and first:alive() then error("old pool lifetime stayed alive") end
            first=self
            self:set("Health",{value=starts})
        end
    )"}});
    auto prefab = parse_prefab_asset(R"({"schema":"kin.prefab/1","id":"script","root":"root","entities":[
        {"id":"root","components":{"Health":{},"ScriptComponent":{"asset":"pool.lua"}}}]})",
                                     world.components());
    assert(prefab.ok());
    PrefabPool2D pool{world, *prefab.prefab, {.capacity = 1}};
    EntityScripts scripts{{.content = content->root(), .check_frames = 0}};
    Input input;
    auto lease = pool.acquire();
    assert(lease);
    scripts.update(world, input, .01f);
    assert(scripts.problems().empty() && pool.resolve(*lease)->root.get<Health>()->value == 1);
    assert(pool.release(*lease));
    lease = pool.acquire();
    assert(lease);
    scripts.update(world, input, .01f);
    assert(scripts.problems().empty() && pool.resolve(*lease)->root.get<Health>()->value == 2);
}
#endif
int main() {
    pool_test();
    memory_content_test();
    preload_test();
#if KIN_ENABLE_SCRIPTING
    pooled_script_test();
#endif
}
