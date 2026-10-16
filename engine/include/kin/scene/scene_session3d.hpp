#pragma once

#include <kin/ecs/scene3d.hpp>
#include <kin/scene/scene_entity3d.hpp>
#include <kin/ecs/physics3d.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/scene/scene_asset_runtime.hpp>
#include <kin/physics3d/assets3d.hpp>

#include <functional>

namespace kin {

struct ScenePrefab3D {
    u64 session = 0, generation = 0, id = 0;
    SceneEntity3D root;
    explicit operator bool() const { return session && id; }
};
struct SceneSpawnContext3D {
    EcsWorld& world;
    EcsEntity root;
    const std::unordered_map<std::string,EcsEntity>& entities;
    EcsEntity entity(std::string_view local_id) const {
        const auto it=entities.find(std::string(local_id));
        return it==entities.end()?EcsEntity{}:it->second;
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
    u64 session=0,generation=0,id=0;
    explicit operator bool()const{return session&&id;}
};
struct ScenePrefabPool3D {
    u64 session=0,generation=0,id=0;
    explicit operator bool()const{return session&&id;}
};
struct ScenePrefabPoolOptions3D {
    std::size_t capacity=16;
    // Reset arbitrary native game state on every acquisition (and warm-up).
    // Configure only game-owned components; engine settings/assets are fixed.
    std::function<void(SceneSpawnContext3D&)> reset;
    // Optional cleanup of per-lease game resources on return. No structure edits.
    std::function<void(SceneSpawnContext3D&)> release;
};
struct ScenePrefabPoolStats3D {
    std::size_t capacity=0,active=0,available=0;
    u64 acquired=0,reused=0,exhausted=0;
};

class EcsCommandBuffer;
namespace detail { struct SceneRequestState3D; struct SceneCommandQueue3D; struct SceneOperation3D; }
enum class SceneRequestStatus3D { Invalid, Pending, Succeeded, Failed, Cancelled };
// Owns a small result only, never a world, assets or constructor captures.
// Safe to retain across scene replacement; inspect the status before the result.
class SceneRequest3D {
public:
    SceneRequestStatus3D status() const;
    ScenePrefab3D prefab() const; // populated only for a successful spawn
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
private:
    explicit SceneCommands3D(std::weak_ptr<detail::SceneCommandQueue3D> queue):_queue(std::move(queue)){}
    SceneRequest3D submit(EcsCommandBuffer&, std::shared_ptr<detail::SceneOperation3D>) const;
    std::weak_ptr<detail::SceneCommandQueue3D> _queue;
    friend class SceneSession3D;
};

struct SceneQueryFilter3D {
    Layer3DMask mask=all_layers3d;
    SceneEntity3D ignore; // checked scene entity; excludes all of its native bodies
    bool sensors=false;
    bool initial_overlap=false; // sweeps: include outward initial convex overlap
};
template<class Hit> struct SceneQueryHit3D {
    Hit physics;
    SceneEntity3D entity; // empty for native bodies without a live ECS binding
};
using SceneRayHit3D=SceneQueryHit3D<RayHit3D>;
using SceneShapeHit3D=SceneQueryHit3D<ShapeHit3D>;
using SceneOverlapHit3D=SceneQueryHit3D<OverlapHit3D>;
// Weak, scene-generation endpoint. Renderer-thread queries read native physics
// as last synchronized/stepped; they never sync or simulate implicitly. Safe to
// retain after reload/unload (unavailable), but do not query from worker systems.
class SceneQueries3D {
public:
    SceneQueries3D()=default;
    explicit operator bool()const;
    SceneEntity3D handle(EcsEntity entity)const;
    EcsEntity resolve(SceneEntity3D entity)const; // owner thread, current scene/lease
    std::optional<SceneRayHit3D> raycast(Vec3f from,Vec3f to,const SceneQueryFilter3D& filter={})const;
    BoundedQueryResult3D raycast_all(Vec3f from,Vec3f to,std::span<SceneRayHit3D> output,const SceneQueryFilter3D& filter={})const;
    std::optional<SceneShapeHit3D> shape_cast(const Shape3D& shape,Vec3f position,Quatf rotation,Vec3f displacement,const SceneQueryFilter3D& filter={})const;
    BoundedQueryResult3D overlap(const Shape3D& shape,Vec3f position,Quatf rotation,std::span<SceneOverlapHit3D> output,const SceneQueryFilter3D& filter={})const;
    // Checks scene/lease and native body identity. A body rebuilt on the same
    // live entity invalidates the captured hit as well. Hits retain no resources.
    template<class Hit> EcsEntity resolve(const SceneQueryHit3D<Hit>& hit)const {return resolve_hit(hit.entity,hit.physics.body);}
private:
    explicit SceneQueries3D(std::weak_ptr<detail::SceneCommandQueue3D> queue):_queue(std::move(queue)){}
    EcsEntity resolve_hit(SceneEntity3D entity,Body3D body)const;
    std::weak_ptr<detail::SceneCommandQueue3D> _queue;
    friend class SceneSession3D;
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

struct SceneSession3DOptions {
    Vec2i viewport{960, 540};
    View3DOptions view;
    // No physics world/scratch pool allocated unless requested. Tune capacities
    // for the game; a transactional replacement briefly owns two physics worlds.
    std::optional<World3DConfig> physics;
    WorldPhysicsState3DConfig bodies;
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
};

enum class SceneSession3DReload { Unchanged, Activated, Failed };

struct SceneFieldEdit3D {
    EcsEntity entity;
    std::string component, field;
    ComponentFieldValue value;
};

// Authored C++ 3D scene ownership and transactional replacement. The renderer
// and renderer-scoped Assets3D must outlive this object and retained handles.
// Synchronous, renderer-thread-only. No background jobs or implicit file polling.
// Owns an isolated ECS world, optional physics, render state and view per scene.
// Scene assets with a Lua script are rejected: use ScriptScene for its context
// lifecycle instead of silently omitting script execution.
class SceneSession3D {
public:
    SceneSession3D(Renderer3D& renderer, Assets3D& assets, SceneSession3DOptions options = {});
    ~SceneSession3D();
    SceneSession3D(const SceneSession3D&) = delete;
    SceneSession3D& operator=(const SceneSession3D&) = delete;

