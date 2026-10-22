#pragma once

#include <functional>
#include <kin/ecs/physics3d.hpp>
#include <kin/ecs/scene3d.hpp>
#include <kin/physics3d/assets3d.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/scene/scene_asset_runtime.hpp>
#include <kin/scene/scene_entity3d.hpp>

namespace sol {
class state;
}
namespace kin {

class AudioEngine;
class AudioCatalog;
class App;
class Input;
class JsonWriter;
class InputActionContext;
class ScriptComponentRegistry;

struct ScenePrefab3D {
    u64 session = 0, generation = 0, id = 0;
    SceneEntity3D root;
    explicit operator bool() const { return session && id; }
};
struct SceneSpawnContext3D {
    EcsWorld& world;
    EcsEntity root;
    const std::unordered_map<std::string, EcsEntity>& entities;
    EcsEntity entity(std::string_view local_id) const {
        const auto it = entities.find(std::string(local_id));
        return it == entities.end() ? EcsEntity{} : it->second;
    }
};
struct SceneSpawn3D {
    SceneEntity3D parent;
    // Optional local-space root values preserve unspecified prefab values.
    std::optional<Vec3f> position, scale;
    std::optional<Quatf> rotation;
    // Runs once before binding/physics. Configure only the supplied new
    // entities; native gameplay components need no inspector registration.
    // Do not create/delete/reparent entities or perform external side effects.
    std::function<void(SceneSpawnContext3D&)> configure;
};

struct ScenePreparedPrefab3D {
    u64 session = 0, generation = 0, id = 0;
    explicit operator bool() const { return session && id; }
};
struct ScenePrefabPool3D {
    u64 session = 0, generation = 0, id = 0;
    explicit operator bool() const { return session && id; }
};
struct ScenePrefabPoolOptions3D {
    std::size_t capacity = 16;
    // Reset arbitrary native game state on every acquisition (and warm-up).
    // Configure only game-owned components; engine settings/assets are fixed.
    std::function<void(SceneSpawnContext3D&)> reset;
    // Optional cleanup of per-lease game resources on return. No structure edits.
    std::function<void(SceneSpawnContext3D&)> release;
};
struct ScenePrefabPoolStats3D {
    std::size_t capacity = 0, active = 0, available = 0;
    u64 acquired = 0, reused = 0, exhausted = 0;
};

class EcsCommandBuffer;
namespace detail {
struct SceneRequestState3D;
struct SceneCommandQueue3D;
struct SceneOperation3D;
} // namespace detail
// A request still Pending when a replacement scene is prepared (prepare() without activate() or cancel() yet) is
// Cancelled by the next step() with an error that says so: requests are never held for a later scene, and a new one
// made after cancel() (or from the new scene's commands() after activate()) runs normally.
enum class SceneRequestStatus3D { Invalid, Pending, Succeeded, Failed, Cancelled };
// Owns a small result only, never a world, assets or constructor captures.
// Safe to retain across scene replacement; inspect the status before the result.
class SceneRequest3D {
public:
    SceneRequestStatus3D status() const;
    ScenePrefab3D prefab() const;           // populated only for a successful spawn
    ScenePreparedPrefab3D prepared() const; // successful preload
    ScenePrefabPool3D pool() const;         // successful create_pool
    std::string error() const;

private:
    std::shared_ptr<detail::SceneRequestState3D> _state;
    friend class SceneCommands3D;
    friend class SceneSession3D;
};
// Copyable, generation-scoped endpoint. Capture context.commands in C++ systems.
// Each system uses its own SystemContext::commands buffer. Submission is safe
// from parallel tasks; partitioned native workers have no command buffer.
// Constructors and all native work run on the scene thread.
class SceneCommands3D {
public:
    SceneCommands3D() = default;
    SceneRequest3D spawn(EcsCommandBuffer&, const std::filesystem::path&, SceneSpawn3D = {}) const;
    SceneRequest3D spawn(EcsCommandBuffer&, ScenePreparedPrefab3D, SceneSpawn3D = {}) const;
    SceneRequest3D spawn(EcsCommandBuffer&, ScenePrefabPool3D, SceneSpawn3D = {}) const;
    SceneRequest3D despawn(EcsCommandBuffer&, ScenePrefab3D) const;
    SceneRequest3D despawn(EcsCommandBuffer&, SceneRequest3D spawn) const;
    // Explicit preparation, executed at the same owner-thread boundaries as spawn.
    SceneRequest3D preload(EcsCommandBuffer&, const std::filesystem::path&) const;
    SceneRequest3D create_pool(EcsCommandBuffer&, ScenePreparedPrefab3D, ScenePrefabPoolOptions3D = {}) const;
    SceneRequest3D release(EcsCommandBuffer&, ScenePreparedPrefab3D) const;
    SceneRequest3D release(EcsCommandBuffer&, ScenePrefabPool3D) const;

private:
    explicit SceneCommands3D(std::weak_ptr<detail::SceneCommandQueue3D> queue) : _queue(std::move(queue)) {}
    SceneRequest3D submit(EcsCommandBuffer&, std::shared_ptr<detail::SceneOperation3D>) const;
    std::weak_ptr<detail::SceneCommandQueue3D> _queue;
    friend class SceneSession3D;
};

struct SceneQueryFilter3D {
    Layer3DMask mask = all_layers3d;
    SceneEntity3D ignore; // checked scene entity; excludes all of its native bodies
    bool sensors = false;
    bool initial_overlap = false; // sweeps: include outward initial convex overlap
};
template <class Hit> struct SceneQueryHit3D {
    Hit physics;
    SceneEntity3D entity; // empty for native bodies without a live ECS binding
};
using SceneRayHit3D = SceneQueryHit3D<RayHit3D>;
using SceneShapeHit3D = SceneQueryHit3D<ShapeHit3D>;
using SceneOverlapHit3D = SceneQueryHit3D<OverlapHit3D>;
// Weak, scene-generation endpoint. Renderer-thread queries read native physics
// as last synchronized/stepped; they never sync or simulate implicitly. Safe to
// retain after reload/unload (unavailable), but do not query from worker systems.
class SceneQueries3D {
public:
    SceneQueries3D() = default;
    explicit operator bool() const;
    SceneEntity3D handle(EcsEntity entity) const;
    EcsEntity resolve(SceneEntity3D entity) const; // owner thread, current scene/lease
    std::optional<SceneRayHit3D> raycast(Vec3f from, Vec3f to, const SceneQueryFilter3D& filter = {}) const;
    BoundedQueryResult3D raycast_all(Vec3f from, Vec3f to, std::span<SceneRayHit3D> output,
                                     const SceneQueryFilter3D& filter = {}) const;
    std::optional<SceneShapeHit3D> shape_cast(const Shape3D& shape, Vec3f position, Quatf rotation, Vec3f displacement,
                                              const SceneQueryFilter3D& filter = {}) const;
    BoundedQueryResult3D overlap(const Shape3D& shape, Vec3f position, Quatf rotation,
                                 std::span<SceneOverlapHit3D> output, const SceneQueryFilter3D& filter = {}) const;
    // Checks scene/lease and native body identity. A body rebuilt on the same
    // live entity invalidates the captured hit as well. Hits retain no resources.
    template <class Hit> EcsEntity resolve(const SceneQueryHit3D<Hit>& hit) const {
        return resolve_hit(hit.entity, hit.physics.body);
    }

private:
    explicit SceneQueries3D(std::weak_ptr<detail::SceneCommandQueue3D> queue) : _queue(std::move(queue)) {}
    EcsEntity resolve_hit(SceneEntity3D entity, Body3D body) const;
    std::weak_ptr<detail::SceneCommandQueue3D> _queue;
    friend class SceneSession3D;
};

// What SceneSession3D::pick found under a pixel of the last rendered frame.
struct ScenePick3D {
    // The live entity drawn there: one whose MeshRenderer3D, ModelRenderer3D or SkinnedMeshRenderer3D draws with `id`
    // (its own id, or the entity's low 32 bits when that is 0). Empty when none does: a compose() draw, or an entity
    // gone since.
    EcsEntity entity;
    u32 id = 0;       // the id drawn there (PickResult3D::id)
    Vec3f position{}; // the point of it the pixel showed, in the world
};

struct SceneSession3DContext {
    EcsWorld& world;
    const SceneInstance& instance;
    WorldRenderState3D& rendering;
    World3D* physics;
    WorldPhysicsState3D* bodies;
    // Initialization can declare custom content dependencies for explicit polling.
    std::vector<std::filesystem::path>& dependencies;
    SceneCommands3D commands;
    SceneQueries3D queries;
};

struct SceneScriptOptions3D {
    // Optional application/input services for Lua callbacks. These and the
    // component registry outlive the session. Missing input reads false.
    App* app = nullptr;
    Input* input = nullptr;
    const ScriptComponentRegistry* components = nullptr;
    // Applied to every fresh VM after the stock ECS and scene3d bindings.
    std::function<void(sol::state&)> bind;
};

struct SceneSession3DOptions {
    Vec2i viewport{960, 540};
    // The scene's SceneEnvironment3D replaces these (scene3d_view_options), but `pick` is the game's: true keeps a
    // pick pass in every render (one more pass over the opaque meshes) for pick(). Off by default.
    ViewOptions3D view;
    // No physics world/scratch pool allocated unless requested. Tune capacities
    // for the game; a transactional replacement briefly owns two physics worlds.
    std::optional<World3DConfig> physics;
    // Debug drawing only; the host owns the fixed timestep and catch-up policy.
    PhysicsDebug3DSettings physics_debug;
    ColliderAssetLimits3D collision_limits;
    ParticleLimits3D particle_limits;
    SceneAssetLoadLimits limits{8u * 1024u * 1024u, 10000};
    std::size_t max_dependencies = 4096;
    // A live field batch stages only touched entities, transform descendants
    // and transform ancestors. No second scene or physics world is allocated.
    std::size_t max_edit_entities = 64, max_edit_fields = 256;
    std::size_t max_spawn_entities = 64, max_runtime_entities = 1024, max_runtime_prefabs = 128;
    // Includes requests still held by unflushed system command buffers.
    std::size_t max_pending_commands = 64;
    std::size_t max_preloaded_prefabs = 16, max_prefab_pools = 16;
    bool warm_up = true; // only a new view; never mutate the active view during preparation
    // Also called for isolated live-edit staging worlds. Register definitions
    // and systems only; keep game initialization/external side effects below.
    std::function<void(EcsWorld&)> register_components;
    // Runs on the renderer thread after asset binding, before body creation.
    // Mutate only the supplied candidate context; throw to reject preparation.
    std::function<void(SceneSession3DContext&)> initialize;
    // Optional per-frame game drawing/settings after the stock scene collection.
    std::function<void(SceneSession3DContext&, Scene3D&)> compose;
    // Optional ECS audio (kin/ecs/audio.hpp), both or neither: each step() then runs update_audio_listeners,
    // update_audio_emitters and consume_audio_one_shots after the Audio phase's systems, so emitters and one-shots
    // sound from their WorldTransform3D and the listener follows the AudioListener entity or else the active
    // SceneCamera3D. Both outlive the session; the host still calls audio.update(dt) once a frame.
    AudioEngine* audio = nullptr;
    const AudioCatalog* audio_catalog = nullptr;
    SceneScriptOptions3D scripts;
};

enum class SceneSession3DReload { Unchanged, Activated };

struct SceneFieldEdit3D {
    EcsEntity entity;
    std::string component, field;
    ComponentFieldValue value;
};

// Authored 3D scene ownership and transactional replacement. The renderer
// and renderer-scoped Assets3D must outlive this object and retained handles.
// Synchronous, renderer-thread-only. No background jobs or implicit file polling.
// Headless with a renderer that has no 3D (Renderer3D::available() false: a server,
// a test without a GPU): everything but drawing runs the same, on CPU-only models
// (Assets3D::load_model); there is no view, and render() and pick() fail with
// "no renderer".
// Owns an isolated ECS world, optional physics, render state and view per scene.
// Optional Lua uses the same frame stages and deferred prefab commands as C++.
// on_load runs in preparation; on_enter must succeed before replacement commits.
// update runs before ECS Input; on_exit is best-effort cleanup on unload/replacement.
class SceneSession3D {
public:
    SceneSession3D(Renderer3D& renderer, Assets3D& assets, SceneSession3DOptions options = {});
    ~SceneSession3D();
    SceneSession3D(const SceneSession3D&) = delete;
    SceneSession3D& operator=(const SceneSession3D&) = delete;

