#include <array>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <kin/core/instrumentation.hpp>
#include <kin/platform/app.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/scene/interaction3d.hpp>
#include <limits>

namespace {
using namespace kin;
void check(const auto& value, const char* why) {
    if (!value)
        throw std::runtime_error(why);
}
struct Scratch {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("kin-interaction3d-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Scratch() { std::filesystem::create_directories(path); }
    ~Scratch() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};
constexpr auto level = R"({"entities":[
 {"id":"a","components":{"Transform3D":{"position":[1,1,0]},"Collider3D":{"radius":0.15,"kind":"sphere"},"RigidBody3D":{"motion":"static","sensor":true,"layer":4}}},
 {"id":"b","components":{"Transform3D":{"position":[0,1,1]},"Collider3D":{"radius":0.15,"kind":"sphere"},"RigidBody3D":{"motion":"static","sensor":true,"layer":4}}},
 {"id":"far","components":{"Transform3D":{"position":[2,1,0]},"Collider3D":{"radius":0.15,"kind":"sphere"},"RigidBody3D":{"motion":"static","sensor":true,"layer":4}}},
 {"id":"actor","components":{"Transform3D":{"position":[0,1,0]},"Collider3D":{"radius":0.2,"kind":"sphere"}}}]})";
constexpr auto prefab =
    R"({"id":"box","root":"box","entities":[{"id":"box","components":{"Transform3D":{},"Collider3D":{"half_extents":[0.1,0.1,0.1]}}}]})";