    // prepare keeps the active scene intact; only one candidate is retained.
    // activate is the explicit frame-boundary swap. load does both. Failure
    // discards the candidate and retains the active world's state and view.
    bool prepare(const std::filesystem::path& scene, bool refresh_all_assets = false);
    bool activate();
    bool load(const std::filesystem::path& scene, bool refresh_all_assets = false);
    void cancel();
    void unload();
    bool reload(bool refresh_all_assets = false);
    // Poll only when desired (e.g. a development timer). Tracks scene/prefab,
    // models/external images/buffers, animator/machine files and custom sources.
    // Includes deletions, creations and content-pack remount identity.
    std::vector<std::filesystem::path> changed_sources() const;
    SceneSession3DReload reload_if_changed();

    // Renderer thread, between steps/collection. Existing reflected fields only;
    // one batch either commits or leaves authored fields/bindings/bodies intact.
    // The active registry's patch_field on loaded authored members (including
    // inspector edits) uses this same path for single fields. New runtime
    // entities opt in explicitly; prefab/spawn construction keeps its old path.
    // Batch edits allow cross-field invariants.
    // Keeps compatible animation/particle state and rigid-body velocity.
    // Controller/ragdoll/rope physics edits and constrained bodies require reload.
    bool edit(std::span<const SceneFieldEdit3D> edits);

    // Synchronous C++ gameplay construction, between frames. Reuses the shared
    // prefab loader and binds only new entities. Failure removes partial objects
    // and native physics bindings. Runtime instances are excluded from normal
    // authored saves and are owned until despawn/reload/unload.
    ScenePrefab3D spawn(const std::filesystem::path& prefab, SceneSpawn3D options = {});
    // Explicit renderer-thread loading. Retains one inactive bound reference;
    // prepared spawning and pool reuse do no source/asset loading. Snapshot
    // stays fixed until released/reloaded. No custom engine asset changes or scale changes.
    ScenePreparedPrefab3D preload(const std::filesystem::path& prefab);
    bool release(ScenePreparedPrefab3D); // refuses while a pool references it
    ScenePrefab3D spawn(ScenePreparedPrefab3D, SceneSpawn3D options = {});
    ScenePrefabPool3D create_pool(ScenePreparedPrefab3D, ScenePrefabPoolOptions3D options = {});
    bool release(ScenePrefabPool3D); // destroys active and inactive slots
    ScenePrefabPoolStats3D pool_stats(ScenePrefabPool3D) const;
    ScenePrefab3D spawn(ScenePrefabPool3D, SceneSpawn3D options = {});
    // despawn returns a pooled lease to its pool. Old lease handles become stale.
    bool despawn(ScenePrefab3D prefab); // includes nested runtime spawns; refuses foreign children
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
    template<class T, class Fn>
    bool reconfigure(SceneEntity3D entity, Fn change) {
        if(!resolve(entity))return configuration_error("configuration requires a live session entity");
        return reconfigure_native(entity,world()->component<T>().id(),
            [change=std::move(change)](EcsEntity copy) mutable {change(*copy.get_mut<T>());});
    }

    // One fixed step: Input..Physics systems, queued lifecycle work and native
    // physics; PostPhysics with fresh contacts and lifecycle cleanup; Animation
    // systems then native animation/clocks/particles; Audio..Editor and final
    // lifecycle/transform cleanup. Native simulations advance once, including
    // when their named ECS phases contain no systems. Failed phases stop later
    // stages; earlier committed work is not rolled back. Custom stepping can
    // instead use the accessors.
    bool step(f32 dt);
    // Clears internal draws, propagates transforms, applies authored settings,
    // clocks/weather, collects meshes/lights/ropes and calls compose. Recreates
    // the view only if its options changed; render also submits the frame.
    bool collect();
    bool render();
    bool resize(Vec2i viewport);

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
    const View3D* view() const;
    const std::vector<std::string>& diagnostics() const;

private:
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