    // prepare keeps the active scene intact; only one candidate is retained.
    // activate is the explicit frame-boundary swap. load does both. Failure
    // discards the candidate and retains the active world's state and view.
    Status3D prepare(const std::filesystem::path& scene, bool refresh_all_assets = false);
    Status3D activate();
    Status3D load(const std::filesystem::path& scene, bool refresh_all_assets = false);
    Status3D cancel();
    Status3D unload();
    Status3D reload(bool refresh_all_assets = false);
    // Poll only when desired (e.g. a development timer). Tracks scene/prefab,
    // models/external images/buffers, animator/machine files and custom sources.
    // Includes deletions, creations and content-pack remount identity.
    std::vector<std::filesystem::path> changed_sources() const;
    Result3D<SceneSession3DReload> reload_if_changed();

    // Renderer thread, between steps/collection. Existing reflected fields only;
    // one batch either commits or leaves authored fields/bindings/bodies intact.
    // The active registry's patch_field on loaded authored members (including
    // inspector edits) uses this same path for single fields. New runtime
    // entities opt in explicitly; prefab/spawn construction keeps its old path.
    // Batch edits allow cross-field invariants.
    // Keeps compatible animation/particle state and rigid-body velocity.
    // Controller/ragdoll/rope physics edits and constrained bodies require reload.
    Status3D edit(std::span<const SceneFieldEdit3D> edits);