void interactions(Renderer2D& renderer) {
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    for (const auto reach : {0.f, -1.f, nan, std::numeric_limits<float>::infinity()}) {
        bool rejected = false;
        try {
            InteractionQuery3D invalid{{.reach = reach}};
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        check(rejected, "invalid interaction radius accepted");
    }
    Scratch scratch;
    std::ofstream(scratch.path / "level.kinscene") << level;
    std::ofstream(scratch.path / "box.kinprefab") << prefab;
    Renderer3D r3d{renderer};
    Assets3D assets{r3d, scratch.path};
    SceneSession3DOptions config;
    config.viewport = {160, 120};
    config.view.samples = 1;
    config.warm_up = false;
    config.physics = World3DConfig{.max_bodies = 16,
                                   .max_body_pairs = 32,
                                   .max_contact_constraints = 32,
                                   .temp_memory_bytes = 1024 * 1024,
                                   .single_threaded = true};
    SceneSession3D session{r3d, assets, config};
    check(session.load("level.kinscene"), "interaction scene load failed");
    auto queries = session.queries();
    const auto a = session.handle(session.entity("a")), b = session.handle(session.entity("b"));
    const auto actor = session.handle(session.entity("actor"));
    InteractionObserver3D observer{actor, {0, 1, 0}};
    InteractionQuery3D interaction{
        {.reach = 2.5f, .candidates = layer_bit(layers3d::Sensor), .candidate_sensors = true, .sight_sensors = true}};
    std::array<SceneOverlapHit3D, 8> hits;
    bool only_a = false, enabled = true;
    InteractionVerb3D verb = 1;
    Vec3f offset{};
    auto describe = [&](kin::EcsEntity entity) -> std::optional<InteractionOffer3D> {
        if (!enabled || entity.id() == actor.entity.id || (only_a && entity.id() != a.entity.id))
            return {};
        return InteractionOffer3D{entity.get<WorldTransform3D>()->matrix.apply({}) + offset, verb};
    };
    auto result = interaction.select(queries, observer, hits, describe);
    check(result && result.focus.target == (a.entity.id < b.entity.id ? a : b),
          "nearest distance tie was not deterministic");
    only_a = true;
    result = interaction.select(queries, observer, hits, describe);
    check(result && result.focus.target == a && result.focus.verb == 1 && result.focus.distance_squared == 1,
          "game policy/point/verb selection failed");
    const auto locked = result.focus;
    check(bool(interaction.revalidate(queries, observer, locked, describe)), "fresh locked target rejected");
    // Revalidation is tied to both checked identity and the original verb.
    verb = 2;
    check(!interaction.revalidate(queries, observer, locked, describe), "changed verb accepted for old action");
    verb = 1;
    enabled = false;
    check(!interaction.revalidate(queries, observer, locked, describe), "disabled target remained valid");
    enabled = true;
    offset = {10, 0, 0};
    check(!interaction.revalidate(queries, observer, locked, describe), "out-of-reach contact accepted");
    offset = {nan, 0, 0};
    check(!interaction.select(queries, observer, hits, describe), "nonfinite offer accepted");
    offset = {};
    verb = 0;
    check(!interaction.select(queries, observer, hits, describe), "zero verb accepted");
    verb = 1;
    check(interaction.select(queries, observer, std::span(hits).first(1), describe).status ==
              InteractionStatus3D::Overflow,
          "truncated candidate prefix selected a target");
    check(interaction.select(queries, observer, std::span<SceneOverlapHit3D>{}, describe).status ==
              InteractionStatus3D::Overflow,
          "zero-capacity discovery did not fail closed");
    auto bad = observer;
    bad.origin.x = nan;
    check(interaction.select(queries, bad, hits, describe).status == InteractionStatus3D::InvalidObserver,
          "invalid origin accepted");
    bad = observer;
    bad.view = Vec3f{0, nan, 0};
    check(interaction.revalidate(queries, bad, locked, describe).status == InteractionStatus3D::InvalidObserver,
          "invalid camera accepted");
    bad = observer;
    ++bad.actor.session;
    check(interaction.select(queries, bad, hits, describe).status == InteractionStatus3D::InvalidObserver,
          "foreign actor accepted");
    InteractionQuery3D no_sensors{{.reach = 2.5f}};
    check(!no_sensors.select(queries, observer, hits, describe), "sensor candidates included without opt-in");
    // Native objects without a checked ECS mapping still obstruct sight, even
    // when their arbitrary user_id pretends to identify the selected entity.
    auto wall = session.physics()->create_body({.shape = Shape3D::sphere(.1f),
                                                .motion = Body3DMotion::Static,
                                                .position = {.5f, 1, 0},
                                                .user_id = a.entity.id});
    check(!interaction.select(queries, observer, hits, describe) &&
              !interaction.revalidate(queries, observer, locked, describe),
          "unmapped obstruction did not block");
    InteractionQuery3D proximity{{.reach = 2.5f, .candidate_sensors = true, .require_sight = false}};
    check(bool(proximity.revalidate(queries, observer, locked, describe)), "optional sight policy ignored");
    (void)session.physics()->destroy_body(wall);
    auto camera = observer;
    camera.view = Vec3f{0, 3, 0};
    check(bool(interaction.revalidate(queries, camera, locked, describe)), "unobstructed view rejected");
    wall = session.physics()->create_body(
        {.shape = Shape3D::sphere(.1f), .motion = Body3DMotion::Static, .position = {.5f, 2, 0}});
    check(!interaction.revalidate(queries, camera, locked, describe) &&
              bool(interaction.revalidate(queries, observer, locked, describe)),
          "second sight line did not independently block");
    (void)session.physics()->destroy_body(wall);
    // Moving the observer close to B must not silently switch an action from A.
    auto closer = observer;
    closer.origin = {0, 1, .5f};
    only_a = false;
    check(interaction.select(queries, closer, hits, describe).focus.target == b &&
              interaction.revalidate(queries, closer, locked, describe).focus.target == a,
          "locked target followed nearest selection");
    only_a = true;
    interaction.select(queries, observer, hits, describe);
    interaction.revalidate(queries, observer, locked, describe);
    {
        AllocationScope guard{true};
        for (int i = 0; i < 10000; ++i) {
            check(bool(interaction.select(queries, observer, hits, describe)), "warmed selection failed");
            check(bool(interaction.revalidate(queries, observer, locked, describe)), "warmed revalidation failed");
        }
    }
    auto prepared = session.preload("box.kinprefab").value_or({});
    auto pool = session.create_pool(prepared, {.capacity = 1}).value_or({});
    auto lease = session.spawn(pool, {.position = Vec3f{0, 2, 0}}).value_or({});
    check(bool(lease), "interaction pool failed");
    const InteractionFocus3D pooled{lease.root, 3};
    auto any = [](kin::EcsEntity e) -> std::optional<InteractionOffer3D> {
        return InteractionOffer3D{e.get<WorldTransform3D>()->matrix.apply({}), 3};
    };
    check(bool(proximity.revalidate(queries, observer, pooled, any)), "live pool target rejected");
    check(session.despawn(lease), "pool return failed");
    auto renewed = session.spawn(pool, {.position = Vec3f{0, 2, 0}}).value_or({});
    check(renewed.root.entity == lease.root.entity && renewed.root != lease.root &&
              !proximity.revalidate(queries, observer, pooled, any),
          "old interaction selected recycled lease");
    check(session.reload() &&
              interaction.revalidate(queries, observer, locked, describe).status == InteractionStatus3D::Unavailable,
          "reload retained old interaction endpoint");
    check(interaction.revalidate(session.queries(), observer, locked, describe).status ==
              InteractionStatus3D::InvalidObserver,
          "old actor survived reload");
    std::cout << "interaction selection, checked locks, policy, bounded overflow, sight and 10000 allocation-guarded "
                 "iterations passed; query_bytes="
              << sizeof(InteractionQuery3D) << " focus_bytes=" << sizeof(InteractionFocus3D) << '\n';
}
} // namespace
int main() {
    bool ready = false;
    try {
        App app;
        auto& window = app.create_window({.title = "interaction-tests", .width = 160, .height = 120, .hidden = true});
        auto backend = make_render_backend(window, false, true);
        if (!backend || backend->name() != "SDL_GPU")
            throw std::runtime_error("GPU backend required");
        ready = true;
        Renderer2D renderer{std::move(backend)};
        interactions(renderer);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        const auto* required = std::getenv("KIN_REQUIRE_GPU_TESTS");
        return ready || (required && std::string_view(required) != "0") ? 1 : 77;
    }
}
