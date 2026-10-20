#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <kin/assets/bake_cache.hpp>
#include <kin/assets/content.hpp>
#include <kin/ecs/editor.hpp>
#include <kin/ecs/physics3d.hpp>
#include <kin/physics3d/assets3d.hpp>
#include <kin/scene/scene_asset_runtime.hpp>
#include <sstream>
#include <stdexcept>

namespace {
using namespace kin;
void check(bool ok, const std::string& message) {
    if (!ok)
        throw std::runtime_error(message);
}
struct Scratch {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("kin-physics-assets-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Scratch() { std::filesystem::create_directories(path); }
    ~Scratch() {
        std::error_code e;
        std::filesystem::remove_all(path, e);
    }
    void write(std::string_view name, std::string_view text) { std::ofstream(path / name) << text; }
};
std::string recipe(std::string_view shape) {
    return "{\"schema\":\"kin.collider3d/1\",\"shape\":" + std::string(shape) + "}";
}
World3DConfig config() {
    World3DConfig c;
    c.single_threaded = true;
    c.max_bodies = 64;
    c.max_body_pairs = 256;
    c.max_contact_constraints = 256;
    c.temp_memory_bytes = 2 * 1024 * 1024;
    return c;
}
void parser_and_cache() {
    for (auto text : {R"({"kind":"oops"})", R"({"kind":"box","radius":-1})", R"({"kind":"box","scale":[0,1,1]})",
                      R"({"kind":"compound","children":[]})", R"({"kind":"sphere","unknown":1})",
                      R"({"kind":"triangle_mesh","points":[[0,0,0],[1,0,0],[0,1,0]],"indices":[0,1,9]})"})
        check(!parse_collider3d_asset(recipe(text)).ok(), "invalid recipe accepted");
    auto small = ColliderAssetLimits3D{};
    small.nodes = 1;
    check(!parse_collider3d_asset(recipe(R"({"kind":"compound","children":[{"kind":"box"}]})"), small).ok(),
          "node budget ignored");
    small = {};
    small.source_bytes = 8;
    check(!parse_collider3d_asset(recipe(R"({"kind":"box"})"), small).ok(), "source budget ignored");
    Scratch scratch;
    scratch.write("box.kincollider", recipe(R"({"kind":"box","half_extents":[1,2,3]})"));
    PhysicsAssets3D assets(scratch.path);
    std::string error;
    auto a = assets.load_collider("box.kincollider", {1, 1, 1});
    error = kin::diagnostics3d_text(assets.diagnostics());
    check(bool(a), error);
    auto b = assets.load_collider("./box.kincollider");
    check(a.shared() == b.shared() && assets.stats().cooks == 1 && assets.stats().hits == 1,
          "shape cache does not share path aliases");
    auto scaled = assets.load_collider("box.kincollider", {2, 1, 3});
    check(bool(scaled), "scaled box failed");
    check(scaled.shared() != a.shared() && length(scaled->shape.bounds_max() - Vec3f{2, 2, 9}) < .001f,
          "scale not baked once");
    scratch.write("box.kincollider", recipe(R"({"kind":"box","scale":[-1,1,1]})"));
    assets.invalidate("box.kincollider");
    check(!assets.load_collider("box.kincollider", {1, 1, 1}) && !kin::diagnostics3d_text(assets.diagnostics()).empty(),
          "bad recook accepted");
    check(a->shape.valid(), "invalidation destroyed live shape");
    std::weak_ptr<const CookedCollider3D> weak = a.shared();
    Shape3D retained = a->shape;
    a = {};
    b = {};
    scaled = {};
    assets.trim();
    check(weak.expired() && assets.stats().entries == 0 && retained.valid(),
          "cache retains collision assets or shape died");
    scratch.write("hull.kincollider", recipe(R"({"kind":"convex_hull","points":[[0,0,0],[1,0,0],[0,1,0],[0,0,1]]})"));
    auto hull = assets.load_collider("hull.kincollider", {1, 1, 1});
    error = kin::diagnostics3d_text(assets.diagnostics());
    check(bool(hull), error);
    check(hull->shape.volume() > .1f && !hull->contains_surface, "convex hull is not volumetric");
    scratch.write("plane.kincollider",
                  recipe(R"({"kind":"triangle_mesh","points":[[0,0,0],[1,0,0],[0,0,1]],"indices":[0,1,2]})"));
    auto plane = assets.load_collider("plane.kincollider", {1, 1, 1});
    error = kin::diagnostics3d_text(assets.diagnostics());
    check(bool(plane) && plane->contains_surface, error);
    scratch.write("flat.kincollider", recipe(R"({"kind":"convex_hull","points":[[0,0,0],[1,0,0],[0,0,1],[1,0,1]]})"));
    check(!assets.load_collider("flat.kincollider", {1, 1, 1}) &&
              !kin::diagnostics3d_text(assets.diagnostics()).empty(),
          "degenerate hull accepted");
    scratch.write("missing.kincollider", recipe(R"({"kind":"convex_hull","mesh":"missing.glb"})"));
    check(!assets.load_collider("missing.kincollider", {1, 1, 1}) &&
              kin::diagnostics3d_text(assets.diagnostics()).find("missing.glb") != std::string::npos,
          "geometry diagnostic lost");
    kin::EcsWorld ecs;
    register_builtin_editor_components(ecs);
    auto entity = ecs.entities()
                      .create({.name = "surface"})
                      .set(Transform3D{})
                      .set(Collider3D{.asset = {"plane.kincollider"}})
                      .set(RigidBody3D{});
    check(!resolve_physics3d_assets(ecs, assets).empty() && !entity.get<Collider3D>()->asset_binding,
          "dynamic mesh accepted");
    entity.get_mut<RigidBody3D>()->motion = Body3DMotion::Static;
    check(resolve_physics3d_assets(ecs, assets).empty(), "static surface rejected");
    const auto bound = entity.get<Collider3D>()->asset_binding;
    entity.get_mut<Transform3D>()->scale = {-1, 1, 1};
    check(!resolve_physics3d_assets(ecs, assets).empty() && entity.get<Collider3D>()->asset_binding == bound,
          "failed binding lost last good shape");
}
void geometry_and_camp() {
    const std::filesystem::path root = KIN_TEST_SUMMIT;
    ModelGeometry3D geometry;
    std::string error;
    check(read_model_geometry3d(root / "kenney-survival/barrel.glb", geometry, {}, &error), error);
    check(!geometry.vertices.empty() && !geometry.indices.empty() && geometry.source_files.size() == 1,
          "CPU geometry missing");
    const auto count = geometry.vertices.size();
    auto tiny = ModelGeometryLimits3D{};
    tiny.vertices = 1;
    check(!read_model_geometry3d(root / "kenney-survival/barrel.glb", geometry, tiny, &error) &&
              geometry.vertices.size() == count,
          "geometry budget or last good output lost");
    check(!read_model_geometry3d(root / "kenney-character/survivor.glb", geometry, {}, &error) &&
              error.find("skinned") != std::string::npos,
          "animated visual accepted as static collision");
    PhysicsAssets3D assets(root);
    kin::EcsWorld ecs;
    register_builtin_editor_components(ecs);
    SceneAssetRuntime source;
    const bool loaded = source.load(root / "scenes/camp-play.kinscene", root, ecs.components());
    check(loaded, source.diagnostics().empty() ? "camp scene parse failed" : source.diagnostics().front());
    check(source.instantiate(ecs), "camp scene instantiate failed");
    const auto errors = resolve_physics3d_assets(ecs, assets);
    check(errors.empty(), errors.empty() ? "" : errors.front());
    auto player = source.instance().by_authored_id.at("player");
    auto brass = source.instance().by_authored_id.at("brass-barrel");
    auto blue = source.instance().by_authored_id.at("blue-barrel");
    check(brass.get<Collider3D>()->asset_binding == blue.get<Collider3D>()->asset_binding,
          "prefab barrels do not share cooked shape");
    check(assets.stats().cooks == 3, "unexpected camp cooking count");
    check(blue.get<Collider3D>()->asset_binding->source_files.size() == 2, "mesh dependency not recorded");
    World3D physics(config());
    WorldPhysicsState3D bodies(ecs, physics);
    bodies.sync_bodies();
    check(bodies.body_count() == 4 && bool(bodies.character_of(ecs.raw().entity(player.id()))),
          "camp physics objects missing");
    const auto barrel_hit = physics.raycast({2.2f, .4f, 2}, {2.2f, .4f, -2});
    check(barrel_hit && barrel_hit->body == bodies.body_of(blue.raw()), "cooked barrel does not block a ray");
    for (int i = 0; i < 90; ++i)
        bodies.step(1.f / 60);
    check(player.get<CharacterController3D>()->state.ground == CharacterGround3D::OnGround,
          "player did not settle on compound floor");
    const auto start = player.get<Transform3D>()->position;
    for (int i = 0; i < 20; ++i) {
        player.get_mut<CharacterController3D>()->input.move = {-1, 0};
        bodies.step(1.f / 60);
    }
    check(player.get<Transform3D>()->position.x < start.x - .3f, "authored player cannot move");
    player.get_mut<CharacterController3D>()->input = {.jump = true};
    bodies.step(1.f / 60);
    check(player.get<CharacterController3D>()->state.jumped, "authored player cannot jump");
    player.get_mut<CharacterController3D>()->input = {};
    for (int i = 0; i < 100; ++i)
        bodies.step(1.f / 60);
    check(player.get<CharacterController3D>()->state.ground == CharacterGround3D::OnGround, "player did not land");
    auto saved = build_scene_asset(ecs);
    check(saved.ok(), "physics scene save failed");
    std::ostringstream text;
    write_scene_asset(text, *saved.scene);
    check(parse_scene_asset(text.str(), ecs.components()).ok(), "physics scene save no longer parses");
    check(text.str().find("asset_binding") == std::string::npos && text.str().find("ground_speed") == std::string::npos,
          "runtime state serialized");
    player.get_mut<CharacterController3D>()->desc.height = .1f;
    check(!resolve_physics3d_assets(ecs, assets).empty(), "invalid capsule dimensions accepted");
}
void external_geometry_and_packs() {
    Scratch scratch;
    const float vertices[] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    {
        std::ofstream file(scratch.path / "mesh.bin", std::ios::binary);
        file.write(reinterpret_cast<const char*>(vertices), sizeof(vertices));
    }
    const std::string gltf =
        R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":36,"uri":"mesh.bin"}],"bufferViews":[{"buffer":0,"byteLength":36}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]}],"images":[{"uri":"missing.png"}],"meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],"nodes":[{"mesh":0,"translation":[2,3,4]}],"scenes":[{"nodes":[0]}],"scene":0})";
    scratch.write("mesh.gltf", gltf);
    ModelGeometry3D geometry;
    std::string error;
    check(read_model_geometry3d(scratch.path / "mesh.gltf", geometry, {}, &error), error);
    check(geometry.vertices[0] == Vec3f{2, 3, 4} && geometry.vertices[1] == Vec3f{3, 3, 4} &&
              geometry.source_files.size() == 2,
          "node transform or external buffer dependency missing");
    auto limits = ModelGeometryLimits3D{};
    limits.buffer_bytes = 35;
    check(!read_model_geometry3d(scratch.path / "mesh.gltf", geometry, limits, &error) &&
              error.find("budget") != std::string::npos,
          "buffer budget ignored");
    scratch.write("mesh.kincollider", recipe(R"({"kind":"triangle_mesh","mesh":"mesh.gltf"})"));
    ColliderAssetLimits3D bounded;
    bounded.cache_entries = 1;
    PhysicsAssets3D assets(scratch.path, bounded);
    auto a = assets.load_collider("mesh.kincollider", {1, 1, 1});
    error = kin::diagnostics3d_text(assets.diagnostics());
    check(bool(a), error);
    auto b = assets.load_collider("mesh.kincollider", {2, 2, 2});
    error = kin::diagnostics3d_text(assets.diagnostics());
    check(bool(b) && assets.stats().entries == 1 && a->shape.valid(), "cache bound evicted live asset");
    assets.invalidate("mesh.bin");
    check(assets.stats().entries == 0 && b->shape.valid(), "external buffer invalidation failed");
    Scratch archive;
    check(write_content_pack(scratch.path, archive.path / "collision.kinpak").ok, "collision pack write failed");
    const auto mount = archive.path / "virtual";
    mount_content_pack(mount, ContentPack::open(archive.path / "collision.kinpak"));
    {
        PhysicsAssets3D packed(mount);
        auto shape = packed.load_collider("mesh.kincollider", {1, 1, 1});
        error = kin::diagnostics3d_text(packed.diagnostics());
        check(bool(shape) && shape->source_files.size() == 3, error);
    }
    unmount_content_pack(mount);
}
void prefab_character_isolation() {
    kin::EcsWorld ecs;
    register_builtin_editor_components(ecs);
    auto source = parse_prefab_asset(
        R"({"schema":"kin.prefab/1","id":"hero","root":"hero","entities":[{"id":"hero","components":{"Transform3D":{},"CharacterController3D":{"walk_speed":2},"Animator3D":{},"CharacterAnimationLink3D":{}}}]})",
        ecs.components());
    check(source.ok(), "controller prefab parse failed");
    auto a = instantiate_prefab(ecs, *source.prefab, {}, ecs.components());
    auto b = instantiate_prefab(ecs, *source.prefab, {}, ecs.components());
    check(a.ok() && b.ok(), "controller prefab instantiate failed");
    b.root.get_mut<Transform3D>()->position = {3, 0, 0};
    ecs.entities().create().set(Transform3D{.position = {0, -.5f, 0}}).set(Collider3D{.half_extents = {10, .5f, 10}});
    World3D physics(config());
    WorldPhysicsState3D bodies(ecs, physics);
    bodies.sync_bodies();
    a.root.get_mut<CharacterController3D>()->input.move = {1, 0};
    for (int i = 0; i < 60; ++i)
        bodies.step(1.f / 60);
    check(a.root.get<Transform3D>()->position.x > 1 && std::abs(b.root.get<Transform3D>()->position.x - 3) < .01f,
          "prefab controller simulation leaked between instances");
    check(a.root.get<CharacterController3D>()->state.ground_speed > 1 &&
              b.root.get<CharacterController3D>()->state.ground_speed < .01f,
          "controller state not per instance");
    check(a.root.get<Animator3D>() != b.root.get<Animator3D>(), "animation runtime still inherited");
}
// A bumpy grid as an inline triangle mesh recipe, and a lumpy ball of points as a hull.
std::string grid_recipe(int cells, float bump) {
    std::ostringstream points, indices;
    for (int z = 0; z <= cells; ++z)
        for (int x = 0; x <= cells; ++x)
            points << (x || z ? "," : "") << "[" << x * .25f - cells * .125f << ","
                   << bump * std::sin(x * .7f) * std::cos(z * .5f) << "," << z * .25f - cells * .125f << "]";
    for (int z = 0; z < cells; ++z)
        for (int x = 0; x < cells; ++x) {
            const int a = z * (cells + 1) + x, b = a + 1, c = a + cells + 1, d = c + 1;
            indices << (x || z ? "," : "") << a << "," << c << "," << b << "," << b << "," << c << "," << d;
        }
    return recipe("{\"kind\":\"triangle_mesh\",\"points\":[" + points.str() + "],\"indices\":[" + indices.str() + "]}");
}
std::string hull_recipe(float lump) {
    std::ostringstream points;
    for (int i = 0; i < 96; ++i) {
        const float t = std::acos(1 - 2 * (i + .5f) / 96), p = 2.39996323f * i,
                    r = .6f + lump * std::sin(3 * p) * std::sin(2 * t);
        points << (i ? "," : "") << "[" << r * std::sin(t) * std::cos(p) << "," << r * std::cos(t) << ","
               << r * std::sin(t) * std::sin(p) << "]";
    }
    return recipe("{\"kind\":\"convex_hull\",\"offset\":[0,0.2,0],\"points\":[" + points.str() + "]}");
}
bool same_bits(Vec3f a, Vec3f b) {
    return std::memcmp(&a, &b, sizeof(Vec3f)) == 0;
}
// What a body of `shape` does: a fan of rays at it (hit or not, the point, normal and fraction to the bit), and the
// world's state after a box (and, for a hull, the shape itself) has fallen on it for 60 steps.
struct Behaviour {
    std::vector<std::optional<RayHit3D>> rays;
    u64 hash = 0;
};
Behaviour behaviour(const Shape3D& shape, bool dynamic_too) {
    World3D world(config());
    Behaviour out;
    world.create_body({.shape = shape, .motion = Body3DMotion::Static, .position = {0, 0, 0}});
    world.create_body({.shape = Shape3D::box({.2f, .15f, .25f}),
                       .position = {.3f, 2, -.2f},
                       .rotation = Quatf::axis_angle({1, .4f, .2f}, 25),
                       .user_id = 1});
    if (dynamic_too) {
        world.create_body(
            {.shape = Shape3D::box({5, .5f, 5}), .motion = Body3DMotion::Static, .position = {8, -.5f, 0}});
        world.create_body({.shape = shape,
                           .position = {8, 1.5f, .1f},
                           .rotation = Quatf::axis_angle({.3f, 1, .5f}, 40),
                           .user_id = 2});
    }
    world.optimize_broad_phase();
    for (int i = 0; i < 64; ++i) {
        const float x = -1.5f + .05f * i, z = 1.2f - .037f * i;
        out.rays.push_back(world.raycast({x, 4, z}, {x * .3f, -4, z + .5f}));
    }
    for (int i = 0; i < 60; ++i)
        world.step(1.f / 60);
    out.hash = world.state_hash();
    return out;
}
void check_same(const Behaviour& a, const Behaviour& b, const std::string& what) {
    check(a.hash == b.hash && a.rays.size() == b.rays.size(), what + ": world states differ");
    size_t hits = 0;
    for (size_t i = 0; i < a.rays.size(); ++i) {
        check(a.rays[i].has_value() == b.rays[i].has_value(), what + ": a ray hits one and misses the other");
        if (!a.rays[i])
            continue;
        ++hits;
        check(same_bits(a.rays[i]->point, b.rays[i]->point) && same_bits(a.rays[i]->normal, b.rays[i]->normal) &&
                  std::memcmp(&a.rays[i]->fraction, &b.rays[i]->fraction, sizeof(f32)) == 0 &&
                  a.rays[i]->sub_shape == b.rays[i]->sub_shape,
              what + ": ray hits differ");
    }
    check(hits > 8, what + ": too few rays hit to tell");
}
void bake_cache() {
    Scratch scratch;
    scratch.write("ground.kincollider", grid_recipe(90, .3f));
    scratch.write("rock.kincollider", hull_recipe(.12f));
    const auto bake_dir = scratch.path / "bake";
    const auto ms = [](auto since) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - since).count();
    };
    std::string error;
    // The first run cooks and writes; a second (a new process: new assets, a new cache on the same folder) restores.
    Shape3D cooked_ground, cooked_rock;
    double cook_ms = 0;
    {
        BakeCache bake{bake_dir};
        PhysicsAssets3D assets(scratch.path);
        assets.set_bake_cache(&bake);
        check(assets.bake_cache() == &bake, "bake cache not kept");
        const auto started = std::chrono::steady_clock::now();
        auto ground = assets.load_collider("ground.kincollider", {1, 1, 1});
        error = kin::diagnostics3d_text(assets.diagnostics());
        check(bool(ground), error);
        cook_ms = ms(started);
        auto rock = assets.load_collider("rock.kincollider", {1, 1, 1});
        error = kin::diagnostics3d_text(assets.diagnostics());
        check(bool(rock), error);
        auto box = assets.load_collider("box.kincollider", {1, 1, 1});
        error = kin::diagnostics3d_text(assets.diagnostics());
        check(!box, "missing recipe loaded");
        scratch.write("box.kincollider", recipe(R"({"kind":"box","half_extents":[1,2,3]})"));
        box = assets.load_collider("box.kincollider", {1, 1, 1});
        error = kin::diagnostics3d_text(assets.diagnostics());
        check(bool(box), error);
        const auto s = bake.stats();
        check(s.misses == 2 && s.hits == 0 && s.writes == 2 && s.bytes_written > 0 && assets.stats().cooks == 3 &&
                  assets.stats().baked == 0,
              "cold bake: expected two misses and writes (a box is not baked)");
        cooked_ground = ground->shape;
        cooked_rock = rock->shape;
    }
    {
        BakeCache bake{bake_dir};
        PhysicsAssets3D assets(scratch.path);
        assets.set_bake_cache(&bake);
        const auto started = std::chrono::steady_clock::now();
        auto ground = assets.load_collider("ground.kincollider", {1, 1, 1});
        error = kin::diagnostics3d_text(assets.diagnostics());
        check(bool(ground), error);
        const double restore_ms = ms(started);
        auto rock = assets.load_collider("rock.kincollider", {1, 1, 1});
        error = kin::diagnostics3d_text(assets.diagnostics());
        check(bool(rock), error);
        const auto s = bake.stats();
        check(s.hits == 2 && s.misses == 0 && s.writes == 0 && assets.stats().baked == 2 && assets.stats().cooks == 0,
              "warm bake: expected two hits and no cooking, got " + std::to_string(s.hits) + " hits, " +
                  std::to_string(s.misses) + " misses (" + std::to_string(s.refused) + " refused), " +
                  std::to_string(s.writes) + " writes, " + std::to_string(assets.stats().baked) + " restored, " +
                  std::to_string(assets.stats().cooks) + " cooked");
        check(ground->contains_surface && !rock->contains_surface && ground->source_files.size() == 1,
              "a restored collider lost its description");
        std::cout << "  bake: 16,200-triangle mesh collider loaded in " << cook_ms << " ms cooking, " << restore_ms
                  << " ms restored\n";
        for (const auto& [a, b] : {std::pair{cooked_ground, ground->shape}, std::pair{cooked_rock, rock->shape}}) {
            check(a.kind() == b.kind() && a.volume() == b.volume() &&
                      same_bits(a.centre_of_mass(), b.centre_of_mass()) && same_bits(a.bounds_min(), b.bounds_min()) &&
                      same_bits(a.bounds_max(), b.bounds_max()) && !(a == b),
                  "restored shape differs in its properties");
        }
        check(cooked_ground.kind() == Shape3DKind::TriangleMesh && cooked_rock.kind() == Shape3DKind::Compound,
              "unexpected cooked kinds");
        check_same(behaviour(cooked_ground, false), behaviour(ground->shape, false), "triangle mesh");
        check_same(behaviour(cooked_rock, true), behaviour(rock->shape, true), "convex hull");
        // Another scale or an edited recipe is another key: cooked, never a stale shape.
        auto big = assets.load_collider("rock.kincollider", {2, 2, 2});
        error = kin::diagnostics3d_text(assets.diagnostics());
        check(bool(big) && assets.stats().cooks == 1 && bake.stats().misses == 1, "scale not in the key");
        scratch.write("rock.kincollider", hull_recipe(.2f));
        assets.invalidate("rock.kincollider");
        auto edited = assets.load_collider("rock.kincollider", {1, 1, 1});
        error = kin::diagnostics3d_text(assets.diagnostics());
        check(bool(edited) && assets.stats().cooks == 2 && bake.stats().misses == 2 &&
                  edited->shape.volume() != cooked_rock.volume(),
              "edited recipe served from the bake");
    }
    {
        // A corrupt bake is refused (and replaced): the collider is cooked as if there were none.
        BakeCache bake{bake_dir};
        u64 corrupted = 0;
        for (const auto& entry : std::filesystem::directory_iterator(bake_dir)) {
            std::fstream file(entry.path(), std::ios::in | std::ios::out | std::ios::binary);
            file.seekp(40);
            file.put('\x5a');
            file.seekp(41);
            file.put('\xa5');
            ++corrupted;
        }
        check(corrupted == 4, "expected four bakes on disk");
        PhysicsAssets3D assets(scratch.path);
        assets.set_bake_cache(&bake);
        auto ground = assets.load_collider("ground.kincollider", {1, 1, 1});
        error = kin::diagnostics3d_text(assets.diagnostics());
        check(bool(ground), error);
        check(bake.stats().refused == 1 && bake.stats().writes == 1 && assets.stats().cooks == 1 &&
                  assets.stats().baked == 0,
              "corrupt bake not refused");
        check_same(behaviour(cooked_ground, false), behaviour(ground->shape, false), "recooked triangle mesh");
    }
    // Shape3D's bytes: whole or nothing, and of the kind asked for.
    std::vector<u8> bytes;
    check(!Shape3D{}.save_binary(bytes) && bytes.empty(), "an invalid shape saved");
    check(cooked_rock.save_binary(bytes) && Shape3D::restore_binary(bytes, Shape3DKind::Compound).valid(),
          "hull round trip failed");
    check(!Shape3D::restore_binary(bytes, Shape3DKind::ConvexHull).valid(), "wrong kind accepted");
    for (size_t size : {size_t{0}, size_t{10}, size_t{33}, bytes.size() / 2, bytes.size() - 1})
        check(!Shape3D::restore_binary(std::span<const u8>(bytes.data(), size), Shape3DKind::Compound).valid(),
              "truncated shape accepted");
    auto longer = bytes;
    longer.push_back(0);
    check(!Shape3D::restore_binary(longer, Shape3DKind::Compound).valid(), "padded shape accepted");
    auto flipped = bytes;
    flipped.back() ^= 1;
    check(!Shape3D::restore_binary(flipped, Shape3DKind::Compound).valid(), "corrupt shape accepted");
    const std::vector<float> heights(16 * 16, .5f);
    const auto field = Shape3D::height_field(heights, 16);
    bytes.clear();
    check(field.save_binary(bytes) &&
              Shape3D::restore_binary(bytes, Shape3DKind::HeightField).volume() == field.volume(),
          "height field round trip failed");
}
} // namespace
int main() {
    try {
        parser_and_cache();
        geometry_and_camp();
        external_geometry_and_packs();
        prefab_character_isolation();
        bake_cache();
        std::cout << "physics3d assets: passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