    // Synchronous C++ gameplay construction, between frames. Reuses the shared
    // prefab loader and binds only new entities. Failure removes partial objects
    // and native physics bindings. Runtime instances are excluded from normal
    // authored saves and are owned until despawn/reload/unload.
    Result3D<ScenePrefab3D> spawn(const std::filesystem::path& prefab, SceneSpawn3D options = {});
    // Explicit renderer-thread loading. Retains one inactive bound reference;
    // prepared spawning and pool reuse do no source/asset loading. Snapshot
    // stays fixed until released/reloaded. No custom engine asset changes or scale changes.
    Result3D<ScenePreparedPrefab3D> preload(const std::filesystem::path& prefab);
    Status3D release(ScenePreparedPrefab3D); // refuses while a pool references it
    Result3D<ScenePrefab3D> spawn(ScenePreparedPrefab3D, SceneSpawn3D options = {});
    Result3D<ScenePrefabPool3D> create_pool(ScenePreparedPrefab3D, ScenePrefabPoolOptions3D options = {});
    Status3D release(ScenePrefabPool3D); // destroys active and inactive slots
    ScenePrefabPoolStats3D pool_stats(ScenePrefabPool3D) const;
    Result3D<ScenePrefab3D> spawn(ScenePrefabPool3D, SceneSpawn3D options = {});
    // despawn returns a pooled lease to its pool. Old lease handles become stale.
    Status3D despawn(ScenePrefab3D prefab); // includes nested runtime spawns; refuses foreign children
    SceneEntity3D entity(ScenePrefab3D prefab, std::string_view local_id);
    SceneEntity3D handle(EcsEntity entity);
    EcsEntity resolve(SceneEntity3D entity);
    bool alive(ScenePrefab3D prefab);
    std::size_t runtime_prefabs() const;
    std::size_t runtime_entities() const;
    std::size_t reserved_entities() const; // includes inactive pool/reference entities
    SceneCommands3D commands() const;
    SceneQueries3D queries() const;

    // Typed reflected configuration changes, using the same validated binding
    // transaction as edit(). Runtime input/state still uses native components
    // directly. Only reflected fields are applied; the callback edits a copy.
    template <class T, class Fn> Status3D reconfigure(SceneEntity3D entity, Fn change) {
        if (!resolve(entity)) {
            configuration_error("configuration requires a live session entity");
            return status3d(false, diagnostics());
        }
        const bool success =
            reconfigure_native(entity, world()->component<T>().id(),
                               [change = std::move(change)](EcsEntity copy) mutable { change(*copy.get_mut<T>()); });
        return status3d(success, diagnostics());
    }

    // One fixed step: Input..Physics systems, queued lifecycle work and native
    // physics; PostPhysics with fresh contacts and lifecycle cleanup; Animation
    // systems then native animation/clocks/particles; Audio..Editor and final
    // lifecycle/transform cleanup. Native simulations advance once, including
    // when their named ECS phases contain no systems. Failed phases stop later
    // stages; earlier committed work is not rolled back. Custom stepping can
    // instead use the accessors.
    Status3D step(f32 dt);
    Status3D collect_actions(InputActionContext&) const;
    Status3D write_report(JsonWriter&) const;
    // Clears internal draws, propagates transforms, applies authored settings,
    // clocks/weather, collects meshes/lights/ropes and calls compose. Recreates
    // the view only if its options changed; render also submits the frame.
    // alpha is the host's fixed-step accumulator / fixed_dt, in [0,1].
    // Interpolates draw/camera/bone/rope placement; gameplay transforms remain authoritative.
    Status3D collect(f32 alpha = 1);
    Status3D render(f32 alpha = 1);
    Status3D resize(Vec2i viewport);
    // What `pixel` (the view's own, from its top left) showed in the last render(): the entity drawn there and the
    // point of it. Draws nothing. Needs options.view.pick; waits for the GPU like Renderer3D::pick, so ask on a click,
    // not every frame. A successful nullopt over the background or outside the view; a failed result
    // without picking or a renderer.
    Result3D<std::optional<ScenePick3D>> pick(Vec2i pixel);

    bool active() const;
    bool prepared() const;
    u64 generation() const;
    EcsWorld* world();
    EcsEntity entity(std::string_view authored_id);
    WorldRenderState3D* rendering();
    World3D* physics();
    WorldPhysicsState3D* bodies();
    PhysicsAssets3D& physics_assets();
    const SceneInstance* instance() const;
    Scene3D* scene();
    const View3D* view() const; // null without an active scene, or headless
    const std::vector<std::string>& diagnostics() const;

private:
    bool collect_actions_operation(InputActionContext&) const;
    bool write_report_operation(JsonWriter&) const;
    bool prepare_operation(const std::filesystem::path& scene, bool refresh_all_assets = false);
    bool activate_operation();
    bool load_operation(const std::filesystem::path& scene, bool refresh_all_assets = false);
    bool reload_operation(bool refresh_all_assets = false);
    bool edit_operation(std::span<const SceneFieldEdit3D> edits);
    bool release_operation(ScenePrefabPool3D);
    bool release_operation(ScenePreparedPrefab3D);
    bool despawn_operation(ScenePrefab3D prefab);
    bool step_operation(f32 dt);
    bool collect_operation(f32 alpha = 1);
    bool render_operation(f32 alpha = 1);
    bool resize_operation(Vec2i viewport);
    ScenePrefab3D spawn_operation(ScenePrefabPool3D, SceneSpawn3D options = {});
    ScenePrefab3D spawn_operation(ScenePreparedPrefab3D, SceneSpawn3D options = {});
    ScenePrefab3D spawn_operation(const std::filesystem::path& prefab, SceneSpawn3D options = {});
    ScenePreparedPrefab3D preload_operation(const std::filesystem::path& prefab);
    ScenePrefabPool3D create_pool_operation(ScenePreparedPrefab3D, ScenePrefabPoolOptions3D options = {});
    std::optional<ScenePick3D> pick_operation(Vec2i pixel);
    ScenePreparedPrefab3D preload_now(const std::filesystem::path&);
    ScenePrefabPool3D create_pool_now(ScenePreparedPrefab3D, ScenePrefabPoolOptions3D);
    bool release_now(ScenePreparedPrefab3D);
    bool release_now(ScenePrefabPool3D);
    ScenePrefab3D spawn_now(const std::filesystem::path&, SceneSpawn3D, const PrefabAsset* = nullptr, u64 prepared = 0);
    ScenePrefab3D spawn_prepared(ScenePreparedPrefab3D, SceneSpawn3D);
    ScenePrefab3D acquire_pool(ScenePrefabPool3D, SceneSpawn3D);
    bool recycle_pool(ScenePrefab3D);
    bool despawn_now(ScenePrefab3D);
    void drain_commands(bool systems_succeeded);
    bool configuration_error(std::string error);
    bool reconfigure_native(SceneEntity3D entity, ComponentId component, const std::function<void(EcsEntity)>& change);
    struct Data;
    std::unique_ptr<Data> _data;
};

} // namespace kin
